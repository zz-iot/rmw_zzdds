# ADR 0001: Use the zzdds C API behind C++ RAII

Status: accepted

The RMW implementation uses zzdds's installed C ABI for DDS operations.  Its
implementation language remains C++ so ownership, locking, and rollback can be
expressed with small RAII types.

The zzdds C++ binding is functional when both generated implementation sources
are compiled, but its typed wrappers and shared ownership add no benefit to a
generic serialized RMW data path.  Private C++ implementation access is
forbidden.

