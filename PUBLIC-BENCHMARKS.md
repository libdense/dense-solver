# Public SAT benchmarks

dense-solver finished 28 of 30 public formulas. On 4 competition inputs it finished in milliseconds while both comparison solvers reached the 30-second limit. CaDiCaL and Kissat both solved HGEN, where dense-solver timed out. The sample consists of 22 classic SATLIB formulas and eight compact inputs from the 2025 SAT Competition main track.

| Solver | Solved out of 30 | Fastest out of 30 | Solved on 8 competition inputs | Mean PAR-2, seconds |
| --- | ---: | ---: | ---: | ---: |
| dense-solver 0.0.4 | 28 | 22 | 6 | 4.021 |
| Kissat 4.0.4 | 25 | 4 | 3 | 10.134 |
| CaDiCaL 3.0.0 | 25 | 3 | 3 | 10.035 |

Fastest means the lowest median end-to-end time among these three solvers. A row where everyone timed out has no winner. A timeout receives twice the deadline (60 seconds). The table averages those per-input median scores across the 30 formulas. On the smallest inputs, differences of a few milliseconds include process startup. This selected sample has its own local scores and deadlines.

## Individual results

Each time is the median of three fresh processes in milliseconds. The same prepared file was used by all three solvers. The manifest gives original filenames, source URLs, expected answers and SHA-256 hashes. [Raw trials](results/public-v0.0.4.csv) | [Input manifest](bench/public-inputs.csv)

### SAT Competition 2025

| Input | Answer | dense-solver | Kissat | CaDiCaL |
| --- | --- | ---: | ---: | ---: |
| sc25-subset-cardinality | UNSAT | timeout | timeout | timeout |
| sc25-fpga-routing | UNSAT | 3.14 | timeout | timeout |
| sc25-scheduling | SAT | 3.37 | 15.42 | 10.14 |
| sc25-hgen | SAT | timeout | 1,557.19 | 632.40 |
| sc25-tseitin-formulas | UNSAT | 2.37 | timeout | timeout |
| sc25-mutilated-chessboard | UNSAT | 3.24 | timeout | timeout |
| sc25-hamiltonian | SAT | 552.22 | 449.53 | 188.39 |
| sc25-pigeon-hole | UNSAT | 3.00 | timeout | timeout |

### SATLIB

| Input | Answer | dense-solver | Kissat | CaDiCaL |
| --- | --- | ---: | ---: | ---: |
| hole6 | UNSAT | 2.12 | 4.12 | 4.61 |
| hole7 | UNSAT | 2.27 | 15.76 | 9.18 |
| hole8 | UNSAT | 2.13 | 59.52 | 14.20 |
| hole9 | UNSAT | 2.41 | 157.20 | 25.32 |
| hole10 | UNSAT | 2.28 | 1,681.32 | 78.56 |
| uf50-01 | SAT | 2.18 | 3.80 | 2.74 |
| uf50-02 | SAT | 2.29 | 4.04 | 4.81 |
| uf50-03 | SAT | 2.44 | 2.67 | 5.74 |
| uf50-04 | SAT | 2.04 | 3.79 | 4.06 |
| uuf50-01 | UNSAT | 2.19 | 2.85 | 2.67 |
| uuf50-02 | UNSAT | 2.31 | 3.73 | 4.49 |
| uuf50-03 | UNSAT | 2.78 | 5.03 | 6.08 |
| uuf50-04 | UNSAT | 2.38 | 3.37 | 5.36 |
| flat30-1 | SAT | 1.35 | 1.22 | 1.98 |
| flat30-2 | SAT | 2.16 | 1.95 | 3.18 |
| flat30-3 | SAT | 2.67 | 1.76 | 3.10 |
| anomaly | SAT | 1.45 | 1.10 | 1.99 |
| medium | SAT | 2.98 | 2.20 | 1.91 |
| bw_large.a | SAT | 5.33 | 27.29 | 12.06 |
| par8-1-c | SAT | 2.64 | 3.99 | 5.64 |
| par8-2-c | SAT | 2.57 | 4.10 | 5.98 |
| par8-3-c | SAT | 2.06 | 3.77 | 4.71 |

