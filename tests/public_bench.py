"""Exercise the public comparison, including checked models, deadlines and file identity."""
import csv
from io import StringIO
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bench"))
from public_suite import DEFAULT_MANIFEST, load_formula, read_manifest, validate_output
from public_scores import score


def rejected(function, *args):
    try:
        function(*args)
    except ValueError:
        return
    raise AssertionError("Expected invalid input to be rejected")


def main():
    records = read_manifest(DEFAULT_MANIFEST)
    assert len(records) == 30
    for r in records:
        load_formula(ROOT / "bench/public-instances" / (r["id"] + ".cnf"), r)
    sat = next(r for r in records if r["id"] == "uf50-01")
    rejected(validate_output, 10, b"s SATISFIABLE\nv 1 -1 0\n", sat, [[1]])
    rejected(validate_output, 10, b"s SATISFIABLE\nv -1 0\n", sat, [[1]])
    rejected(validate_output, 20, b"s UNSATISFIABLE\n", sat, [[1]])
    with TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        manifest = tmp / "one.csv"
        with manifest.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, sat)
            writer.writeheader()
            writer.writerow(sat)
        for limit in ("30", "0.000001"):
            out = tmp / f"run-{limit}.csv"
            cmd = [sys.executable, ROOT / "bench/public_suite.py", "--manifest", manifest,
                   "--timeout", limit, "--output", out]
            subprocess.run(cmd, check=True, capture_output=True)
            with out.open() as f:
                rows = list(csv.DictReader(f))
            assert len(rows) == 3
            solvers, scores, _ = score(rows, [sat])
            result = scores[sat["id"], solvers[0]]
            if limit != "30":
                assert result == 2 * float(limit)
            else:
                assert 0 < result < 30
            rejected(score, rows[:-1] + [rows[0]], [sat])
            assert subprocess.run(cmd, capture_output=True).returncode != 0
    raw = ("instance,scope,solver,trial,verdict,time_ns,timeout_s\n"
           "case,process,demo,1,SAT,1000000,1\ncase,process,demo,2,SAT,2000000,1\n"
           "case,process,demo,3,TIMEOUT,,1\n")
    report = subprocess.check_output([ROOT / "build/dense-solver", "--report", "-"], input=raw, text=True)
    assert next(csv.DictReader(StringIO(report)))["median_ms"] == "2.000000"
    for name in ("sc25-fpga-routing", "sc25-mutilated-chessboard", "sc25-pigeon-hole"):
        path = ROOT / "bench/public-instances" / (name + ".cnf")
        cert = subprocess.run([ROOT / "build/dense-solver", "--explain", path], capture_output=True, timeout=15)
        assert cert.returncode == 20 and b"c method hall" in cert.stdout
        subprocess.run([sys.executable, ROOT / "tests/check_certificate.py", path],
                       input=cert.stdout, capture_output=True, check=True)
    print("Passed 30 input hashes, three public Hall certificates, runner deadlines, SAT model checks and scoring.")


if __name__ == "__main__":
    main()
