#!/usr/bin/env python3
"""Build rmw_zzdds against a chosen zzdds and run its test suite.

This is the single entry point shared by every context that builds rmw_zzdds
in an automated way:

  * this repo's ``.github/workflows/ci.yml`` -- the PR and scheduled build
    matrix over (rmw_zzdds ref, zzdds ref, ROS image);
  * zzdds's ``.github/workflows/ci.yml`` -- a regression gate that calls this
    repo's reusable workflow, which in turn runs this script with
    ``--zzdds-ref`` pointing at the zzdds commit under test;
  * a developer (or a CI-parity check on a second machine) running the whole
    flow inside a ``ros:rolling`` container.

Keeping the logic here -- not spread across workflow YAML -- means all three
paths exercise the same steps, and the whole thing is runnable and debuggable
locally:

    podman run --rm -it \\
        -v "$PWD":/src/rmw_zzdds:ro -v /path/to/zzdds:/src/zzdds:ro \\
        -v /path/to/zig-0.16.0:/opt/zig:ro \\
        docker.io/library/ros:rolling \\
        bash -lc 'PATH=/opt/zig:$PATH \\
            python3 /src/rmw_zzdds/ci/build_and_test.py \\
                --rmw-zzdds-src /src/rmw_zzdds --zzdds-src /src/zzdds --test'

Design notes
------------
* Every subprocess call has an explicit timeout and, where a process is given
  the chance to shut down gracefully, ``stop()`` escalates to SIGKILL. A bash
  predecessor elsewhere in this project once hung for 40+ minutes on an
  unbounded ``wait`` -- that class of hang is structurally impossible here.
* Sources are copied into a scratch workspace before anything is built, so
  read-only mounts and pinned ``git`` checkouts both work, and a build never
  writes into the caller's tree.
* The set of packages built and tested is intentionally small (the wired
  ``BUILD_TESTING`` gtests). The hooks for the follow-on work -- the upstream
  ``test_rmw_implementation`` suites and an overlay build of the pinned
  ``ci/rolling.repos`` revisions -- are present but stubbed; see
  ``run_upstream_rmw_tests`` and ``--rolling-repos overlay``.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# ── What we build and test ───────────────────────────────────────────────────
#
# The build runs in two `colcon build` passes against the same workspace:
#
#   pass 1  --packages-up-to TYPESUPPORT_PACKAGES
#   pass 2  --packages-up-to BUILD_PACKAGES   (with pass 1's install/ sourced)
#
# Why two passes: rmw_zzdds_cpp links
# `rmw_dds_common::..__rosidl_typesupport_zzdds_cpp` and includes
# `rmw_dds_common/msg/detail/participant_entities_info__rosidl_typesupport_zzdds_cpp.hpp`
# -- headers/targets that only exist if `rmw_dds_common` is (re)built with the
# zzdds type support already discoverable in the ament index. `rmw_dds_common`
# has no dependency edge to the zzdds type support, so a single colcon
# invocation may order it first and silently omit the zzdds variant. Building
# the type support in a first pass and sourcing its install/ before the second
# forces the ordering. This is also why --rolling-repos overlay is effectively
# mandatory for a full RMW build (see main() / --rolling-repos help).
TYPESUPPORT_PACKAGES = [
    "rosidl_typesupport_zzdds_c",
    "rosidl_typesupport_zzdds_cpp",
]
BUILD_PACKAGES = [
    "rmw_zzdds_cpp",
    "rmw_zzdds_test",
]

# The subset with wired gtest suites. `rosidl_typesupport_zzdds_c` /
# `_cpp` carry only lint tests; the real coverage is in these two.
TEST_PACKAGES = [
    "rmw_zzdds_cpp",
    "rmw_zzdds_test",
]

# The zzdds build flags rmw_zzdds's CMake consume path requires: the C ABI
# (dcps.h / libzzdds) plus the C++ generated implementation sources that
# ZZDDSConfig.cmake points at. Mirrors the `examples` job in zzdds CI.
ZZDDS_BUILD_ARGS = ["-Dc-binding=true", "-Dcpp-binding=true", "install"]

# Pinned digest for the gating ROS leg. `ros:rolling` (Docker Hub official
# image, currently Ubuntu 26.04 / ament_cmake 2.9.1) is retagged often; the
# matrix's non-gating "rolling" leg tracks the moving tag, the gating leg
# tracks this digest. Refresh deliberately -- bump it, re-run
# ci/build_and_test.py in that image, and only then commit. Resolve a current
# value with:
#   podman pull docker.io/library/ros:rolling
#   podman image inspect docker.io/library/ros:rolling --format '{{index .RepoDigests 0}}'
ROS_ROLLING_DIGEST = "ros@sha256:6536553889f36066b3ecea5e46d80c31c6d70aa66edb5d396beb8c22248c3524"
ROS_ROLLING_TAG = "ros:rolling"

DEFAULT_ZZDDS_REPO = "zz-iot/zzdds"
DEFAULT_RMW_ZZDDS_REPO = "zz-iot/rmw_zzdds"

REPO_ROOT = Path(__file__).resolve().parents[1]


# ── Matrix definition (one source of truth, consumed by the workflow) ────────


@dataclasses.dataclass
class Combo:
    """One cell of the build matrix."""

    name: str
    zzdds_ref: str
    ros_target: str  # "digest" (gating, pinned) | "rolling" (non-gating, moving tag)
    gating: bool

    @property
    def ros_image(self) -> str:
        return ROS_ROLLING_DIGEST if self.ros_target == "digest" else ROS_ROLLING_TAG

    def as_matrix_entry(self) -> dict:
        return {
            "name": self.name,
            "zzdds_ref": self.zzdds_ref,
            "ros_target": self.ros_target,
            "ros_image": self.ros_image,
            "gating": self.gating,
        }


def build_matrix(mode: str, *, zzdds_ref: str, zzdds_release: str | None) -> list[Combo]:
    """Return the combos for a given trigger.

    mode:
      pr        -- a PR against rmw_zzdds: does HEAD build against zzdds main
                   (and the last zzdds release, once one exists)?
      schedule  -- the weekly sweep: the pr set, plus a non-gating leg against
                   the moving `ros:rolling` tag for upstream-ROS early warning.
      zzdds-pr  -- invoked from zzdds CI: a single gating leg against the
                   zzdds commit under test (`zzdds_ref`).

    The rmw_zzdds-ref axis (HEAD vs last rmw_zzdds release) is selected by the
    workflow's checkout, not here; a release leg joins `pr`/`schedule` once
    rmw_zzdds has a release tag.
    """
    if mode == "zzdds-pr":
        return [Combo("zzdds-under-test", zzdds_ref, "digest", gating=True)]

    combos = [Combo("zzdds-main", "main", "digest", gating=True)]
    if zzdds_release:
        combos.append(Combo("zzdds-release", zzdds_release, "digest", gating=True))
    if mode == "schedule":
        combos.append(Combo("zzdds-main-ros-rolling", "main", "rolling", gating=False))
    return combos


# ── Small process helpers (bounded, never-hang) ─────────────────────────────


class StepError(RuntimeError):
    """A build/test step failed; message is already user-facing."""


def _run(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    env: dict | None = None,
    timeout: int,
    label: str,
) -> None:
    """Run a command, streaming its output, with a hard timeout.

    Raises StepError on nonzero exit, timeout, or a missing executable. On
    timeout the child (and its group) is killed before we return.
    """
    print(f"\n$ {' '.join(cmd)}", flush=True)
    start = time.monotonic()
    try:
        proc = subprocess.Popen(cmd, cwd=cwd, env=env, start_new_session=True)
    except (FileNotFoundError, OSError) as e:
        raise StepError(f"{label}: could not start ({e})") from e
    try:
        rc = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        _kill(proc)
        raise StepError(f"{label}: timed out after {timeout}s") from None
    elapsed = time.monotonic() - start
    if rc != 0:
        raise StepError(f"{label}: exit {rc} (after {elapsed:.0f}s)")
    print(f"[ok] {label} ({elapsed:.0f}s)", flush=True)


def _kill(proc: subprocess.Popen) -> None:
    import signal

    for sig in (signal.SIGTERM, signal.SIGKILL):
        if proc.poll() is not None:
            return
        try:
            os.killpg(proc.pid, sig)
        except (ProcessLookupError, PermissionError):
            try:
                proc.send_signal(sig)
            except ProcessLookupError:
                return
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            continue


def _bash(
    script: str,
    *,
    ros_setup: Path,
    cwd: Path | None,
    timeout: int,
    label: str,
    extra_setup: Path | None = None,
) -> None:
    """Run a snippet through bash with the ROS environment sourced first.

    GitHub Actions `container:` steps do not run the image's ROS entrypoint,
    so colcon/rosdep/vcs must be reached through an explicit `source`. Doing
    it here keeps every caller uniform. `extra_setup` (a workspace
    install/setup.bash) is sourced after the base ROS env when given.

    Note: no `set -u` -- ROS's own setup.bash references unbound variables
    (AMENT_TRACE_SETUP_FILES and friends) and aborts under nounset.
    """
    lines = ["set -eo pipefail", f'source "{ros_setup}"']
    if extra_setup is not None:
        lines.append(f'source "{extra_setup}"')
    lines.append(script)
    _run(["bash", "-c", "\n".join(lines) + "\n"], cwd=cwd, timeout=timeout, label=label)


def _require(tool: str, *, hint: str = "") -> None:
    if shutil.which(tool) is None:
        msg = f"required tool not found on PATH: {tool}"
        if hint:
            msg += f"\n  {hint}"
        raise StepError(msg)


# ── Source preparation ─────────────────────────────────────────────────────


def _resolve_source(
    *, src: str | None, repo: str, ref: str | None, dest: Path, label: str
) -> Path:
    """Populate `dest` with a source tree: copy from `src` if given, else
    clone `repo` at `ref` from github.com. Returns `dest`.
    """
    if dest.exists():
        shutil.rmtree(dest)
    if src:
        src_path = Path(src).resolve()
        if not src_path.is_dir():
            raise StepError(f"{label}: --*-src path is not a directory: {src_path}")
        print(f"[{label}] copying {src_path} -> {dest}", flush=True)
        shutil.copytree(
            src_path,
            dest,
            ignore=shutil.ignore_patterns(
                ".git", "build", "install", "log", "zig-out", ".zig-cache", "zig-cache",
                ".ci-ws", "__pycache__",
            ),
            symlinks=True,
        )
        return dest
    if not ref:
        raise StepError(f"{label}: need either --*-src or --*-ref")
    url = f"https://github.com/{repo}.git"
    print(f"[{label}] cloning {url} @ {ref}", flush=True)
    dest.mkdir(parents=True)
    _run(["git", "-C", str(dest), "init", "-q"], timeout=60, label=f"{label}: git init")
    _run(["git", "-C", str(dest), "remote", "add", "origin", url], timeout=60,
         label=f"{label}: git remote")
    _run(["git", "-C", str(dest), "fetch", "-q", "--depth", "1", "origin", ref],
         timeout=600, label=f"{label}: git fetch")
    _run(["git", "-C", str(dest), "checkout", "-q", "FETCH_HEAD"], timeout=120,
         label=f"{label}: git checkout")
    return dest


# ── Build steps ────────────────────────────────────────────────────────────


def build_zzdds(zzdds_src: Path, *, jobs: int) -> Path:
    """Build + install zzdds's C/C++ bindings. Returns the install prefix
    (`zig-out`) to hand to CMake as CMAKE_PREFIX_PATH.
    """
    _require("zig", hint="pass --zig /path/to/zig, or add the Zig 0.16.0 dir to PATH")
    zig_out = zzdds_src / "zig-out"
    _run(
        ["zig", "build", *ZZDDS_BUILD_ARGS, f"-j{jobs}"],
        cwd=zzdds_src,
        timeout=1800,
        label="zzdds: zig build (c+cpp bindings, install)",
    )
    cfg = zig_out / "lib" / "cmake" / "ZZDDS" / "zzdds-config.cmake"
    if not cfg.is_file():
        raise StepError(f"zzdds build produced no {cfg} -- consume path would fail")
    print(f"[ok] zzdds install prefix: {zig_out}", flush=True)
    return zig_out


def make_workspace(ws: Path, rmw_src: Path, *, rolling_repos: str, ros_setup: Path) -> Path:
    """Assemble a colcon workspace at `ws` with rmw_zzdds under src/. With
    rolling_repos='overlay', also import the pinned ci/rolling.repos
    revisions alongside it. Returns the workspace path.
    """
    src_dir = ws / "src"
    if src_dir.exists():
        shutil.rmtree(src_dir)
    (src_dir / "rmw_zzdds").mkdir(parents=True)
    for child in rmw_src.iterdir():
        if child.name in {".git", "build", "install", "log", ".ci-ws", "__pycache__"}:
            continue
        dst = src_dir / "rmw_zzdds" / child.name
        if child.is_dir():
            shutil.copytree(child, dst, symlinks=True)
        else:
            shutil.copy2(child, dst)

    if rolling_repos == "overlay":
        repos_file = src_dir / "rmw_zzdds" / "ci" / "rolling.repos"
        _require("vcs", hint="apt-get install -y python3-vcstool")
        _bash(
            f'vcs import "{src_dir}" < "{repos_file}"',
            ros_setup=ros_setup,
            cwd=ws,
            timeout=900,
            label="overlay: vcs import ci/rolling.repos",
        )
    return ws


# rosdep scans every package under src/, so the overlaid `rmw_implementation`
# drags in its `test_depend`s on other vendors' RMWs -- `rmw_connextdds` in
# particular pulls `rti-connext-dds-*`, whose .deb preinst aborts without an
# interactively-accepted RTI licence. rmw_zzdds neither builds nor tests
# against those, so skip them. (industrial_ci's SKIP_KEYS does the same.)
DEFAULT_ROSDEP_SKIP_KEYS = "rmw_connextdds rti-connext-dds-7.7.0"


def rosdep_install(ws: Path, *, ros_setup: Path, ros_distro: str, skip_keys: str) -> None:
    _require("rosdep", hint="apt-get install -y python3-rosdep && rosdep init")
    # ros:<distro> images ship with the apt lists cleared to save space;
    # rosdep shells out to `apt-get install` and needs a populated index.
    if shutil.which("apt-get"):
        try:
            _run(["apt-get", "update", "-qq"], timeout=300, label="apt-get update")
        except StepError as e:
            print(f"[warn] {e} -- continuing; rosdep install may fail on a stale index",
                  flush=True)
    # `rosdep update` is cheap and guards against a stale cache in a
    # freshly-pulled image; failure to update is not fatal on its own.
    try:
        _bash("rosdep update --rosdistro " + ros_distro, ros_setup=ros_setup, cwd=ws,
              timeout=300, label="rosdep update")
    except StepError as e:
        print(f"[warn] {e} -- continuing with existing rosdep cache", flush=True)
    skip = f' --skip-keys "{skip_keys}"' if skip_keys else ""
    _bash(
        f"rosdep install --from-paths src --ignore-src -y --rosdistro {ros_distro}{skip}",
        ros_setup=ros_setup,
        cwd=ws,
        timeout=900,
        label="rosdep install",
    )


def _colcon_build_cmd(packages: list[str], zzdds_prefix: Path, *, jobs: int, testing: bool) -> str:
    return (
        f"colcon build "
        f"--packages-up-to {' '.join(packages)} "
        f"--event-handlers console_direct+ "
        f"--parallel-workers {jobs} "
        f"--cmake-args "
        f"-DCMAKE_PREFIX_PATH='{zzdds_prefix}' "
        f"-DCMAKE_BUILD_TYPE=RelWithDebInfo "
        f"-DBUILD_TESTING={'ON' if testing else 'OFF'}"
    )


def colcon_build(
    ws: Path, zzdds_prefix: Path, *, ros_setup: Path, jobs: int, testing: bool, full: bool
) -> None:
    _require("colcon", hint="apt-get install -y python3-colcon-common-extensions")
    setup = ws / "install" / "setup.bash"

    # Pass 1: the zzdds type support, so it is in the ament index before
    # rmw_dds_common configures in pass 2 (see TYPESUPPORT_PACKAGES comment).
    _bash(
        _colcon_build_cmd(TYPESUPPORT_PACKAGES, zzdds_prefix, jobs=jobs,
                          testing=testing and not full),
        ros_setup=ros_setup, cwd=ws, timeout=2400, label="colcon build (pass 1: type support)",
    )
    if not setup.is_file():
        raise StepError(f"pass 1 produced no {setup}")
    if not full:
        return

    # Pass 2: rmw_dds_common (rebuilt with the zzdds type support visible) and
    # the RMW itself, with pass 1's install/ sourced on top of the ROS env.
    _bash(
        _colcon_build_cmd(BUILD_PACKAGES, zzdds_prefix, jobs=jobs, testing=testing),
        ros_setup=ros_setup, cwd=ws, timeout=3600, label="colcon build (pass 2: rmw)",
        extra_setup=setup,
    )


def colcon_test(ws: Path, zzdds_prefix: Path, *, ros_setup: Path, jobs: int, full: bool) -> None:
    setup = ws / "install" / "setup.bash"
    packages = TEST_PACKAGES if full else TYPESUPPORT_PACKAGES
    # The test CMakeLists prepend the zzdds lib dir via ENVIRONMENT_MODIFICATION;
    # exporting it here too covers anything that slips through.
    script_test = (
        f'export LD_LIBRARY_PATH="{zzdds_prefix / "lib"}:${{LD_LIBRARY_PATH:-}}"\n'
        f"colcon test "
        f"--packages-select {' '.join(packages)} "
        f"--event-handlers console_direct+ "
        f"--return-code-on-test-failure "
        f"--parallel-workers {jobs}"
    )
    _bash(script_test, ros_setup=ros_setup, cwd=ws, timeout=1800, label="colcon test",
          extra_setup=setup)
    _bash("colcon test-result --verbose --all", ros_setup=ros_setup, cwd=ws, timeout=120,
          label="colcon test-result")


def run_upstream_rmw_tests(ws: Path, *, ros_setup: Path) -> None:
    """FOLLOW-ON HOOK -- not implemented.

    docs/testing.md describes running the upstream `test_rmw_implementation`
    conformance suites (publisher / subscription / QoS / graph / CFT / loan)
    with RMW_IMPLEMENTATION=rmw_zzdds_cpp against the `test_rmw_implementation`
    package from the pinned `rmw_implementation` checkout in ci/rolling.repos.
    That package is not currently a dependency of any package here, so wiring
    it up means: build with --rolling-repos overlay (to get the pinned
    `rmw_implementation`), add `test_rmw_implementation` to BUILD_PACKAGES,
    then `colcon test --packages-select test_rmw_implementation` with the env
    var set. Kept as a named seam so the follow-on is a localized change.
    """
    raise NotImplementedError(
        "upstream test_rmw_implementation suites are a follow-on; see "
        "run_upstream_rmw_tests.__doc__ and docs/testing.md"
    )


# ── Orchestration ─────────────────────────────────────────────────────────


def run(args: argparse.Namespace) -> int:
    ros_setup = Path(args.ros_setup)
    if not ros_setup.is_file():
        print(f"FAIL: ROS setup script not found: {ros_setup}\n"
              f"  run inside a ros:{args.ros_distro} container, or pass --ros-setup",
              file=sys.stderr)
        return 2

    ws_root = Path(args.workspace).resolve()
    ws_root.mkdir(parents=True, exist_ok=True)
    zzdds_dest = ws_root / "zzdds"
    rmw_dest = ws_root / "rmw_zzdds_src"
    ros_ws = ws_root / "ros_ws"

    started = time.monotonic()
    try:
        rmw_src = _resolve_source(
            src=args.rmw_zzdds_src, repo=args.rmw_zzdds_repo, ref=args.rmw_zzdds_ref,
            dest=rmw_dest, label="rmw_zzdds",
        )
        zzdds_src = _resolve_source(
            src=args.zzdds_src, repo=args.zzdds_repo, ref=args.zzdds_ref,
            dest=zzdds_dest, label="zzdds",
        )
        if args.ros_target:
            print(f"[info] ros-target={args.ros_target}  ROS_DISTRO={args.ros_distro}",
                  flush=True)
        full = args.rolling_repos == "overlay"
        if not full:
            print("[warn] --rolling-repos skip: building/testing only the zzdds type "
                  "support packages; the RMW itself needs the overlay.", flush=True)
        zzdds_prefix = build_zzdds(zzdds_src, jobs=args.jobs)
        make_workspace(ros_ws, rmw_src, rolling_repos=args.rolling_repos, ros_setup=ros_setup)
        if not args.skip_rosdep:
            rosdep_install(ros_ws, ros_setup=ros_setup, ros_distro=args.ros_distro,
                           skip_keys=args.rosdep_skip_keys)
        colcon_build(ros_ws, zzdds_prefix, ros_setup=ros_setup, jobs=args.jobs,
                     testing=args.test, full=full)
        if args.upstream_tests:
            run_upstream_rmw_tests(ros_ws, ros_setup=ros_setup)
        if args.test:
            colcon_test(ros_ws, zzdds_prefix, ros_setup=ros_setup, jobs=args.jobs, full=full)
    except StepError as e:
        print(f"\nFAIL: {e}", file=sys.stderr)
        return 1
    except NotImplementedError as e:
        print(f"\nFAIL (not implemented): {e}", file=sys.stderr)
        return 3

    print(f"\n[ok] rmw_zzdds build{' + test' if args.test else ''} passed "
          f"({time.monotonic() - started:.0f}s total)", flush=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )

    src = p.add_argument_group("sources (pass --*-src for a local tree, or --*-ref to clone)")
    src.add_argument("--rmw-zzdds-src", metavar="PATH",
                     help="local rmw_zzdds checkout (default: this repo)")
    src.add_argument("--rmw-zzdds-repo", default=DEFAULT_RMW_ZZDDS_REPO, metavar="OWNER/REPO")
    src.add_argument("--rmw-zzdds-ref", metavar="REF", help="rmw_zzdds git ref to clone")
    src.add_argument("--zzdds-src", metavar="PATH", help="local zzdds checkout")
    src.add_argument("--zzdds-repo", default=DEFAULT_ZZDDS_REPO, metavar="OWNER/REPO")
    src.add_argument("--zzdds-ref", metavar="REF",
                     help="zzdds git ref to clone (the commit under test, from zzdds CI)")

    build = p.add_argument_group("build / test")
    build.add_argument("--rolling-repos", choices=("overlay", "skip"), default="overlay",
                       help="overlay (default): vcs import + build the pinned "
                            "ci/rolling.repos revisions. Required for a full RMW build -- "
                            "rmw_dds_common must be rebuilt with the zzdds type support. "
                            "skip: build/test only the zzdds type support packages against "
                            "the ambient ROS install (a fast codegen smoke, not the RMW).")
    build.add_argument("--test", dest="test", action="store_true", default=True,
                       help="run colcon test after building (default)")
    build.add_argument("--no-test", dest="test", action="store_false",
                       help="build only")
    build.add_argument("--upstream-tests", action="store_true",
                       help="(follow-on, not implemented) also run test_rmw_implementation")
    build.add_argument("--skip-rosdep", action="store_true",
                       help="assume ROS deps are already installed")
    build.add_argument("--rosdep-skip-keys", default=DEFAULT_ROSDEP_SKIP_KEYS,
                       metavar="KEYS", help="space-separated rosdep keys to skip")
    build.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    build.add_argument("--workspace", default=str(REPO_ROOT / ".ci-ws"), metavar="DIR",
                       help="scratch workspace (default: <repo>/.ci-ws)")

    env = p.add_argument_group("environment")
    env.add_argument("--zig", metavar="PATH", help="zig binary (default: from PATH)")
    env.add_argument("--ros-target", choices=("digest", "rolling"), default=None,
                     help='which ROS image leg this run represents ("digest" = pinned, '
                          '"rolling" = moving tag). Logging only -- a label for the CI '
                          "logs; the image itself is selected by the workflow's container:.")
    env.add_argument("--ros-distro", default=os.environ.get("ROS_DISTRO", "rolling"))
    env.add_argument("--ros-setup", default=None, metavar="PATH",
                     help="ROS setup.bash (default: /opt/ros/<distro>/setup.bash)")

    mtx = p.add_argument_group("matrix (used by the workflow, not a build)")
    mtx.add_argument("--print-matrix", choices=("pr", "schedule", "zzdds-pr"), metavar="MODE",
                     help="emit the build matrix as JSON for this trigger and exit")
    mtx.add_argument("--zzdds-release", default=os.environ.get("ZZDDS_RELEASE_REF", ""),
                     metavar="REF", help="last zzdds release tag, for the release matrix leg")

    args = p.parse_args(argv)

    if args.print_matrix:
        combos = build_matrix(
            args.print_matrix,
            zzdds_ref=args.zzdds_ref or "main",
            zzdds_release=args.zzdds_release or None,
        )
        json.dump([c.as_matrix_entry() for c in combos], sys.stdout)
        sys.stdout.write("\n")
        return 0

    if args.zig:
        os.environ["PATH"] = f"{Path(args.zig).resolve().parent}:{os.environ.get('PATH', '')}"
    if args.rmw_zzdds_src is None and args.rmw_zzdds_ref is None:
        args.rmw_zzdds_src = str(REPO_ROOT)
    if args.ros_setup is None:
        args.ros_setup = f"/opt/ros/{args.ros_distro}/setup.bash"

    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
