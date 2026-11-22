"""Run public DIMACS benchmarks on Linux, timing complete native solver processes."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import hashlib
import os
from pathlib import Path
import random
import re
import signal
import subprocess
import tempfile
import threading
from time import perf_counter_ns

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "bench/public-inputs.csv"
FIELDS = ("instance", "scope", "solver", "trial", "verdict", "time_ns", "timeout_s")


def read_manifest(path):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    seen = set()
    for row in rows:
        name = row["id"]
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,98}", name) or name in seen:
            raise ValueError(f"Invalid or duplicate input ID: {name}")
        if row["answer"] not in ("SAT", "UNSAT") or not re.fullmatch(r"[a-f0-9]{64}", row["input_sha256"]):
            raise ValueError(f"Invalid expected verdict or hash: {name}")
        seen.add(name)
    if not rows:
        raise ValueError("Empty manifest")
    return rows


def load_formula(path, row):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != row["input_sha256"]:
        raise ValueError(f"Input checksum mismatch: {path}")
    clauses = []
    current = []
    header = None
    for line in data.splitlines():
        line = line.strip()
        if not line or line.startswith(b"c"):
            continue
        if line.startswith(b"p"):
            kind, fmt, vs, cs = line.split()
            if header is not None or (kind, fmt) != (b"p", b"cnf"):
                raise ValueError(f"Invalid header in {path}")
            header = (int(vs), int(cs))
            continue
        if header is None:
            raise ValueError(f"Missing header in {path}")
        for lit in map(int, line.split()):
            if abs(lit) > header[0]:
                raise ValueError(f"Invalid literal in {path}")
            if lit:
                current.append(lit)
            else:
                clauses.append(current)
                current = []
    if current or header != (int(row["vars"]), len(clauses)) or len(clauses) != int(row["clauses"]):
        raise ValueError(f"Formula dimensions differ from manifest: {path}")
    return clauses


def validate_output(code, out, row, clauses):
    expected = row["answer"]
    status_lines = [line.split() for line in out.splitlines() if line.startswith(b"s ")]
    if code != (10 if expected == "SAT" else 20) or status_lines != [[b"s", (expected + "ISFIABLE").encode()]]:
        raise ValueError(f"Unexpected solver verdict or exit {code}: {status_lines}")
    if expected == "SAT":
        values = [int(lit) for line in out.splitlines() if line.startswith(b"v ") for lit in line.split()[1:]]
        if not values or values[-1] != 0 or 0 in values[:-1]:
            raise ValueError("Missing or malformed SAT model")
        model = set(values[:-1])
        if any(abs(lit) > int(row["vars"]) or -lit in model for lit in model):
            raise ValueError("Invalid literal in model")
        if not all(any(lit in model for lit in clause) for clause in clauses):
            raise ValueError("SAT model does not satisfy input")


def timed_trial(cmd, path, row, clauses, limit):
    begin = perf_counter_ns()
    with subprocess.Popen([*cmd, path], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          start_new_session=True) as proc:
        try:
            remaining = max(0.0, limit - (perf_counter_ns() - begin) / 1e9)
            out, err = proc.communicate(timeout=remaining)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.communicate()
            return "TIMEOUT", ""
        elapsed = perf_counter_ns() - begin
    if elapsed >= limit * 1e9:
        return "TIMEOUT", ""
    try:
        validate_output(proc.returncode, out, row, clauses)
    except ValueError as exc:
        raise ValueError(f"{path.name} with {cmd[0]}: {exc}; stderr {err[-200:]!r}") from exc
    return row["answer"], elapsed


def run_lane(cpu, cases, commands, trials, limit, record):
    os.sched_setaffinity(0, {cpu})
    for index, (row, path, clauses) in cases:
        order = list(commands)
        random.Random(731 + index).shuffle(order)
        for trial in range(1, trials + 1):
            for label in order:
                answer, ns = timed_trial(commands[label], path, row, clauses, limit)
                record((row["id"], "process", label, trial, answer, ns, limit))
            order = order[1:] + order[:1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", type=Path, default=ROOT / "bench/public-instances")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--kissat", type=Path)
    parser.add_argument("--cadical", type=Path)
    parser.add_argument("--solver", nargs=2, metavar=("LABEL", "EXECUTABLE"), action="append", default=[],
                        help="additional native CLI or adapter taking one DIMACS path")
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--jobs", type=int, default=1, help="parallel inputs, each assigned one distinct logical CPU")
    args = parser.parse_args()
    cpus = sorted(os.sched_getaffinity(0))
    if not (0 < args.timeout <= 86400 and args.trials > 0 and 1 <= args.jobs <= len(cpus)):
        parser.error("invalid trial count, deadline or number of CPUs")
    if args.output.exists():
        parser.error("choose a new output file")
    binary = ROOT / "build/dense-solver"
    version = subprocess.check_output([binary, "--version"], text=True).strip().replace(" ", "-")
    commands = {version: [binary]}
    for name, executable in (("kissat", args.kissat), ("cadical", args.cadical)):
        if executable:
            exe = executable.resolve(strict=True)
            tag = subprocess.check_output([exe, "--version"], text=True).splitlines()[0].strip()
            commands[f"{name}-{tag}"] = [exe, "-q"]
    for label, executable in args.solver:
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,98}", label) or label in commands:
            parser.error("invalid or duplicate solver label")
        commands[label] = [Path(executable).resolve(strict=True)]
    cases = []
    for index, row in enumerate(read_manifest(args.manifest)):
        path = args.inputs / (row["id"] + ".cnf")
        cases.append((index, (row, path, load_formula(path, row))))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    lock = threading.Lock()
    with tempfile.NamedTemporaryFile("w", dir=args.output.parent, newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(FIELDS)

        def record(row):
            with lock:
                writer.writerow(row)
                stream.flush()

        with ThreadPoolExecutor(args.jobs) as pool:
            futures = [pool.submit(run_lane, cpu, cases[i::args.jobs], commands, args.trials,
                                   args.timeout, record) for i, cpu in enumerate(cpus[:args.jobs])]
            for future in futures:
                future.result()
        os.link(stream.name, args.output)
    print(f"Saved {len(cases)} inputs x {len(commands)} solvers x {args.trials} trials to {args.output}")


if __name__ == "__main__":
    main()
