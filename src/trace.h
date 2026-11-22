#ifndef DS_TRACE_H
#define DS_TRACE_H
#include "dense_solver/reason.h"
ds_status ds_count_traced(const ds_cnf *, ds_verdict *, ds_count_report *, FILE *);
ds_status ds_xor_traced(const ds_cnf *, ds_verdict *, int8_t *, ds_xor_report *, FILE *);
#endif
