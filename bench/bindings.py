"""Time one solve on a benchmark CNF already loaded into a native solver."""
import sys
from time import perf_counter_ns

from pysat.formula import CNF


def main():
    name = sys.argv[1]
    clauses = CNF(from_fp=sys.stdin).clauses
    if name == "cryptominisat":
        from pycryptosat import Solver
        solver = Solver(threads=1)
        for clause in clauses:
            solver.add_clause(clause)
        start = perf_counter_ns()
        answer, _ = solver.solve()
        elapsed = perf_counter_ns() - start
    else:
        from pysat.solvers import Solver
        backend = {"kissat": "kissat404", "cadical": "cadical300", "minisat": "minisat22"}[name]
        with Solver(name=backend, bootstrap_with=clauses) as solver:
            start = perf_counter_ns()
            answer = solver.solve()
            elapsed = perf_counter_ns() - start
    verdict = {True: "SAT", False: "UNSAT", None: "UNKNOWN"}[answer]
    print(f"{verdict} {name} {elapsed}")
    return {True: 10, False: 20, None: 0}[answer]


if __name__ == "__main__":
    sys.exit(main())
