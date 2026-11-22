/*
 * XOR extraction and GF(2) Gaussian elimination.
 *
 * Detection is exact, not heuristic: a group of clauses over the same
 * variable set is recognized as an XOR constraint only when it forbids
 * exactly the 2^(k-1) assignments of one parity. A recognized XOR is
 * therefore logically implied by the clauses verbatim; there are no
 * false positives by construction. Unit clauses qualify with k = 1
 * (the parity fact v = b). Everything is deterministic.
 */

#include <stdlib.h>
#include <string.h>

#include "trace.h"

#define DS_XOR_MAX_WIDTH 8u

typedef struct clause_signature {
    uint32_t clause_index;
    uint32_t width;
    uint32_t vars[DS_XOR_MAX_WIDTH];   /* sorted ascending */
    uint32_t forbidden_mask;           /* bit j: vars[j] true */
    bool usable;
} clause_signature;

typedef struct xor_row {
    uint32_t width;
    uint32_t vars[DS_XOR_MAX_WIDTH];
    uint32_t rhs; /* parity the XOR must equal */
} xor_row;

static int compare_signatures(const void *left, const void *right)
{
    const clause_signature *first = (const clause_signature *)left;
    const clause_signature *second = (const clause_signature *)right;
    uint32_t index;

    if (first->usable != second->usable) {
        return first->usable ? -1 : 1;
    }
    if (first->width != second->width) {
        return (first->width < second->width) ? -1 : 1;
    }
    for (index = 0u; index < first->width; index++) {
        if (first->vars[index] != second->vars[index]) {
            return (first->vars[index] < second->vars[index]) ? -1 : 1;
        }
    }
    if (first->forbidden_mask != second->forbidden_mask) {
        return (first->forbidden_mask < second->forbidden_mask) ? -1 : 1;
    }
    return (first->clause_index < second->clause_index) ? -1
        : (first->clause_index > second->clause_index) ? 1 : 0;
}

static uint32_t mask_parity(uint32_t mask)
{
    mask ^= mask >> 16;
    mask ^= mask >> 8;
    mask ^= mask >> 4;
    mask ^= mask >> 2;
    mask ^= mask >> 1;
    return mask & 1u;
}

static bool same_variable_set(const clause_signature *first,
    const clause_signature *second)
{
    uint32_t index;

    if (!first->usable || !second->usable
        || first->width != second->width) {
        return false;
    }
    for (index = 0u; index < first->width; index++) {
        if (first->vars[index] != second->vars[index]) {
            return false;
        }
    }
    return true;
}

