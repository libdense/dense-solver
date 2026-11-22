CC ?= cc
CFLAGS ?= -O3
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -ffp-contract=off
VERSION := $(strip $(file <VERSION))
INCLUDES = -Iinclude -DDENSE_SOLVER_VERSION='"$(VERSION)"'
APP = app verify benchmark
CORE = cnf dimacs rng solve_cdcl reason_xor reason_count reason_hall solve_reasoned status
OBJECTS = $(addprefix build/,$(addsuffix .o,$(CORE)))
LINGELING ?= build/deps/lingeling-rel-1.0.0
.PHONY: all test clean lingeling
all: build/dense-solver build/libdense-solver.a

build/libdense-solver.a: $(OBJECTS)
	$(AR) rcs $@ $^

build:
	mkdir -p $@

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(INCLUDES) -MMD -MP -c $< -o $@

build/main.o build/web.o build/benchmark.o: VERSION

build/dense-solver: build/main.o $(addprefix build/,$(addsuffix .o,$(APP))) $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -lm -o $@

lingeling: build/bench-lingeling

build/bench-lingeling: bench/lingeling.c $(OBJECTS) $(LINGELING)/liblgl.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(INCLUDES) -I"$(LINGELING)" $< $(OBJECTS) "$(LINGELING)/liblgl.a" $(LDFLAGS) $(LDLIBS) -lm -o $@

build/test-hall-api: tests/hall_api.c build/libdense-solver.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(INCLUDES) $^ $(LDFLAGS) $(LDLIBS) -lm -o $@

build/test-hall-limit.o: src/reason_hall.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(INCLUDES) -DDS_HALL_EDGE_CAP=3 -MMD -MP -c $< -o $@

build/test-hall-limit: tests/hall_api.c build/test-hall-limit.o build/libdense-solver.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(INCLUDES) $^ $(LDFLAGS) $(LDLIBS) -lm -o $@

test: build/dense-solver build/test-hall-api build/test-hall-limit
	./build/test-hall-api count instances/php9x8-sequential.cnf
	./build/test-hall-api hall instances/chessboard12.cnf
	./build/test-hall-limit cap instances/chessboard12.cnf
	./build/test-hall-limit cap instances/php9x8-totalizer.cnf
	sha256sum -c instances/SHA256SUMS
	./build/dense-solver --verify
	python3 tests/check.py
	python3 tests/port.py
	python3 tests/hall.py
	python3 tests/public_bench.py

clean:
	rm -rf build

-include $(OBJECTS:.o=.d) build/main.d $(addprefix build/,$(addsuffix .d,$(APP))) build/web.d build/test-hall-limit.d

.PHONY: web
web: build/dense-solver-web
EVENT_CFLAGS ?= $(shell pkg-config --cflags libevent)
EVENT_LIBS ?= $(shell pkg-config --libs libevent)
build/web.o: CPPFLAGS += $(EVENT_CFLAGS)
build/dense-solver-web: build/web.o $(addprefix build/,$(addsuffix .o,$(APP))) $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(EVENT_LIBS) $(LDLIBS) -lm -o $@
