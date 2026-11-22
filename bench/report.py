"""Print median milliseconds from the release or from a new timing CSV."""
import argparse
from collections import defaultdict
import csv
from pathlib import Path
from statistics import median
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    args = parser.parse_args()
    groups = defaultdict(list)
    with args.csv.open(newline="") as stream:
        for row in csv.DictReader(stream):
            groups[(row["instance"], row["scope"], row["solver"])].append(row)
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(["instance", "scope", "solver", "completed", "timeouts", "median_ms"])
    for group, trials in groups.items():
        times = [int(row["time_ns"]) for row in trials if row["verdict"] == "UNSAT"]
        censored = sum(row["verdict"] == "TIMEOUT" for row in trials)
        if len(times) + censored != len(trials):
            raise ValueError(f"Unrecognized verdict for {group}")
        value = f"{median(times) / 1000000:.6f}" if times and not censored else ""
        writer.writerow([*group, len(times), censored, value])


if __name__ == "__main__":
    main()