ds_status ds_xor_traced(const ds_cnf *cnf, ds_verdict *out_verdict,
    int8_t *out_model, ds_xor_report *out_report, FILE *trace)
{
    ds_xor_report report;
    clause_signature *signatures = NULL;
    xor_row *rows = NULL;
    uint32_t row_capacity = 0u;
    uint8_t *covered = NULL;
    uint32_t *column_of_var = NULL; /* 0 = unmapped, else column + 1 */
    uint32_t *var_of_column = NULL;
    uint64_t *matrix = NULL;
    uint8_t *rhs = NULL;
    uint32_t *pivot_column_of_row = NULL;
    uint8_t *column_value = NULL;
    ds_verdict verdict = DS_VERDICT_UNKNOWN;
    uint32_t clause_index;
    uint32_t signature_count = 0u;
    uint32_t run_begin;
    uint32_t columns = 0u;
    uint32_t words;
    uint32_t rank = 0u;
    ds_status status = DS_OK;

    if (cnf == NULL || out_verdict == NULL) {
        return DS_ERR_INVALID_ARGUMENT;
    }
    memset(&report, 0, sizeof(report));
    report.total_clauses = cnf->clause_count;

    if (cnf->clause_count == 0u) {
        /* Empty formula: trivially SAT with the default model. */
        report.full_coverage = true;
        *out_verdict = DS_VERDICT_SAT;
        if (out_model != NULL) {
            memset(out_model, 1, (size_t)cnf->variable_count + 1u);
            out_model[0] = 0;
        }
        if (out_report != NULL) {
            *out_report = report;
        }
        return DS_OK;
    }

    /* A parity row of width k needs at least 2^(k-1) clauses. Binary
     * rows additionally need complementary sign masks in sorted variable order. */
    uint32_t widths[DS_XOR_MAX_WIDTH+1] = {0}, masks = 0, eligible = 0, slots = 0;
    report.detection_ops = cnf->literal_count;
    for (uint32_t i = 0; i < cnf->clause_count; ++i) {
        uint32_t begin = cnf->offsets[i], width = cnf->offsets[i+1]-begin;
        if (!width) { verdict = DS_VERDICT_UNSAT; goto conclude; }
        if (width > DS_XOR_MAX_WIDTH) continue;
        if (width == 2) {
            ds_literal a = cnf->literals[begin], b = cnf->literals[begin+1];
            if (abs(a) == abs(b)) continue;
            if (abs(a) > abs(b)) { ds_literal tmp = a; a = b; b = tmp; }
            masks |= 1u << ((unsigned)(a < 0) + 2u*(unsigned)(b < 0));
        }
        ++widths[width];
    }
    for (uint32_t w = 1; w <= DS_XOR_MAX_WIDTH; ++w) {
        if (widths[w] < (1u << (w-1))) continue;
        if (w == 2 && (masks & 9) != 9 && (masks & 6) != 6) continue;
        eligible |= 1u << w;
        slots += widths[w];
    }
    if (!slots) goto conclude;
    signatures = (clause_signature *)calloc(slots, sizeof(clause_signature));
    covered = (uint8_t *)calloc(cnf->clause_count, sizeof(uint8_t));
    if (signatures == NULL || covered == NULL) {
        status = DS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    /* Build signatures. */
    for (clause_index = 0u; clause_index < cnf->clause_count;
        clause_index++) {
        const ds_literal *literals = NULL;
        uint32_t count = 0u;
        clause_signature *signature;
        uint32_t position;
        bool usable = true;

        (void)ds_cnf_clause(cnf, clause_index, &literals, &count);
        if (count > DS_XOR_MAX_WIDTH || !(eligible & (1u << count))) continue;
        if (count == 2 && abs(literals[0]) == abs(literals[1])) continue;
        signature = &signatures[signature_count];
        /* A unit clause fixes one parity bit. An empty clause is handled separately. */
        if (count < 1u || count > DS_XOR_MAX_WIDTH) {
            usable = false;
        }
        signature->clause_index = clause_index;
        signature->width = usable ? count : 0u;
        signature->forbidden_mask = 0u;
        if (usable) {
            /* Insertion sort variables, tracking literal signs. */
            uint32_t sorted_vars[DS_XOR_MAX_WIDTH];
            uint8_t sorted_negative[DS_XOR_MAX_WIDTH];

            for (position = 0u; position < count; position++) {
                ds_literal literal_value = literals[position];
                uint32_t variable = (uint32_t)((literal_value > 0)
                    ? literal_value : -literal_value);
                uint8_t negative = (uint8_t)(literal_value < 0);
                uint32_t slot = position;

                while (slot > 0u && sorted_vars[slot - 1u] > variable) {
                    sorted_vars[slot] = sorted_vars[slot - 1u];
                    sorted_negative[slot] = sorted_negative[slot - 1u];
                    slot--;
                }
                sorted_vars[slot] = variable;
                sorted_negative[slot] = negative;
            }
            for (position = 1u; position < count && usable; position++) {
                if (sorted_vars[position] == sorted_vars[position - 1u]) {
                    usable = false; /* duplicate variable */
                }
            }
            if (usable) {
                for (position = 0u; position < count; position++) {
                    signature->vars[position] = sorted_vars[position];
                    if (sorted_negative[position] != 0u) {
                        /* Negative literal is false when the variable
                         * is true: forbidden assignment sets it true. */
                        signature->forbidden_mask |= 1u << position;
                    }
                }
            }
        }
        signature->usable = usable;
        if (!usable) {
            signature->width = 0u;
        }
        signature_count++;
    }
    qsort(signatures, signature_count, sizeof(clause_signature),
        compare_signatures);
    report.detection_ops += signature_count;

    /* Scan runs of identical variable sets. */
    run_begin = 0u;
    while (run_begin < signature_count && signatures[run_begin].usable) {
        uint32_t run_end = run_begin + 1u;
        uint32_t width = signatures[run_begin].width;
        uint32_t distinct_masks = 0u;
        uint32_t previous_mask = 0u;
        uint32_t parity_seen[2] = { 0u, 0u };
        uint32_t index;

        while (run_end < signature_count
            && same_variable_set(&signatures[run_begin],
                &signatures[run_end])) {
            run_end++;
        }
        /* Masks are sorted within the run (comparator includes them);
         * count distinct and split by parity. */
        for (index = run_begin; index < run_end; index++) {
            uint32_t mask = signatures[index].forbidden_mask;

            report.detection_ops++;
            if (distinct_masks == 0u || mask != previous_mask) {
                distinct_masks++;
                previous_mask = mask;
                parity_seen[mask_parity(mask)]++;
            }
        }
        if (distinct_masks == (1u << width)) {
            /* Every assignment forbidden: contradiction on the spot. */
            verdict = DS_VERDICT_UNSAT;
            report.inconsistent = true;
            goto conclude;
        }
        if (distinct_masks == (1u << (width - 1u))
            && (parity_seen[0] == distinct_masks
                || parity_seen[1] == distinct_masks)) {
            uint32_t forbidden_parity =
                (parity_seen[1] == distinct_masks) ? 1u : 0u;

            if (report.constraints_found == row_capacity) {
                uint32_t replacement_capacity =
                    (row_capacity == 0u) ? 64u : row_capacity * 2u;
                xor_row *replacement = (xor_row *)malloc(
                    (size_t)replacement_capacity * sizeof(xor_row));

                if (replacement == NULL) {
                    status = DS_ERR_OUT_OF_MEMORY;
                    goto cleanup;
                }
                if (rows != NULL) {
                    memcpy(replacement, rows,
                        (size_t)report.constraints_found
                            * sizeof(xor_row));
                    free(rows);
                }
                rows = replacement;
                row_capacity = replacement_capacity;
            }
            rows[report.constraints_found].width = width;
            for (index = 0u; index < width; index++) {
                rows[report.constraints_found].vars[index] =
                    signatures[run_begin].vars[index];
            }
            /* Forbidden parity p means the XOR must equal 1 - p. */
            rows[report.constraints_found].rhs = forbidden_parity ^ 1u;
            if (trace && report.constraints_found < 32) {
                fputs("c xor-row", trace);
                for (index = 0; index < width; ++index)
                    fprintf(trace, " %u", signatures[run_begin].vars[index]);
                fprintf(trace, " = %u\n", forbidden_parity ^ 1u);
            }
            report.constraints_found++;
            for (index = run_begin; index < run_end; index++) {
                if (covered[signatures[index].clause_index] == 0u) {
                    covered[signatures[index].clause_index] = 1u;
                    report.covered_clauses++;
                }
            }
        }
        run_begin = run_end;
    }
    report.full_coverage =
        (report.covered_clauses == report.total_clauses);

    if (report.constraints_found == 0u) {
        verdict = DS_VERDICT_UNKNOWN;
        goto conclude;
    }

    /* Column mapping for variables that appear in XOR rows. */
    column_of_var = (uint32_t *)calloc(
        (size_t)cnf->variable_count + 1u, sizeof(uint32_t));
    var_of_column = (uint32_t *)calloc(
        (size_t)cnf->variable_count + 1u, sizeof(uint32_t));
    if (column_of_var == NULL || var_of_column == NULL) {
        status = DS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    {
        uint32_t row_index;

        for (row_index = 0u; row_index < report.constraints_found;
            row_index++) {
            uint32_t index;

            for (index = 0u; index < rows[row_index].width; index++) {
                uint32_t variable = rows[row_index].vars[index];

                if (column_of_var[variable] == 0u) {
                    columns++;
                    column_of_var[variable] = columns;
                    var_of_column[columns - 1u] = variable;
                }
            }
        }
    }
    report.gauss_rows = report.constraints_found;
    report.gauss_columns = columns;
    words = (columns + 63u) / 64u;
    matrix = (uint64_t *)calloc(
        (size_t)report.constraints_found * words, sizeof(uint64_t));
    rhs = (uint8_t *)calloc(report.constraints_found, sizeof(uint8_t));
    pivot_column_of_row = (uint32_t *)calloc(report.constraints_found,
        sizeof(uint32_t));
    column_value = (uint8_t *)calloc(columns, sizeof(uint8_t));
    if (matrix == NULL || rhs == NULL || pivot_column_of_row == NULL
        || column_value == NULL) {
        status = DS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    {
        uint32_t row_index;

        for (row_index = 0u; row_index < report.constraints_found;
            row_index++) {
            uint32_t index;

            for (index = 0u; index < rows[row_index].width; index++) {
                uint32_t column =
                    column_of_var[rows[row_index].vars[index]] - 1u;

                matrix[(size_t)row_index * words + column / 64u] ^=
                    1ULL << (column % 64u);
            }
            rhs[row_index] = (uint8_t)rows[row_index].rhs;
        }
    }

    /* Forward elimination. */
    {
        uint32_t column;

        for (column = 0u; column < columns; column++) {
            uint32_t word = column / 64u;
            uint64_t bit = 1ULL << (column % 64u);
            uint32_t pivot = rank;
            uint32_t row_index;

            while (pivot < report.constraints_found
                && (matrix[(size_t)pivot * words + word] & bit) == 0u) {
                pivot++;
                report.elimination_ops++;
            }
            if (pivot == report.constraints_found) {
                continue;
            }
            if (pivot != rank) {
                uint32_t swap_word;
                uint8_t held_rhs;

                for (swap_word = 0u; swap_word < words; swap_word++) {
                    uint64_t held =
                        matrix[(size_t)rank * words + swap_word];

                    matrix[(size_t)rank * words + swap_word] =
                        matrix[(size_t)pivot * words + swap_word];
                    matrix[(size_t)pivot * words + swap_word] = held;
                }
                held_rhs = rhs[rank];
                rhs[rank] = rhs[pivot];
                rhs[pivot] = held_rhs;
                report.elimination_ops += words;
            }
            for (row_index = rank + 1u;
                row_index < report.constraints_found; row_index++) {
                if ((matrix[(size_t)row_index * words + word] & bit)
                    != 0u) {
                    uint32_t xor_word;

                    for (xor_word = 0u; xor_word < words; xor_word++) {
                        matrix[(size_t)row_index * words + xor_word] ^=
                            matrix[(size_t)rank * words + xor_word];
                    }
                    rhs[row_index] =
                        (uint8_t)(rhs[row_index] ^ rhs[rank]);
                    report.elimination_ops += words;
                }
            }
            pivot_column_of_row[rank] = column;
            rank++;
            if (rank == report.constraints_found) {
                break;
            }
        }
    }
    /* Rows beyond the rank have all-zero left sides. */
    {
        uint32_t row_index;

        for (row_index = rank; row_index < report.constraints_found;
            row_index++) {
            report.elimination_ops++;
            if (rhs[row_index] != 0u) {
                report.inconsistent = true;
                verdict = DS_VERDICT_UNSAT;
                goto conclude;
            }
        }
    }

    if (!report.full_coverage) {
        verdict = DS_VERDICT_UNKNOWN;
        goto conclude;
    }

    /* Full coverage and consistent: back-substitute a model.
     * Free columns default to false. */
    {
        uint32_t row_index = rank;

        while (row_index > 0u) {
            uint32_t pivot_column;
            uint32_t word;
            uint8_t value;

            row_index--;
            pivot_column = pivot_column_of_row[row_index];
            value = rhs[row_index];
            for (word = 0u; word < words; word++) {
                uint64_t row_word =
                    matrix[(size_t)row_index * words + word];

                report.elimination_ops++;
                while (row_word != 0u) {
                    uint32_t bit_index =
                        (uint32_t)__builtin_ctzll(row_word);
                    uint32_t column = word * 64u + bit_index;

                    row_word &= row_word - 1u;
                    if (column == pivot_column) {
                        continue;
                    }
                    value = (uint8_t)(value ^ column_value[column]);
                    report.elimination_ops++;
                }
            }
            column_value[pivot_column] = value;
        }
    }
    verdict = DS_VERDICT_SAT;
    if (out_model != NULL) {
        uint32_t variable;

        out_model[0] = 0;
        for (variable = 1u; variable <= cnf->variable_count;
            variable++) {
            if (column_of_var[variable] != 0u) {
                out_model[variable] =
                    (column_value[column_of_var[variable] - 1u] != 0u)
                        ? (int8_t)1 : (int8_t)-1;
            } else {
                out_model[variable] = 1;
            }
        }
    }

conclude:
    *out_verdict = verdict;
    if (out_report != NULL) {
        *out_report = report;
    }
cleanup:
    free(signatures);
    free(rows);
    free(covered);
    free(column_of_var);
    free(var_of_column);
    free(matrix);
    free(rhs);
    free(pivot_column_of_row);
    free(column_value);
    return status;
}

ds_status ds_reason_xor(const ds_cnf *cnf, ds_verdict *out, int8_t *model, ds_xor_report *report)
{
    return ds_xor_traced(cnf, out, model, report, NULL);
}
