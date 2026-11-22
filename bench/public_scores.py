"""Summarize complete process trials. Each timeout costs twice the deadline."""
import argparse
from collections import defaultdict
import csv
from pathlib import Path
from statistics import mean, median

from public_suite import DEFAULT_MANIFEST, read_manifest


def score(rows, manifest):
    cases = {r["id"]: r["answer"] for r in manifest}
    trials = defaultdict(dict)
    solvers = set()
    limits = set()
    for row in rows:
        name, solver = row["instance"], row["solver"]
        trial = int(row["trial"])
        limit = float(row["timeout_s"])
        if row["scope"] != "process" or row["verdict"] not in ("TIMEOUT", cases[name]):
            raise ValueError(f"Unexpected clock or verdict on {name}")
        runs = trials[name, solver]
        if trial < 1 or trial in runs or not 0 < limit <= 86400:
            raise ValueError(f"Invalid trial or deadline on {name}")
        if row["verdict"] == "TIMEOUT":
            if row["time_ns"]:
                raise ValueError(f"Unexpected time on timeout: {name}")
            runs[trial] = 2 * limit
        else:
            seconds = int(row["time_ns"]) / 1e9
            if not 0 < seconds < limit:
                raise ValueError(f"Invalid process time: {name}")
            runs[trial] = seconds
        limits.add(limit)
        solvers.add(solver)
    if len(limits) != 1 or not solvers:
        raise ValueError("Expected nonempty trials with one shared deadline")
    limit = limits.pop()
    count = max(max(runs) for runs in trials.values())
    if count % 2 == 0:
        raise ValueError("Use an odd number of trials for median scoring")
    expected = set(range(1, count + 1))
    data = {}
    for name in cases:
        for solver in solvers:
            runs = trials[name, solver]
            if runs.keys() != expected:
                raise ValueError(f"Incomplete trials for {solver} on {name}")
            data[name, solver] = median(runs.values())
    return sorted(solvers), data, limit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args()
    manifest = read_manifest(args.manifest)
    with args.csv.open(newline="") as stream:
        solvers, data, limit = score(csv.DictReader(stream), manifest)
    print("instance," + ",".join(f"{solver}_median_ms" for solver in solvers))
    wins = defaultdict(int)
    for row in manifest:
        name = row["id"]
        times = [data[name, solver] for solver in solvers]
        best = min(times)
        if best < limit and times.count(best) == 1:
            wins[solvers[times.index(best)]] += 1
        cells = ["TIMEOUT" if t >= limit else f"{t * 1000:.3f}" for t in times]
        print(f"{name}," + ",".join(cells))
    print("\nsolver,solved,unique_fastest,mean_PAR2_s")
    for solver in solvers:
        times = [data[row["id"], solver] for row in manifest]
        print(f"{solver},{sum(t < limit for t in times)},{wins[solver]},{mean(times):.6f}")


if __name__ == "__main__":
    main()
