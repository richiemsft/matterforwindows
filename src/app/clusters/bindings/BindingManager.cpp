/*
 *
 *    Copyright (c) 2022-2025 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <app/clusters/bindings/BindingManager.h>
#include <app/clusters/bindings/binding-table.h>
#include <credentials/FabricTable.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>

namespace chip {
namespace app {
namespace Clusters {
namespace Binding {

Manager Manager::sBindingManager;

CHIP_ERROR Manager::UnicastBindingCreated(uint8_t fabricIndex, NodeId nodeId)
{
    return EstablishConnection(ScopedNodeId(nodeId, fabricIndex));
}

CHIP_ERROR Manager::UnicastBindingRemoved(uint8_t bindingEntryId)
{
    VerifyOrReturnError(mInitParams.mFabricTable != nullptr, CHIP_ERROR_INCORRECT_STATE);
    ++mDispatchDepth;
    auto dispatch = ScopeExit([this] { --mDispatchDepth; });
    mPendingNotificationMap.RemoveEntry(bindingEntryId);
    return CHIP_NO_ERROR;
}

CHIP_ERROR Manager::Init(const ManagerInitParams & params)
{
    VerifyOrReturnError(!mShuttingDown && mDispatchDepth == 0, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(params.mCASESessionManager != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(params.mFabricTable != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(params.mStorage != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    if (mInitParams.mFabricTable != nullptr)
    {
        // Preserve repeated singleton initialization without reloading live bindings or duplicating callbacks.
        VerifyOrReturnError(params.mFabricTable == mInitParams.mFabricTable &&
                                params.mCASESessionManager == mInitParams.mCASESessionManager &&
                                params.mStorage == mInitParams.mStorage &&
                                params.mEstablishConnectionOnInit == mInitParams.mEstablishConnectionOnInit,
                            CHIP_ERROR_INCORRECT_STATE);
        return CHIP_NO_ERROR;
    }
    mBindingTable.SetPersistentStorage(params.mStorage);

    bool storagePresent;
    CHIP_ERROR error = mBindingTable.LoadFromStorage(storagePresent);
    if (error == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND && !storagePresent)
    {
        error = CHIP_NO_ERROR;
    }
    if (error == CHIP_NO_ERROR)
    {
        error = params.mFabricTable->AddFabricDelegate(this);
    }
    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(AppServer, "Cannot initialize binding manager: %" CHIP_ERROR_FORMAT, error.Format());
        mBindingTable.ClearInMemory();
        mBindingTable.SetPersistentStorage(nullptr);
        return error;
    }
    mInitParams = params;
    if (params.mEstablishConnectionOnInit)
    {
        for (const TableEntry & entry : mBindingTable)
        {
            if (entry.type == MATTER_UNICAST_BINDING)
            {
                error = UnicastBindingCreated(entry.fabricIndex, entry.nodeId);
                if (error != CHIP_NO_ERROR)
                {
                    ChipLogError(AppServer, "Binding session will retry on demand: %" CHIP_ERROR_FORMAT, error.Format());
                }
            }
        }
    }
    return CHIP_NO_ERROR;
}

void Manager::RemoveConnectionCallback(ConnectionCallback * callback)
{
    auto ** link = &mConnectionCallbacks;
    while (*link != nullptr && *link != callback)
    {
        link = &(*link)->mNext;
    }
    VerifyOrDie(*link == callback);
    *link           = callback->mNext;
    callback->mNext = nullptr;
}

void Manager::Shutdown()
{
    VerifyOrReturn(!mShuttingDown);
    mShuttingDown = true;
    if (mInitParams.mFabricTable != nullptr)
    {
        mInitParams.mFabricTable->RemoveFabricDelegate(this);
    }
    mInitParams = {};
    while (mConnectionCallbacks != nullptr)
    {
        auto * callback = mConnectionCallbacks;
        RemoveConnectionCallback(callback);
        Platform::Delete(callback);
    }
    mPendingNotificationMap.Clear();
    mBindingTable.ClearInMemory();
    mBindingTable.SetPersistentStorage(nullptr);
    mLastSessionEstablishmentError = CHIP_NO_ERROR;
    mShuttingDown                  = false;
}

CHIP_ERROR Manager::EstablishConnection(const ScopedNodeId & nodeId)
{
    VerifyOrReturnError(mInitParams.mCASESessionManager != nullptr, CHIP_ERROR_INCORRECT_STATE);
    ++mDispatchDepth;
    auto dispatch = ScopeExit([this] { --mDispatchDepth; });

    mLastSessionEstablishmentError = CHIP_NO_ERROR;
    auto * connectionCallback      = Platform::New<ConnectionCallback>(*this);
    VerifyOrReturnError(connectionCallback != nullptr, CHIP_ERROR_NO_MEMORY);
    connectionCallback->mNext = mConnectionCallbacks;
    mConnectionCallbacks      = connectionCallback;

    mInitParams.mCASESessionManager->FindOrEstablishSession(nodeId, connectionCallback->GetOnDeviceConnected(),
                                                            connectionCallback->GetOnDeviceConnectionFailure());
    VerifyOrReturnError(mInitParams.mCASESessionManager != nullptr, CHIP_ERROR_INCORRECT_STATE);
    if (mLastSessionEstablishmentError == CHIP_ERROR_NO_MEMORY)
    {
        // Release the least recently used entry
        ScopedNodeId peerToRemove;
        if (mPendingNotificationMap.FindLRUConnectPeer(peerToRemove) == CHIP_NO_ERROR)
        {
            mPendingNotificationMap.RemoveAllEntriesForNode(peerToRemove);
            VerifyOrReturnError(mInitParams.mCASESessionManager != nullptr, CHIP_ERROR_INCORRECT_STATE);

            // Now retry
            mLastSessionEstablishmentError = CHIP_NO_ERROR;
            // At this point connectionCallback is null since it deletes itself when the callback is called.
            connectionCallback = Platform::New<ConnectionCallback>(*this);
            VerifyOrReturnError(connectionCallback != nullptr, CHIP_ERROR_NO_MEMORY);
            connectionCallback->mNext = mConnectionCallbacks;
            mConnectionCallbacks      = connectionCallback;
            mInitParams.mCASESessionManager->FindOrEstablishSession(nodeId, connectionCallback->GetOnDeviceConnected(),
                                                                    connectionCallback->GetOnDeviceConnectionFailure());
        }
    }
    return mLastSessionEstablishmentError;
}

void Manager::HandleDeviceConnected(Messaging::ExchangeManager & exchangeMgr, const SessionHandle & sessionHandle)
{
    ++mDispatchDepth;
    auto dispatch              = ScopeExit([this] { --mDispatchDepth; });
    FabricIndex fabricToRemove = kUndefinedFabricIndex;
    NodeId nodeToRemove        = kUndefinedNodeId;

    // Note: not using a const ref here, because the mPendingNotificationMap
    // iterator returns things by value anyway.
    for (PendingNotificationEntry pendingNotification : mPendingNotificationMap)
    {
        TableEntry entry = mBindingTable.GetAt(pendingNotification.mBindingEntryId);

        if (sessionHandle->GetPeer() == ScopedNodeId(entry.nodeId, entry.fabricIndex))
        {
            fabricToRemove = entry.fabricIndex;
            nodeToRemove   = entry.nodeId;
            OperationalDeviceProxy device(&exchangeMgr, sessionHandle);
            mBoundDeviceChangedHandler(entry, &device, pendingNotification.mContext->GetContext());
            VerifyOrReturn(mInitParams.mFabricTable != nullptr);
        }
    }

    mPendingNotificationMap.RemoveAllEntriesForNode(ScopedNodeId(nodeToRemove, fabricToRemove));
}

void Manager::HandleDeviceConnectionFailure(const ScopedNodeId & peerId, CHIP_ERROR error)
{
    // Simply release the entry, the connection will be re-established as needed.
    ChipLogError(AppServer, "Failed to establish connection to node 0x" ChipLogFormatX64, ChipLogValueX64(peerId.GetNodeId()));
    mLastSessionEstablishmentError = error;
    // We don't release the entry when connection fails, because inside
    // BindingManager::EstablishConnection we may try again the connection.
    // TODO(#22173): The logic in there doesn't actually make any sense with how
    // mPendingNotificationMap and CASESessionManager are implemented today.
}

void Manager::FabricRemoved(FabricIndex fabricIndex)
{
    VerifyOrDie(mInitParams.mCASESessionManager != nullptr);
    ++mDispatchDepth;
    auto dispatch = ScopeExit([this] { --mDispatchDepth; });

    mPendingNotificationMap.RemoveAllEntriesForFabric(fabricIndex);
    VerifyOrReturn(mInitParams.mCASESessionManager != nullptr);

    // TODO(#18436): NOC cluster should handle fabric removal without needing binding manager
    //               to execute such a release. Currently not done because paths were not tested.
    mInitParams.mCASESessionManager->ReleaseSessionsForFabric(fabricIndex);
}

CHIP_ERROR Manager::NotifyBoundClusterChanged(EndpointId endpoint, ClusterId cluster, void * context)
{
    VerifyOrReturnError(mInitParams.mFabricTable != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mBoundDeviceChangedHandler != nullptr, CHIP_ERROR_HANDLER_NOT_SET);
    ++mDispatchDepth;
    auto dispatch = ScopeExit([this] { --mDispatchDepth; });

    CHIP_ERROR error      = CHIP_NO_ERROR;
    auto * bindingContext = mPendingNotificationMap.NewPendingNotificationContext(context);
    VerifyOrReturnError(bindingContext != nullptr, CHIP_ERROR_NO_MEMORY);

    bindingContext->IncrementConsumersNumber();

    for (auto iter = mBindingTable.begin(); iter != mBindingTable.end(); ++iter)
    {
        if (iter->local == endpoint && (iter->clusterId.value_or(cluster) == cluster))
        {
            if (iter->type == MATTER_UNICAST_BINDING)
            {
                error = mPendingNotificationMap.AddPendingNotification(iter.GetIndex(), bindingContext);
                SuccessOrExit(error);
                if (mInitParams.mFabricTable == nullptr)
                {
                    mPendingNotificationMap.Clear();
                    error = CHIP_ERROR_INCORRECT_STATE;
                    goto exit;
                }
                error = EstablishConnection(ScopedNodeId(iter->nodeId, iter->fabricIndex));
                SuccessOrExit(error);
            }
            else if (iter->type == MATTER_MULTICAST_BINDING)
            {
                TableEntry entry = *iter;
                mBoundDeviceChangedHandler(entry, nullptr, bindingContext->GetContext());
            }
            VerifyOrExit(mInitParams.mFabricTable != nullptr, error = CHIP_ERROR_INCORRECT_STATE);
        }
    }

exit:
    bindingContext->DecrementConsumersNumber();

    return error;
}

CHIP_ERROR Manager::AddBindingEntry(const Binding::TableEntry & entry)
{
    VerifyOrReturnError(mInitParams.mFabricTable != nullptr, CHIP_ERROR_INCORRECT_STATE);
    CHIP_ERROR err = mBindingTable.Add(entry);
    if (err == CHIP_ERROR_NO_MEMORY)
    {
        return CHIP_IM_GLOBAL_STATUS(ResourceExhausted);
    }

    if (err != CHIP_NO_ERROR)
    {
        return err;
    }

    if (entry.type == Binding::MATTER_UNICAST_BINDING)
    {
        err = UnicastBindingCreated(entry.fabricIndex, entry.nodeId);
        if (err != CHIP_NO_ERROR)
        {
            // Unicast connection failure can happen if peer is offline. We'll retry connection on-demand.
            ChipLogError(
                Zcl, "Binding: Failed to create session for unicast binding to device " ChipLogFormatX64 ": %" CHIP_ERROR_FORMAT,
                ChipLogValueX64(entry.nodeId), err.Format());
        }
    }

    return CHIP_NO_ERROR;
}

} // namespace Binding

CHIP_ERROR AddBindingEntry(const Binding::TableEntry & entry)
{
    return Binding::Manager::GetInstance().AddBindingEntry(entry);
}

} // namespace Clusters
} // namespace app
} // namespace chip
