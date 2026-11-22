# Counting and matching benchmarks

The v0.0.4 comparison uses 30 public formulas with known answers. [Public benchmark results](PUBLIC-BENCHMARKS.md)

## Version 0.0.3

The release was tested on 20 DIMACS inputs. The sparse counter or Hall matching settled all 18 unsatisfiable inputs before search. The solver returned valid assignments on both satisfiable controls. The table shows median **end-to-end milliseconds** on selected inputs. [Raw trials for all 20](results/native-v0.0.3.csv)

| Input | dense-solver v0.0.3 | v0.0.3 with Hall off | dense-solver v0.0.2 | satsuma-iter+kissat |
| --- | ---: | ---: | ---: | ---: |
| 65 pigeons / 64 holes, sequential | 2.83 | 2.76 | 2.90 | 123.78 |
| 9 pigeons / 8 holes, totalizer | 2.36 | 175.09 | 172.14 | 6.09 |
| 9 pigeons / 8 holes, renamed sequential | 2.34 | 248.72 | 244.61 | 6.16 |
| 12-by-12 chessboard | 2.36 | 1,072.15 | 1,099.48 | 420.83 |
| 32-by-32 chessboard | 3.64 | >5,000 | >5,000 | >5,000 |
| 8 pigeons / 8 holes, sequential, SAT | 1.21 | 1.12 | 1.14 | 3.85 |

On the original six pigeonhole inputs, the sparse counter still settled every case. Matching made the largest difference on the boards and several alternate encodings. On the two satisfiable controls, enabling Hall added 0.05 ms and 0.13 ms to median solve time. These measurements cover structured inputs on one machine.

Each completed cell contains seven fresh processes pinned to one logical CPU of an AMD EPYC 9V74 on Debian 13 x86-64, November 22, 2026. The dense-solver builds used GCC 14.2.0 and `-O3`. Run order was shuffled within each trial. Each solver received the same input file, with proof output off. The end-to-end clock includes startup, parsing and exit. For satsuma-iter+kissat it also includes both executables and the intermediate file. A five-second process timeout ended further trials for that solver and input. The CSV also gives dense-solver's internal solve time after parsing. [Competitor source and checksum](SOURCES.md)

```sh
make
python3 bench/head_to_head.py --all-inputs --no-hall-control --previous /path/to/v0.0.2/build/dense-solver --satsuma-src /path/to/satsuma-iter-kissat/src --timeout 5 --output build/comparison.csv
./build/dense-solver --report build/comparison.csv
```

Omit `--previous` or `--satsuma-src` to run with the binaries you have. Choose a new output filename for each run.

### The 368-by-368 chessboard

Remove the two opposite corners from an even board and there are two more squares of one color than the other. Each domino covers one square of each color. The solver receives only the placement clauses, and finds the shortage through matching. On this input with 270,108 variables and 943,544 clauses, its certificate identifies 67,712 requirements competing for 67,710 groups. [Compressed certificate](results/chessboard368-certificate.txt.gz) | [Raw trials](results/chessboard368-timings.csv)

| Operation | Seconds | Runs |
| --- | ---: | ---: |
| Solve after parsing | 0.473 | 7, median |
| Whole process, verdict only | 0.531 | 7, median |
| Whole process, writing the explanation and certificate | 0.524 | 7, median |
| Independently check the saved certificate | 5.067 | 1 |

Verdict and certificate timings came from separate invocations on the same logical CPU. All seven certificate runs emitted the same certificate. This board was measured with dense-solver only. The independent checker verifies the project's counting certificate format.

```sh
make
python3 bench/chessboard.py 368 build/chessboard368.cnf
gzip -dc results/chessboard368-certificate.txt.gz | python3 tests/check_certificate.py build/chessboard368.cnf
python3 bench/board_times.py build/chessboard368.cnf build/board-run
```

The generated DIMACS SHA-256 is `df2414720063d6ab260af693aab6bcfe46134c96190695ec6d265d11976dfb5c`. The uncompressed certificate SHA-256 is `c7655674a21696881a02aa86b592f207bb8489c03002f07696575466faf42422`.

### Correctness

`make test` checks 2,000 small formulas against truth tables in both solver modes. It also checks the standalone C API, 300 matching cases, 293 brute-force controls, shuffled and signed formulas, and acceptance or rejection of counting certificates. In a separate check, CaDiCaL 3.0.0 agreed on 1,000 formulas in both modes (654 SAT, 346 UNSAT). The optional comparison dependencies are listed in `bench/requirements.txt`. Run `python3 tests/crosscheck.py` to repeat.

## Historical v0.0.2 benchmark

All six inputs returned UNSAT by counting. Each row gives median milliseconds on the same DIMACS file. Process time includes launching each executable, parsing, solving and exit. [Individual trials](results/native-v0.0.2.csv)

| Pigeons / holes | Encoding | v0.0.2 solve only | v0.0.1 process | v0.0.2 process | satsuma-iter+kissat process |
| --- | --- | ---: | ---: | ---: | ---: |
| 9 / 8 | Pairwise | 0.0062 | 1.97 | 1.87 | 5.14 |
| 9 / 8 | Sequential | 0.0094 | 1,032.30 | 2.24 | 5.77 |
| 31 / 30 | Pairwise | 0.164 | 4.00 | 2.83 | 17.74 |
| 31 / 30 | Sequential | 0.080 | >5,000 | 2.43 | 18.60 |
| 65 / 64 | Pairwise | 1.414 | 28.00 | 10.28 | 164.83 |
| 65 / 64 | Sequential | 0.326 | >5,000 | 2.87 | 114.63 |

