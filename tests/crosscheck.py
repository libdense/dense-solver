"""Optional independent comparison using python-sat and CaDiCaL 3.0.0."""
from pathlib import Path
import random
import subprocess

from pysat.solvers import Solver

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/dense-solver"


def generate(rng, planted):
    size = rng.randrange(7, 76)
    count = rng.randint(2 * size, 6 * size)
    clauses = []
    for _ in range(count):
        variables = rng.sample(range(1, size + 1), rng.choice([2, 3, 3, 3, 4, 5]))
        clauses.append([v if rng.getrandbits(1) else -v for v in variables])
    if planted:
        assignment = [rng.choice([1, -1]) for _ in range(size)]
        for clause in clauses:
            if not any(assignment[abs(lit) - 1] * lit > 0 for lit in clause):
                clause[0] *= -1
    return size, clauses


def check(size, clauses, expected, options):
    body = "".join(" ".join(map(str, [*clause, 0])) + "\n" for clause in clauses)
    dimacs = f"p cnf {size} {len(clauses)}\n{body}"
    run = subprocess.run([BINARY, *options], input=dimacs, text=True, capture_output=True, timeout=15)
    if run.returncode != (10 if expected else 20):
        raise ValueError(f"Disagreement under {options}: {run.stdout[-300:]} {run.stderr}")
    if expected:
        line = next(line for line in run.stdout.splitlines() if line.startswith("v "))
        model = set(map(int, line.split()[1:-1]))
        if len(model) != size or not all(any(lit in model for lit in clause) for clause in clauses):
            raise ValueError(f"Invalid witness under {options}")


def main():
    rng = random.Random(191197)
    counts = {True: 0, False: 0}
    for i in range(1000):
        size, clauses = generate(rng, i % 5 == 0)
        with Solver(name="cadical300", bootstrap_with=clauses) as reference:
            expected = reference.solve()
        check(size, clauses, expected, [])
        heuristic = ["fixed", "activity", "placebo"][i % 3]
        check(size, clauses, expected, ["--cdcl", "--heuristic", heuristic, "--luby", "1", "--interval", "1"])
        counts[expected] += 1
    print(f"Matched CaDiCaL on {counts[True]} SAT and {counts[False]} UNSAT formulas in both modes.")


if __name__ == "__main__":
    main()
