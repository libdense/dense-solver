# dense-solver

A C11 SAT solver by Merlin Numa. It recognizes when too many requirements compete for too few places. Hall matching can find a shortage confined to part of a problem. The same code handles pigeonhole formulas and domino tiling. It can produce a certificate that a separate program checks against the original input.

Version 0.0.4 compares dense-solver, Kissat and CaDiCaL on 30 public formulas from SATLIB and the 2025 SAT Competition. The ZIP includes the timed inputs, their checksums, all trials and the comparison runner. [Public benchmark results](PUBLIC-BENCHMARKS.md) | [Earlier pigeonhole and chessboard results](BENCHMARK.md)

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

The solver accepts DIMACS on standard input or from a file. It tries parity elimination, sparse counting, Hall matching and finally conflict-driven clause learning (CDCL). Use `--no-hall` to run the v0.0.2 stage order. The solver checks every SAT model against the input. Use `--explain` to emit a project-format certificate for an UNSAT verdict reached by counting or matching. The checker uses only Python 3. Exit codes are 10 for SAT, 20 for UNSAT, 0 for UNKNOWN, and 1 for errors. Use `--help` for search controls.

## Check an UNSAT result

```sh
./build/dense-solver --explain instances/chessboard32.cnf | python3 tests/check_certificate.py instances/chessboard32.cnf
```

## Use the rules in another solver

Link `build/libdense-solver.a` and include `dense_solver/reason.h`. Call `ds_reason_count` on a loaded `ds_cnf` to try the sparse counter. If it returns UNKNOWN, call `ds_reason_hall` to try matching. If both return UNKNOWN, continue with the host solver. Each pass can run independently. To collect a certificate, use `ds_reason_count_certified` or pass a certificate to `ds_reason_hall`. Initialize it to zero and free it with `ds_count_certificate_destroy`. Recompile existing callers that allocate `ds_reason_summary` to pick up its new Hall report.

## Local browser workbench

```sh
make web
./build/dense-solver-web
```

Open `http://127.0.0.1:8080/` from the same computer. The optional web build needs libevent 2 development headers and pkg-config (`libevent-dev` and `pkg-config` on Debian or Devuan). Run it from the project root to read the recorded benchmarks. Pass a port number to use another port. Each process holds one working formula in memory.

Load, edit, generate or download a DIMACS file. The Structure view shows the extracted groups, selected demands, Hall matching statistics, parity equations and search counters. Benchmarks shows individual datasets, clocks and medians and can run fresh native trials. Verification runs the same C checks as the CLI. All application logic runs in C. The browser displays HTML and CSS.

## Credits

dense-solver grew out of the Eclipse research project initiated and directed by Merlin Numa. Eclipse v0.0.4 recorded its first pigeonhole counting implementation on August 19, 2026. [Eclipse project history](ECLIPSE-HISTORY.md)

Numa directed this project using the most capable OpenAI ChatGPT and Anthropic Claude models available through the author's paid subscriptions during the August to November 2026 development sessions. The November 22 native port incorporates the changes from the TypeScript prototype. [Sources and credits](SOURCES.md) | [Cite this software](CITATION.cff) | [MIT license](LICENSE)
