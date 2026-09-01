# Testing strategy

Failures are isolated into these layers:

1. generated ROS/CDR type support without DDS;
2. the installed zzdds C binding with fixed CDR samples;
3. the zzdds core;
4. RMW lifecycle and adapter behavior;
5. ROS integration through `test_rmw_implementation` and higher layers;
6. cross-vendor RTPS interoperability.

Round trips through a single serializer are insufficient evidence.  Type
support uses independent golden vectors, and the Stage 1 gate includes traffic
in both directions with established DDS implementations.

The current static-type-support tests verify an independent XCDR1
little-endian golden vector, nested messages, `std::vector<bool>`, rejection of
truncated and over-bound sequence input, canonical big-endian DDS key hashing,
and the public `rmw_serialize`/`rmw_deserialize` buffer adapter.  The adapter
tests begin with a valid zero-capacity RMW buffer and also reject foreign
type-support handles.

As a generator integration check, the C++ type support also builds the standard
`builtin_interfaces`, `unique_identifier_msgs`, `service_msgs`, `action_msgs`,
and `test_msgs` interface sets. This covers multiple messages emitted from one
service/action IDL file and nested references among those local messages.
The equivalent C-layout type support builds the same interface sets and is
selected directly for ROS C aggregate handles.

The data-plane boundary has two independent tests. A C-binding contract test
creates ordinary DDS entities, waits for matching and data availability, and
round-trips an opaque serialized payload. An RMW integration test performs
both typed and serialized publish/take through matched endpoints, checks the
decoded message and exact serialized bytes, verifies subscription readiness
through `rmw_wait`, and then verifies complete RMW/DDS lifecycle teardown. The
lifecycle suite separately checks finite timeout nulling and one-shot guard
condition triggering. Endpoint tests additionally verify that system-default
QoS is reported as concrete effective policy values. The upstream
`test_qos_profile_check_compatible` suite passes through the RMW library.

Normal tests run with strict compiler warnings.  Lifecycle and data-plane tests
also run under AddressSanitizer, LeakSanitizer, UndefinedBehaviorSanitizer, and
ThreadSanitizer where supported.

## Sanitizer status

The allocator-only and DDS lifecycle paths pass ASan, LSan, and UBSan.  Shared
library builds of zzdds disable Zig's automatic per-thread alternate signal
stack so that an embedding sanitizer runtime retains ownership of that
resource.  The standalone C-binding smoke test and the complete RMW lifecycle
suite pass with this configuration.  DDS lifecycle paths are additionally run
under Valgrind; the initial context and active-entity cases report no definite
or indirect leaks.

The generated type-support, RMW serialization adapter, data plane, and wait-set
foundation also pass their focused suites under ASan, LSan, and UBSan.

## Upstream RMW status

Against the pinned Rolling `test_rmw_implementation` checkout, the publisher
suite passes all 18 tests and the subscription suite passes all 32 tests,
including the loaned-message API cases.

The passing subscription cases include a ROS C-layout
`test_msgs/UnboundedSequences` publish/take path, readiness across repeated
waits, typed and serialized take-with-info argument handling, and batched take
with publisher-GID agreement. These results establish the static C and message
metadata foundations; they are not a claim that the complete subscription API
is conformant.

Ignore-local subscriptions filter by the originating publication handle. A
focused two-context regression queues a local sample before a remote sample and
verifies that one take skips the local sample, returns the remote payload, and
reports the remote publisher GID.

Remote graph tests consume DDS publication/subscription built-in-topic samples,
verify endpoint and participant identities and ROS type hashes, associate the
endpoint with its remote ROS node, and remove it after a not-alive built-in
instance change.

Content-filtered subscriptions use zzdds ContentFilteredTopic evaluation with
generated ROS CDR field accessors. Parameter-only updates preserve the reader;
expression changes and reset replace the reader and filtered topic while
retaining the RMW subscription handle. The upstream creation, get/set,
filtering, reset, and same-base-topic lifecycle tests pass.

The focused RMW suite additionally exercises the compatibility-loan data path
end to end: borrow, populate, publish, deserialize into a subscription loan,
verify publisher metadata, return, and reject foreign or duplicate returns.
This verifies ownership and message lifetime, not DDS transport zero-copy.
