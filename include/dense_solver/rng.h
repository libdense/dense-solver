#ifndef DENSE_SOLVER_RNG_H
#define DENSE_SOLVER_RNG_H

/* Deterministic xorshift64 generator. */

#include "dense_solver/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ds_rng {
    uint64_t state;
} ds_rng;

/* Seed of zero is remapped internally; any seed value is acceptable. */
void ds_rng_seed(ds_rng *rng, uint64_t seed);

uint64_t ds_rng_next(ds_rng *rng);

/* Uniform value in [0, bound). bound of zero returns zero. */
uint64_t ds_rng_below(ds_rng *rng, uint64_t bound);

#ifdef __cplusplus
}
#endif

#endif /* DENSE_SOLVER_RNG_H */
