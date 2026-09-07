# CI

`build_and_test.py` builds rmw_zzdds against a chosen zzdds and runs its test
suite. It is the single entry point for:

- `.github/workflows/ci.yml` in this repo — the PR / scheduled build matrix;
- zzdds's own CI, which calls this repo's `ci.yml` as a reusable workflow with
  the zzdds commit under test (so a zzdds change that breaks rmw_zzdds is
  caught in the zzdds PR);
- a local run, for parity with CI or to debug a failure.

## What it does

1. Build + install zzdds's C and C++ bindings (`zig build -Dc-binding=true
   -Dcpp-binding=true install`) into `zig-out`.
2. Assemble a colcon workspace with rmw_zzdds and, by default, the pinned
   revisions from [`rolling.repos`](rolling.repos) (`--rolling-repos overlay`).
3. `rosdep install` the remaining system/ROS dependencies.
4. Build in two `colcon` passes against the same workspace:
   - pass 1: the zzdds type support packages;
   - pass 2: `rmw_dds_common` (rebuilt so it picks up the zzdds type support)
     and `rmw_zzdds_cpp` / `rmw_zzdds_test`.
   The split is required: rmw_zzdds includes
   `rmw_dds_common/...__rosidl_typesupport_zzdds_cpp.hpp`, which only exists if
   `rmw_dds_common` is built with the zzdds type support already discoverable,
   and there is no dependency edge to force that ordering in one pass.
5. `colcon test` the wired gtest suites and print the results.

`--rolling-repos skip` builds and tests only the zzdds type support packages
against the ambient ROS install — a fast "did a zzdds header break rosidl
codegen" smoke, not a build of the RMW itself.

## Run it locally

Needs [podman](https://podman.io/) (or docker) and a local Zig 0.16.0. Nothing
else — the `ros:rolling` image supplies colcon, rosdep, vcstool and ROS.

```sh
podman pull docker.io/library/ros:rolling

podman run --rm \
  -v "$PWD":/src/rmw_zzdds:ro \
  -v /path/to/zzdds:/src/zzdds:ro \
  -v /path/to/zig-0.16.0:/opt/zig:ro \
  -v /tmp/rmw-ci-ws:/ws \
  docker.io/library/ros:rolling \
  bash -lc 'PATH=/opt/zig:$PATH python3 /src/rmw_zzdds/ci/build_and_test.py \
      --rmw-zzdds-src /src/rmw_zzdds --zzdds-src /src/zzdds \
      --workspace /ws --jobs "$(nproc)" --test'
```

`--zzdds-src` / `--rmw-zzdds-src` take a local checkout; drop them and pass
`--zzdds-ref` / `--rmw-zzdds-ref` to clone a specific commit from github.com
instead. `--print-matrix {pr,schedule,zzdds-pr}` emits the workflow's build
matrix as JSON and exits.

## The pinned ROS image

The gating matrix leg pins `ros:rolling` by digest (`ROS_ROLLING_DIGEST` in
`build_and_test.py`); the scheduled run adds a non-gating leg on the moving
`ros:rolling` tag as an upstream-drift early warning. To bump the pin: update
the digest, run `build_and_test.py` in the new image, and only commit once it
passes.

## Follow-on work

- **Upstream RMW conformance suites.** `docs/testing.md` describes running
  `test_rmw_implementation` (publisher / subscription / QoS / graph / CFT /
  loan) with `RMW_IMPLEMENTATION=rmw_zzdds_cpp`. That package is not yet a
  dependency of anything here; the seam is `run_upstream_rmw_tests()` +
  `--upstream-tests` in `build_and_test.py`.
- **Release-axis matrix legs.** `build_matrix()` adds a "last zzdds release"
  leg when given `--zzdds-release`; a "last rmw_zzdds release" leg is a
  checkout-ref change in `ci.yml`. Both are inert until the first release tags.
