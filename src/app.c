#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"

const char *const ds_presets[] = {"php9x8-pairwise", "php9x8-sequential",
    "php31x30-pairwise", "php31x30-sequential", "php65x64-pairwise",
    "php65x64-sequential", "xor-system-sat", "xor-system-unsat"};
const size_t ds_preset_count = sizeof(ds_presets)/sizeof(*ds_presets);

uint64_t ds_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

bool ds_number(const char *s, uint64_t max, uint64_t *value)
{
    if (!s || *s < '0' || *s > '9') return false;
    char *end;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 10);
    if (errno || *end || n > max) return false;
    *value = n;
    return true;
}

bool ds_configure(ds_cdcl_config *cfg, const char *key, const char *value)
{
    if (!value) return false;
    if (!strcmp(key, "heuristic")) {
        if (!strcmp(value, "fixed")) cfg->heuristic = DS_BRANCH_FIXED;
        else if (!strcmp(value, "activity")) cfg->heuristic = DS_BRANCH_ACTIVITY;
        else if (!strcmp(value, "placebo")) cfg->heuristic = DS_BRANCH_PLACEBO;
        else return false;
        return true;
    }
    uint64_t n;
    if (!ds_number(value, UINT64_MAX, &n)) return false;
    if (!strcmp(key,"decisions")) cfg->decision_budget = n;
    else if (!strcmp(key,"conflicts")) cfg->conflict_budget = n;
    else if (!strcmp(key,"seed")) cfg->placebo_seed = n;
    else if (!strcmp(key,"luby") && n && n <= UINT32_MAX) cfg->luby_unit = (uint32_t)n;
    else if (!strcmp(key,"interval") && n && n <= UINT32_MAX) cfg->db_reduction_interval = (uint32_t)n;
    else return false;
    return true;
}

ds_status ds_generate(ds_cnf *out, unsigned p, unsigned h, bool seq)
{
    if (!out || !p || !h || p > 100 || h > 100 || out->variable_count || out->clause_count)
        return DS_ERR_INVALID_ARGUMENT;
    ds_cnf f = {0};
    ds_literal row[100], column[100], aux[100];
    ds_literal next = (ds_literal)(p*h + 1);
    ds_status status = DS_OK;
    for (unsigned i = 0; i < p; ++i) {
        for (unsigned j = 0; j < h; ++j) row[j] = (ds_literal)(1+i*h+j);
        status = ds_cnf_add_clause(&f, row, h);
        if (status != DS_OK) goto fail;
    }
    for (unsigned j = 0; j < h; ++j) {
        for (unsigned i = 0; i < p; ++i) column[i] = (ds_literal)(1+i*h+j);
        if (!seq || p <= 2) {
            for (unsigned a = 0; a < p; ++a)
                for (unsigned b = a+1; b < p; ++b) {
                    ds_literal pair[] = {-column[a], -column[b]};
                    status = ds_cnf_add_clause(&f, pair, 2);
                    if (status != DS_OK) goto fail;
                }
        } else {
            for (unsigned i = 0; i+1 < p; ++i) aux[i] = next++;
            ds_literal first[] = {-column[0], aux[0]};
            status = ds_cnf_add_clause(&f, first, 2);
            if (status != DS_OK) goto fail;
            for (unsigned i = 1; i+1 < p; ++i) {
                ds_literal pairs[][2] = {{-aux[i-1],aux[i]}, {-column[i],-aux[i-1]}, {-column[i],aux[i]}};
                for (unsigned k = 0; k < 3; ++k) {
                    status = ds_cnf_add_clause(&f, pairs[k], 2);
                    if (status != DS_OK) goto fail;
                }
            }
            ds_literal last[] = {-column[p-1], -aux[p-2]};
            status = ds_cnf_add_clause(&f, last, 2);
            if (status != DS_OK) goto fail;
        }
    }
    *out = f;
    return DS_OK;
fail:
    ds_cnf_destroy(&f);
    return status;
}

ds_status ds_preset(ds_cnf *out, size_t index)
{
    static const unsigned sizes[] = {8,30,64};
    if (index >= ds_preset_count) return DS_ERR_INVALID_ARGUMENT;
    if (index < 6) return ds_generate(out, sizes[index/2]+1, sizes[index/2], index%2 == 1);
    const ds_literal sat[][2] = {{1,2},{-1,-2},{2,-3},{-2,3},{1,0}};
    const ds_literal unsat[][2] = {{1,2},{-1,-2},{2,3},{-2,-3},{1,3},{-1,-3}};
    for (unsigned k = 0; k < (index == 6 ? 5u : 6u); ++k) {
        const ds_literal *l = index == 6 ? sat[k] : unsat[k];
        ds_status s = ds_cnf_add_clause(out, l, l[1] ? 2 : 1);
        if (s != DS_OK) return s;
    }
    return DS_OK;
}

static void print_certificate(FILE *out, const ds_count_certificate *certificate)
{
    uint32_t index;

    for (index = 0u; index < certificate->clause_count; ++index) {
        fprintf(out, "c certificate clause %u\n", certificate->clauses[index] + 1u);
    }
    for (index = 0u; index < certificate->group_count; ++index) {
        uint32_t position;

        fputs("c certificate group", out);
        for (position = certificate->group_offsets[index];
            position < certificate->group_offsets[index + 1u]; ++position) {
            fprintf(out, " %d", certificate->group_literals[position]);
        }
        fputs(" 0\n", out);
    }
    if (certificate->refuted_count > 0u) {
        fputs("c certificate refuted", out);
        for (index = 0u; index < certificate->refuted_count; ++index) {
            fprintf(out, " %d", certificate->refuted[index]);
        }
        fputs(" 0\n", out);
    }
}

