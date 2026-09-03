# rmw_zzdds

`rmw_zzdds` is an experimental ROS 2 middleware implementation backed by
[zzdds](https://github.com/zz-iot/zzdds).

The repository is being rebuilt from a discarded prototype.  The first
development target is a small, trustworthy statically typed publish/subscribe
path.  It does **not** currently claim general ROS 2 compatibility.

See [docs/feature-status.md](docs/feature-status.md) for the implemented feature
set and [docs/architecture.md](docs/architecture.md) for the design boundaries.

## Development baseline

The current baseline is ROS 2 Rolling at the revisions recorded in
`ci/rolling.repos`, zidl release `v0.3.12-zig.0.16.0`, and zzdds release
`v0.3.0-zig.0.16.0`. zzdds is built with its C and C++ bindings enabled. The former
`zzdds-examples` repository has been folded into zzdds and is no longer a
separate dependency.

## Build

Install zzdds to a prefix, add that prefix to `CMAKE_PREFIX_PATH`, place this
repository in a ROS 2 colcon workspace, then build with colcon.  The build uses
the upstream `ZZDDSConfig.cmake`; source-tree paths and private zzdds headers are
not supported integration mechanisms.

## License

Apache License 2.0.
