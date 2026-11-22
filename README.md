# dense-solver

A C11 SAT solver by Merlin Numa. It can recognize when several independent requirements compete for too few slots. On the included pigeonhole inputs it reads the constraints, counts the available slots and returns UNSAT without branching. Version 0.0.2 recognizes both explicit conflicts and conflicts expressed through chains of binary clauses.

On the 65-pigeon, 64-hole sequential input, dense-solver finished in 2.87 ms and satsuma-iter+kissat finished in 114.63 ms. Both figures include startup, parsing and solving, with proof logging off. The experiment covers six pigeonhole formulas on one machine. [Benchmark methods and raw trials](BENCHMARK.md)

## Build and run

```sh
make -j
make test
./build/dense-solver instances/php9x8-sequential.cnf
./build/dense-solver --explain instances/php9x8-sequential.cnf
./build/dense-solver --generate 31 30 sequential > example.cnf
./build/dense-solver --benchmark 7 > trials.csv
./build/dense-solver --report trials.csv
```

The default build needs a C compiler and GNU Make on Linux. It produces the command-line solver and a static library. The native verification suite runs with `./build/dense-solver --verify`. The additional integration checks in `make test` use Python 3.

The solver accepts DIMACS on standard input or from a file. It tries parity elimination, then counting, then conflict-driven clause learning (CDCL). Every SAT verdict from the CLI or browser includes a model checked against the input. UNSAT output gives the method used. The release does not emit a DRAT or LRAT proof. Exit codes are 10 for SAT, 20 for UNSAT, 0 for UNKNOWN, and 1 for errors. Use `--help` for search controls.

## Use the counting rule in another solver

Link `build/libdense-solver.a` and include `dense_solver/reason.h`. Pass an initialized, loaded `ds_cnf` to `ds_reason_count`. On success the verdict is UNSAT or UNKNOWN. For UNKNOWN, continue with the host solver. The function borrows the input, works without global state, and can run independently of the other dense-solver stages.

## Local browser workbench

```sh
make web
./build/dense-solver-web
```

Open `http://127.0.0.1:8080/` from the same computer. The optional web build needs libevent 2 development headers and pkg-config (`libevent-dev` and `pkg-config` on Debian or Devuan). Run it from the project root to read the recorded benchmarks. Pass a port number to use another port. Each process holds one working formula in memory.

Load, edit, generate or download a DIMACS file. The Structure view shows the extracted groups, selected demands, parity equations and search counters. Benchmarks shows individual datasets, clocks and medians and can run fresh native trials. Verification runs the same C checks as the CLI. All application logic runs in C. The browser displays HTML and CSS.

## Credits

dense-solver grew out of the Eclipse research project initiated and directed by Merlin Numa. Eclipse v0.0.4 recorded its first pigeonhole counting implementation on August 19, 2026. [Eclipse project history](ECLIPSE-HISTORY.md)

Numa directed this project using the most capable OpenAI ChatGPT and Anthropic Claude models available through the author's paid subscriptions during the August to November 2026 development sessions. The November 22 native port incorporates the changes from the TypeScript prototype. [Sources and credits](SOURCES.md) | [Cite this software](CITATION.cff) | [MIT license](LICENSE)
