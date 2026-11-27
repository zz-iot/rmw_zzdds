"""Run reliable 1 KiB ROS messages between two RMWs, including a reader restart."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

ZZDDS = "rmw_zzdds_cpp"
PEERS = ("rmw_fastrtps_cpp", "rmw_cyclonedds_cpp")
SAMPLE_COUNT = 10
PAYLOAD_BYTES = 1024


def child(role, topic):
    import rclpy
    from std_msgs.msg import String

    rclpy.init()
    node = rclpy.create_node(f"interop_smoke_{role}")
    try:
        if role == "publisher":
            pub = node.create_publisher(String, topic, 10)
            sequence = 0
            matches = 0
            was_matched = False
            print("READY", flush=True)
            # Direct publish isolates data delivery from Python timer/wait-set behavior.
            while rclpy.ok():
                matched = pub.get_subscription_count() > 0
                if matched and not was_matched:
                    matches += 1
                    print(f"MATCHED {matches}", flush=True)
                elif was_matched and not matched:
                    print("UNMATCHED", flush=True)
                was_matched = matched
                sequence += 1
                pub.publish(String(data=f"{sequence:016d}" + "x" * (PAYLOAD_BYTES - 16)))
                time.sleep(0.1)
        else:
            sequences = []

            def receive(message):
                if len(message.data) == PAYLOAD_BYTES and message.data[16:] == "x" * (PAYLOAD_BYTES - 16):
                    sequences.append(int(message.data[:16]))

            sub = node.create_subscription(String, topic, receive, 10)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                rclpy.spin_once(node, timeout_sec=0.1)
                if len(sequences) >= SAMPLE_COUNT and sub.get_publisher_count():
                    if any(b <= a for a, b in zip(sequences, sequences[1:])):
                        raise RuntimeError(f"non-increasing sample sequence: {sequences}")
                    print(f"RECEIVED {sequences[0]} {sequences[-1]}", flush=True)
                    return
            raise RuntimeError(f"matched={sub.get_publisher_count()}, received={len(sequences)}")
    finally:
        node.destroy_node()
        rclpy.try_shutdown()


def wait_for_log(process, path, marker, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if marker in path.read_text().splitlines():
            return
        if process.poll() is not None:
            break
        time.sleep(0.05)
    raise RuntimeError(f"missing {marker} in {path}: {path.read_text()}")


def run_pair(publisher, subscriber, root):
    directory = root / f"{publisher}-to-{subscriber}"
    directory.mkdir(parents=True, exist_ok=True)
    topic = f"/interop_smoke_{uuid.uuid4().hex}"
    command = [sys.executable, str(Path(__file__).resolve())]
    pub_log = directory / "publisher.log"
    with pub_log.open("w") as log:
        process = subprocess.Popen(
            command + ["--role", "publisher", "--topic", topic],
            env=dict(os.environ, RMW_IMPLEMENTATION=publisher),
            stdout=log, stderr=subprocess.STDOUT,
        )
        try:
            wait_for_log(process, pub_log, "READY", 10)
            time.sleep(0.4)
            last = 0
            for phase in ("late-join", "restart"):
                result = subprocess.run(
                    command + ["--role", "subscriber", "--topic", topic],
                    env=dict(os.environ, RMW_IMPLEMENTATION=subscriber),
                    capture_output=True, text=True, timeout=15,
                )
                (directory / f"{phase}.log").write_text(result.stdout + result.stderr)
                result.check_returncode()
                line = next(line for line in result.stdout.splitlines() if line.startswith("RECEIVED "))
                _, first, end = line.split()
                if int(first) <= last:
                    raise RuntimeError(f"{phase}: received {first} after {last}")
                last = int(end)
                if phase == "late-join":
                    wait_for_log(process, pub_log, "MATCHED 1", 2)
                    wait_for_log(process, pub_log, "UNMATCHED", 10)
                else:
                    wait_for_log(process, pub_log, "MATCHED 2", 2)
                print(f"PASS {publisher} -> {subscriber} {phase}: {first}..{end}", flush=True)
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--role", choices=("publisher", "subscriber"))
    parser.add_argument("--topic")
    parser.add_argument("--output", type=Path, default=Path("rmw-smoke-results"))
    args = parser.parse_args()
    if args.role:
        child(args.role, args.topic)
        return
    failures = []
    for peer in PEERS:
        for pub, sub in ((ZZDDS, peer), (peer, ZZDDS)):
            try:
                run_pair(pub, sub, args.output)
            except (RuntimeError, subprocess.SubprocessError) as error:
                failures.append((pub, sub))
                print(f"FAIL {pub} -> {sub}: {error}", file=sys.stderr, flush=True)
    if failures:
        sys.exit(1)


if __name__ == "__main__":
    main()
