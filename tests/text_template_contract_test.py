"""Assert the C implementation against the same corpus as the Go author tools."""
import json
import pathlib
import subprocess
import sys


def parse(probe, data):
    result = subprocess.run([probe], input=data, capture_output=True, check=True)
    return json.loads(result.stdout)


def main():
    probe = sys.argv[1]
    fixtures = json.loads(pathlib.Path(sys.argv[2]).read_text())
    for fixture in fixtures:
        actual = parse(probe, fixture["input"].encode())
        if fixture.get("invalid"):
            assert actual.get("invalid"), fixture["name"]
            continue
        assert not actual.get("invalid"), (fixture["name"], actual)
        for run in actual["runs"]:
            run.pop("source_offset")
            if not run["style"]:
                run.pop("style")
        assert actual["runs"] == fixture["runs"], (fixture["name"], actual)

    nested = b"<i>" * 16 + b"x" + b"</i>" * 16
    assert not parse(probe, nested).get("invalid")
    for invalid in (b"<i>" + nested + b"</i>", b"\xff", b"\0", b"x" * (256 * 1024 + 1), b"{value}" * 4097):
        assert parse(probe, invalid).get("invalid")
    runs = parse(probe, "é<i>{value}</i>x".encode())["runs"]
    assert [run["source_offset"] for run in runs] == [0, 5, 16]
    print(f"{len(fixtures)} shared template cases and boundary checks passed")


if __name__ == "__main__":
    main()
