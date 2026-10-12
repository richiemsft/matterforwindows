### Upgrade notes

The Binding table size was changed from `MATTER_BINDING_TABLE_SIZE` to
`CHIP_CONFIG_MAX_BINDING_ENTRIES_PER_FABRIC * CHIP_CONFIG_MAX_FABRICS`. The
previous value was defined in `src/app/util/config.h` by default and it is
defined in `src/lib/core/CHIPConfig.h`.

### Owned manager lifecycle

An application can use an owned `Binding::Manager` with the existing
`BindingCluster`, rather than the process-lifetime singleton. Initialize it on
the Matter thread with storage, a fabric table, and the shared CASE session
manager. Set `mEstablishConnectionOnInit=false` when sessions should be created
only on demand. A missing table header represents first boot; storage failures,
malformed tables, missing stored entries, and cyclic lists fail initialization
without publishing partial bindings.
Repeated initialization with identical dependencies and connection policy is a
no-op, preserving singleton callers and any active callbacks. Changing dependencies
or connection policy requires shutdown first.

Call `Manager::Shutdown()` before destroying an owned manager or its dependencies
and **before controller shutdown forgets its fabric**. Shutdown detaches the
fabric delegate, cancels this manager's connection callbacks, and releases pending
notification contexts. It does not delete persistent bindings or release shared
CASE sessions. Reinitialization reloads the stored bindings. Singleton users
continue to use explicit shutdown; there is no destructor-driven access to
process-lifetime dependencies.

Notification and context-release handlers may call `Shutdown()`, but must not
destroy the manager from inside a handler. Reinitialization is rejected until
the active notification returns.
