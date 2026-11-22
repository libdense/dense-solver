"""Time a chessboard verdict, certificate generation and independent verification."""
import argparse
import csv
import gzip
import hashlib
import os
from pathlib import Path
from statistics import median
import subprocess
import sys
from time import perf_counter_ns

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path, help="new results directory")
    parser.add_argument("--trials", type=int, default=7)
    args = parser.parse_args()
    if args.trials < 1:
        parser.error("trials must be positive")
    args.output.mkdir(parents=True, exist_ok=False)
    os.sched_setaffinity(0, {min(os.sched_getaffinity(0))})
    binary = ROOT / "build/dense-solver"
    version = subprocess.check_output([binary, "--version"], text=True).strip()
    expected_cert = None
    raw = args.output / "explanation.txt"
    rows = []
    for trial in range(1, args.trials + 1):
        start = perf_counter_ns()
        result = subprocess.run([binary, "--measure", args.input], capture_output=True)
        elapsed = perf_counter_ns() - start
        tokens = result.stdout.split()
        if result.returncode != 20 or tokens[:2] != [b"UNSAT", b"hall"]:
            raise RuntimeError(f"Unexpected result: {result.stdout[:200]}")
        rows.append((trial, "solve", int(tokens[2])))
        rows.append((trial, "process", elapsed))
        with raw.open("wb") as out:
            start = perf_counter_ns()
            result = subprocess.run([binary, "--explain", args.input], stdout=out)
            elapsed = perf_counter_ns() - start
        if result.returncode != 20:
            raise RuntimeError(f"Certificate generation exited {result.returncode}")
        rows.append((trial, "process_with_certificate", elapsed))
        with raw.open("rb") as inp:
            cert = b"".join(line for line in inp if line.startswith(b"c certificate "))
        if trial == 1:
            expected_cert = cert
        if not cert or cert != expected_cert:
            raise ValueError("Certificate is missing or inconsistent between runs")
    raw.write_bytes(expected_cert)
    checker = ROOT / "tests/check_certificate.py"
    with raw.open("rb") as inp:
        start = perf_counter_ns()
        check = subprocess.run([sys.executable, checker, args.input], stdin=inp,
                               check=True, text=True, capture_output=True)
        rows.append((1, "check_certificate", perf_counter_ns()-start))
    compressed = args.output / f"{args.input.stem}-certificate.txt.gz"
    compressed.write_bytes(gzip.compress(expected_cert, compresslevel=9, mtime=0))
    raw.unlink()
    csv_path = args.output / f"{args.input.stem}-timings.csv"
    with csv_path.open("w", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(["instance", "version", "trial", "scope", "time_ns"])
        for trial, mode, ns in rows:
            writer.writerow([args.input.stem, version.split()[-1], trial, mode, ns])
    print(check.stdout.strip())
    print(f"Input SHA-256 {hashlib.sha256(args.input.read_bytes()).hexdigest()}")
    print(f"Certificate SHA-256 (uncompressed) {hashlib.sha256(expected_cert).hexdigest()}")
    for mode in ("solve", "process", "process_with_certificate", "check_certificate"):
        times = [ns for _, scope, ns in rows if scope == mode]
        print(f"{mode}: {median(times) / 1e9:.6f} s ({len(times)} runs)")


if __name__ == "__main__":
    main()
