# Architecture

## Boundaries

`rmw_zzdds_cpp` implements the C RMW ABI in C++ and calls the installed zzdds C
ABI.  C++ is used for deterministic ownership and synchronization, not for the
zzdds typed C++ wrapper layer.

```text
rcl / rclcpp
    |
ROS 2 rmw C ABI
    |
rmw_zzdds_cpp
    |-- generated, statically typed ROS <-> CDR callbacks
    |-- C++ RAII owners for DDS and RMW resources
    `-- installed zzdds C ABI
            `-- zzdds core
```

No RMW code may reach through the public binding into zzdds implementation
objects.  A missing operation is an upstream zzdds API requirement.

Remote endpoint discovery is consumed through the standard DDS built-in
subscriber. `DCPSPublication` and `DCPSSubscription` samples populate the RMW
graph cache, and their not-alive instance changes remove departed endpoints.
The RMW does not inspect or enumerate zzdds's internal SEDP registries.

## Ownership

The context owns the factory, participant, DDS publisher/subscriber containers,
graph state, and its allocator.  Endpoint objects own their DDS entities and
copies of all strings exposed through RMW handles.  Destruction proceeds from
children to parents and every partially constructed object is safe to destroy.

All public handles carry `rmw_zzdds_cpp`'s identifier and are rejected by other
implementations.  DDS identities come from DDS; addresses and process-local
counters are not network identities.

## Type support

Stage 1 accepts only the exact `rosidl_typesupport_zzdds_cpp` identifier.  It
does not cast generic or introspection handles to implementation-specific
callbacks.  Generated callbacks provide serialization, deserialization,
serialized size, key hashing, the canonical DDS type name, and the ROS type
hash.  A future optional field will carry DDS-XTypes TypeInformation.

## Unsupported features

Loaning, dynamic type discovery, dynamic-message take, security enforcement,
and unimplemented DDS status events report unsupported.  They must not return
success with invented data.