int ds_run(FILE *out, const ds_cnf *f, const ds_cdcl_config *cfg, bool cdcl,
    bool measure, bool detail, bool hall, bool emit_certificate, uint64_t parse_ns)
{
    /* Even in timing mode a SAT result must supply a verifiable witness.
     * Validation runs after the solve clock. */
    int8_t *model = calloc((size_t)f->variable_count+1, sizeof(*model));
    if (!model) return 1;
    ds_work_ledger ledger = {0};
    ds_reason_summary summary = {0};
    ds_verdict verdict;
    ds_count_certificate certificate = {0};
    uint64_t start = ds_now_ns();
    ds_status status = cdcl ? ds_solve_cdcl(f, cfg, &verdict, model, &ledger)
        : ds_solve_configured(f, cfg, hall, &verdict, model, &ledger, &summary,
            detail ? out : NULL, emit_certificate ? &certificate : NULL);
    uint64_t elapsed = ds_now_ns()-start;
    bool valid = false;
    if (status != DS_OK || (verdict == DS_VERDICT_SAT &&
        (ds_cnf_evaluate(f, model, &valid) != DS_OK || !valid))) {
        fprintf(out, "c solver error %s\n", status == DS_OK ? "invalid SAT witness" : ds_status_string(status));
        ds_count_certificate_destroy(&certificate);
        free(model);
        return 1;
    }
    const char *method = cdcl ? "search" : ds_reason_source_string(summary.source);
    if (measure) fprintf(out, "%s %s %" PRIu64 "\n", ds_verdict_string(verdict), method, elapsed);
    else {
        fprintf(out, "c method %s\n", method);
        if (!cdcl && summary.source == DS_SOURCE_COUNT) {
            fprintf(out, "c demand %u capacity %u\nc counting %s\n", summary.count_report.demand,
                summary.count_report.capacity, summary.count_report.pass);
            if (summary.count_report.forced_clause)
                fprintf(out, "c forced-false demand clause %u\n", summary.count_report.forced_clause);
        }
        if (summary.source == DS_SOURCE_HALL) {
            fprintf(out, "c demand %u capacity %u\n", summary.hall_report.violator_demands,
                summary.hall_report.violator_groups);
        }
        if (emit_certificate) print_certificate(out, &certificate);
        if (detail) {
            fprintf(out, "c hall phase %u probes %u edges %" PRIu64 " limit %u\n",
                summary.hall_report.phase, summary.hall_report.probes, summary.hall_report.conflict_edges,
                (unsigned)summary.hall_report.budget_exhausted);
            fprintf(out, "c variables %u clauses %u literals %u hash 0x%016" PRIx64 "\n",
                f->variable_count, f->clause_count, f->literal_count, ds_cnf_hash(f));
            fprintf(out, "c parse_ns %" PRIu64 " solve_with_trace_ns %" PRIu64 "\n", parse_ns, elapsed);
            const ds_xor_report *x = &summary.xor_report;
            fprintf(out, "c xor rows %u columns %u constraints %u covered %u inconsistent %u full_coverage %u\n",
                x->gauss_rows, x->gauss_columns, x->constraints_found, x->covered_clauses,
                (unsigned)x->inconsistent, (unsigned)x->full_coverage);
            fprintf(out, "c xor_detection_ops %" PRIu64 " xor_elimination_ops %" PRIu64
                " count_ops %" PRIu64 " reasoning_ops %" PRIu64 "\n",
                x->detection_ops, x->elimination_ops, summary.count_report.analysis_ops, summary.reasoning_ops);
            fprintf(out, "c groups %u selected %u demand %u capacity %u\n", summary.count_report.amo_groups,
                summary.count_report.alo_selected, summary.count_report.demand, summary.count_report.capacity);
#define METRIC(name) fprintf(out, "c " #name " %" PRIu64 "\n", ledger.name)
            METRIC(decisions); METRIC(conflicts); METRIC(propagations); METRIC(clause_visits);
            METRIC(backtracks); METRIC(max_depth); METRIC(pure_eliminations); METRIC(branches_avoided);
            METRIC(restarts); METRIC(learned_clauses); METRIC(learned_literals);
            METRIC(learned_propagations); METRIC(db_reductions); METRIC(learned_deleted);
#undef METRIC
        }
        if (verdict == DS_VERDICT_SAT) {
            fputs("s SATISFIABLE\nv", out);
            for (uint32_t i = 1; i <= f->variable_count; ++i)
                fprintf(out, " %d", model[i] < 0 ? -(int)i : (int)i);
            fputs(" 0\n", out);
        } else fputs(verdict == DS_VERDICT_UNSAT ? "s UNSATISFIABLE\n" : "s UNKNOWN\n", out);
    }
    ds_count_certificate_destroy(&certificate);
    free(model);
    return ferror(out) ? 1 : verdict == DS_VERDICT_UNSAT ? 20 : verdict == DS_VERDICT_SAT ? 10 : 0;
}
