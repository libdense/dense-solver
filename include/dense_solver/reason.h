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

/*
 * A counting certificate, checkable without trusting the solver.
 *
 * clauses lists input clauses (0-based) that pairwise share no literal,
 * ignoring literals that unit propagation from the input makes false.
 * Each group is a set of literals of which at most one can be true: for
 * every pair a, b in a group, unit propagation from the input plus a
 * makes b false, or the reverse. Every other literal of every listed
 * clause is in some group, or is listed as refuted: unit propagation
 * from the input plus that literal reaches a conflict. There are more
 * clauses than groups. Each clause needs its own true literal and each
 * group holds at most one, so no assignment satisfies them all.
 */
typedef struct ds_count_certificate {
    uint32_t clause_count;
    uint32_t *clauses;
    uint32_t group_count;
    uint32_t *group_offsets;   /* group_count + 1 entries */
    ds_literal *group_literals;
    uint32_t refuted_count;
    ds_literal *refuted;
} ds_count_certificate;

/* Free the arrays and zero the structure. Accepts NULL. */
void ds_count_certificate_destroy(ds_count_certificate *certificate);

typedef struct ds_hall_report {
    uint32_t demands;          /* clauses selected as demands */
    uint32_t groups;           /* at-most-one groups built over them */
    uint32_t matched;          /* maximum matching, demands to groups */
    uint32_t violator_demands; /* |S|: demands in the Hall violator */
    uint32_t violator_groups;  /* |N(S)|: groups those demands touch */
    uint32_t probes;           /* literals probed by propagation */
    uint32_t refuted_literals; /* probes that reached a conflict */
    uint64_t conflict_edges;   /* distinct conflict pairs recorded */
    uint64_t probe_visits;     /* clause visits spent probing */
    uint64_t analysis_ops;
    uint32_t phase;            /* 1 binary clauses, 2 probes, 0 none */
    uint32_t attempt;          /* 1 to 6, which selection fired */
    bool budget_exhausted;     /* probing stopped at its budget */
    bool barren;               /* probing stopped: a sample found nothing */
    bool skipped;              /* no unit or binary clause: cannot fire */
    bool fired;
} ds_hall_report;

/*
 * Hall counting over literals, with at-most-one groups read from binary
 * clauses and then learned by unit-propagation probes (see
 * reason_hall.c). probe_budget caps clause visits spent probing. 0
 * selects an automatic budget: 2,000,000 plus 8 per input literal, plus
 * 8 per conflict pair found so far, at most 64,000,000 in all.
 * Verdict is UNSAT or UNKNOWN. When it fires and out_certificate is
 * non NULL, a certificate is written.
 */
ds_status ds_reason_hall(const ds_cnf *cnf, uint64_t probe_budget,
    ds_verdict *out_verdict, ds_hall_report *out_report,
    ds_count_certificate *out_certificate);

typedef enum ds_reason_source {
    DS_SOURCE_XOR = 0,
    DS_SOURCE_COUNT = 1,
    DS_SOURCE_SEARCH = 2,
    DS_SOURCE_HALL = 3
} ds_reason_source;

const char *ds_reason_source_string(ds_reason_source source);

typedef struct ds_reason_summary {
    ds_reason_source source;   /* which stage settled the verdict */
    ds_xor_report xor_report;
    ds_count_report count_report;
    uint64_t reasoning_ops;    /* operations across the reasoning stages */
    ds_hall_report hall_report;
} ds_reason_summary;

/* Initialize certificate outputs to zero before first use, and destroy them
 * before reuse. A nonempty certificate is produced for a counting UNSAT.
 * Rebuild callers that allocate ds_reason_summary with the v0.0.3 header. */
ds_status ds_reason_count_certified(const ds_cnf *cnf, ds_verdict *out_verdict,
    ds_count_report *out_report, ds_count_certificate *out_certificate);

/* Pass false to skip Hall and use the v0.0.2 reasoning order. */
ds_status ds_solve_configured(const ds_cnf *cnf, const ds_cdcl_config *config,
    bool enable_hall, ds_verdict *verdict, int8_t *model, ds_work_ledger *ledger,
    ds_reason_summary *summary, FILE *trace, ds_count_certificate *certificate);

/* Optional stream of sampled parity rows, groups and demand clause indices. */
ds_status ds_solve_traced(const ds_cnf *cnf, const ds_cdcl_config *config,
    ds_verdict *out_verdict, int8_t *out_model, ds_work_ledger *out_ledger,
    ds_reason_summary *out_summary, FILE *trace);

/*
 * Run XOR/Gauss, sparse counting, Hall matching, then CDCL search as
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
