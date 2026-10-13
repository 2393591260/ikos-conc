#!/usr/bin/env python3
"""Check an IKOS-ConC registration file with the official FM-Tools scripts."""

import argparse
import runpy
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fm-tools", type=Path, required=True)
    parser.add_argument(
        "--entry", type=Path, default=Path(__file__).with_name("fm-tools-entry.yml")
    )
    args = parser.parse_args()
    source = args.entry.read_bytes()
    if b"\r" in source:
        parser.error("the entry contains CRLF/CR line endings; save it as LF")

    checkout = args.fm_tools.resolve()
    with tempfile.TemporaryDirectory(prefix="ikos-fm-entry-") as directory:
        entry = Path(directory) / "ikos-conc.yml"
        entry.write_bytes(source)
        subprocess.run(
            [
                sys.executable,
                str(checkout / "ci/format_data.py"),
                "--file",
                str(entry),
                "--check",
            ],
            check=True,
        )
        sorter = runpy.run_path(str(checkout / "ci/format_data_sort.py"))
        sorter["sort_elements"](entry)
        if entry.read_bytes() != source:
            parser.error("the official FM-Tools sorter changes this entry")
        subprocess.run(
            [
                sys.executable,
                str(checkout / "ci/check-data.py"),
                "--schema",
                str(checkout / "data/schema.yml"),
                "--fm-data",
                str(entry),
                "--year",
                "2027",
            ],
            check=True,
        )
    print("PASS: LF, official formatting, sorting, and 2027 registration checks")


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"Registration check failed: {error}", file=sys.stderr)
        sys.exit(1)
