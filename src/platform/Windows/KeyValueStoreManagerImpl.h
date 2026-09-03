/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
 *    All rights reserved.
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

/**
 *    @file
 *          Native Windows key-value store for the Matter Device Layer.
 *
 *          The store is file-per-key under a per-user versioned root
 *          (%LOCALAPPDATA%\Matter\KVS\v<n> by default). Each value is written
 *          to its own file with an integrity header and durably replaced with
 *          same-volume temp file + FlushFileBuffers + MoveFileEx semantics, so a
 *          crash never leaves a partially written value. The root is resolved
 *          with the known-folder API (Unicode), not ANSI environment-variable
 *          concatenation, and can be overridden at Init() time for tests and for
 *          future per-service / per-machine deployments.
 *
 *          Windows-specific implementation notes intentionally live in the .cpp
 *          so that <windows.h> does not leak into the many translation units
 *          that include <platform/KeyValueStoreManager.h>. The lock handle is
 *          therefore stored as an opaque pointer here.
 */

#pragma once

#include <cstddef>
#include <mutex>
#include <string>

namespace chip {
namespace DeviceLayer {
namespace PersistedStorage {

class KeyValueStoreManagerImpl final : public KeyValueStoreManager
{
    // Allow the KeyValueStoreManager interface class to delegate method calls to
    // the implementation methods provided by this class.
    friend class KeyValueStoreManager;

public:
    /**
     * @brief Initialize the store. Must be called before any Get/Put/Delete.
     *
     * @param[in] storageRoot Optional override for the storage root directory,
     *            as an absolute UTF-8 path. When @p storageRoot is nullptr or empty the
     *            default per-user versioned root is used
     *            (%LOCALAPPDATA%\Matter\KVS\v<n>). A non-null value is used
     *            verbatim, which is how tests isolate storage and how a service
     *            or machine-wide deployment points at its own exclusively
     *            owned directory. Relative paths are rejected.
     *
     * @return CHIP_NO_ERROR on success; a mapped Windows error on failure to
     *         resolve or create the directory, or if another process already
     *         owns the directory.
     */
    CHIP_ERROR Init(const char * storageRoot);

    /**
     * @brief Release the store and its cross-process ownership lock. Safe to
     *        call when uninitialized. Does not delete any stored data.
     */
    void Shutdown();

    /**
     * @brief Delete every value owned by this store. Scoped to valid KVS value
     *        and temporary file names; it never recurses or removes the root.
     */
    CHIP_ERROR ClearAll();

    CHIP_ERROR _Get(const char * key, void * value, size_t value_size, size_t * read_bytes_size = nullptr, size_t offset = 0);
    CHIP_ERROR _Delete(const char * key);
    CHIP_ERROR _Put(const char * key, const void * value, size_t value_size);

private:
    // ===== Members for internal use by the following friends.
    friend KeyValueStoreManager & KeyValueStoreMgr();
    friend KeyValueStoreManagerImpl & KeyValueStoreMgrImpl();

    static KeyValueStoreManagerImpl sInstance;

    // Serializes all operations so concurrent in-process calls are safe.
    std::recursive_mutex mMutex;

    // Absolute, canonicalized storage root as a wide path (no trailing slash).
    std::wstring mRootPath;

    // Open HANDLE to the ownership lock file (see .cpp). Stored opaquely so this
    // header does not need <windows.h>. INVALID_HANDLE_VALUE-equivalent when the
    // store is not initialized.
    void * mLockHandle = nullptr;

    bool mInitialized = false;
};

/**
 * Returns the public interface of the KeyValueStoreManager singleton object.
 *
 * Chip applications should use this to access features of the
 * KeyValueStoreManager object that are common to all platforms.
 */
inline KeyValueStoreManager & KeyValueStoreMgr()
{
    return KeyValueStoreManagerImpl::sInstance;
}

/**
 * Returns the platform-specific implementation of the KeyValueStoreManager
 * singleton object.
 *
 * Chip applications can use this to gain access to features of the
 * KeyValueStoreManager that are specific to the Windows platform.
 */
inline KeyValueStoreManagerImpl & KeyValueStoreMgrImpl()
{
    return KeyValueStoreManagerImpl::sInstance;
}

} // namespace PersistedStorage
} // namespace DeviceLayer
} // namespace chip
