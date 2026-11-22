"""Check the public CLI, benchmark routes, and SAT models on small CNFs."""
from itertools import product
from pathlib import Path
import random
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/dense-solver"


def invoke(text, *args):
    return subprocess.run([str(BINARY), *args], input=text, text=True, capture_output=True, timeout=30)


def dimacs(variables, clauses):
    rows = [f"p cnf {variables} {len(clauses)}"]
    rows.extend(" ".join(map(str, [*clause, 0])) for clause in clauses)
    return "\n".join(rows) + "\n"


def satisfies(clauses, values):
    return all(any(values[abs(lit) - 1] == (lit > 0) for lit in clause) for clause in clauses)


def check_formula(n, clauses, expected, method=None):
    run = invoke(dimacs(n, clauses))
    if run.returncode != (10 if expected else 20):
        raise ValueError(f"Unexpected verdict on {clauses}: {run.stdout} {run.stderr}")
    lines = run.stdout.splitlines()
    if method and f"c method {method}" not in lines:
        raise ValueError(f"Expected {method}: {lines}")
    if expected:
        assignment = next(line for line in lines if line.startswith("v ")).split()
        literals = list(map(int, assignment[1:-1]))
        if assignment[-1] != "0" or list(map(abs, literals)) != list(range(1, n + 1)):
            raise ValueError(f"Invalid model: {assignment}")
        if not satisfies(clauses, [lit > 0 for lit in literals]):
            raise ValueError(f"Model failed independent check: {clauses}, {assignment}")


def main():
    check_formula(0, [], True)
    check_formula(0, [[]], False)
    check_formula(1, [[1], [-1]], False, "xor")
    check_formula(2, [[1, 2], [-1, -2]], True, "xor")
    check_formula(2, [[1, 2]], True, "search")
    pairwise = (ROOT / "instances/php9x8-pairwise.cnf").read_text()
    pair = invoke(pairwise)
    if pair.returncode != 20 or "c demand 9 capacity 8" not in pair.stdout:
        raise ValueError("Expected the 9 > 8 counting contradiction.")
    clauses = [list(map(int, line.split()[:-1])) for line in pairwise.splitlines()[1:]]
    check_formula(72, clauses[1:], True)
    sequential = invoke((ROOT / "instances/php9x8-sequential.cnf").read_text(), "--measure")
    if sequential.returncode != 20 or sequential.stdout.split()[:2] != ["UNSAT", "count"]:
        raise ValueError("Sequential input must be settled by counting.")
    for invalid in ("p cnf 2 1\n3 0\n", "p cnf 1 1\n1\n", "p cnf 1 2\n1 0\n"):
        if invoke(invalid).returncode != 1:
            raise ValueError("Malformed input was accepted.")
    rng = random.Random(731)
    for _ in range(100):
        n = rng.randrange(1, 7)
        clauses = []
        for _ in range(rng.randrange(1, 18)):
            variables = rng.sample(range(1, n + 1), rng.randrange(1, min(n, 4) + 1))
            clauses.append([v if rng.getrandbits(1) else -v for v in variables])
        expected = any(satisfies(clauses, bits) for bits in product([False, True], repeat=n))
        check_formula(n, clauses, expected)
    print("Passed fixture, encoding, model, and 100 exhaustive small-formula checks.")


if __name__ == "__main__":
    main()
