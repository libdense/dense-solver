"""Check the public CLI, the counting certificates and SAT models.

Uses only the Python standard library, the built solver, and the two
helper modules next to this file.
"""
from itertools import product
from pathlib import Path
import random
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_certificate  # noqa: E402
import families  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/dense-solver"


def invoke(text, *args):
    return subprocess.run([str(BINARY), *args], input=text, text=True,
                          capture_output=True, timeout=60)


def dimacs(variables, clauses):
    return families.dimacs(variables, clauses)


def satisfies(clauses, values):
    return all(any(values[abs(lit) - 1] == (lit > 0) for lit in clause) for clause in clauses)


def brute_force(n, clauses):
    return any(satisfies(clauses, bits) for bits in product([False, True], repeat=n))


def check_formula(n, clauses, expected, method=None, args=()):
    """Run the solver; check the verdict, the model, and any certificate."""
    text = dimacs(n, clauses)
    run = invoke(text, "--explain", *args)
    if run.returncode != (10 if expected else 20):
        raise ValueError(f"Unexpected verdict on {clauses}: {run.stdout} {run.stderr}")
    lines = run.stdout.splitlines()
    used = next(line.split()[2] for line in lines if line.startswith("c method"))
    if method and used != method:
        raise ValueError(f"Expected {method}, got {used}: {lines[:3]}")
    if expected:
        assignment = next(line for line in lines if line.startswith("v ")).split()
        literals = list(map(int, assignment[1:-1]))
        if assignment[-1] != "0" or list(map(abs, literals)) != list(range(1, n + 1)):
            raise ValueError(f"Invalid model: {assignment}")
        if not satisfies(clauses, [lit > 0 for lit in literals]):
            raise ValueError(f"Model failed independent check: {clauses}, {assignment}")
    elif used in ("count", "hall"):
        check_certificate.check(text, run.stdout)
    return used


def structured_families():
    """Each family is UNSAT by counting: settled by count or hall, with a
    certificate the independent checker accepts. Returns how many."""
    cases = []
    for encoding in families.ENCODINGS:
        base = families.pigeonhole(9, 8, encoding)
        cases += [base, families.pigeonhole(9, 8, encoding, functional=True),
                  families.rename(*base, seed=5), families.rename(*base, seed=6, flip=False)]
    for size in (4, 6, 8):
        board = families.mutilated_chessboard(size)
        cases += [board, families.rename(*board, seed=3)]
    for variables, clauses in cases:
        method = check_formula(variables, clauses, False)
        if method not in ("count", "hall"):
            raise ValueError(f"Expected counting, got {method}")
    # Satisfiable relatives: as many holes as pigeons.
    for encoding in families.ENCODINGS:
        variables, clauses = families.pigeonhole(8, 8, encoding)
        check_formula(variables, clauses, True)
        check_formula(*families.rename(variables, clauses, seed=9), True)
    return len(cases)


def hall_subset():
    """Nine pigeons share eight holes while five more pigeons have ten holes
    of their own: demand 14 against capacity 18 overall, so only a subset
    shows the contradiction."""
    holes = 18
    allowed = [list(range(8))] * 9 + [list(range(8, 18))] * 5
    variables, clauses = bipartite_formula(allowed, holes, ["pairwise"] * holes)
    if check_formula(variables, clauses, False) != "hall":
        raise ValueError("Expected Hall counting on the subset violation.")
    run = invoke(dimacs(variables, clauses))
    if "c demand 9 capacity 8" not in run.stdout:
        raise ValueError(f"Expected the 9-pigeon violator: {run.stdout}")


def bipartite_formula(allowed, holes, encodings):
    """Pigeon p must sit in one of allowed[p]; each hole takes at most one
    pigeon, encoded with encodings[hole]."""
    index = {}
    for pigeon, options in enumerate(allowed):
        for hole in options:
            index[(pigeon, hole)] = len(index) + 1
    fresh = families.Fresh(len(index) + 1)
    clauses = [[index[(p, h)] for h in options] for p, options in enumerate(allowed)]
    for hole in range(holes):
        members = [index[(p, hole)] for p, options in enumerate(allowed) if hole in options]
        clauses += families.ENCODINGS[encodings[hole]](members, fresh)
    return fresh.next - 1, clauses


def has_matching(allowed, holes):
    owner = [None] * holes

    def augment(pigeon, seen):
        for hole in allowed[pigeon]:
            if hole not in seen:
                seen.add(hole)
                if owner[hole] is None or augment(owner[hole], seen):
                    owner[hole] = pigeon
                    return True
        return False

    return all(augment(pigeon, set()) for pigeon in range(len(allowed)))


