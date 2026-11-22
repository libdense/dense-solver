#include <stdlib.h>
#include <string.h>

#include "dense_solver/cnf.h"

static ds_status grow_u32(uint32_t **buffer, uint32_t *capacity,
    uint32_t minimum_capacity)
{
    uint32_t replacement_capacity;
    uint32_t *replacement;

    if (*capacity >= minimum_capacity) {
        return DS_OK;
    }
    replacement_capacity = (*capacity == 0u) ? 16u : *capacity;
    while (replacement_capacity < minimum_capacity) {
        if (replacement_capacity > (UINT32_MAX / 2u)) {
            return DS_ERR_CAPACITY;
        }
        replacement_capacity *= 2u;
    }
    replacement = (uint32_t *)malloc(
        (size_t)replacement_capacity * sizeof(uint32_t));
    if (replacement == NULL) {
        return DS_ERR_OUT_OF_MEMORY;
    }
    if (*buffer != NULL) {
        memcpy(replacement, *buffer, (size_t)(*capacity) * sizeof(uint32_t));
        free(*buffer);
    }
    *buffer = replacement;
    *capacity = replacement_capacity;
    return DS_OK;
}

static ds_status grow_literals(ds_cnf *cnf, uint32_t minimum_capacity)
{
    uint32_t replacement_capacity;
    ds_literal *replacement;

    if (cnf->literal_capacity >= minimum_capacity) {
        return DS_OK;
    }
    replacement_capacity =
        (cnf->literal_capacity == 0u) ? 64u : cnf->literal_capacity;
    while (replacement_capacity < minimum_capacity) {
        if (replacement_capacity > (UINT32_MAX / 2u)) {
            return DS_ERR_CAPACITY;
        }
        replacement_capacity *= 2u;
    }
    replacement = (ds_literal *)malloc(
        (size_t)replacement_capacity * sizeof(ds_literal));
    if (replacement == NULL) {
        return DS_ERR_OUT_OF_MEMORY;
    }
    if (cnf->literals != NULL) {
        memcpy(replacement, cnf->literals,
            (size_t)cnf->literal_count * sizeof(ds_literal));
        free(cnf->literals);
    }
    cnf->literals = replacement;
    cnf->literal_capacity = replacement_capacity;
    return DS_OK;
}

ds_status ds_cnf_init(ds_cnf *cnf)
{
    if (cnf == NULL) {
        return DS_ERR_INVALID_ARGUMENT;
    }
    memset(cnf, 0, sizeof(*cnf));
    return DS_OK;
}

void ds_cnf_destroy(ds_cnf *cnf)
{
    if (cnf == NULL) {
        return;
    }
    free(cnf->literals);
    free(cnf->offsets);
    memset(cnf, 0, sizeof(*cnf));
}

ds_status ds_cnf_add_clause(ds_cnf *cnf, const ds_literal *literal_values,
    uint32_t count)
{
    ds_status status;
    uint32_t index;

    if (cnf == NULL || (count > 0u && literal_values == NULL)) {
        return DS_ERR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < count; index++) {
        if (literal_values[index] == 0 || literal_values[index] == INT32_MIN) {
            return DS_ERR_INVALID_ARGUMENT;
        }
    }
    if (count > UINT32_MAX - cnf->literal_count || cnf->clause_count > UINT32_MAX-2u)
        return DS_ERR_CAPACITY;
    status = grow_u32(&cnf->offsets, &cnf->offset_capacity,
        cnf->clause_count + 2u);
    if (status != DS_OK) {
        return status;
    }
    status = grow_literals(cnf, cnf->literal_count + count);
    if (status != DS_OK) {
        return status;
    }
    if (cnf->clause_count == 0u) {
        cnf->offsets[0] = 0u;
    }
    for (index = 0u; index < count; index++) {
        ds_literal literal_value = literal_values[index];
        uint32_t variable = (uint32_t)((literal_value > 0)
            ? literal_value : -literal_value);

        if (variable > cnf->variable_count) {
            cnf->variable_count = variable;
        }
        cnf->literals[cnf->literal_count] = literal_value;
        cnf->literal_count++;
    }
    cnf->clause_count++;
    cnf->offsets[cnf->clause_count] = cnf->literal_count;
    return DS_OK;
}

