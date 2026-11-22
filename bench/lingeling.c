#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "dense_solver/cnf.h"
#include "lglib.h"

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

int main(void)
{
    ds_cnf formula = {0};
    if (ds_cnf_init(&formula) != DS_OK ||
        ds_cnf_parse_dimacs(&formula, stdin) != DS_OK || ferror(stdin)) {
        ds_cnf_destroy(&formula);
        return 1;
    }
    LGL *solver = lglinit();
    for (uint32_t i = 0; i < formula.clause_count; ++i) {
        for (uint32_t j = formula.offsets[i]; j < formula.offsets[i + 1]; ++j) {
            lgladd(solver, formula.literals[j]);
        }
        lgladd(solver, 0);
    }
    uint64_t begin = now_ns();
    int verdict = lglsat(solver);
    uint64_t elapsed = now_ns() - begin;
    const char *label = verdict == 20 ? "UNSAT" : (verdict == 10 ? "SAT" : "UNKNOWN");
    printf("%s lingeling %" PRIu64 "\n", label, elapsed);
    lglrelease(solver);
    ds_cnf_destroy(&formula);
    return verdict;
}
