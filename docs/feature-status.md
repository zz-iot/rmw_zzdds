# Feature status

This table is normative for the current development branch.

| Feature | Status |
|---|---|
| Initialization and shutdown | Foundation implemented; upstream lifecycle tests pass |
| Node lifecycle | Foundation implemented; upstream lifecycle tests pass |
| Guard conditions | Foundation implemented, including one-shot wait-set integration |
| Static C++ message type support | Foundation implemented: generated XCDR1 callbacks, RMW serialization adapter, and endpoint integration |
| Publishers and subscriptions | Foundation implemented: typed and serialized single-sample publish/take and subscription readiness pass; broader conformance remains Stage 1 |
| Message metadata and publisher GIDs | Foundation implemented: timestamps, typed/serialized take-with-info, GID comparison, and batched take |
| Wait sets | Foundation implemented for subscriptions, services, clients, guard conditions, and supported events |
| Remote graph | Foundation implemented through DDS publication/subscription built-in topics; broader multi-process coverage remains |
| Services and clients | Foundation implemented: request/response transport, readiness, identity, lifecycle, and upstream suites pass |
| Static C message type support | Foundation implemented: generated XCDR1 callbacks and endpoint integration; broader conformance remains Stage 2 |
| Dynamic type discovery/messages | Unsupported |
| Loaned messages | Compatibility implementation: middleware-owned initialized ROS messages are serialized/deserialized through zzdds; not transport zero-copy |
| DDS Security | Unsupported |

“In development” is not a compatibility claim.  A feature becomes supported
only after its conformance, failure-path, and resource-lifetime tests pass.

The library exports the complete RMW symbol set required by the pinned Rolling
proxy.  Operations beyond the table's implemented subset currently return
`RMW_RET_UNSUPPORTED` (or `nullptr`/`false` for the corresponding API shape).

The static C++ and C-layout generators currently cover ROS primitives (except
`long double`), strings and wide strings, fixed arrays, bounded and unbounded
sequences, nested messages, and DDS keys. The standard `builtin_interfaces`,
`unique_identifier_msgs`, `service_msgs`, `action_msgs`, and `test_msgs`
interface sets compile with both backends. This remains deliberately narrower
than a general ROS 2 compatibility claim.

The initial data plane applies reliability, durability, history/depth, and
liveliness-kind QoS. It supports ROS topic prefixing, endpoint match counts,
typed publish/take, and serialized publish/take for keyless messages. Serialized
publishing of keyed messages remains unsupported because that API does not
provide the typed value needed by the current key-hash callback.

Loaned-message APIs use RMW-owned ROS message allocations with strict
per-endpoint ownership tracking. Publishing still serializes into zzdds, and
taking temporarily borrows the serialized zzdds sample before deserializing it
into the RMW-owned ROS message. This provides the ROS ownership/lifetime API but
does not claim allocation-free or zero-copy transport behavior.

System-default endpoint policies are resolved to the effective DDS history,
depth, reliability, durability, and liveliness values returned by the actual
QoS APIs. Publisher/subscription compatibility checking delegates to the shared
DDS compatibility rules in `rmw_dds_common`.
