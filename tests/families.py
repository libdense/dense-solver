"""Pure-Python CNF families for the tests: pigeonhole variants and the
mutilated chessboard. No third-party packages.

Each at-most-one encoding takes a list of literals and a fresh-variable
allocator and returns clauses. All are standard:

  pairwise     every pair (-a or -b)
  sequential   Sinz (2005) sequential counter, one direction
  counter      Sinz counter with both directions, s_i <-> (s_(i-1) or x_i);
               its extra clauses contain the input literals positively
  bitwise      Frisch et al. binary encoding, one bit pattern per literal
  commander    Klieber and Kwon, groups of three, pairwise inside and
               across commanders
  product      Chen (2010) two-dimensional product encoding
"""
from itertools import combinations
import math
import random


class Fresh:
    def __init__(self, first):
        self.next = first

    def __call__(self):
        value = self.next
        self.next += 1
        return value


def amo_pairwise(literals, fresh):
    return [[-a, -b] for a, b in combinations(literals, 2)]


def amo_sequential(literals, fresh):
    n = len(literals)
    if n <= 1:
        return []
    s = [fresh() for _ in range(n - 1)]
    clauses = [[-literals[0], s[0]]]
    for i in range(1, n - 1):
        clauses += [[-literals[i], s[i]], [-s[i - 1], s[i]], [-literals[i], -s[i - 1]]]
    clauses.append([-literals[-1], -s[-1]])
    return clauses


def amo_counter(literals, fresh):
    n = len(literals)
    if n <= 1:
        return []
    s = [fresh() for _ in range(n - 1)]
    clauses = [[-literals[0], s[0]], [-s[0], literals[0]]]
    for i in range(1, n - 1):
        clauses += [[-literals[i], s[i]], [-s[i - 1], s[i]], [-literals[i], -s[i - 1]],
                    [-s[i], s[i - 1], literals[i]]]
    clauses.append([-literals[-1], -s[-1]])
    return clauses


def amo_bitwise(literals, fresh):
    n = len(literals)
    if n <= 1:
        return []
    bits = [fresh() for _ in range(max(1, math.ceil(math.log2(n))))]
    clauses = []
    for index, literal in enumerate(literals):
        for position, bit in enumerate(bits):
            clauses.append([-literal, bit if (index >> position) & 1 else -bit])
    return clauses


def amo_commander(literals, fresh):
    if len(literals) <= 3:
        return amo_pairwise(literals, fresh)
    clauses, commanders = [], []
    for start in range(0, len(literals), 3):
        group = literals[start:start + 3]
        commander = fresh()
        commanders.append(commander)
        clauses += amo_pairwise(group, fresh)
        clauses += [[-literal, commander] for literal in group]
    return clauses + amo_commander(commanders, fresh)


def amo_product(literals, fresh):
    n = len(literals)
    if n <= 4:
        return amo_pairwise(literals, fresh)
    rows = math.ceil(math.sqrt(n))
    columns = math.ceil(n / rows)
    u = [fresh() for _ in range(rows)]
    v = [fresh() for _ in range(columns)]
    clauses = []
    for index, literal in enumerate(literals):
        clauses += [[-literal, u[index // columns]], [-literal, v[index % columns]]]
    return clauses + amo_product(u, fresh) + amo_product(v, fresh)


ENCODINGS = {
    "pairwise": amo_pairwise,
    "sequential": amo_sequential,
    "counter": amo_counter,
    "bitwise": amo_bitwise,
    "commander": amo_commander,
    "product": amo_product,
}


def pigeonhole(pigeons, holes, encoding="pairwise", functional=False):
    """Every pigeon in some hole, at most one pigeon per hole; functional
    also allows at most one hole per pigeon (pairwise)."""
    fresh = Fresh(pigeons * holes + 1)
    rows = [list(range(1 + p * holes, 1 + (p + 1) * holes)) for p in range(pigeons)]
    clauses = [list(row) for row in rows]
    for hole in zip(*rows):
        clauses += ENCODINGS[encoding](list(hole), fresh)
    if functional:
        for row in rows:
            clauses += amo_pairwise(row, fresh)
    return fresh.next - 1, clauses


def chessboard_cells(size):
    """Domino placement variables incident on each remaining square, in input order."""
    cells = [[] for _ in range(size * size)]
    last = len(cells) - 1
    variable = 0
    for square in range(1, last):
        neighbors = []
        if square + size < last:
            neighbors.append(square + size)
        if square % size != size - 1 and square + 1 < last:
            neighbors.append(square + 1)
        for other in neighbors:
            variable += 1
            cells[square].append(variable)
            cells[other].append(variable)
    return variable, cells[1:last]


def chessboard_clauses(cells):
    for literals in cells:
        yield literals
        for a, b in combinations(literals, 2):
            yield [-a, -b]


def mutilated_chessboard(size):
    variables, cells = chessboard_cells(size)
    return variables, list(chessboard_clauses(cells))


def rename(variables, clauses, seed, flip=True, shuffle=True):
    """Same formula under a random variable permutation, polarity flips,
    clause order and literal order. Satisfiability is unchanged."""
    rng = random.Random(seed)
    permutation = list(range(1, variables + 1))
    if shuffle:
        rng.shuffle(permutation)
    sign = {v: (-1 if flip and rng.getrandbits(1) else 1) for v in range(1, variables + 1)}
    renamed = []
    for clause in clauses:
        mapped = [sign[abs(lit)] * (1 if lit > 0 else -1) * permutation[abs(lit) - 1]
                  for lit in clause]
        if shuffle:
            rng.shuffle(mapped)
        renamed.append(mapped)
    if shuffle:
        rng.shuffle(renamed)
    return variables, renamed


def dimacs(variables, clauses):
    rows = [f"p cnf {variables} {len(clauses)}"]
    rows.extend(" ".join(map(str, [*clause, 0])) for clause in clauses)
    return "\n".join(rows) + "\n"
