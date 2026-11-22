"""Check a dense-solver counting certificate without trusting the solver.

Usage: dense-solver --explain FORMULA.cnf | python3 tests/check_certificate.py FORMULA.cnf

The certificate lists input clauses and at-most-one groups of literals.
This checker parses the formula itself and runs its own unit propagation
(UP). It accepts when:

  1. after dropping literals that UP from the formula makes false, no
     literal appears in two listed clauses;
  2. every remaining literal of a listed clause is in some group, or is
     listed as refuted and UP from the formula plus that literal reaches
     a conflict;
  3. for every pair a, b in a group, b is -a, or UP from the formula
     plus a makes b false or conflicts, or the same with a and b swapped;
  4. there are more listed clauses than groups.

Each listed clause then needs its own true literal, no group holds two,
and so no assignment satisfies the formula.
"""
from collections import defaultdict
from pathlib import Path
import sys


class CertificateError(ValueError):
    pass


def parse_dimacs(text):
    variables = None
    clauses = []
    current = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("c"):
            continue
        if line.startswith("p"):
            fields = line.split()
            if variables is not None or clauses or current or len(fields) != 4 or fields[:2] != ["p", "cnf"]:
                raise CertificateError(f"bad header: {line}")
            variables, declared = map(int, fields[2:])
            if variables < 0 or declared < 0:
                raise CertificateError("negative header count")
            continue
        for token in line.split():
            literal = int(token)
            if variables is None or abs(literal) > variables:
                raise CertificateError("literal outside the declared formula")
            if literal == 0:
                clauses.append(current)
                current = []
            else:
                current.append(literal)
    if current:
        raise CertificateError("last clause is not terminated")
    if variables is None:
        raise CertificateError("missing header")
    if len(clauses) != declared:
        raise CertificateError("clause count differs from the header")
    return variables, clauses


def parse_certificate(text):
    clauses, groups, refuted = [], [], []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 3 or fields[0] != "c" or fields[1] != "certificate":
            continue
        kind, values = fields[2], [int(value) for value in fields[3:]]
        if kind == "clause":
            if len(values) != 1:
                raise CertificateError(f"bad clause line: {line}")
            clauses.append(values[0])
        elif kind in ("group", "refuted"):
            if not values or values[-1] != 0 or 0 in values[:-1]:
                raise CertificateError(f"bad {kind} line: {line}")
            (groups.append if kind == "group" else refuted.extend)(values[:-1])
        else:
            raise CertificateError(f"unknown certificate line: {line}")
    return clauses, groups, refuted


class Propagator:
    """Plain occurrence-list unit propagation, independent of the C code."""

    def __init__(self, clauses):
        self.clauses = [sorted(set(clause)) for clause in clauses]
        self.occurrences = defaultdict(list)
        for index, clause in enumerate(self.clauses):
            for literal in clause:
                self.occurrences[literal].append(index)
        self.root = {}
        self.root_conflict = not self._propagate(self.root, [
            clause[0] for clause in self.clauses if len(clause) == 1
        ]) or any(not clause for clause in self.clauses)
        self.cache = {}

    def _propagate(self, values, assumptions):
        """Extend values (literal -> True for true literals). False on conflict."""
        queue = []
        for literal in assumptions:
            if values.get(-literal):
                return False
            if not values.get(literal):
                values[literal] = True
                queue.append(literal)
        while queue:
            falsified = -queue.pop()
            for index in self.occurrences[falsified]:
                unassigned = None
                open_count = 0
                for literal in self.clauses[index]:
                    if values.get(literal):
                        break
                    if not values.get(-literal):
                        open_count += 1
                        unassigned = literal
                else:
                    if open_count == 0:
                        return False
                    if open_count == 1:
                        values[unassigned] = True
                        queue.append(unassigned)
        return True

    def probe(self, literal):
        """(conflict, values) for UP from the formula plus literal."""
        if literal not in self.cache:
            values = dict(self.root)
            ok = self._propagate(values, [literal])
            self.cache[literal] = (not ok, values)
        return self.cache[literal]

    def excludes(self, first, second):
        """True when UP shows first and second cannot both be true."""
        if first == -second:
            return True
        conflict, values = self.probe(first)
        return conflict or bool(values.get(-second))


def check(formula_text, certificate_text):
    _, clauses = parse_dimacs(formula_text)
    listed, groups, refuted = parse_certificate(certificate_text)
    if not listed:
        raise CertificateError("no certificate lines")
    propagator = Propagator(clauses)
    if propagator.root_conflict:
        return f"root propagation refutes the formula ({len(listed)} clauses listed)"
    if len(set(listed)) != len(listed):
        raise CertificateError("a clause is listed twice")
    if len(listed) <= len(groups):
        raise CertificateError(f"{len(listed)} clauses do not exceed {len(groups)} groups")
    for literal in refuted:
        if not propagator.probe(literal)[0]:
            raise CertificateError(f"refuted literal {literal} does not reach a conflict")
    grouped = set()
    for group in groups:
        grouped.update(group)
        for i, first in enumerate(group):
            for second in group[i + 1:]:
                if first == second:
                    raise CertificateError(f"literal {first} repeated in a group")
                if not (propagator.excludes(first, second)
                        or propagator.excludes(second, first)):
                    raise CertificateError(f"group does not exclude {first} and {second}")
    seen = set()
    refuted_set = set(refuted)
    for number in listed:
        if not 1 <= number <= len(clauses):
            raise CertificateError(f"clause {number} does not exist")
        live = {lit for lit in clauses[number - 1] if not propagator.root.get(-lit) and lit not in refuted_set}
        if live & seen:
            raise CertificateError(f"clause {number} shares a literal with another")
        seen |= live
        missing = live - grouped - refuted_set
        if missing:
            raise CertificateError(f"clause {number} has ungrouped literals {sorted(missing)}")
    return f"{len(listed)} clauses, {len(groups)} groups, {len(refuted)} refuted"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    try:
        summary = check(Path(sys.argv[1]).read_text(), sys.stdin.read())
    except ValueError as error:
        sys.exit(f"Certificate rejected: {error}")
    print(f"Certificate verified: {summary}")


if __name__ == "__main__":
    main()