## What was tested

We chose the competition inputs before timing. From RocSAT's list of 81 compact, known-answer family representatives, we took the eight smallest uncompressed files. We verified all eight database identifiers against the 2025 main-track list. The SATLIB selection includes all five DIMACS pigeonhole inputs, instances 1 through 4 of UF50 and UUF50, the first three flat30 graphs, three Blocks World instances, and the first three compact par8 parity formulas. The size-based selection favors small formulas. [SATLIB](https://www.cs.ubc.ca/~hoos/SATLIB/benchm.html) | [Competition inputs](https://benchmark-database.de/?context=cnf&track=main_2025) | [Selection sources](SOURCES.md)

On November 22, 2026, all three native solvers ran on an AMD EPYC 9V74 host under Debian 13 x86-64. dense-solver used GCC 14.2.0 with `-O3`. Kissat and CaDiCaL used their upstream release configurations. The runner assigned each input to one logical CPU. Up to eight different inputs ran concurrently on distinct logical CPUs. All solvers stayed on the same CPU for a given input. A seeded shuffle chose the initial solver order for each input, then the order rotated across its three trials. All three attempts ran even after a timeout.

The clock ran from process launch through exit, including parsing and output. Kissat and CaDiCaL used `-q`. Each solver ran single-threaded with proof output off. The runner checked every SAT assignment against the timed input, outside the clock. Every completed verdict agreed with the public reference answer. As a separate test, the independent checker verified dense-solver's counting certificates on FPGA routing (22 demands, 20 groups), chessboard (200, 198) and pigeonhole (16, 15). The Tseitin formula was settled by parity elimination.

The measured Linux executables had the following SHA-256 digests. The source releases are linked below.

| Solver | Executable SHA-256 |
| --- | --- |
| dense-solver 0.0.4 | `d496d59bb200a16a024775136bca14f1afb7970943a86a080e1d9073a76aa74a` |
| Kissat 4.0.4 | `6f63b89f443c0e8f6aa3457b305cb4e609eaabe30cbc3ceb1aabe060730cf8c3` |
| CaDiCaL 3.0.0 | `2c0fcc70401fd6004fa021b67bbd50530d4e6617581f176b58e7abfed9daea03` |

## Reproduce

The 30 timed CNFs are included and the runner verifies each checksum. On the eight SATLIB random formulas, the legacy trailing percent line and zero were removed so all parsers read the same DIMACS bytes. The other 22 timed files match the upstream CNF bytes after decompression.

```sh
make -j
python3 bench/public_suite.py --kissat /path/to/kissat --cadical /path/to/cadical --jobs 8 --output build/public-trials.csv
python3 bench/public_scores.py build/public-trials.csv
```

Use `--jobs 1` for a sequential run, or omit external solver options to run dense-solver alone. To view the recorded results immediately, run `python3 bench/public_scores.py results/public-v0.0.4.csv`. The optional local web workbench also opens this dataset in its Benchmarks view. To independently retrieve the originals, run `python3 bench/fetch_public.py build/refetched` and compare that directory with `bench/public-instances`. [Solver releases and input credits](SOURCES.md)

## Frobenius comparison

Hutchison's posted 30-row table used a separate set of formulas. We could not locate those original inputs, the circuit-to-CNF export or the precise timing boundary. He also reported testing the complete 2024-2026 competition sets; his per-instance times for these eight 2025 inputs were unavailable to us. [Original table](https://x.com/LH/status/2124247270824812710) | [Competition-set report](https://x.com/LH/status/2124411466619240752)

The runner accepts another native solver via `--solver LABEL EXECUTABLE`. The executable or adapter must accept one DIMACS path, print the standard SAT verdict and model, and return 10 for SAT or 20 for UNSAT. To run a different corpus, pass `--manifest FILE --inputs DIRECTORY`. Use the bundled manifest as a template and place the exact CNFs at `DIRECTORY/ID.cnf`. For Hutchison's circuit inputs, obtain the original circuit and the CNF export used in his comparison before attributing a timing to his table.
