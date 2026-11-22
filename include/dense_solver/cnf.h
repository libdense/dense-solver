#ifndef DENSE_SOLVER_CNF_H
#define DENSE_SOLVER_CNF_H

/* Flat literal storage with one offset per clause boundary. */

#include <stdio.h>

#include "dense_solver/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ds_cnf {
    uint32_t variable_count;
    uint32_t clause_count;
    ds_literal *literals;
    uint32_t *offsets; /* clause_count + 1 entries when clause_count > 0 */
    uint32_t literal_capacity;
    uint32_t offset_capacity;
    uint32_t literal_count;
} ds_cnf;

typedef struct ds_cnf_metrics {
    uint32_t variable_count;
    uint32_t clause_count;
    uint32_t literal_count;
    uint32_t growth_operations;
} ds_cnf_metrics;

ds_status ds_cnf_init(ds_cnf *cnf);
void ds_cnf_destroy(ds_cnf *cnf);

/*
 * Append a clause. Literals must be nonzero. variable_count is raised
 * automatically to cover the largest variable seen. An empty clause
 * (count == 0) is legal and makes the formula trivially unsatisfiable.
 */
ds_status ds_cnf_add_clause(ds_cnf *cnf, const ds_literal *literal_values,
    uint32_t count);

/* Borrow a view of clause clause_index. The pointer aliases the formula. */
ds_status ds_cnf_clause(const ds_cnf *cnf, uint32_t clause_index,
    const ds_literal **out_literals, uint32_t *out_count);

/*
 * Evaluate the formula under a complete assignment.
 * assignment[v] for v in [1, variable_count] must be +1 (true) or -1
 * (false); assignment[0] is ignored. Returns DS_OK and writes true or
 * false to out_satisfied.
 */
ds_status ds_cnf_evaluate(const ds_cnf *cnf, const int8_t *assignment,
    bool *out_satisfied);

ds_status ds_cnf_write_dimacs(const ds_cnf *cnf, FILE *stream);

/*
 * Parse a DIMACS CNF stream into a caller-initialized, empty formula.
 * Accepts 'c' comment lines and one 'p cnf <variables> <clauses>'
 * header. Strict: literals must stay within the declared variable
 * range, every clause must be 0-terminated, and the clause count must
 * match the header. On any violation returns DS_ERR_PARSE and leaves
 * partial content in the formula (destroy it).
 */
ds_status ds_cnf_parse_dimacs(ds_cnf *cnf, FILE *stream);

/* Order-sensitive FNV-1a hash of the structure, for determinism tests. */
uint64_t ds_cnf_hash(const ds_cnf *cnf);

void ds_cnf_get_metrics(const ds_cnf *cnf, ds_cnf_metrics *out_metrics);

#ifdef __cplusplus
}
#endif

#endif /* DENSE_SOLVER_CNF_H */
