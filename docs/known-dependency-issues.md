# Known dependency issues

## Stale ROSIDL install caused a `string.h` header collision

The workspace's `rosidl_runtime_c` source had adopted scoped installation but
its install tree was older than that source.  The stale tree left
`rosidl_runtime_c/string.h` directly on an exported include root, so C++
`<cstring>` resolved the ROSIDL file instead of the platform C library header.
The discarded prototype concealed this by force-including
`/usr/include/string.h`.

A clean rebuild of `rosidl_runtime_c` installs the header under the expected
nested package directory and removes the collision.  The RMW does not carry a
workaround; clean dependency installs are part of the reproducible build
contract.

## Debug `zidl_cdr` static target does not carry its sanitizer runtime

The Debug static `zidl_cdr` archive can contain UBSan references, while its
exported CMake target does not propagate the corresponding sanitizer link
flags to C++ consumers.  Linking that archive directly therefore leaves
unresolved `__ubsan_*` symbols unless every consumer reproduces zzdds's private
build flags.

Generated type support links the public `ZZDDS::zzdds` shared target instead.
That library exports the public zidl CDR API and owns its own runtime
dependencies, avoiding both private build-flag coupling and source-tree paths.
