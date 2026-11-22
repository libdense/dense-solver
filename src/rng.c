#include "dense_solver/rng.h"

void ds_rng_seed(ds_rng *rng, uint64_t seed)
{
    if (rng == NULL) {
        return;
    }
    /* xorshift64 has a zero fixed point; remap it deterministically. */
    rng->state = (seed != 0u) ? seed : 0x9E3779B97F4A7C15ULL;
}

uint64_t ds_rng_next(ds_rng *rng)
{
    uint64_t value = rng->state;

    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    rng->state = value;
    return value;
}

uint64_t ds_rng_below(ds_rng *rng, uint64_t bound)
{
    uint64_t threshold;
    uint64_t value;

    if (bound == 0u) {
        return 0u;
    }
    /* Rejection sampling for an unbiased draw. */
    threshold = (0u - bound) % bound;
    for (;;) {
        value = ds_rng_next(rng);
        if (value >= threshold) {
            return value % bound;
        }
    }
}
