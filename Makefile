CC ?= cc
CFLAGS ?= -O3
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -ffp-contract=off
VERSION := $(strip $(file <VERSION))
INCLUDES = -Iinclude -DDENSE_SOLVER_VERSION='"$(VERSION)"'
APP = app verify benchmark
CORE = cnf dimacs rng solve_cdcl reason_xor reason_count solve_reasoned status
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

test: build/dense-solver
	sha256sum -c instances/SHA256SUMS
	./build/dense-solver --verify
	python3 tests/check.py
	python3 tests/port.py

clean:
	rm -rf build

-include $(OBJECTS:.o=.d) build/main.d $(addprefix build/,$(addsuffix .d,$(APP))) build/web.d

.PHONY: web
web: build/dense-solver-web
EVENT_CFLAGS ?= $(shell pkg-config --cflags libevent)
EVENT_LIBS ?= $(shell pkg-config --libs libevent)
build/web.o: CPPFLAGS += $(EVENT_CFLAGS)
build/dense-solver-web: build/web.o $(addprefix build/,$(addsuffix .o,$(APP))) $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(EVENT_LIBS) $(LDLIBS) -lm -o $@
