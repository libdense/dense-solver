#ifndef DENSE_SOLVER_REASON_H
#define DENSE_SOLVER_REASON_H

/* Parity and counting checks used before CDCL search. */

#include "dense_solver/cnf.h"
#include "dense_solver/solve.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ds_xor_report {
    uint32_t constraints_found;
    uint32_t covered_clauses;
    uint32_t total_clauses;
    uint32_t gauss_rows;
    uint32_t gauss_columns;
    uint64_t detection_ops;
    uint64_t elimination_ops;
    bool inconsistent;   /* GF(2) system derives 0 = 1 */
    bool full_coverage;  /* every clause consumed by some XOR row */
} ds_xor_report;

/*
 * Detect XOR constraints and Gaussian-eliminate them.
 * Verdict: UNSAT when the parity system is inconsistent; SAT (with
 * model written to out_model when non NULL, sized variable_count + 1)
 * when the system is consistent AND covers every clause; UNKNOWN
 * otherwise. A returned model is complete and independently checkable.
 */
ds_status ds_reason_xor(const ds_cnf *cnf, ds_verdict *out_verdict,
    int8_t *out_model, ds_xor_report *out_report);

typedef struct ds_count_report {
    uint32_t amo_groups;     /* at-most-one cliques found */
    uint32_t alo_selected;   /* variable-disjoint all-positive clauses */
    uint32_t demand;         /* = alo_selected, each forces >= 1 true */
    uint32_t capacity;       /* distinct AMO groups those clauses touch */
    uint64_t analysis_ops;
    const char *pass;       /* direct, chain, implied, or NULL */
    uint32_t forced_clause; /* 1-based demand whose literals all imply false */
    bool fired;              /* UNSAT by counting or an entirely forced-false demand */
} ds_count_report;

/*
 * Counting argument: greedy at-most-one clique partition of the
 * explicit or implied binary conflict graph, then greedy selection of
 * variable-disjoint all-positive clauses fully covered by cliques.
 * demand > capacity proves UNSAT (each selected clause forces one
 * true variable, each clique admits at most one). Verdict is UNSAT or
 * UNKNOWN; counting alone can never certify SAT.
 */
ds_status ds_reason_count(const ds_cnf *cnf, ds_verdict *out_verdict,
    ds_count_report *out_report);

typedef enum ds_reason_source {
    DS_SOURCE_XOR = 0,
    DS_SOURCE_COUNT = 1,
    DS_SOURCE_SEARCH = 2
} ds_reason_source;

const char *ds_reason_source_string(ds_reason_source source);

typedef struct ds_reason_summary {
    ds_reason_source source;   /* which stage settled the verdict */
    ds_xor_report xor_report;
    ds_count_report count_report;
    uint64_t reasoning_ops;    /* operations across both checks */
} ds_reason_summary;

/* Optional stream of sampled parity rows, groups and demand clause indices. */
ds_status ds_solve_traced(const ds_cnf *cnf, const ds_cdcl_config *config,
    ds_verdict *out_verdict, int8_t *out_model, ds_work_ledger *out_ledger,
    ds_reason_summary *out_summary, FILE *trace);

/*
 * Run XOR/Gauss, then counting, then CDCL search as
 * fallback (with the given config, or defaults when NULL). The search
 * ledger is written only when search ran; it is zeroed otherwise.
 */
ds_status ds_solve_reasoned(const ds_cnf *cnf,
    const ds_cdcl_config *config, ds_verdict *out_verdict,
    int8_t *out_model, ds_work_ledger *out_ledger,
    ds_reason_summary *out_summary);

#ifdef __cplusplus
}
#endif

#endif /* DENSE_SOLVER_REASON_H */
