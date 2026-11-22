"""Time fresh native processes on the bundled inputs. External solvers are optional."""
import argparse
import csv
import os
from pathlib import Path
import random
import subprocess
import tempfile
from time import perf_counter_ns

ROOT = Path(__file__).resolve().parents[1]


def run(cmd, code, deadline):
    result = subprocess.run(cmd, capture_output=True, timeout=max(0.001, (deadline-perf_counter_ns())/1e9))
    if result.returncode != code:
        raise RuntimeError(f"{cmd[0]} exited {result.returncode}: {result.stderr[-300:]}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--previous", type=Path, help="path to dense-solver 0.0.1")
    parser.add_argument("--satsuma-src", type=Path, help="satsuma-iter-kissat submission src directory")
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--timeout", type=float, default=5)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.trials < 1 or args.timeout <= 0:
        parser.error("trials and timeout must be positive")
    os.sched_setaffinity(0, {min(os.sched_getaffinity(0))})
    files = list(sorted((ROOT/"instances").glob("php*.cnf")))
    with args.output.open("x", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(["instance","scope","solver","trial","verdict","time_ns","timeout_s"])
        for file in files:
            active = ["dense-solver-0.0.2"]
            if args.previous:
                active.append("dense-solver-0.0.1")
            if args.satsuma_src:
                active.append("satsuma-iter+kissat-proof-off")
            for trial in range(1, args.trials+1):
                order = active.copy()
                random.Random(7999+trial).shuffle(order)
                for solver in order:
                    with tempfile.TemporaryDirectory() as tmp:
                        reduced = Path(tmp)/"reduced.cnf"
                        is_dense = solver.startswith("dense-solver")
                        try:
                            start = perf_counter_ns()
                            deadline = start+int(args.timeout*1e9)
                            if is_dense:
                                binary = ROOT/"build/dense-solver" if solver.endswith(".2") else args.previous
                                out = run([binary, "--measure", file], 20, deadline)
                            else:
                                src = args.satsuma_src
                                run([src/"satsuma", "fix", file, "--silent", "--full-skip-limit",
                                     "100000000", "--add-reduced-as-unit", "--out-file", reduced], 0, deadline)
                                out = run([src/"solver/kissat/build/kissat", reduced], 20, deadline)
                            elapsed = perf_counter_ns()-start
                            if is_dense:
                                verdict, method, ns = out.split()
                                if verdict != b"UNSAT" or (solver.endswith(".2") and method != b"count"):
                                    raise ValueError(f"Unexpected result {out!r}")
                                writer.writerow([file.stem,"solve",solver,trial,"UNSAT",int(ns),args.timeout])
                            elif b"s UNSATISFIABLE" not in out.splitlines():
                                raise ValueError(f"Missing verdict {out[-300:]!r}")
                            writer.writerow([file.stem,"process",solver,trial,"UNSAT",elapsed,args.timeout])
                        except subprocess.TimeoutExpired:
                            writer.writerow([file.stem,"process",solver,trial,"TIMEOUT","",args.timeout])
                            active.remove(solver)
                    stream.flush()


if __name__ == "__main__":
    main()
