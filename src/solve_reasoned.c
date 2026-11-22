/* Try parity, sparse counting and Hall matching before conflict-driven search. */

#include <string.h>

#include "trace.h"

const char *ds_reason_source_string(ds_reason_source source)
{
    switch (source) {
    case DS_SOURCE_XOR:
        return "xor";
    case DS_SOURCE_COUNT:
        return "count";
    case DS_SOURCE_HALL:
        return "hall";
    case DS_SOURCE_SEARCH:
        return "search";
    }
    return "invalid";
}

ds_status ds_solve_configured(const ds_cnf *cnf,
    const ds_cdcl_config *config, bool enable_hall, ds_verdict *out_verdict,
    int8_t *out_model, ds_work_ledger *out_ledger,
    ds_reason_summary *out_summary, FILE *trace, ds_count_certificate *certificate)
{
    ds_reason_summary summary;
    ds_verdict verdict = DS_VERDICT_UNKNOWN;
    ds_status status;

    if (cnf == NULL || out_verdict == NULL) {
        return DS_ERR_INVALID_ARGUMENT;
    }
    memset(&summary, 0, sizeof(summary));
    if (out_ledger != NULL) {
        memset(out_ledger, 0, sizeof(*out_ledger));
    }

    status = ds_xor_traced(cnf, &verdict, out_model,
        &summary.xor_report, trace);
    if (status != DS_OK) {
        return status;
    }
    summary.reasoning_ops = summary.xor_report.detection_ops
        + summary.xor_report.elimination_ops;
    if (verdict != DS_VERDICT_UNKNOWN) {
        summary.source = DS_SOURCE_XOR;
        goto conclude;
    }

    status = ds_count_traced(cnf, &verdict, &summary.count_report, trace, certificate);
    if (status != DS_OK) {
        return status;
    }
    summary.reasoning_ops += summary.count_report.analysis_ops;
    if (verdict != DS_VERDICT_UNKNOWN) {
        summary.source = DS_SOURCE_COUNT;
        goto conclude;
    }

    if (enable_hall) {
        status = ds_reason_hall(cnf, 0, &verdict, &summary.hall_report, certificate);
        if (status != DS_OK && status != DS_ERR_OUT_OF_MEMORY) return status;
        summary.reasoning_ops += summary.hall_report.analysis_ops;
        if (status == DS_OK && verdict == DS_VERDICT_UNSAT) {
            summary.source = DS_SOURCE_HALL;
            goto conclude;
        }
    }
    summary.source = DS_SOURCE_SEARCH;
    status = ds_solve_cdcl(cnf, config, &verdict, out_model, out_ledger);
    if (status != DS_OK) {
        return status;
    }

conclude:
    *out_verdict = verdict;
    if (out_summary != NULL) {
        *out_summary = summary;
    }
    return DS_OK;
}

ds_status ds_solve_traced(const ds_cnf *cnf, const ds_cdcl_config *config,
    ds_verdict *out, int8_t *model, ds_work_ledger *ledger,
    ds_reason_summary *summary, FILE *trace)
{
    return ds_solve_configured(cnf, config, true, out, model, ledger, summary, trace, NULL);
}

ds_status ds_solve_reasoned(const ds_cnf *cnf, const ds_cdcl_config *config,
    ds_verdict *out, int8_t *model, ds_work_ledger *ledger, ds_reason_summary *summary)
{
    return ds_solve_traced(cnf, config, out, model, ledger, summary, NULL);
}
