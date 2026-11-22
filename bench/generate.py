"""Recreate the published DIMACS files in a directory of your choice."""
import argparse
from itertools import combinations
from pathlib import Path

from pysat.card import CardEnc, EncType
from pysat.formula import CNF, IDPool


def pigeonhole(pigeons, holes, sequential=False):
    formula = CNF()
    pool = IDPool(start_from=pigeons * holes + 1)
    rows = [list(range(1 + p * holes, 1 + (p + 1) * holes)) for p in range(pigeons)]
    for row in rows:
        formula.append(row)
    for hole in zip(*rows):
        if sequential:
            formula.extend(CardEnc.atmost(lits=list(hole), bound=1, vpool=pool,
                                          encoding=EncType.seqcounter))
        else:
            for first, second in combinations(hole, 2):
                formula.append([-first, -second])
    return formula


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    for pigeons, holes, encoding in [(9, 8, "pairwise"), (9, 8, "sequential"),
                                     (31, 30, "pairwise"), (65, 64, "pairwise"),
                                     (31, 30, "sequential"), (65, 64, "sequential")]:
        name = f"php{pigeons}x{holes}-{encoding}.cnf"
        formula = pigeonhole(pigeons, holes, sequential=encoding == "sequential")
        formula.to_file(str(args.directory / name))


if __name__ == "__main__":
    main()
