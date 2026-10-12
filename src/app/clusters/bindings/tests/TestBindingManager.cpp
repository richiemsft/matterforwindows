/*
 *    Copyright (c) 2026 Project CHIP Authors
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
#include <pw_unit_test/framework.h>

#include <app/clusters/bindings/BindingManager.h>
#include <app/server-cluster/testing/FabricTestFixture.h>
#include <lib/core/TLV.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/TestPersistentStorageDelegate.h>

namespace chip::app::Clusters::Binding {
class ManagerTestAccess
{
public:
    static CHIP_ERROR QueueConnection(Manager & manager, Callback::CallbackDeque & connected, Callback::CallbackDeque & failed)
    {
        auto * callback = Platform::New<Manager::ConnectionCallback>(manager);
        VerifyOrReturnError(callback != nullptr, CHIP_ERROR_NO_MEMORY);
        callback->mNext              = manager.mConnectionCallbacks;
        manager.mConnectionCallbacks = callback;
        connected.Enqueue(callback->GetOnDeviceConnected()->Cancel());
        failed.Enqueue(callback->GetOnDeviceConnectionFailure()->Cancel());
        return CHIP_NO_ERROR;
    }

    static CHIP_ERROR QueueNotification(Manager & manager, uint8_t bindingIndex, void * context)
    {
        auto * pending = manager.mPendingNotificationMap.NewPendingNotificationContext(context);
        VerifyOrReturnError(pending != nullptr, CHIP_ERROR_NO_MEMORY);
        pending->IncrementConsumersNumber();
        auto error = manager.mPendingNotificationMap.AddPendingNotification(bindingIndex, pending);
        pending->DecrementConsumersNumber();
        return error;
    }
};
} // namespace chip::app::Clusters::Binding

namespace {
using namespace chip;
using namespace chip::app::Clusters::Binding;

class TestBindingManager : public ::testing::Test
{
public:
    static void SetUpTestSuite() { ASSERT_EQ(Platform::MemoryInit(), CHIP_NO_ERROR); }
    static void TearDownTestSuite() { Platform::MemoryShutdown(); }
    void TearDown() override { manager.Shutdown(); }

    ManagerInitParams Params()
    {
        ManagerInitParams params;
        params.mFabricTable               = &fabrics;
        params.mCASESessionManager        = &sessions;
        params.mStorage                   = &storage;
        params.mEstablishConnectionOnInit = false;
        return params;
    }

    TestPersistentStorageDelegate storage;
    FabricTable fabrics;
    CASESessionManager sessions;
    Manager manager;
};

TEST_F(TestBindingManager, UninitializedOperationsAreRejected)
{
    EXPECT_EQ(manager.NotifyBoundClusterChanged(1, 6, nullptr), CHIP_ERROR_INCORRECT_STATE);
    EXPECT_EQ(manager.UnicastBindingCreated(1, 2), CHIP_ERROR_INCORRECT_STATE);
    EXPECT_EQ(manager.UnicastBindingRemoved(0), CHIP_ERROR_INCORRECT_STATE);
    EXPECT_EQ(manager.AddBindingEntry(TableEntry::ForGroup(1, 2, 1, 6)), CHIP_ERROR_INCORRECT_STATE);
    manager.Shutdown();
    manager.Shutdown();
}

TEST_F(TestBindingManager, EmptyStorageAndRepeatInitialization)
{
    ASSERT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
    EXPECT_EQ(manager.GetBindingTable().Size(), 0);
    EXPECT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
    manager.Shutdown();
    ASSERT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
    EXPECT_EQ(manager.NotifyBoundClusterChanged(1, 6, nullptr), CHIP_ERROR_HANDLER_NOT_SET);
}

TEST_F(TestBindingManager, RepeatInitializationPreservesLiveStateAndRejectsChangedDependencies)
{
    ASSERT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
    const auto entry = TableEntry::ForGroup(1, 2, 1, 6);
    ASSERT_EQ(manager.AddBindingEntry(entry), CHIP_NO_ERROR);
    storage.AddPoisonKey(DefaultStorageKeyAllocator::BindingTable().KeyName());
    Callback::CallbackDeque connected;
    Callback::CallbackDeque failed;
    ASSERT_EQ(ManagerTestAccess::QueueConnection(manager, connected, failed), CHIP_NO_ERROR);
    auto * first = connected.First();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
    EXPECT_EQ(connected.First(), first);
    EXPECT_EQ(manager.GetBindingTable().Size(), 1);
    EXPECT_TRUE(manager.GetBindingTable().GetAt(0) == entry);

    FabricTable otherFabrics;
    CASESessionManager otherSessions;
    TestPersistentStorageDelegate otherStorage;
    auto params         = Params();
    params.mFabricTable = &otherFabrics;
    EXPECT_EQ(manager.Init(params), CHIP_ERROR_INCORRECT_STATE);
    params                     = Params();
    params.mCASESessionManager = &otherSessions;
    EXPECT_EQ(manager.Init(params), CHIP_ERROR_INCORRECT_STATE);
    params          = Params();
    params.mStorage = &otherStorage;
    EXPECT_EQ(manager.Init(params), CHIP_ERROR_INCORRECT_STATE);
    params                            = Params();
    params.mEstablishConnectionOnInit = true;
    EXPECT_EQ(manager.Init(params), CHIP_ERROR_INCORRECT_STATE);
    EXPECT_EQ(connected.First(), first);
    manager.Shutdown();
    EXPECT_TRUE(connected.IsEmpty());
    EXPECT_TRUE(failed.IsEmpty());
}

TEST_F(TestBindingManager, ShutdownPreservesBindingsAndReloadDoesNotDuplicate)
{
    ASSERT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
    const auto entry = TableEntry::ForGroup(1, 2, 1, 6);
    ASSERT_EQ(manager.AddBindingEntry(entry), CHIP_NO_ERROR);
    const auto keys = storage.GetNumKeys();
    storage.SetRejectWrites(true);
    manager.Shutdown();
    EXPECT_EQ(storage.GetNumKeys(), keys);
    EXPECT_EQ(manager.GetBindingTable().Size(), 0);
    EXPECT_EQ(manager.GetBindingTable().Add(entry), CHIP_ERROR_INCORRECT_STATE);
    for (int i = 0; i < 3; ++i)
    {
        ASSERT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
        EXPECT_EQ(manager.GetBindingTable().Size(), 1);
        EXPECT_TRUE(manager.GetBindingTable().GetAt(0) == entry);
        manager.Shutdown();
    }
}

TEST_F(TestBindingManager, StorageErrorsAreReturnedAndInitializationCanRetry)
{
    storage.AddPoisonKey(DefaultStorageKeyAllocator::BindingTable().KeyName());
    EXPECT_EQ(manager.Init(Params()), CHIP_ERROR_PERSISTED_STORAGE_FAILED);
    EXPECT_EQ(manager.GetBindingTable().Size(), 0);
    EXPECT_EQ(manager.AddBindingEntry(TableEntry::ForGroup(1, 2, 1, 6)), CHIP_ERROR_INCORRECT_STATE);
    storage.ClearPoisonKeys();
    ASSERT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
}

TEST_F(TestBindingManager, MissingStoredEntryIsNotTreatedAsFirstBoot)
{
    ASSERT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
    ASSERT_EQ(manager.AddBindingEntry(TableEntry::ForGroup(1, 2, 1, 6)), CHIP_NO_ERROR);
    manager.Shutdown();
    ASSERT_EQ(storage.SyncDeleteKeyValue(DefaultStorageKeyAllocator::BindingTableEntry(0).KeyName()), CHIP_NO_ERROR);
    EXPECT_EQ(manager.Init(Params()), CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND);
    EXPECT_EQ(manager.GetBindingTable().Size(), 0);
}

TEST_F(TestBindingManager, MalformedStoredTableIsNotTreatedAsFirstBoot)
{
    const uint8_t invalid = 0xFF;
    ASSERT_EQ(storage.SyncSetKeyValue(DefaultStorageKeyAllocator::BindingTable().KeyName(), &invalid, 1), CHIP_NO_ERROR);
    EXPECT_NE(manager.Init(Params()), CHIP_NO_ERROR);
    EXPECT_EQ(manager.GetBindingTable().Size(), 0);
}

TEST_F(TestBindingManager, TableReloadAndMemoryClearPreserveStorage)
{
    Table table;
    table.SetPersistentStorage(&storage);
    const auto entry = TableEntry::ForGroup(1, 2, 1, 6);
    ASSERT_EQ(table.Add(entry), CHIP_NO_ERROR);
    ASSERT_EQ(table.LoadFromStorage(), CHIP_NO_ERROR);
    ASSERT_EQ(table.LoadFromStorage(), CHIP_NO_ERROR);
    EXPECT_EQ(table.Size(), 1);
    storage.SetRejectWrites(true);
    table.ClearInMemory();
    EXPECT_EQ(table.Size(), 0);
    ASSERT_EQ(table.LoadFromStorage(), CHIP_NO_ERROR);
    EXPECT_TRUE(table.GetAt(0) == entry);
}

TEST_F(TestBindingManager, CyclicStoredListFailsWithoutPartialEntries)
{
    uint8_t buffer[64];
    TLV::TLVWriter writer;
    writer.Init(buffer);
    TLV::TLVType container;
    ASSERT_EQ(writer.StartContainer(TLV::AnonymousTag(), TLV::kTLVType_Structure, container), CHIP_NO_ERROR);
    ASSERT_EQ(writer.Put(TLV::ContextTag(1), static_cast<FabricIndex>(1)), CHIP_NO_ERROR);
    ASSERT_EQ(writer.Put(TLV::ContextTag(2), static_cast<EndpointId>(1)), CHIP_NO_ERROR);
    ASSERT_EQ(writer.Put(TLV::ContextTag(6), static_cast<GroupId>(2)), CHIP_NO_ERROR);
    ASSERT_EQ(writer.Put(TLV::ContextTag(7), static_cast<uint8_t>(0)), CHIP_NO_ERROR);
    ASSERT_EQ(writer.EndContainer(container), CHIP_NO_ERROR);
    ASSERT_EQ(writer.Finalize(), CHIP_NO_ERROR);
    ASSERT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
    ASSERT_EQ(manager.AddBindingEntry(TableEntry::ForGroup(1, 2, 1, std::nullopt)), CHIP_NO_ERROR);
    manager.Shutdown();
    ASSERT_EQ(storage.SyncSetKeyValue(DefaultStorageKeyAllocator::BindingTableEntry(0).KeyName(), buffer,
                                      static_cast<uint16_t>(writer.GetLengthWritten())),
              CHIP_NO_ERROR);
    EXPECT_EQ(manager.Init(Params()), CHIP_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(manager.GetBindingTable().Size(), 0);
}

struct ShutdownNotification
{
    Manager * manager;
    ManagerInitParams params;
    int notified = 0;
    int released = 0;
};

TEST_F(TestBindingManager, HandlerShutdownStopsNotificationsAndPreventsReentrantInit)
{
    ASSERT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
    ASSERT_EQ(manager.AddBindingEntry(TableEntry::ForGroup(1, 2, 1, 6)), CHIP_NO_ERROR);
    ASSERT_EQ(manager.AddBindingEntry(TableEntry::ForGroup(1, 3, 1, 6)), CHIP_NO_ERROR);
    ShutdownNotification context{ &manager, Params() };
    manager.RegisterBoundDeviceChangedHandler([](const TableEntry &, OperationalDeviceProxy *, void * value) {
        auto & state = *static_cast<ShutdownNotification *>(value);
        ++state.notified;
        EXPECT_EQ(state.manager->Init(state.params), CHIP_ERROR_INCORRECT_STATE);
        state.manager->Shutdown();
        EXPECT_EQ(state.manager->Init(state.params), CHIP_ERROR_INCORRECT_STATE);
    });
    manager.RegisterBoundDeviceContextReleaseHandler([](void * value) {
        auto & state = *static_cast<ShutdownNotification *>(value);
        ++state.released;
        EXPECT_EQ(state.manager->Init(state.params), CHIP_ERROR_INCORRECT_STATE);
    });
    EXPECT_EQ(manager.NotifyBoundClusterChanged(1, 6, &context), CHIP_ERROR_INCORRECT_STATE);
    EXPECT_EQ(context.notified, 1);
    EXPECT_EQ(context.released, 1);
    EXPECT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
    EXPECT_EQ(manager.GetBindingTable().Size(), 2);
}

struct PendingRelease
{
    PendingNotificationMap * map;
    int count = 0;
};

TEST_F(TestBindingManager, PendingContextsAreReleasedOnceWithReentrantRemoval)
{
    PendingNotificationMap map(manager.GetBindingTable());
    PendingRelease state{ &map };
    map.RegisterPendingNotificationContextReleaseHandler([](void * value) {
        auto & released = *static_cast<PendingRelease *>(value);
        ++released.count;
        released.map->Clear();
    });
    auto * first = map.NewPendingNotificationContext(&state);
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(map.AddPendingNotification(0, first), CHIP_NO_ERROR);
    ASSERT_EQ(map.AddPendingNotification(1, first), CHIP_NO_ERROR);
    auto * second = map.NewPendingNotificationContext(&state);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(map.AddPendingNotification(2, second), CHIP_NO_ERROR);
    map.RemoveEntry(2);
    EXPECT_EQ(state.count, 2);
    EXPECT_EQ(map.begin(), map.end());
    map.Clear();
    EXPECT_EQ(state.count, 2);
}

TEST_F(TestBindingManager, LocallyConstructedMapDoesNotRequireReleaseHandler)
{
    PendingNotificationMap map(manager.GetBindingTable());
    auto * context = map.NewPendingNotificationContext(nullptr);
    ASSERT_NE(context, nullptr);
    ASSERT_EQ(map.AddPendingNotification(0, context), CHIP_NO_ERROR);
    map.Clear();
    EXPECT_EQ(map.begin(), map.end());
}

TEST_F(TestBindingManager, ShutdownCancelsOwnedCallbacksButNotOtherClients)
{
    Callback::CallbackDeque connected;
    Callback::CallbackDeque failed;
    Callback::Callback<OnDeviceConnectionFailure> other([](void *, const ScopedNodeId &, CHIP_ERROR) { ADD_FAILURE(); }, nullptr);
    failed.Enqueue(other.Cancel());
    {
        Manager owned;
        ASSERT_EQ(owned.Init(Params()), CHIP_NO_ERROR);
        ASSERT_EQ(ManagerTestAccess::QueueConnection(owned, connected, failed), CHIP_NO_ERROR);
        ASSERT_EQ(ManagerTestAccess::QueueConnection(owned, connected, failed), CHIP_NO_ERROR);
        EXPECT_FALSE(connected.IsEmpty());
        owned.Shutdown();
    }
    EXPECT_TRUE(connected.IsEmpty());
    ASSERT_FALSE(failed.IsEmpty());
    auto * remaining = failed.First();
    EXPECT_EQ(remaining, other.Cancel());
    EXPECT_TRUE(failed.IsEmpty());
}

TEST_F(TestBindingManager, ShutdownDrainsOwnedPendingContextsAndRejectsReentrantInit)
{
    ASSERT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
    ShutdownNotification state{ &manager, Params() };
    manager.RegisterBoundDeviceContextReleaseHandler([](void * context) {
        auto & released = *static_cast<ShutdownNotification *>(context);
        ++released.released;
        EXPECT_EQ(released.manager->Init(released.params), CHIP_ERROR_INCORRECT_STATE);
        released.manager->Shutdown();
    });
    ASSERT_EQ(ManagerTestAccess::QueueNotification(manager, 0, &state), CHIP_NO_ERROR);
    ASSERT_EQ(ManagerTestAccess::QueueNotification(manager, 1, &state), CHIP_NO_ERROR);
    manager.Shutdown();
    EXPECT_EQ(state.released, 2);
    EXPECT_EQ(manager.Init(Params()), CHIP_NO_ERROR);
}

TEST_F(TestBindingManager, DetachedManagerPreservesBindingsWhenFabricIsRemoved)
{
    Testing::FabricTestFixture fixture(&storage);
    FabricIndex index = 1;
    ASSERT_EQ(fixture.SetUpTestFabric(index), CHIP_NO_ERROR);
    auto cleanup        = ScopeExit([&] {
        manager.Shutdown();
        EXPECT_EQ(fixture.TearDownTestFabric(index), CHIP_NO_ERROR);
    });
    auto params         = Params();
    params.mFabricTable = &fixture.GetFabricTable();
    ASSERT_EQ(manager.Init(params), CHIP_NO_ERROR);
    const auto entry = TableEntry::ForGroup(index, 2, 1, 6);
    ASSERT_EQ(manager.AddBindingEntry(entry), CHIP_NO_ERROR);
    manager.Shutdown();
    fixture.GetFabricTable().Forget(index);
    Table restored;
    restored.SetPersistentStorage(&storage);
    ASSERT_EQ(restored.LoadFromStorage(), CHIP_NO_ERROR);
    EXPECT_EQ(restored.Size(), 1);
    EXPECT_TRUE(restored.GetAt(0) == entry);
}
} // namespace