ds_status ds_cnf_clause(const ds_cnf *cnf, uint32_t clause_index,
    const ds_literal **out_literals, uint32_t *out_count)
{
    if (cnf == NULL || out_literals == NULL || out_count == NULL) {
        return DS_ERR_INVALID_ARGUMENT;
    }
    if (clause_index >= cnf->clause_count) {
        return DS_ERR_INVALID_ARGUMENT;
    }
    *out_literals = cnf->literals ? cnf->literals + cnf->offsets[clause_index] : NULL;
    *out_count = cnf->offsets[clause_index + 1u] - cnf->offsets[clause_index];
    return DS_OK;
}

ds_status ds_cnf_evaluate(const ds_cnf *cnf, const int8_t *assignment,
    bool *out_satisfied)
{
    uint32_t clause_index;

    if (cnf == NULL || assignment == NULL || out_satisfied == NULL) {
        return DS_ERR_INVALID_ARGUMENT;
    }
    for (clause_index = 0u; clause_index < cnf->clause_count;
        clause_index++) {
        uint32_t begin = cnf->offsets[clause_index];
        uint32_t end = cnf->offsets[clause_index + 1u];
        bool clause_satisfied = false;
        uint32_t position;

        for (position = begin; position < end; position++) {
            ds_literal literal_value = cnf->literals[position];
            uint32_t variable = (uint32_t)((literal_value > 0)
                ? literal_value : -literal_value);
            int8_t polarity = (int8_t)((literal_value > 0) ? 1 : -1);

            if (assignment[variable] == polarity) {
                clause_satisfied = true;
                break;
            }
        }
        if (!clause_satisfied) {
            *out_satisfied = false;
            return DS_OK;
        }
    }
    *out_satisfied = true;
    return DS_OK;
}

ds_status ds_cnf_write_dimacs(const ds_cnf *cnf, FILE *stream)
{
    uint32_t clause_index;

    if (cnf == NULL || stream == NULL) {
        return DS_ERR_INVALID_ARGUMENT;
    }
    if (fprintf(stream, "p cnf %u %u\n", cnf->variable_count,
            cnf->clause_count) < 0) {
        return DS_ERR_STATE;
    }
    for (clause_index = 0u; clause_index < cnf->clause_count;
        clause_index++) {
        uint32_t begin = cnf->offsets[clause_index];
        uint32_t end = cnf->offsets[clause_index + 1u];
        uint32_t position;

        for (position = begin; position < end; position++) {
            if (fprintf(stream, "%d ", cnf->literals[position]) < 0) {
                return DS_ERR_STATE;
            }
        }
        if (fprintf(stream, "0\n") < 0) {
            return DS_ERR_STATE;
        }
    }
    return DS_OK;
}

uint64_t ds_cnf_hash(const ds_cnf *cnf)
{
    uint64_t hash = 0xCBF29CE484222325ULL; /* FNV-1a offset basis */
    uint32_t index;

    if (cnf == NULL) {
        return 0u;
    }
    hash ^= cnf->variable_count;
    hash *= 0x100000001B3ULL;
    hash ^= cnf->clause_count;
    hash *= 0x100000001B3ULL;
    for (index = 0u; index < cnf->literal_count; index++) {
        hash ^= (uint64_t)(uint32_t)cnf->literals[index];
        hash *= 0x100000001B3ULL;
    }
    for (index = 0u; index <= cnf->clause_count && cnf->clause_count > 0u;
        index++) {
        hash ^= cnf->offsets[index];
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

void ds_cnf_get_metrics(const ds_cnf *cnf, ds_cnf_metrics *out_metrics)
{
    if (cnf == NULL || out_metrics == NULL) {
        return;
    }
    memset(out_metrics, 0, sizeof(*out_metrics));
    out_metrics->variable_count = cnf->variable_count;
    out_metrics->clause_count = cnf->clause_count;
    out_metrics->literal_count = cnf->literal_count;
    out_metrics->growth_operations = 0u;
}
