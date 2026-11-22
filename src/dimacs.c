#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdlib.h>

#include "dense_solver/cnf.h"

/* Largest clause the parser accepts; external benchmark instances
 * rarely exceed a few hundred literals per clause. */
#define DS_DIMACS_MAX_CLAUSE 4096u

static int skip_whitespace_and_comments(FILE *stream)
{
    int character;

    for (;;) {
        character = getc_unlocked(stream);
        if (character == 'c') {
            do {
                character = getc_unlocked(stream);
            } while (character != '\n' && character != EOF);
            continue;
        }
        if (character == EOF || !isspace(character)) {
            return character;
        }
    }
}

static ds_status read_int(FILE *stream, int first_character,
    long *out_value)
{
    long value = 0;
    bool negative = false;
    bool any_digit = false;
    int character = first_character;

    if (character == '-') {
        negative = true;
        character = getc_unlocked(stream);
    }
    while (character != EOF && isdigit(character)) {
        any_digit = true;
        if (value > (100000000L - (character - '0')) / 10L) {
            return DS_ERR_PARSE; /* absurd magnitude */
        }
        value = value * 10L + (long)(character - '0');
        character = getc_unlocked(stream);
    }
    if (!any_digit) {
        return DS_ERR_PARSE;
    }
    if (character != EOF && !isspace(character)) {
        return DS_ERR_PARSE;
    }
    *out_value = negative ? -value : value;
    return DS_OK;
}

static ds_status parse_locked(ds_cnf *cnf, FILE *stream)
{
    ds_literal clause_literals[DS_DIMACS_MAX_CLAUSE];
    long declared_variables = 0;
    long declared_clauses = 0;
    uint32_t parsed_clauses = 0u;
    uint32_t clause_length = 0u;
    int character;
    ds_status status;

    if (cnf == NULL || stream == NULL) {
        return DS_ERR_INVALID_ARGUMENT;
    }
    if (cnf->clause_count != 0u || cnf->variable_count != 0u) {
        return DS_ERR_STATE; /* parser requires an empty formula */
    }

    /* Header. */
    character = skip_whitespace_and_comments(stream);
    if (character != 'p') {
        return DS_ERR_PARSE;
    }
    if (getc_unlocked(stream) != ' ' || getc_unlocked(stream) != 'c'
        || getc_unlocked(stream) != 'n' || getc_unlocked(stream) != 'f') {
        return DS_ERR_PARSE;
    }
    character = getc_unlocked(stream);
    if (character == EOF || !isspace(character)) return DS_ERR_PARSE;
    character = skip_whitespace_and_comments(stream);
    status = read_int(stream, character, &declared_variables);
    if (status != DS_OK) {
        return status;
    }
    character = skip_whitespace_and_comments(stream);
    status = read_int(stream, character, &declared_clauses);
    if (status != DS_OK) {
        return status;
    }
    if (declared_variables < 0 || declared_clauses < 0
        || declared_variables > 10000000L) {
        return DS_ERR_PARSE;
    }

    /* Clauses. */
    for (;;) {
        long value;

        character = skip_whitespace_and_comments(stream);
        if (character == EOF) {
            break;
        }
        status = read_int(stream, character, &value);
        if (status != DS_OK) {
            return status;
        }
        if (value == 0) {
            if ((long)parsed_clauses >= declared_clauses) return DS_ERR_PARSE;
            status = ds_cnf_add_clause(cnf, clause_literals,
                clause_length);
            if (status != DS_OK) {
                return status;
            }
            parsed_clauses++;
            clause_length = 0u;
            continue;
        }
        if (value > declared_variables || value < -declared_variables) {
            return DS_ERR_PARSE;
        }
        if (clause_length >= DS_DIMACS_MAX_CLAUSE) {
            return DS_ERR_CAPACITY;
        }
        clause_literals[clause_length] = (ds_literal)value;
        clause_length++;
    }
    if (clause_length != 0u) {
        return DS_ERR_PARSE; /* trailing clause without terminating 0 */
    }
    if ((long)parsed_clauses != declared_clauses) {
        return DS_ERR_PARSE;
    }
    if (cnf->variable_count < (uint32_t)declared_variables) {
        cnf->variable_count = (uint32_t)declared_variables;
    }
    return DS_OK;
}

ds_status ds_cnf_parse_dimacs(ds_cnf *cnf, FILE *stream)
{
    if (!cnf || !stream) return DS_ERR_INVALID_ARGUMENT;
    flockfile(stream);
    ds_status status = parse_locked(cnf, stream);
    if (status == DS_OK && ferror(stream)) status = DS_ERR_STATE;
    funlockfile(stream);
    return status;
}
