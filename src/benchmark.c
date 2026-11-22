#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"

int ds_benchmark(FILE *out, unsigned trials)
{
    fputs("instance,scope,solver,trial,verdict,time_ns,timeout_s\n",out);
    for (size_t i = 0; i < 6; ++i) {
        ds_cnf f = {0};
        ds_status s = ds_preset(&f, i);
        for (unsigned k = 1; s == DS_OK && k <= trials; ++k) {
            ds_verdict v;
            ds_reason_summary summary;
            uint64_t begin = ds_now_ns();
            s = ds_solve_reasoned(&f, NULL, &v, NULL, NULL, &summary);
            uint64_t ns = ds_now_ns()-begin;
            if (s != DS_OK || v != DS_VERDICT_UNSAT || summary.source != DS_SOURCE_COUNT) {
                s = DS_ERR_STATE; break;
            }
            fprintf(out,"%s,solve,dense-solver-" DENSE_SOLVER_VERSION ",%u,UNSAT,%" PRIu64 ",\n",
                ds_presets[i], k, ns);
        }
        ds_cnf_destroy(&f);
        if (s != DS_OK) return 1;
    }
    return ferror(out) ? 1 : 0;
}

typedef struct {
    char instance[100], solver[100], scope[10];
    uint64_t *times;
    size_t count, cap, timeouts;
} series;

static bool token(const char *s)
{
    if (!*s || strlen(s) >= 100) return false;
    for (; *s; ++s)
        if (!isalnum((unsigned char)*s) && !strchr("-._+",*s)) return false;
    return true;
}

static int compare(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a>b)-(a<b);
}

int ds_report(FILE *out, FILE *csv, const char *scope, const char *instance, bool html)
{
    char *line = NULL;
    size_t cap = 0, n = 0;
    series *rows = NULL;
    bool ok = getline(&line,&cap,csv) > 0;
    if (ok) line[strcspn(line,"\r\n")] = 0;
    ok = ok && !strcmp(line,"instance,scope,solver,trial,verdict,time_ns,timeout_s");
    while (ok && getline(&line,&cap,csv) >= 0) {
        line[strcspn(line,"\r\n")] = 0;
        if (!*line) continue;
        char *f[7] = {line};
        for (size_t i = 1; i < 7; ++i) {
            char *sep = strchr(f[i-1], ',');
            if (!sep) { ok = false; break; }
            *sep = 0; f[i] = sep+1;
        }
        uint64_t trial, ns = 0;
        if (!ok || !token(f[0]) || !token(f[2]) || (strcmp(f[1],"solve") && strcmp(f[1],"process"))
            || !ds_number(f[3],UINT32_MAX,&trial) || !trial) { ok = false; break; }
        bool timeout = !strcmp(f[4],"TIMEOUT");
        if ((!timeout && strcmp(f[4],"SAT") && strcmp(f[4],"UNSAT"))
            || (!timeout && !ds_number(f[5],UINT64_MAX,&ns)) || strchr(f[6],',')) { ok = false; break; }
        if ((scope && strcmp(scope,f[1])) || (instance && strcmp(instance,f[0]))) continue;
        size_t idx = 0;
        while (idx < n && (strcmp(rows[idx].instance,f[0]) || strcmp(rows[idx].scope,f[1]) || strcmp(rows[idx].solver,f[2]))) ++idx;
        if (idx == n) {
            series *p = realloc(rows,(n+1)*sizeof(*p));
            if (!p) { ok = false; break; }
            rows = p;
            rows[n++] = (series){0};
            strcpy(rows[idx].instance,f[0]); strcpy(rows[idx].solver,f[2]); strcpy(rows[idx].scope,f[1]);
        }
        series *r = &rows[idx];
        if (timeout) { ++r->timeouts; continue; }
        if (r->cap == r->count) {
            size_t next = r->cap ? 2*r->cap : 8;
            uint64_t *p = realloc(r->times,next*sizeof(*p));
            if (!p) { ok = false; break; }
            r->times = p; r->cap = next;
        }
        r->times[r->count++] = ns;
    }
    if (ok && html) fputs("<table><thead><tr><th>Instance</th><th>Solver</th><th>Clock</th><th>Completed</th><th>Timeouts</th><th>Min (ms)</th><th>Median (ms)</th><th>Max (ms)</th><th>Log time</th></tr></thead><tbody>", out);
    if (ok && !html) fputs("instance,solver,scope,completed,timeouts,min_ms,median_ms,max_ms\n",out);
    for (size_t i = 0; i < n; ++i) {
        series *r = &rows[i];
        if (ok) {
            char low[40]="", mid[40]="", high[40]="";
            double median_ms = 0;
            if (r->count) {
                qsort(r->times, r->count, sizeof(*r->times), compare);
                snprintf(low,sizeof(low),"%.6f",(double)r->times[0]/1e6);
                snprintf(high,sizeof(high),"%.6f",(double)r->times[r->count-1]/1e6);
                median_ms = ((double)r->times[(r->count-1)/2]+(double)r->times[r->count/2])/2e6;
                if (!r->timeouts) snprintf(mid,sizeof(mid),"%.6f",median_ms);
            }
            if (html) {
                fprintf(out,"<tr><td>%s</td><td>%s</td><td>%s</td><td>%zu</td><td>%zu</td><td>%s</td><td>%s</td><td>%s</td><td>",
                    r->instance,r->solver,r->scope,r->count,r->timeouts,low,*mid ? mid : "timeout",high);
                if (*mid) fprintf(out,"<meter min=0 max=10 value=\"%.5f\" title=\"%s ms\"></meter>", log10(1+median_ms*1e6),mid);
                fputs("</td></tr>",out);
            } else fprintf(out,"%s,%s,%s,%zu,%zu,%s,%s,%s\n",r->instance,r->solver,r->scope,r->count,r->timeouts,low,mid,high);
        }
        free(r->times);
    }
    if (ok && html) fputs("</tbody></table>",out);
    free(rows); free(line);
    return ok && !ferror(csv) && !ferror(out) ? 0 : 1;
}
