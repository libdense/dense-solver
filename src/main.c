#include <stdlib.h>
#include <string.h>
#include "app.h"

static int usage(FILE *out)
{
    fputs("dense-solver [--measure | --explain] [--cdcl] [--no-hall] [options] [FILE|-]\n"
        "  --heuristic activity|fixed|placebo   --seed N\n"
        "  --no-restarts   --luby N   --no-reduce   --interval N\n"
        "  --decisions N   --conflicts N  (zero means unlimited)\n"
        "dense-solver --generate PIGEONS HOLES pairwise|sequential\n"
        "dense-solver --verify | --benchmark [TRIALS] | --report FILE.csv\n", out);
    return out == stdout ? 0 : 1;
}

int main(int argc, char **argv)
{
    ds_cnf formula = {0};
    bool measure = false, cdcl = false, detail = false, hall = true;
    ds_cdcl_config cfg = ds_cdcl_config_default();
    cfg.enable_db_reduction = true;
    const char *file = NULL;
    if (argc == 2 && !strcmp(argv[1], "--help")) return usage(stdout);
    if (argc == 2 && !strcmp(argv[1], "--version")) { puts("dense-solver " DENSE_SOLVER_VERSION); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--verify")) return ds_verify(stdout);
    if (argc >= 2 && !strcmp(argv[1], "--benchmark")) {
        uint64_t n = 7;
        if (argc > 3 || (argc == 3 && (!ds_number(argv[2], 1000, &n) || !n))) return usage(stderr);
        return ds_benchmark(stdout, (unsigned)n);
    }
    if (argc == 3 && !strcmp(argv[1], "--report")) {
        FILE *fp = strcmp(argv[2], "-") ? fopen(argv[2],"r") : stdin;
        if (!fp) { perror(argv[2]); return 1; }
        int result = ds_report(stdout, fp, NULL, NULL, false);
        if (fp != stdin) fclose(fp);
        return result;
    }
    if (argc == 5 && !strcmp(argv[1], "--generate")) {
        uint64_t p, h;
        bool seq = !strcmp(argv[4], "sequential");
        if ((!seq && strcmp(argv[4], "pairwise")) || !ds_number(argv[2],100,&p) ||
            !ds_number(argv[3],100,&h) || !p || !h) return usage(stderr);
        ds_status s = ds_generate(&formula, (unsigned)p, (unsigned)h, seq);
        if (s == DS_OK) s = ds_cnf_write_dimacs(&formula, stdout);
        ds_cnf_destroy(&formula);
        return s == DS_OK && !fflush(stdout) ? 0 : 1;
    }
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a,"--measure")) measure = true;
        else if (!strcmp(a,"--explain")) detail = true;
        else if (!strcmp(a,"--no-hall")) hall = false;
        else if (!strcmp(a,"--cdcl")) cdcl = true;
        else if (!strcmp(a,"--no-restarts")) cfg.enable_restarts = false;
        else if (!strcmp(a,"--no-reduce")) cfg.enable_db_reduction = false;
        else if (!strncmp(a,"--",2)) {
            if (i+1 == argc || !ds_configure(&cfg, a+2, argv[++i])) return usage(stderr);
        } else if (file || (a[0] == '-' && a[1])) return usage(stderr);
        else file = a;
    }
    if (measure && detail) return usage(stderr);
    FILE *fp = file && strcmp(file,"-") ? fopen(file,"r") : stdin;
    if (!fp) { perror(file); return 1; }
    uint64_t start = ds_now_ns();
    ds_status status = ds_cnf_parse_dimacs(&formula, fp);
    uint64_t elapsed = ds_now_ns()-start;
    bool failed = ferror(fp);
    if (fp != stdin && fclose(fp)) failed = true;
    int result = 1;
    if (status == DS_OK && !failed) result = ds_run(stdout, &formula, &cfg, cdcl, measure, detail, hall, detail, elapsed);
    else fprintf(stderr,"Could not read DIMACS input (%s).\n",ds_status_string(status));
    ds_cnf_destroy(&formula);
    return fflush(stdout) ? 1 : result;
}