def matching_fuzz(count):
    """Random pigeon-hole compatibility graphs. The formula is satisfiable
    exactly when a matching covers every pigeon, which Python decides
    independently. Random encodings and renamings."""
    rng = random.Random(2026)
    names = list(families.ENCODINGS)
    tally = {"count": 0, "hall": 0, "search": 0, "xor": 0, "sat": 0}
    for trial in range(count):
        pigeons, holes = rng.randrange(2, 9), rng.randrange(1, 9)
        allowed = [sorted(rng.sample(range(holes), rng.randrange(1, holes + 1)))
                   for _ in range(pigeons)]
        variables, clauses = bipartite_formula(
            allowed, holes, [rng.choice(names) for _ in range(holes)])
        if trial % 2:
            variables, clauses = families.rename(variables, clauses, seed=trial)
        expected = has_matching(allowed, holes)
        method = check_formula(variables, clauses, expected)
        tally["sat" if expected else method] += 1
    return tally


def cardinality_soup(count):
    """At-most-one groups over random literals, random clauses and noise,
    small enough to check by brute force."""
    rng = random.Random(4242)
    names = list(families.ENCODINGS)
    methods = {}
    for trial in range(count):
        base = rng.randrange(3, 8)
        fresh = families.Fresh(base + 1)
        clauses = []
        for _ in range(rng.randrange(1, 4)):
            members = rng.sample(range(1, base + 1), rng.randrange(2, min(base, 5) + 1))
            literals = [v if rng.getrandbits(1) else -v for v in members]
            clauses += families.ENCODINGS[rng.choice(names)](literals, fresh)
        for _ in range(rng.randrange(1, 6)):
            members = rng.sample(range(1, base + 1), rng.randrange(1, min(base, 4) + 1))
            clauses.append([v if rng.getrandbits(1) else -v for v in members])
        variables = fresh.next - 1
        if variables > 14:
            continue
        for _ in range(rng.randrange(0, 4)):
            members = rng.sample(range(1, variables + 1), rng.randrange(1, min(variables, 3) + 1))
            clauses.append([v if rng.getrandbits(1) else -v for v in members])
        if trial % 3 == 0:
            variables, clauses = families.rename(variables, clauses, seed=trial)
        method = check_formula(variables, clauses, brute_force(variables, clauses))
        methods[method] = methods.get(method, 0) + 1
    return methods


def checker_rejects_tampering():
    """The checker is not vacuous: damaged certificates fail."""
    text = (ROOT / "instances/php9x8-sequential.cnf").read_text()
    good = invoke(text, "--explain").stdout
    lines = good.splitlines()
    groups = [i for i, line in enumerate(lines) if line.startswith("c certificate group")]
    clauses = [i for i, line in enumerate(lines) if line.startswith("c certificate clause")]
    def group_literals(index):
        return lines[index].split()[3:-1]

    def group_line(literals):
        return "c certificate group " + " ".join(literals) + " 0"

    dropped = list(lines)
    dropped[groups[0]] = group_line(group_literals(groups[0])[1:])
    merged = list(lines)
    merged[groups[0]] = group_line(group_literals(groups[0]) + group_literals(groups[1]))
    del merged[groups[1]]
    damaged = {
        "dropped literal": dropped,
        "extra group": lines + ["c certificate group 1 0"],
        "dropped clause": [line for i, line in enumerate(lines) if i != clauses[0]],
        "group that is not at-most-one": merged,
    }
    for name, version in damaged.items():
        try:
            check_certificate.check(text, "\n".join(version))
        except check_certificate.CertificateError:
            continue
        raise ValueError(f"Checker accepted a certificate with a {name}.")



def main():
    count = structured_families()
    hall_subset()
    matching = matching_fuzz(300)
    cardinality = cardinality_soup(300)
    checker_rejects_tampering()
    text = (ROOT / "instances/php9x8-pairwise.cnf").read_text()
    duplicated = text.replace("\n1 2 ", "\n1 1 2 ", 1)
    assert duplicated != text
    for case in (duplicated, "p cnf 3 4\n1 0\n-1 2 0\n-2 3 0\n-3 -1 0\n"):
        run = invoke(case, "--explain")
        assert run.returncode == 20 and "c method count" in run.stdout
        check_certificate.check(case, run.stdout)
    for path in sorted((ROOT / "instances").glob("*.cnf")):
        text = path.read_text()
        run = invoke(text, "--explain")
        if path.name.startswith("php8x8"):
            assert run.returncode == 10
        else:
            assert run.returncode == 20
            check_certificate.check(text, run.stdout)
    print(f"Passed {count} structured cases, {sum(matching.values())} matching cases, "
          f"{sum(cardinality.values())} brute-force controls, and release certificates.")


if __name__ == "__main__":
    main()
