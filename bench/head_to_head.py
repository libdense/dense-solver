"""Compare native solvers on identical DIMACS files, one fresh process per trial."""
import argparse
import csv
import hashlib
import os
from pathlib import Path
import random
import subprocess
import tempfile
from time import perf_counter_ns

ROOT = Path(__file__).resolve().parents[1]
FIELDS = ("instance", "scope", "solver", "trial", "verdict", "time_ns", "timeout_s")
ORIGINAL = {f"php{p}x{h}-{enc}" for p, h in ((9, 8), (31, 30), (65, 64))
            for enc in ("pairwise", "sequential")}


def run(cmd, code, deadline):
    time_left = (deadline - perf_counter_ns()) / 1e9
    if time_left <= 0:
        raise subprocess.TimeoutExpired(cmd, 0)
    result = subprocess.run(cmd, capture_output=True, timeout=time_left)
    if result.returncode != code:
        raise RuntimeError(f"{cmd[0]} exited {result.returncode}: {result.stderr[-300:]}")
    return result.stdout


def dense_name(binary):
    name, version = subprocess.check_output([binary, "--version"], text=True).strip().split()
    return f"{name}-{version}"


def trial(binary, options, src, file, timeout, reduced, expected):
    code = 20 if expected == "UNSAT" else 10
    start = perf_counter_ns()
    deadline = start + int(timeout * 1e9)
    if binary is None:
        run([src / "satsuma", "fix", file, "--silent", "--full-skip-limit", "100000000",
             "--add-reduced-as-unit", "--out-file", reduced], 0, deadline)
        out = run([src / "solver/kissat/build/kissat", reduced], code, deadline)
        end = perf_counter_ns()
        if b"s " + expected.encode() + b"ISFIABLE" not in out.splitlines():
            raise ValueError(f"Unexpected Kissat verdict on {file.name}")
        return end - start, None, None
    out = run([binary, *options, "--measure", file], code, deadline)
    end = perf_counter_ns()
    verdict, method, ns = out.decode().split()
    if verdict != expected or int(ns) <= 0:
        raise ValueError(f"Unexpected verdict or time: {out}")
    return end - start, int(ns), method


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--previous", type=Path, help="previous native executable")
    parser.add_argument("--satsuma-src", type=Path, help="satsuma-iter-kissat submission src directory")
    parser.add_argument("--all-inputs", action="store_true", help="time all 20 bundled inputs")
    parser.add_argument("--no-hall-control", action="store_true", help="also run this binary with Hall disabled")
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--timeout", type=float, default=5)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.trials < 1 or args.timeout <= 0:
        parser.error("trials and timeout must be positive")
    os.sched_setaffinity(0, {min(os.sched_getaffinity(0))})
    binary = ROOT / "build/dense-solver"
    current = dense_name(binary)
    commands = {current: (binary, [])}
    if args.previous:
        previous = args.previous.resolve()
        label = dense_name(previous)
        if label == current:
            parser.error("previous version must differ from the current version")
        commands[label] = (previous, [])
    if args.no_hall_control:
        commands[current + "-no-hall"] = (binary, ["--no-hall"])
    if args.satsuma_src:
        commands["satsuma-iter+kissat-proof-off"] = (None, [])
    src = args.satsuma_src.resolve() if args.satsuma_src else None
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(FIELDS)
        for line in (ROOT / "instances/SHA256SUMS").read_text().splitlines():
            digest, name = line.split()
            file = ROOT / name
            if not args.all_inputs and file.stem not in ORIGINAL:
                continue
            if hashlib.sha256(file.read_bytes()).hexdigest() != digest:
                raise ValueError(f"Checksum mismatch: {name}")
            expected = "SAT" if file.stem.startswith("php8x8-") else "UNSAT"
            active = list(commands)
            for iteration in range(1, args.trials + 1):
                order = active.copy()
                random.Random(7999 + iteration).shuffle(order)
                for label in order:
                    executable, options = commands[label]
                    with tempfile.TemporaryDirectory() as tmp:
                        try:
                            elapsed, solve, method = trial(executable, options, src, file,
                                args.timeout, Path(tmp) / "reduced.cnf", expected)
                        except subprocess.TimeoutExpired:
                            writer.writerow([file.stem, "process", label, iteration, "TIMEOUT", "", args.timeout])
                            active.remove(label)
                            continue
                    if label == current and expected == "UNSAT":
                        wanted = "count" if file.stem in ORIGINAL else None
                        if method not in ("hall", "count") or (wanted and wanted != method):
                            raise ValueError(f"Unexpected method on {name}: {method}")
                    if solve is not None:
                        writer.writerow([file.stem, "solve", label, iteration, expected, solve, args.timeout])
                    writer.writerow([file.stem, "process", label, iteration, expected, elapsed, args.timeout])
                stream.flush()
                if not active:
                    break


if __name__ == "__main__":
    main()
