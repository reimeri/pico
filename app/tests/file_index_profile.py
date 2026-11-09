#!/usr/bin/env python3
"""Descriptive filesystem benchmark, not a machine-speed-dependent unit test.

Creates real empty files in an isolated temporary directory; never changes an
existing project. Uses a private cache and cleans only its temporary directory.
Run an optimized runner, e.g. see docs/file-completion-performance.md.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--sizes", nargs="+", type=int, default=[10_000, 100_000, 1_000_000])
    parser.add_argument("--backend", choices=["git", "rg"], default="git")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    runner = args.runner.resolve()
    results = {"platform": platform.platform(), "cpu": platform.processor(),
               "backend": args.backend, "files_per_directory": 1000, "runs": []}
    for size in args.sizes:
        with tempfile.TemporaryDirectory(prefix="pico-file-profile-") as tmp:
            root = Path(tmp) / "project"
            root.mkdir()
            if args.backend == "git":
                subprocess.run(["git", "-C", str(root), "init", "-q"], check=True)
            start = time.monotonic()
            for n in range(size - 1):
                directory = root / "src" / f"group_{n // 1000:05d}"
                if n % 1000 == 0:
                    directory.mkdir(parents=True)
                fd = os.open(directory / f"component_{n % 1000:05d}.gd", os.O_CREAT | os.O_WRONLY, 0o600)
                os.close(fd)
            notes = root / "Programmer_stuff_nocode"
            notes.mkdir()
            (notes / "target_notes.md").touch()
            setup_seconds = time.monotonic() - start
            env = dict(os.environ, XDG_CACHE_HOME=str(Path(tmp) / "cache"))
            # One cold-index run and one persisted-cache run per corpus. No
            # privileged OS page-cache dropping: files were just created.
            for mode in ["cold", "cached"]:
                completed = subprocess.run([str(runner), str(root), mode], env=env,
                                           check=True, capture_output=True, text=True)
                result = json.loads(completed.stdout)
                result.update(requested_files=size, setup_seconds=setup_seconds)
                results["runs"].append(result)
                print(json.dumps(result), flush=True)
        if args.output:
            args.output.write_text(json.dumps(results, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
