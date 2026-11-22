#ifndef DENSE_SOLVER_SOLVE_H
#define DENSE_SOLVER_SOLVE_H

/* CDCL configuration, verdicts and operation counts. */

#include "dense_solver/cnf.h"
#include "dense_solver/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ds_verdict {
    DS_VERDICT_SAT = 0,
    DS_VERDICT_UNSAT = 1,
    DS_VERDICT_UNKNOWN = 2 /* decision budget exhausted; never a guess */
} ds_verdict;

const char *ds_verdict_string(ds_verdict verdict);

typedef struct ds_work_ledger {
    uint64_t decisions;          /* branches actually opened */
    uint64_t propagations;       /* assignments forced by unit clauses */
    uint64_t pure_eliminations;  /* assignments forced by pure literals */
    uint64_t clause_visits;      /* clause scans */
    uint64_t backtracks;
    uint64_t conflicts;
    uint64_t max_depth;
    uint64_t branches_avoided;
    /* Learned clauses and restarts. */
    uint64_t restarts;
    uint64_t learned_clauses;
    uint64_t learned_literals;
    /* Propagations whose reason is a learned clause: repeated work
     * that clause learning converted into a lookup. */
    uint64_t learned_propagations;
    /* Learned-clause database reduction. */
    uint64_t db_reductions;
    uint64_t learned_deleted;
} ds_work_ledger;

typedef enum ds_branch_heuristic {
    DS_BRANCH_FIXED = 0,
    DS_BRANCH_ACTIVITY = 1,
    DS_BRANCH_PLACEBO = 2
} ds_branch_heuristic;

typedef struct ds_cdcl_config {
    ds_branch_heuristic heuristic;
    uint64_t placebo_seed;    /* used only by DS_BRANCH_PLACEBO */
    bool enable_restarts;     /* Luby restarts */
    uint32_t luby_unit;       /* conflicts per Luby unit, default 64 */
    /* Periodically discard eligible low-quality learned clauses. */
    bool enable_db_reduction;
    uint32_t db_reduction_interval; /* conflicts between reductions */
    /* 0 means unlimited. Exhaustion returns UNKNOWN, never a guess. */
    uint64_t decision_budget;
    uint64_t conflict_budget;
} ds_cdcl_config;

/* Defaults: ACTIVITY heuristic, restarts on, luby_unit 64,
 * db reduction OFF (interval 2000 when enabled), unlimited budgets. */
ds_cdcl_config ds_cdcl_config_default(void);

ds_status ds_solve_cdcl(const ds_cnf *cnf, const ds_cdcl_config *config,
    ds_verdict *out_verdict, int8_t *out_model, ds_work_ledger *out_ledger);

#ifdef __cplusplus
}
#endif

#endif /* DENSE_SOLVER_SOLVE_H */