Seven fresh runs per completed cell on one logical CPU of an AMD EPYC 9V74, Debian 13 x86-64, GCC 14.2.0, `-O3`, November 22, 2026. Run order was shuffled deterministically. A five-second timeout stopped further trials for that solver on that input. dense-solver used its native measurement mode. The competitor used the submission's satsuma preprocessing flags followed by its bundled Kissat binary. The process clock includes both stages and the intermediate file. Both solvers ran with proof logging off. Sub-millisecond solve times depend on the CPU, build and timer.

Run the following historical comparison from a v0.0.2 checkout. The original C release and the competition software are optional inputs to the runner. Set the paths below to their local build locations. The source archive and checksum for the competitor are listed in [SOURCES.md](SOURCES.md).

```sh
make
python3 bench/head_to_head.py --output build/native.csv
python3 bench/head_to_head.py --previous /path/to/v0.0.1/build/dense-solver --satsuma-src /path/to/satsuma-iter-kissat/src --output build/comparison.csv
./build/dense-solver --report build/comparison.csv
```

For a quick measurement within a running process, use `./build/dense-solver --benchmark 7`. Its clock includes the solve call after the formula has been generated. The browser's live benchmark uses the same routine. The separate historic proof-on and proof-off trials are labeled as full-process results. [Historical satsuma trials](results/satsuma-v0.0.1.csv)

## Earlier v0.0.1 release


Every completed run returned UNSAT. Each comparison within a row uses the same DIMACS file for every solver. The reference run and a repeat with v0.0.1 are available as individual observations in nanoseconds. [Reference trials](results/reference.csv) | [Release repeat](results/repeat.csv)

### Results

| Solver | 9 pigeons, 8 holes, pairwise | 9 pigeons, 8 holes, sequential |
| --- | ---: | ---: |
| dense-solver v0.0.1 | **0.030096 ms** | 1,103.50 ms |
| Lingeling 1.0.0 | 0.088213 ms | **17.76 ms** |
| Kissat 4.0.4 | 77.87 ms | 19.65 ms |
| MiniSat 2.2 | 118.38 ms | |
| CaDiCaL 3.0.0 | 323.04 ms | |
| CryptoMiniSat 5.18.0 | 406.07 ms | |
| dense-solver, CDCL search alone | 708.02 ms | |

Empty cells were unmeasured. The pairwise input has 72 variables and 297 clauses. The sequential counter uses 136 variables and 193 clauses. Both express the same placement rules. The v0.0.1 repeat returned 0.030246 ms for dense-solver on the pairwise input and 1,060.59 ms on the sequential input.

| Larger pairwise input | Variables | Clauses | dense-solver | Lingeling 1.0.0 |
| --- | ---: | ---: | ---: | ---: |
| 31 pigeons, 30 holes | 930 | 13,981 | 1.194 ms | 3.924 ms |
| 65 pigeons, 64 holes | 4,160 | 133,185 | 14.894 ms | 5 s process limit reached |

### Timing

Each trial started a fresh process pinned to one logical CPU of an AMD EPYC 9V74. The reference used Debian 13 x86-64, GCC 14.2.0 with `-O3` for dense-solver and upstream Lingeling, and Python 3.12.14. The other solvers ran through version-pinned binary wheels. CryptoMiniSat used one thread. The tests requested verdicts without proof logging. [Build details and upstream sources](SOURCES.md)

The solve clock starts after the clauses are loaded into each solver and includes any preprocessing inside its solve call. The tables give the median of seven trials on the 9-by-8 pairwise input and three on each other input. A whole-process deadline stopped further trials for that solver and input after the first timeout. The raw CSV leaves its time blank.

A separate measurement includes launching the native harness, parsing the file, solving and exiting. Across 101 runs per solver on the 9-by-8 pairwise input, the medians were **0.703 ms for dense-solver** and **0.951 ms for Lingeling**. The parent opened the input before starting this clock. The short solve-only result is sensitive to machine and build.

### Reproduce

```sh
make
python3 bench/report.py results/reference.csv
python3 bench/run.py --output build/local.csv
```

Run the commands in a v0.0.1 checkout for the historical comparison. The runner in v0.0.2 expects sequential inputs to return by counting. The last command runs dense-solver alone. To run every recorded comparison, use Python 3.12 on x86-64 Linux, with a C compiler, GNU Make, curl, tar and sha256sum. The setup downloads Lingeling's source and the Python packages.

```sh
python3.12 -m venv .venv
.venv/bin/python -m pip install --only-binary=:all: -r bench/requirements.txt
sh bench/build-lingeling.sh
.venv/bin/python bench/run.py --all --output build/comparison.csv
.venv/bin/python bench/report.py build/comparison.csv
```

The runner checks the inputs and writes a new CSV. To regenerate the formulas independently of the solver and compare every byte, run the commands below.

```sh
.venv/bin/python bench/generate.py build/regenerated
for file in instances/*.cnf; do cmp "$file" "build/regenerated/${file##*/}" || exit 1; done
```

## Correctness checks for v0.0.2

The native suite checks 2,000 small formulas against complete truth tables in both reasoned and CDCL-only modes. The integration tests also exercise shuffled sequential encodings, bitwise and ladder fixtures, satisfiable controls, forced-false demands, search budgets, restarts and database reduction.

An independent cross-check against CaDiCaL 3.0.0 agreed on all 1,000 generated formulas (654 SAT, 346 UNSAT) in both modes. Each SAT assignment was evaluated again in Python. With the optional comparison requirements installed, run `python3 tests/crosscheck.py`. To exercise the local browser endpoints after `make web`, run `python3 tests/web.py`.
