#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dense_solver/reason.h"

int main(int argc, char **argv)
{
    if (argc != 3) return 1;
    FILE *input = fopen(argv[2], "r");
    if (!input) return 1;
    ds_cnf f = {0};
    ds_status status = ds_cnf_parse_dimacs(&f, input);
    fclose(input);
    ds_verdict verdict = DS_VERDICT_UNKNOWN;
    ds_count_certificate certificate = {0};
    ds_hall_report hall = {0};
    ds_count_report count = {0};
    bool ok = false;
    if (status == DS_OK && !strcmp(argv[1], "count")) {
        status = ds_reason_count(&f, &verdict, &count);
        ok = status == DS_OK && verdict == DS_VERDICT_UNSAT &&
            count.demand == 9 && count.capacity == 8;
    } else if (status == DS_OK) {
        status = ds_reason_hall(&f, 0, &verdict, &hall, &certificate);
        if (!strcmp(argv[1], "cap"))
            ok = status == DS_OK && hall.budget_exhausted && hall.conflict_edges <= 3 &&
                verdict == DS_VERDICT_UNKNOWN && !certificate.clause_count;
        else if (!strcmp(argv[1], "hall"))
            ok = status == DS_OK && verdict == DS_VERDICT_UNSAT &&
                certificate.clause_count > certificate.group_count;
    }
    ds_count_certificate_destroy(&certificate);
    ds_cnf_destroy(&f);
    return ok ? 0 : 1;
}
