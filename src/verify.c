#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <stdlib.h>
#include "app.h"
#include "dense_solver/rng.h"

static bool confirm(const ds_cnf *f, ds_verdict expected, int source, bool cdcl)
{
    ds_work_ledger ledger;
    ds_reason_summary report = {0};
    ds_verdict actual;
    ds_cdcl_config cfg = ds_cdcl_config_default();
    cfg.enable_db_reduction = true;
    int8_t *model = calloc((size_t)f->variable_count+1, sizeof(*model));
    if (!model) return false;
    ds_status status = cdcl ? ds_solve_cdcl(f, &cfg, &actual, model, &ledger)
        : ds_solve_reasoned(f, &cfg, &actual, model, &ledger, &report);
    bool ok = status == DS_OK && actual == expected && (source < 0 || (int)report.source == source);
    if (ok && actual == DS_VERDICT_SAT) {
        bool verified = false;
        ok = ds_cnf_evaluate(f, model, &verified) == DS_OK && verified;
    }
    free(model);
    return ok;
}

static bool from_text(const char *text, int expected, int source)
{
    ds_cnf f = {0};
    FILE *fp = fmemopen((void *)text, strlen(text), "r");
    if (!fp) return false;
    ds_status s = ds_cnf_parse_dimacs(&f, fp);
    fclose(fp);
    bool ok = expected < 0 ? s == DS_ERR_PARSE :
        s == DS_OK && confirm(&f, (ds_verdict)expected, source, false);
    ds_cnf_destroy(&f);
    return ok;
}

/* Evaluate input literals directly against each bit assignment. This oracle
 * is independent of the solver, parser, and public model checker. */
static ds_verdict truth_table(const ds_cnf *f)
{
    for (unsigned bits = 0; bits < (1u << f->variable_count); ++bits) {
        bool valid = true;
        for (uint32_t i = 0; valid && i < f->clause_count; ++i) {
            bool hit = false;
            for (uint32_t j = f->offsets[i]; j < f->offsets[i+1]; ++j) {
                ds_literal lit = f->literals[j];
                bool bit = (bits & (1u << (unsigned)(abs(lit)-1))) != 0;
                hit |= bit == (lit > 0);
            }
            valid = hit;
        }
        if (valid) return DS_VERDICT_SAT;
    }
    return DS_VERDICT_UNSAT;
}

static bool small_formulas(void)
{
    ds_rng rng;
    ds_rng_seed(&rng, 731);
    for (unsigned k = 0; k < 2000; ++k) {
        ds_cnf f = {0};
        unsigned n = 1+(unsigned)ds_rng_below(&rng, 7);
        unsigned clauses = 1+(unsigned)ds_rng_below(&rng, 30);
        bool ok = true;
        for (unsigned i = 0; ok && i < clauses; ++i) {
            ds_literal row[5];
            unsigned width = 1+(unsigned)ds_rng_below(&rng, 5);
            for (unsigned j = 0; j < width; ++j) {
                ds_literal v = 1+(ds_literal)ds_rng_below(&rng,n);
                row[j] = ds_rng_below(&rng,2) ? v : -v;
            }
            ok = ds_cnf_add_clause(&f, row, width) == DS_OK;
        }
        ds_verdict expected = truth_table(&f);
        ok = ok && confirm(&f, expected, -1, false) && confirm(&f, expected, -1, true);
        ds_cnf_destroy(&f);
        if (!ok) return false;
    }
    return true;
}

static bool php(unsigned p, unsigned h, bool seq, bool remove_demand, bool cdcl)
{
    ds_cnf f = {0}, edited = {0};
    bool ok = ds_generate(&f, p, h, seq) == DS_OK;
    const ds_cnf *input = &f;
    if (remove_demand) {
        for (uint32_t i = 1; ok && i < f.clause_count; ++i)
            ok = ds_cnf_add_clause(&edited, f.literals+f.offsets[i], f.offsets[i+1]-f.offsets[i]) == DS_OK;
        input = &edited;
    }
    bool sat = p-(unsigned)remove_demand <= h;
    ok = ok && confirm(input, sat ? DS_VERDICT_SAT : DS_VERDICT_UNSAT, cdcl || sat ? -1 : DS_SOURCE_COUNT, cdcl);
    if (ok && !cdcl && !sat) {
        ds_count_report r;
        ds_verdict v;
        ok = ds_reason_count(input, &v, &r) == DS_OK && v == DS_VERDICT_UNSAT
            && r.demand == p && r.capacity == h;
    }
    ds_cnf_destroy(&edited);
    ds_cnf_destroy(&f);
    return ok;
}

int ds_verify(FILE *out)
{
    unsigned total = 0, failed = 0;
#define CHECK(label, expr) do { uint64_t begin = ds_now_ns(); bool ok = (expr); ++total; if (!ok) ++failed; \
    fprintf(out, "%s  %-42s %.3f ms\n", ok ? "PASS" : "FAIL", label, (double)(ds_now_ns()-begin)/1e6); } while (0)
    CHECK("Empty formula", from_text("p cnf 0 0\n", DS_VERDICT_SAT, -1));
    CHECK("Empty clause", from_text("p cnf 0 1\n0\n", DS_VERDICT_UNSAT, -1));
    CHECK("Conflicting units", from_text("p cnf 1 2\n1 0\n-1 0\n", DS_VERDICT_UNSAT, DS_SOURCE_XOR));
    CHECK("Parity with a model", from_text("p cnf 2 2\n1 2 0\n-1 -2 0\n", DS_VERDICT_SAT, DS_SOURCE_XOR));
    CHECK("Search with a model", from_text("p cnf 2 1\n1 2 0\n", DS_VERDICT_SAT, DS_SOURCE_SEARCH));
    CHECK("9/8 pairwise", php(9,8,false,false,false));
    CHECK("9/8 minus one demand", php(9,8,false,true,false));
    CHECK("9/8 sequential", php(9,8,true,false,false));
    CHECK("8/8 sequential SAT control", php(8,8,true,false,false));
    CHECK("4/3 forced CDCL", php(4,3,true,false,true));
    CHECK("Out-of-range literal", from_text("p cnf 2 1\n3 0\n",-1,-1));
    CHECK("Unterminated clause", from_text("p cnf 1 1\n1\n",-1,-1));
    CHECK("Wrong clause count", from_text("p cnf 1 2\n1 0\n",-1,-1));
    CHECK("2000 truth tables, both solver modes", small_formulas());
    fprintf(out, "%u/%u checks passed\n", total-failed,total);
    return failed || ferror(out) ? 1 : 0;
}
