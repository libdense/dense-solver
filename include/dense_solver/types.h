#ifndef DENSE_SOLVER_TYPES_H
#define DENSE_SOLVER_TYPES_H

/*
 * dense-solver: shared types and status codes.
 *
 * Every fallible function in dense-solver returns ds_status. DS_OK is 0.
 * Destroy and reset functions accept NULL and return silently.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ds_status {
    DS_OK = 0,
    DS_ERR_INVALID_ARGUMENT,
    DS_ERR_OUT_OF_MEMORY,
    DS_ERR_CAPACITY,
    DS_ERR_PARSE,
    DS_ERR_STATE,
    DS_ERR_UNSUPPORTED
} ds_status;

const char *ds_status_string(ds_status status);

/*
 * Literal encoding: a literal is a nonzero int32_t. Positive means the
 * variable is asserted true, negative means asserted false. Variables are
 * numbered from 1, DIMACS style. Variable of literal L is abs(L).
 */
typedef int32_t ds_literal;

#ifdef __cplusplus
}
#endif

#endif /* DENSE_SOLVER_TYPES_H */
