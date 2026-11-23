import json
import pathlib
import signal
import subprocess
import sys


binary, fixture = sys.argv[1:]
batch = pathlib.Path(fixture).read_bytes()


def run(*args, data=None, check=True):
    return subprocess.run(
        [binary, *args], input=data, capture_output=True, check=check, timeout=3
    )


def rows(output):
    return [json.loads(line) for line in output.splitlines()]


caps = json.loads(run("--capabilities").stdout)
assert caps["schema_version"] == 1
assert caps["live_log"] is True
assert set(caps["intents"]) == {"locomotion.translate", "locomotion.turn", "locomotion.stop"}

from_file = run("--input", fixture).stdout
from_stdin = run(data=batch).stdout
assert from_file == from_stdin

parsed_fixture = [
    json.loads(line) if i < 12 else None
    for i, line in enumerate(batch.decode("utf-8").splitlines())
]
ids = [item["event_id"] if item else None for item in parsed_fixture]

expected = [
    (ids[0], "forward", 0.2, 0, 0, "accepted", True),
    (ids[1], "backward", -0.2, 0, 0, "accepted", True),
    (ids[2], "strafe_left", 0, 0.2, 0, "accepted", True),
    (ids[3], "strafe_right", 0, -0.2, 0, "accepted", True),
    (ids[4], "turn_left", 0, 0, 0.3, "accepted", True),
    (ids[5], "turn_right", 0, 0, -0.3, "accepted", True),
    (ids[6], "stop", 0, 0, 0, "accepted", True),
    (ids[7], "stop", 0, 0, 0, "rejected", True),
    (ids[8], "skip", 0, 0, 0, "duplicate", False),
    (ids[9], "stop", 0, 0, 0, "unsupported_intent", True),
    (ids[10], "stop", 0, 0, 0, "empty_intent", True),
    (ids[11], "stop", 0, 0, 0, "unsupported_schema", True),
    (None, "stop", 0, 0, 0, "invalid_json", True),
]
fields = ("event_id", "action", "vx", "vy", "vyaw", "reason", "applied")
actual = [tuple(row[key] for key in fields) for row in rows(from_stdin)]
assert actual == expected, actual

first, second, *_ = batch.splitlines()
assert rows(run(data=first + b"\r\n" + second).stdout) == rows(from_stdin)[:2]
overlong = rows(run(data=b" " * (1024 * 1024 + 1) + b"\n" + first).stdout)
assert [row["reason"] for row in overlong] == ["line_too_long", "accepted"]

changed = rows(
    run("--linear-speed", "0.5", "--lateral-speed", "0.35", "--angular-speed", "0.7", data=batch).stdout
)
assert [changed[i]["vx"] for i in (0, 1)] == [0.5, -0.5]
assert [changed[i]["vy"] for i in (2, 3)] == [0.35, -0.35]
assert [changed[i]["vyaw"] for i in (4, 5)] == [0.7, -0.7]

# When --lateral-speed is omitted, it follows --linear-speed.
coupled = rows(run("--linear-speed", "0.4", data=batch).stdout)
assert [coupled[i]["vy"] for i in (2, 3)] == [0.4, -0.4]

for args in (
    ("--g1", "test-interface"),
    ("--g1", "test-interface", "--linear-speed", "0.2", "--angular-speed", "0.3", "--input", fixture),
    ("--live", "--input", fixture),
    ("--linear-speed", "nan"),
    ("--lateral-speed", "0"),
    ("--angular-speed", "-1"),
    ("--watchdog-ms", "10"),
    ("--watchdog-ms", "1500"),
):
    assert run(*args, check=False).returncode == 2, args

for stop in (signal.SIGINT, signal.SIGTERM):
    with subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE) as process:
        process.stdin.write(first + b"\n")
        process.stdin.flush()
        assert rows(process.stdout.readline()) == rows(from_stdin)[:1]
        process.send_signal(stop)
        assert process.wait(timeout=2) == 0
