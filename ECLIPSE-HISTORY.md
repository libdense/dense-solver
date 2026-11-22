# From Eclipse to dense-solver

Merlin Numa initiated and directed Eclipse through successive research prompts. The project's roadmap credits Appendix C of Numa's book with the idea of measuring which work a solver can avoid. For pigeonhole formulas, that led to a concrete question. Could the solver read how many places were available and how many were required, and settle the formula before trying assignments? Eclipse adopted deterministic runs from libdense and comparison controls from babelsuite. The roadmap records these influences in lines 1175-1186 and 1229-1246 of `ECLIPSE_ROADMAP.md`.

## The dated record

| Date | Milestone | Primary record |
| --- | --- | --- |
| August 19, 2026 | Eclipse v0.0.1 measured search on pigeonhole formulas. v0.0.2 added clause learning. After testing it, v0.0.3 named counting as the next pigeonhole experiment. | `CHANGELOG.md`, entries 0.0.1-0.0.3. The respective phase test reports record the dates. |
| August 19, 2026 | Eclipse v0.0.4 added `ec_reason_count`. It grouped placements that cannot coexist and counted the separate requirements those groups must satisfy. On 31 pigeons and 30 holes it found demand 31 and capacity 30. Three relaxed, satisfiable controls went through search and returned valid placements. | `CHANGELOG.md`, lines 1031-1036 and 1044-1052. `PHASE_0.0.4_TEST_REPORT.txt`. Experiments 09 and 10. |
| November 8, 2026 | Eclipse v0.0.61 reported independent Lean kernel checks of the original pigeonhole verdicts for one through five holes. Counting also settled the six-hole case, where the independent refuter ran out of memory. | `PHASE_0.0.61_REPORT.md`, section *Result*. |
| November 22, 2026 | dense-solver v0.0.1 extracted the C reasoner from the v0.0.61 snapshot. v0.0.2 incorporated a prototype's extensions for implied conflicts and sequential counters and measured the new C solver against satsuma-iter+kissat. | [Source credits](SOURCES.md) and [release benchmarks](BENCHMARK.md). |

## Two changelog excerpts

**The plan.** Eclipse v0.0.3, `CHANGELOG.md`, lines 1089-1092.

> - Advances Phase 3 to the stronger-reasoning gate: XOR extraction
>   with Gaussian elimination aimed at Tseitin, and counting-aware
>   reasoning aimed at the pigeonhole wall, both required to pass the
>   falsification protocol before any claim is recorded.

**The implementation.** Eclipse v0.0.4, `CHANGELOG.md`, lines 1031-1034.

> - Adds ec_reason_count: greedy at-most-one clique partition of the
>   negative-binary conflict graph plus variable-disjoint all-positive
>   demand selection; demand exceeding touched-group capacity proves
>   UNSAT by the counting argument, with no search.

## Reading the original timing

The v0.0.4 changelog gives **309 microseconds** for the 31/30 run. The archived experiment CSV and the dated validation report give **200 microseconds**. The available records establish the same count and verdict. The reason for the timing difference is undocumented.

`results/exp09_php_falls.csv`, columns `solver,holes,variables,clauses,verdict,ops,demand,capacity,seconds`:

```csv
count,30,930,13981,UNSAT,56793,31,30,0.000200
```

The November comparison has its own inputs, timing protocol and raw trials. The published literature on cardinality reasoning predates Eclipse. Haken's 1985 resolution lower bound is cited in the original changelog. Biere and colleagues described detecting cardinality constraints in CNF in 2014. [Haken](https://doi.org/10.1016/0304-3975%2885%2990144-6) | [Biere et al.](https://doi.org/10.1007/978-3-319-09284-3_22)

## Source identification

**Merlin Numa. *Eclipse*, v0.0.61. Project source archive, November 8, 2026.** Paths and line numbers above are relative to `eclipse-0.0.61/` in the author's retained ZIP. The dates are those recorded by the project. The SHA-256 digests identify the exact files used for this history.

| File | SHA-256 |
| --- | --- |
| Eclipse v0.0.61 source ZIP | `bdb694f0123f094eb6c46db92bc8714e1dc11e67e811a0e97ab78749e62e4d83` |
| `CHANGELOG.md` inside that ZIP | `b25e169a04381f54369f5bc46f268305ba0123106a692c95939f3f46048c2379` |

Numa directed the work through the most capable ChatGPT and Claude models available through the author's paid subscriptions during the August-November 2026 development sessions.
