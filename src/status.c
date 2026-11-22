#include "dense_solver/solve.h"

const char *ds_status_string(ds_status status)
{
    switch (status) {
    case DS_OK:
        return "ok";
    case DS_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case DS_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case DS_ERR_CAPACITY:
        return "capacity exceeded";
    case DS_ERR_PARSE:
        return "parse error";
    case DS_ERR_STATE:
        return "invalid state";
    case DS_ERR_UNSUPPORTED:
        return "unsupported";
    }
    return "unknown status";
}

const char *ds_verdict_string(ds_verdict verdict)
{
    switch (verdict) {
    case DS_VERDICT_SAT:
        return "SAT";
    case DS_VERDICT_UNSAT:
        return "UNSAT";
    case DS_VERDICT_UNKNOWN:
        return "UNKNOWN";
    }
    return "invalid";
}
