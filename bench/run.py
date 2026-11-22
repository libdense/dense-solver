"""Run the pigeonhole comparison, taking one timing per fresh process."""
import argparse
import csv
import hashlib
import os
from pathlib import Path
import random
import subprocess
import sys
from time import perf_counter_ns

ROOT = Path(__file__).resolve().parents[1]
FIELDS = ("instance", "scope", "solver", "trial", "verdict", "time_ns", "timeout_s")
PAIR = "php9x8-pairwise"
SEQ = "php9x8-sequential"
LARGE = "php31x30-pairwise"
LARGEST = "php65x64-pairwise"
SELECTION = {
    PAIR: (7, ["dense-solver", "dense-cdcl", "lingeling", "kissat", "minisat", "cadical", "cryptominisat"]),
    SEQ: (3, ["dense-solver", "lingeling", "kissat"]),
    LARGE: (3, ["dense-solver", "lingeling"]),
    LARGEST: (3, ["dense-solver", "lingeling"]),
    "php31x30-sequential": (3, ["dense-solver"]),
    "php65x64-sequential": (3, ["dense-solver"]),
}


def check_inputs():
    for line in (ROOT / "instances/SHA256SUMS").read_text().splitlines():
        expected, name = line.split()
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != expected:
            raise ValueError(f"Input checksum mismatch: {name}")


def command(solver):
    if solver == "lingeling":
        return [str(ROOT / "build/bench-lingeling")]
    if solver in ("dense-solver", "dense-cdcl"):
        args = [str(ROOT / "build/dense-solver"), "--measure"]
        if solver == "dense-cdcl":
            args.append("--cdcl")
        return args
    return [sys.executable, str(ROOT / "bench/bindings.py"), solver]


def run_trial(instance, scope, solver, trial, timeout):
    cmd = command(solver)
    with (ROOT / "instances" / f"{instance}.cnf").open("rb") as source:
        begin = perf_counter_ns()
        try:
            result = subprocess.run(cmd, stdin=source, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return instance, scope, solver, trial, "TIMEOUT", "", timeout
        process_ns = perf_counter_ns() - begin
    output = result.stdout.decode().split()
    if result.returncode != 20 or len(output) != 3 or output[0] != "UNSAT":
        raise RuntimeError(f"{solver} on {instance}: exit {result.returncode}, "
                           f"stdout={result.stdout!r}, stderr={result.stderr!r}")
    solve_ns = int(output[2])
    if solve_ns <= 0:
        raise ValueError(f"Invalid solve time: {output[2]}")
    if solver == "dense-solver":
        method = "count"
        if output[1] != method:
            raise ValueError(f"{instance}: expected {method}, got {output[1]}")
    elapsed = process_ns if scope == "process" else solve_ns
    return instance, scope, solver, trial, "UNSAT", elapsed, timeout


def sample(writer, stream, instance, scope, solvers, repeats, timeout_override):
    active = list(solvers)
    for trial in range(1, repeats + 1):
        order = active.copy()
        random.Random(7999 + trial).shuffle(order)
        for solver in order:
            timeout = timeout_override or (5 if solver == "lingeling" else 8)
            row = run_trial(instance, scope, solver, trial, timeout)
            writer.writerow(row)
            stream.flush()
            if row[4] == "TIMEOUT":
                active.remove(solver)
        if not active:
            break


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true", help="include external solvers")
    parser.add_argument("--output", required=True, type=Path, help="write a new CSV")
    parser.add_argument("--timeout", type=float, help="override per-process timeout in seconds")
    args = parser.parse_args()
    if args.timeout is not None and args.timeout <= 0:
        parser.error("--timeout must be positive")
    if not hasattr(os, "sched_setaffinity"):
        parser.error("The benchmark uses Linux CPU affinity.")
    os.sched_setaffinity(0, {min(os.sched_getaffinity(0))})
    check_inputs()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(FIELDS)
        for instance, (repeats, solvers) in SELECTION.items():
            selected = solvers if args.all else ["dense-solver"]
            sample(writer, stream, instance, "solve", selected, repeats, args.timeout)
        selected = ["dense-solver", "lingeling"] if args.all else ["dense-solver"]
        sample(writer, stream, PAIR, "process", selected, 101, args.timeout)
    print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
