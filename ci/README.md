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
4. Build in up to three `colcon` passes against the same workspace:
   - pass 1: the zzdds type support packages;
   - pass 2: `rmw_dds_common` (rebuilt so it picks up the zzdds type support)
     and `rmw_zzdds_cpp` / `rmw_zzdds_test`;
   - pass 3 (unless `--no-upstream-tests`): `test_rmw_implementation`.
   The splits are required: rmw_zzdds includes
   `rmw_dds_common/...__rosidl_typesupport_zzdds_cpp.hpp`, which only exists if
   `rmw_dds_common` is built with the zzdds type support already discoverable;
   and `test_rmw_implementation` enumerates RMW implementations from the ament
   index at configure time, so `rmw_zzdds_cpp` must already be installed. There
   is no dependency edge to force either ordering in one pass.
5. `colcon test` the wired gtest suites and print the results.
6. Unless `--no-upstream-tests`: `colcon test test_rmw_implementation` with
   `RMW_IMPLEMENTATION=rmw_zzdds_cpp`, filtered (ctest `-R _rmw_zzdds_cpp`) to
   the rmw_zzdds_cpp-parameterised conformance tests, run serially. A filter
   that matches nothing (rmw_zzdds_cpp not registered during pass 3) is a hard
   failure, not a silent pass.

`--rolling-repos skip` builds and tests only the zzdds type support packages
against the ambient ROS install — a fast "did a zzdds header break rosidl
codegen" smoke, not a build of the RMW itself (and no upstream conformance).

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
matrix as JSON and exits. Add `--no-upstream-tests` to skip the
`test_rmw_implementation` layer for a faster local iteration.

## The pinned ROS image

The gating matrix leg pins `ros:rolling` by digest (`ROS_ROLLING_DIGEST` in
`build_and_test.py`); the scheduled run adds a non-gating leg on the moving
`ros:rolling` tag as an upstream-drift early warning. To bump the pin: update
the digest, run `build_and_test.py` in the new image, and only commit once it
passes.

## Follow-on work

- **Restrict the pass-3 build to rmw_zzdds_cpp.** `test_rmw_implementation`'s
  CMake compiles its test sources once per RMW implementation found in the
  image (fastrtps, cyclonedds, …); the ctest `-R` filter drops the others from
  the *run*, but they are still *built*. If pass 3 build time becomes a
  problem, restrict it (e.g. `RMW_IMPLEMENTATIONS=rmw_zzdds_cpp`, version
  permitting) rather than widening the filter.
- **Promote/quarantine per suite.** The upstream layer is currently all-or-
  nothing and gating on every leg. If a specific suite proves flaky under
  zzdds's discovery timing, split the `-R` filter so the stable suites keep
  gating while the flaky one runs non-gating.
- **Release-axis matrix legs.** `build_matrix()` adds a "last zzdds release"
  leg when given `--zzdds-release`; a "last rmw_zzdds release" leg is a
  checkout-ref change in `ci.yml`. Both are inert until the first release tags.
