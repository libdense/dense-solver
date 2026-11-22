"""Regression tests for implied conflicts, input reordering, search controls and reports."""
import csv
from io import StringIO
from pathlib import Path
import random
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "build/dense-solver"


def run(*args, text=None, code=0):
    p = subprocess.run([BIN, *args], input=text, text=True, capture_output=True, timeout=30)
    assert p.returncode == code, (args, p.returncode, p.stdout[-400:], p.stderr)
    return p.stdout


def read_formula(text):
    lines = text.splitlines()
    return int(lines[0].split()[2]), [list(map(int, line.split()[:-1])) for line in lines[1:]]


def dimacs(n, clauses):
    return f"p cnf {n} {len(clauses)}\n" + "".join(" ".join(map(str,[*row,0]))+"\n" for row in clauses)


def shuffled(text, rng):
    n, clauses = read_formula(text)
    names = list(range(1,n+1))
    rng.shuffle(names)
    rows = []
    for clause in clauses:
        row = [names[abs(lit)-1] * (1 if lit > 0 else -1) for lit in clause]
        rng.shuffle(row)
        rows.append(row)
    rng.shuffle(rows)
    return dimacs(n, rows)


def main():
    for file in sorted((ROOT/"instances").glob("php*.cnf")):
        size, encoding = file.stem[3:].split("-", 1)
        if encoding not in ("pairwise", "sequential"):
            continue
        p,h = size.split("x")
        assert run("--generate",p,h,encoding) == file.read_text()
    rng = random.Random(61031)
    for h in (8,30,64):
        text = run("--generate",str(h+1),str(h),"sequential")
        for _ in range(20):
            out = run(text=shuffled(text,rng),code=20)
            assert f"c demand {h+1} capacity {h}\n" in out
    for path in sorted((ROOT/"tests/fixtures").glob("*.cnf")):
        text = path.read_text()
        assert "c demand 9 capacity 8\n" in run(text=text,code=20)
        n, clauses = read_formula(text)
        assert "s SATISFIABLE" in run(text=dimacs(n,clauses[1:]),code=10)
    forced = "p cnf 3 4\n1 0\n-1 2 0\n-2 3 0\n-3 -1 0\n"
    assert "c forced-false demand clause 1" in run(text=forced,code=20)
    text = run("--generate","7","6","pairwise")
    for heuristic in ("fixed","activity","placebo"):
        out = run("--explain","--cdcl","--heuristic",heuristic,"--luby","2","--interval","1",text=text,code=20)
        for name in ("learned_clauses","restarts","db_reductions"):
            assert int(next(l.split()[2] for l in out.splitlines() if l.startswith(f"c {name} "))) > 0
    assert "s UNKNOWN" in run("--cdcl","--decisions","1",text=text)
    assert "s UNKNOWN" in run("--cdcl","--conflicts","1",text=text)
    assert "s UNSATISFIABLE" in run("--cdcl","--no-reduce","--no-restarts",text=text,code=20)
    trials=run("--benchmark","3")
    rows=list(csv.DictReader(StringIO(run("--report","-",text=trials))))
    assert len(rows)==6 and all(r["completed"]=="3" and r["scope"]=="solve" for r in rows)
    for label in ("reference","repeat","satsuma-v0.0.1","native-v0.0.2"):
        assert run("--report",ROOT/"results"/f"{label}.csv").count("\n")>2
    print("Passed generator parity, 60 shuffled sequential cases, alternate encodings, search budgets, reductions and benchmark reports.")


if __name__ == "__main__":
    main()
