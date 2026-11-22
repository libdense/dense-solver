#ifndef DS_APP_H
#define DS_APP_H
#include "dense_solver/reason.h"
extern const char *const ds_presets[];
extern const size_t ds_preset_count;
uint64_t ds_now_ns(void);
bool ds_number(const char *s, uint64_t max, uint64_t *value);
bool ds_configure(ds_cdcl_config *cfg, const char *key, const char *value);
ds_status ds_generate(ds_cnf *out, unsigned pigeons, unsigned holes, bool sequential);
ds_status ds_preset(ds_cnf *out, size_t index);
int ds_run(FILE *out, const ds_cnf *f, const ds_cdcl_config *cfg, bool cdcl,
    bool measure, bool detail, uint64_t parse_ns);
int ds_verify(FILE *out);
int ds_benchmark(FILE *out, unsigned trials);
int ds_report(FILE *out, FILE *csv, const char *scope, const char *instance, bool html);
#endif
