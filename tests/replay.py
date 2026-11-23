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


from_file = run("--input", fixture).stdout
from_stdin = run(data=batch).stdout
assert from_file == from_stdin

expected = [
    ("mock-01", "forward", 0.2, 0, 0, "accepted", True),
    ("mock-02", "backward", -0.2, 0, 0, "accepted", True),
    ("mock-03", "left", 0, 0, 0.3, "accepted", True),
    ("mock-04", "right", 0, 0, -0.3, "accepted", True),
    ("mock-05", "stop", 0, 0, 0, "accepted", True),
    ("mock-06", "stop", 0, 0, 0, "rejected", True),
    ("mock-01", "skip", 0, 0, 0, "duplicate", False),
    ("mock-07", "stop", 0, 0, 0, "unsupported_intent", True),
    ("mock-08", "stop", 0, 0, 0, "unsupported_intent", True),
    ("mock-09", "stop", 0, 0, 0, "empty_intent", True),
    (None, "stop", 0, 0, 0, "invalid_json", True),
]
fields = ("event_id", "action", "vx", "vy", "vyaw", "reason", "applied")
actual = [tuple(row[key] for key in fields) for row in rows(from_stdin)]
assert actual == expected, actual

first, second, *_ = batch.splitlines()
assert rows(run(data=first + b"\r\n" + second).stdout) == rows(from_stdin)[:2]
overlong = rows(run(data=b" " * (1024 * 1024 + 1) + b"\n" + first).stdout)
assert [row["reason"] for row in overlong] == ["line_too_long", "accepted"]

changed = rows(run("--linear-speed", "0.5", "--angular-speed", "0.7", data=batch).stdout)
assert [changed[i]["vx"] for i in (0, 1)] == [0.5, -0.5]
assert [changed[i]["vyaw"] for i in (2, 3)] == [0.7, -0.7]

for args in (
    ("--g1", "test-interface"),
    ("--g1", "test-interface", "--linear-speed", "0.2", "--angular-speed", "0.3", "--input", fixture),
    ("--linear-speed", "nan"),
    ("--angular-speed", "-1"),
):
    assert run(*args, check=False).returncode == 2, args

for stop in (signal.SIGINT, signal.SIGTERM):
    with subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE) as process:
        process.stdin.write(first + b"\n")
        process.stdin.flush()
        assert rows(process.stdout.readline()) == rows(from_stdin)[:1]
        process.send_signal(stop)
        assert process.wait(timeout=2) == 0
