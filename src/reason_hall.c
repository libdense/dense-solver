/*
 * Hall reasoner: counting with at-most-one groups found by propagation.
 *
 * The counting stage reads at-most-one groups only from negative binary
 * clauses over variables, so the same pigeonhole formula written with a
 * sequential counter, a ladder, a bitwise encoding or flipped polarities
 * falls through to search. This stage works on literals and recovers the
 * groups from the formula's own unit propagation:
 *
 *   1. Root: propagate the unit clauses. A conflict here is left to
 *      search, which finds it at once.
 *   2. Demands: clauses, not satisfied at the root, that pairwise share
 *      no literal. Three selection orders are tried: input order,
 *      longest clause first, and the larger side of each two-colorable
 *      group of clauses linked by shared literals (the mutilated
 *      chessboard needs the color class with more squares).
 *   3. Conflict edges between demand literals: a and b conflict when the
 *      formula implies (-a or -b). Phase one reads them from binary
 *      clauses. Phase two probes each demand literal a: assign it,
 *      propagate, and record every demand literal made false. A probe
 *      that reaches a conflict refutes a: it is false in every model.
 *   4. Groups: a greedy clique partition of the conflict graph, either
 *      across demands only (no two members from one demand) or over all
 *      edges, growing each clique in demand order. Each clique is an
 *      at-most-one group.
 *   5. Hall's theorem: every model gives each demand its own true literal
 *      (the demands share no literal), and no group holds two true
 *      literals. So a model yields a matching from demands to groups that
 *      covers every demand. When a maximum matching leaves a demand
 *      uncovered, alternating paths from the uncovered demands give a set
 *      S of demands that together touch fewer than |S| groups: UNSAT.
 *
 * Every recorded edge is an implication of the formula, so any subset of
 * the edges keeps the verdict sound; the probe budget only limits what
 * can be found. Deterministic throughout. The verdict is UNSAT or
 * UNKNOWN; this stage never certifies SAT.
 */

#include <stdlib.h>
#include <string.h>

#include "dense_solver/reason.h"

#define HALL_NONE UINT32_MAX
/* Automatic probe budget, in clause visits: a base that scales with the
 * input, plus a share for every conflict pair the probes find. Formulas
 * without at-most-one structure stop near the base; formulas full of it
 * earn the visits that recovering it costs. */
#define HALL_BUDGET_BASE UINT64_C(2000000)
#define HALL_BUDGET_PER_LITERAL UINT64_C(8)
#define HALL_BUDGET_PER_PAIR UINT64_C(8)
#define HALL_BUDGET_CAP UINT64_C(64000000)
/* Recorded conflict pairs, each stored twice in the adjacency. */
#ifndef DS_HALL_EDGE_CAP
#define DS_HALL_EDGE_CAP UINT32_C(16000000)
#endif
#define HALL_POLICY_COUNT 3u
/* Probing stops early when this many probes, or one in 32 of the demand
 * literals if that is more, find no conflict pair and refute nothing:
 * the formula shows no at-most-one structure to recover. */
#define HALL_BARREN_SAMPLE 1024u
/* Test hook: build with -DDS_HALL_FIRST_STAMP=4294960000u to run the
 * stamp wrap-around path within a normal test run. */
#ifndef DS_HALL_FIRST_STAMP
#define DS_HALL_FIRST_STAMP 0u
#endif

typedef enum propagate_result {
    PROPAGATE_OK = 0,
    PROPAGATE_CONFLICT = 1,
    PROPAGATE_ABORT = 2
} propagate_result;

typedef struct u32_vec {
    uint32_t *data;
    uint32_t count;
    uint32_t capacity;
} u32_vec;

typedef struct hall_context {
    const ds_cnf *cnf;
    uint32_t variable_count;
    uint32_t code_count;          /* 2 * (variable_count + 1) */

    /* Normalized clauses of size >= 2: duplicates removed, tautologies
     * dropped. The arena is a private copy; propagation reorders it. */
    uint32_t *arena;
    uint32_t *clause_begin;
    uint32_t *clause_size;
    uint32_t clause_count;
    uint32_t *units;              /* unit clause codes */
    uint32_t unit_count;
    bool empty_clause;

    u32_vec *watches;             /* per code: clauses watching it */
    int8_t *values;               /* per variable */
    uint32_t *trail;
    uint32_t trail_count;
    uint32_t head;
    uint64_t visits;
    uint64_t budget;              /* base budget */
    bool adaptive;                /* budget grows with pairs found */
    bool watches_attached;
    bool out_of_memory;           /* a watch could not move */

    /* Candidate demands: clauses not satisfied at the root, with their
     * unassigned literals. */
    uint32_t *candidate_begin;
    uint32_t *candidate_size;
    uint32_t *candidate_source;   /* input clause index */
    uint32_t *candidate_codes;
    uint32_t candidate_count;

    /* Selected demands per policy, as candidate ids. */
    uint32_t *selection[HALL_POLICY_COUNT];
    uint32_t selection_count[HALL_POLICY_COUNT];

    uint8_t *interest;            /* per code: literal of some demand */
    uint8_t *refuted;             /* per code: probe reached a conflict */
    uint8_t *probed;              /* per code */
    u32_vec pairs;                /* conflict pairs, flattened (a, b) */

    /* Adjacency over codes, rebuilt from pairs. */
    uint32_t *adjacency_offsets;  /* code_count + 1 */
    uint32_t *adjacency;

    /* Attempt scratch. */
    uint32_t *owner;              /* per code: demand position + 1 */
    uint32_t *rank;               /* per code: position in demand order */
    uint64_t *keys;               /* sort scratch, per demand literal */
    uint32_t *group_of;           /* per code: group id, 0 none */
    uint32_t *mark;               /* per code: intersection stamp */
    uint32_t *owner_mark;         /* per demand: group serial using it */
    uint32_t stamp;
    uint32_t group_serial;
    u32_vec candidates;

    uint64_t ops;
} hall_context;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint32_t code_from_literal(ds_literal literal_value)
{
    uint32_t variable = (uint32_t)((literal_value > 0)
        ? literal_value : -literal_value);

    return (literal_value > 0) ? (variable << 1) : ((variable << 1) | 1u);
}

static ds_literal literal_from_code(uint32_t code)
{
    ds_literal variable = (ds_literal)(code >> 1);

    return ((code & 1u) == 0u) ? variable : -variable;
}

static int8_t code_value(const int8_t *values, uint32_t code)
{
    int8_t variable_value = values[code >> 1];

    return ((code & 1u) == 0u) ? variable_value : (int8_t)-variable_value;
}

static ds_status vec_push(u32_vec *vec, uint32_t value)
{
    if (vec->count == vec->capacity) {
        uint32_t grown;
        uint32_t *replacement;

        if (vec->capacity > UINT32_MAX / 2u) {
            return DS_ERR_CAPACITY;
        }
        grown = (vec->capacity == 0u) ? 4u : vec->capacity * 2u;
        replacement = (uint32_t *)realloc(vec->data,
            (size_t)grown * sizeof(uint32_t));
        if (replacement == NULL) {
            return DS_ERR_OUT_OF_MEMORY;
        }
        vec->data = replacement;
        vec->capacity = grown;
    }
    vec->data[vec->count] = value;
    vec->count++;
    return DS_OK;
}

/* A fresh value for the per-code mark array. Every membership test
 * compares a mark with the current stamp, so a wrapped counter could
 * match a mark left from long ago and admit a literal that does not
 * belong. Before the counter wraps, the marks are cleared. */
static uint32_t next_stamp(hall_context *context)
{
    if (context->stamp == UINT32_MAX) {
        memset(context->mark, 0,
            ((size_t)context->code_count + 1u) * sizeof(uint32_t));
        context->stamp = 0u;
    }
    context->stamp++;
    return context->stamp;
}

/* ------------------------------------------------------------------ */
/* propagation                                                         */
/* ------------------------------------------------------------------ */

static void assign(hall_context *context, uint32_t code)
{
    context->values[code >> 1] = ((code & 1u) == 0u) ? (int8_t)1
        : (int8_t)-1;
    context->trail[context->trail_count] = code;
    context->trail_count++;
}

static void undo_to(hall_context *context, uint32_t keep)
{
    while (context->trail_count > keep) {
        context->trail_count--;
        context->values[context->trail[context->trail_count] >> 1] = 0;
    }
    context->head = keep;
}

/* Two watched literals. limited: stop with PROPAGATE_ABORT once visits
 * pass the budget (the watch invariant still holds, so undo is safe).
 * A watch that cannot move for lack of memory also aborts, with
 * out_of_memory set, rather than mistake the clause for a unit. */
static propagate_result propagate(hall_context *context, bool limited)
{
    while (context->head < context->trail_count) {
        uint32_t false_code = context->trail[context->head] ^ 1u;
        u32_vec *list = &context->watches[false_code];
        uint32_t read_index = 0u;
        uint32_t write_index = 0u;

        context->head++;
        while (read_index < list->count) {
            uint32_t clause = list->data[read_index];
            uint32_t *codes = context->arena
                + context->clause_begin[clause];
            uint32_t size = context->clause_size[clause];
            uint32_t other;
            uint32_t scan;
            bool moved = false;

            context->visits++;
            read_index++;
            if (codes[0] == false_code) {
                codes[0] = codes[1];
                codes[1] = false_code;
            }
            other = codes[0];
            if (code_value(context->values, other) == 1) {
                list->data[write_index] = clause;
                write_index++;
                continue;
            }
            for (scan = 2u; scan < size; scan++) {
                if (code_value(context->values, codes[scan]) != -1) {
                    codes[1] = codes[scan];
                    codes[scan] = false_code;
                    if (vec_push(&context->watches[codes[1]], clause)
                        != DS_OK) {
                        codes[scan] = codes[1];
                        codes[1] = false_code;
                        context->out_of_memory = true;
                    } else {
                        moved = true;
                    }
                    break;
                }
            }
            if (moved) {
                continue;
            }
            if (context->out_of_memory) {
                /* Keep this clause and the rest of the list watched. */
                list->data[write_index] = clause;
                write_index++;
                while (read_index < list->count) {
                    list->data[write_index] = list->data[read_index];
                    write_index++;
                    read_index++;
                }
                list->count = write_index;
                return PROPAGATE_ABORT;
            }
            list->data[write_index] = clause;
            write_index++;
            if (code_value(context->values, other) == -1) {
                while (read_index < list->count) {
                    list->data[write_index] = list->data[read_index];
                    write_index++;
                    read_index++;
                }
                list->count = write_index;
                return PROPAGATE_CONFLICT;
            }
            if (code_value(context->values, other) == 0) {
                assign(context, other);
            }
        }
        list->count = write_index;
        if (limited && context->visits > context->budget) {
            uint64_t earned = context->adaptive
                ? HALL_BUDGET_PER_PAIR * (uint64_t)(context->pairs.count / 2u)
                : 0u;
            uint64_t limit = context->budget + earned;

            if (limit > HALL_BUDGET_CAP && context->adaptive) {
                limit = HALL_BUDGET_CAP;
            }
            if (context->visits > limit) {
                return PROPAGATE_ABORT;
            }
        }
    }
    return PROPAGATE_OK;
}

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */

static ds_status load_clauses(hall_context *context)
{
    const ds_cnf *cnf = context->cnf;
    uint32_t *scratch = NULL;
    uint32_t longest = 0u;
    uint32_t clause_index;
    uint32_t arena_count = 0u;
    ds_status status = DS_OK;

    for (clause_index = 0u; clause_index < cnf->clause_count;
        clause_index++) {
        uint32_t size = cnf->offsets[clause_index + 1u]
            - cnf->offsets[clause_index];

        if (size > longest) {
            longest = size;
        }
    }
    scratch = (uint32_t *)malloc(((size_t)longest + 1u) * sizeof(uint32_t));
    context->arena = (uint32_t *)malloc(
        ((size_t)cnf->literal_count + 1u) * sizeof(uint32_t));
    context->clause_begin = (uint32_t *)malloc(
        ((size_t)cnf->clause_count + 1u) * sizeof(uint32_t));
    context->clause_size = (uint32_t *)malloc(
        ((size_t)cnf->clause_count + 1u) * sizeof(uint32_t));
    context->units = (uint32_t *)malloc(
        ((size_t)cnf->clause_count + 1u) * sizeof(uint32_t));
    if (scratch == NULL || context->arena == NULL
        || context->clause_begin == NULL || context->clause_size == NULL
        || context->units == NULL) {
        status = DS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (clause_index = 0u; clause_index < cnf->clause_count;
        clause_index++) {
        uint32_t begin = cnf->offsets[clause_index];
        uint32_t end = cnf->offsets[clause_index + 1u];
        uint32_t size = 0u;
        bool tautology = false;
        uint32_t position;

        for (position = begin; position < end && !tautology; position++) {
            uint32_t code = code_from_literal(cnf->literals[position]);
            bool duplicate = false;
            uint32_t check;

            for (check = 0u; check < size; check++) {
                if (scratch[check] == code) {
                    duplicate = true;
                    break;
                }
                if (scratch[check] == (code ^ 1u)) {
                    tautology = true;
                    break;
                }
            }
            if (!duplicate && !tautology) {
                scratch[size] = code;
                size++;
            }
        }
        context->ops += end - begin;
        if (tautology) {
            continue;
        }
        if (size == 0u) {
            context->empty_clause = true;
            continue;
        }
        if (size == 1u) {
            context->units[context->unit_count] = scratch[0];
            context->unit_count++;
            continue;
        }
        context->clause_begin[context->clause_count] = arena_count;
        context->clause_size[context->clause_count] = size;
        memcpy(context->arena + arena_count, scratch,
            (size_t)size * sizeof(uint32_t));
        arena_count += size;
        context->clause_count++;
    }
cleanup:
    free(scratch);
    return status;
}

/* Watch the first two literals of every clause. Done only when
 * propagation is needed: a formula without units that phase one settles
 * never pays for it. Lists are sized exactly up front. */
static ds_status attach_watches(hall_context *context)
{
    uint32_t *counts;
    uint32_t clause;
    uint32_t code;

    if (context->watches_attached) {
        return DS_OK;
    }
    counts = (uint32_t *)calloc((size_t)context->code_count + 1u,
        sizeof(uint32_t));
    if (counts == NULL) {
        return DS_ERR_OUT_OF_MEMORY;
    }
    for (clause = 0u; clause < context->clause_count; clause++) {
        const uint32_t *codes = context->arena
            + context->clause_begin[clause];

        counts[codes[0]]++;
        counts[codes[1]]++;
    }
    for (code = 0u; code < context->code_count; code++) {
        if (counts[code] == 0u) {
            continue;
        }
        context->watches[code].data = (uint32_t *)malloc(
            (size_t)counts[code] * sizeof(uint32_t));
        if (context->watches[code].data == NULL) {
            free(counts);
            return DS_ERR_OUT_OF_MEMORY;
        }
        context->watches[code].capacity = counts[code];
    }
    free(counts);
    for (clause = 0u; clause < context->clause_count; clause++) {
        const uint32_t *codes = context->arena
            + context->clause_begin[clause];
        u32_vec *first = &context->watches[codes[0]];
        u32_vec *second = &context->watches[codes[1]];

        first->data[first->count] = clause;
        first->count++;
        second->data[second->count] = clause;
        second->count++;
    }
    context->ops += context->clause_count;
    context->watches_attached = true;
    return DS_OK;
}

/* Root propagation of the units. Sets *out_conflict on a conflict. */
static ds_status propagate_root(hall_context *context, bool *out_conflict)
{
    uint32_t index;
    propagate_result result;

    *out_conflict = false;
    if (context->empty_clause) {
        *out_conflict = true;
        return DS_OK;
    }
    for (index = 0u; index < context->unit_count; index++) {
        int8_t value = code_value(context->values, context->units[index]);

        if (value == -1) {
            *out_conflict = true;
            return DS_OK;
        }
        if (value == 0) {
            assign(context, context->units[index]);
        }
    }
    if (context->trail_count > 0u) {
        ds_status status = attach_watches(context);

        if (status != DS_OK) {
            return status;
        }
    }
    result = propagate(context, false);
    if (context->out_of_memory) {
        return DS_ERR_OUT_OF_MEMORY;
    }
    *out_conflict = (result == PROPAGATE_CONFLICT);
    return DS_OK;
}

/* Candidates in input order, keeping only unassigned literals. The input
 * clause index is recovered by re-normalizing, which keeps load_clauses
 * free of a second index array. */
static ds_status collect_candidates(hall_context *context)
{
    const ds_cnf *cnf = context->cnf;
    uint32_t clause_index;
    uint32_t total = 0u;

    context->candidate_begin = (uint32_t *)malloc(
        ((size_t)cnf->clause_count + 1u) * sizeof(uint32_t));
    context->candidate_size = (uint32_t *)malloc(
        ((size_t)cnf->clause_count + 1u) * sizeof(uint32_t));
    context->candidate_source = (uint32_t *)malloc(
        ((size_t)cnf->clause_count + 1u) * sizeof(uint32_t));
    context->candidate_codes = (uint32_t *)malloc(
        ((size_t)cnf->literal_count + 1u) * sizeof(uint32_t));
    if (context->candidate_begin == NULL || context->candidate_size == NULL
        || context->candidate_source == NULL
        || context->candidate_codes == NULL) {
        return DS_ERR_OUT_OF_MEMORY;
    }
    /* Work from the input so the source index is exact. A literal is kept
     * once (stamp by code) and tautologies are skipped. */
    for (clause_index = 0u; clause_index < cnf->clause_count;
        clause_index++) {
        uint32_t begin = cnf->offsets[clause_index];
        uint32_t end = cnf->offsets[clause_index + 1u];
        uint32_t start = total;
        uint32_t position;
        bool satisfied = false;
        bool tautology = false;

        (void)next_stamp(context);
        for (position = begin; position < end; position++) {
            uint32_t code = code_from_literal(cnf->literals[position]);
            int8_t value = code_value(context->values, code);

            if (context->mark[code ^ 1u] == context->stamp) {
                tautology = true;
                break;
            }
            if (value == 1) {
                satisfied = true;
                break;
            }
            if (value == -1 || context->mark[code] == context->stamp) {
                continue;
            }
            context->mark[code] = context->stamp;
            context->candidate_codes[total] = code;
            total++;
        }
        context->ops += end - begin;
        if (satisfied || tautology || total - start < 2u) {
            total = start;
            continue;
        }
        context->candidate_begin[context->candidate_count] = start;
        context->candidate_size[context->candidate_count] = total - start;
        context->candidate_source[context->candidate_count] = clause_index;
        context->candidate_count++;
    }
    return DS_OK;
}

/*
 * Order for policy 2. Clauses are linked when they share a literal. A
 * component is clean when every literal of its clauses occurs in at most
 * two candidates (so the links are the whole story) and it is
 * two-colorable; the larger color class of each clean component comes
 * first, in input order. Everything else follows longest first, and among
 * equal lengths the clause whose most shared literal is shared least
 * first: a pigeon's clause before an auxiliary clause that reuses one of
 * its literals. Greedy selection over this order keeps the result
 * literal-disjoint.
 */
static ds_status bipartite_order(hall_context *context, uint32_t *order)
{
    uint32_t count = context->candidate_count;
    uint32_t *occurrence_offsets = NULL;
    uint32_t *occurrences = NULL;
    uint32_t *fill = NULL;
    uint8_t *color = NULL;       /* 0 unset, 1 or 2 */
    uint8_t *preferred = NULL;
    uint32_t *queue = NULL;
    uint32_t candidate;
    uint32_t written = 0u;
    uint32_t code;
    ds_status status = DS_OK;

    occurrence_offsets = (uint32_t *)calloc((size_t)context->code_count + 1u,
        sizeof(uint32_t));
    fill = (uint32_t *)calloc((size_t)context->code_count + 1u,
        sizeof(uint32_t));
    color = (uint8_t *)calloc((size_t)count + 1u, 1u);
    preferred = (uint8_t *)calloc((size_t)count + 1u, 1u);
    queue = (uint32_t *)malloc(((size_t)count + 1u) * sizeof(uint32_t));
    if (occurrence_offsets == NULL || fill == NULL || color == NULL
        || preferred == NULL || queue == NULL) {
        status = DS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (candidate = 0u; candidate < count; candidate++) {
        const uint32_t *codes = context->candidate_codes
            + context->candidate_begin[candidate];
        uint32_t position;

        for (position = 0u; position < context->candidate_size[candidate];
            position++) {
            occurrence_offsets[codes[position] + 1u]++;
        }
    }
    for (code = 0u; code < context->code_count; code++) {
        occurrence_offsets[code + 1u] += occurrence_offsets[code];
    }
    occurrences = (uint32_t *)malloc(
        ((size_t)occurrence_offsets[context->code_count] + 1u)
        * sizeof(uint32_t));
    if (occurrences == NULL) {
        status = DS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (candidate = 0u; candidate < count; candidate++) {
        const uint32_t *codes = context->candidate_codes
            + context->candidate_begin[candidate];
        uint32_t position;

        for (position = 0u; position < context->candidate_size[candidate];
            position++) {
            uint32_t literal_code = codes[position];

            occurrences[occurrence_offsets[literal_code]
                + fill[literal_code]] = candidate;
            fill[literal_code]++;
        }
    }
    for (candidate = 0u; candidate < count; candidate++) {
        uint32_t head = 0u;
        uint32_t tail = 0u;
        uint32_t sides[2] = { 0u, 0u };
        bool clean = true;
        uint32_t index;

        if (color[candidate] != 0u) {
            continue;
        }
        color[candidate] = 1u;
        queue[tail] = candidate;
        tail++;
        while (head < tail) {
            uint32_t current = queue[head];
            const uint32_t *codes = context->candidate_codes
                + context->candidate_begin[current];
            uint32_t position;

            head++;
            sides[color[current] - 1u]++;
            for (position = 0u; position < context->candidate_size[current];
                position++) {
                uint32_t literal_code = codes[position];
                uint32_t begin = occurrence_offsets[literal_code];
                uint32_t end = occurrence_offsets[literal_code + 1u];
                uint32_t scan;

                context->ops++;
                if (end - begin > 2u) {
                    clean = false;
                    continue;
                }
                for (scan = begin; scan < end; scan++) {
                    uint32_t other = occurrences[scan];

                    if (other == current) {
                        continue;
                    }
                    if (color[other] == 0u) {
                        color[other] = (uint8_t)(3u - color[current]);
                        queue[tail] = other;
                        tail++;
                    } else if (color[other] == color[current]) {
                        clean = false;
                    }
                }
            }
        }
        if (clean) {
            uint8_t larger = (sides[1] > sides[0]) ? 2u : 1u;

            for (index = 0u; index < tail; index++) {
                if (color[queue[index]] == larger) {
                    preferred[queue[index]] = 1u;
                }
            }
        }
    }
    for (candidate = 0u; candidate < count; candidate++) {
        if (preferred[candidate] != 0u) {
            order[written] = candidate;
            written++;
        }
    }
    {
        /* The rest: stable counting sorts, by most-shared literal
         * ascending and then by size descending (least significant key
         * first), starting from input order. */
        uint32_t *rest = NULL;
        uint32_t *sorted = NULL;
        uint32_t *starts = NULL;
        uint32_t *share = NULL;
        uint32_t rest_count = 0u;
        uint32_t widest_share = 0u;
        uint32_t longest = 0u;
        uint32_t index;

        rest = (uint32_t *)malloc(((size_t)count + 1u) * sizeof(uint32_t));
        sorted = (uint32_t *)malloc(((size_t)count + 1u) * sizeof(uint32_t));
        share = (uint32_t *)malloc(((size_t)count + 1u) * sizeof(uint32_t));
        if (rest == NULL || sorted == NULL || share == NULL) {
            free(rest);
            free(sorted);
            free(share);
            status = DS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        for (candidate = 0u; candidate < count; candidate++) {
            const uint32_t *codes = context->candidate_codes
                + context->candidate_begin[candidate];
            uint32_t position;
            uint32_t most = 0u;

            if (preferred[candidate] != 0u) {
                continue;
            }
            for (position = 0u; position < context->candidate_size[candidate];
                position++) {
                uint32_t occurrences_here =
                    occurrence_offsets[codes[position] + 1u]
                    - occurrence_offsets[codes[position]];

                if (occurrences_here > most) {
                    most = occurrences_here;
                }
            }
            share[candidate] = most;
            if (most > widest_share) {
                widest_share = most;
            }
            if (context->candidate_size[candidate] > longest) {
                longest = context->candidate_size[candidate];
            }
            rest[rest_count] = candidate;
            rest_count++;
        }
        starts = (uint32_t *)calloc((size_t)((widest_share > longest)
            ? widest_share : longest) + 2u, sizeof(uint32_t));
        if (starts == NULL) {
            free(rest);
            free(sorted);
            free(share);
            status = DS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        for (index = 0u; index < rest_count; index++) {
            starts[share[rest[index]] + 1u]++;
        }
        for (index = 0u; index <= widest_share; index++) {
            starts[index + 1u] += starts[index];
        }
        for (index = 0u; index < rest_count; index++) {
            sorted[starts[share[rest[index]]]] = rest[index];
            starts[share[rest[index]]]++;
        }
        memset(starts, 0, ((size_t)((widest_share > longest) ? widest_share
            : longest) + 2u) * sizeof(uint32_t));
        for (index = 0u; index < rest_count; index++) {
            starts[longest - context->candidate_size[sorted[index]] + 1u]++;
        }
        for (index = 0u; index <= longest; index++) {
            starts[index + 1u] += starts[index];
        }
        for (index = 0u; index < rest_count; index++) {
            uint32_t slot = longest - context->candidate_size[sorted[index]];

            order[written + starts[slot]] = sorted[index];
            starts[slot]++;
        }
        written += rest_count;
        context->ops += 2u * (uint64_t)rest_count;
        free(rest);
        free(sorted);
        free(share);
        free(starts);
    }
cleanup:
    free(occurrence_offsets);
    free(occurrences);
    free(fill);
    free(color);
    free(preferred);
    free(queue);
    return status;
}

/* Greedy literal-disjoint selection in the given order. */
static ds_status select_demands(hall_context *context, uint32_t policy)
{
    uint32_t *order = NULL;
    uint32_t *chosen;
    uint32_t index;

    chosen = (uint32_t *)malloc(
        ((size_t)context->candidate_count + 1u) * sizeof(uint32_t));
    order = (uint32_t *)malloc(
        ((size_t)context->candidate_count + 1u) * sizeof(uint32_t));
    if (chosen == NULL || order == NULL) {
        free(chosen);
        free(order);
        return DS_ERR_OUT_OF_MEMORY;
    }
    if (policy == 0u) {
        for (index = 0u; index < context->candidate_count; index++) {
            order[index] = index;
        }
    } else if (policy == 2u) {
        ds_status status = bipartite_order(context, order);

        if (status != DS_OK) {
            free(chosen);
            free(order);
            return status;
        }
    } else {
        /* Stable counting sort by decreasing size. */
        uint32_t longest = 0u;
        uint32_t *starts;
        uint32_t size;

        for (index = 0u; index < context->candidate_count; index++) {
            if (context->candidate_size[index] > longest) {
                longest = context->candidate_size[index];
            }
        }
        starts = (uint32_t *)calloc((size_t)longest + 2u, sizeof(uint32_t));
        if (starts == NULL) {
            free(chosen);
            free(order);
            return DS_ERR_OUT_OF_MEMORY;
        }
        for (index = 0u; index < context->candidate_count; index++) {
            starts[longest - context->candidate_size[index] + 1u]++;
        }
        for (size = 0u; size <= longest; size++) {
            starts[size + 1u] += starts[size];
        }
        for (index = 0u; index < context->candidate_count; index++) {
            uint32_t slot = longest - context->candidate_size[index];

            order[starts[slot]] = index;
            starts[slot]++;
        }
        free(starts);
        context->ops += context->candidate_count;
    }
    (void)next_stamp(context);
    context->selection_count[policy] = 0u;
    for (index = 0u; index < context->candidate_count; index++) {
        uint32_t candidate = order[index];
        const uint32_t *codes = context->candidate_codes
            + context->candidate_begin[candidate];
        uint32_t size = context->candidate_size[candidate];
        uint32_t position;
        bool disjoint = true;

        context->ops += size;
        for (position = 0u; position < size; position++) {
            if (context->mark[codes[position]] == context->stamp) {
                disjoint = false;
                break;
            }
        }
        if (!disjoint) {
            continue;
        }
        for (position = 0u; position < size; position++) {
            context->mark[codes[position]] = context->stamp;
            context->interest[codes[position]] = 1u;
        }
        chosen[context->selection_count[policy]] = candidate;
        context->selection_count[policy]++;
    }
    free(order);
    context->selection[policy] = chosen;
    return DS_OK;
}

/* ------------------------------------------------------------------ */
/* conflict edges                                                      */
/* ------------------------------------------------------------------ */

static ds_status record_pair(hall_context *context, uint32_t first,
    uint32_t second)
{
    ds_status status = vec_push(&context->pairs, first);

    if (status == DS_OK) {
        status = vec_push(&context->pairs, second);
    }
    return status;
}

/* Phase one: a clause that is binary over unassigned literals,
 * (x or y), says at most one of -x, -y is true. */
static ds_status binary_edges(hall_context *context, ds_hall_report *report)
{
    uint32_t candidate;

    for (candidate = 0u; candidate < context->candidate_count;
        candidate++) {
        const uint32_t *codes = context->candidate_codes
            + context->candidate_begin[candidate];
        uint32_t first;
        uint32_t second;

        context->ops++;
        if (context->candidate_size[candidate] != 2u) {
            continue;
        }
        first = codes[0] ^ 1u;
        second = codes[1] ^ 1u;
        if (context->interest[first] != 0u
            && context->interest[second] != 0u) {
            if (context->pairs.count / 2u >= DS_HALL_EDGE_CAP) {
                report->budget_exhausted = true;
                break;
            }
            ds_status status = record_pair(context, first, second);

            if (status != DS_OK) {
                return status;
            }
        }
    }
    return DS_OK;
}

/* Probe one literal: record every demand literal it makes false, or
 * mark it refuted. *out_stop is set when the budget or the pair cap is
 * reached (the partial edges stay valid). */
static ds_status probe_literal(hall_context *context, uint32_t probe,
    ds_hall_report *report, bool *out_stop)
{
    uint32_t root = context->trail_count;
    propagate_result result;
    uint32_t trail_index;
    ds_status status = DS_OK;

    *out_stop = false;
    if (context->probed[probe] != 0u) {
        return DS_OK;
    }
    context->probed[probe] = 1u;
    report->probes++;
    assign(context, probe);
    result = propagate(context, true);
    if (result == PROPAGATE_ABORT) {
        undo_to(context, root);
        if (context->out_of_memory) {
            return DS_ERR_OUT_OF_MEMORY;
        }
        report->budget_exhausted = true;
        *out_stop = true;
        return DS_OK;
    }
    if (result == PROPAGATE_CONFLICT) {
        context->refuted[probe] = 1u;
        report->refuted_literals++;
    } else {
        for (trail_index = root + 1u; trail_index < context->trail_count;
            trail_index++) {
            uint32_t forced_false = context->trail[trail_index] ^ 1u;

            if (context->interest[forced_false] == 0u) {
                continue;
            }
            if (context->pairs.count / 2u >= DS_HALL_EDGE_CAP) {
                report->budget_exhausted = true;
                *out_stop = true;
                break;
            }
            status = record_pair(context, probe, forced_false);
            if (status != DS_OK) {
                break;
            }
        }
    }
    undo_to(context, root);
    return status;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t first = *(const uint64_t *)left;
    uint64_t second = *(const uint64_t *)right;

    return (first < second) ? -1 : (first > second) ? 1 : 0;
}

/* Symmetric, duplicate-free adjacency from the recorded pairs. */
static ds_status build_adjacency(hall_context *context,
    ds_hall_report *report)
{
    uint32_t *offsets;
    uint32_t *fill = NULL;
    uint32_t pair_count = context->pairs.count / 2u;
    uint32_t index;
    uint32_t code;
    uint32_t write;
    uint64_t distinct = 0u;

    free(context->adjacency_offsets);
    free(context->adjacency);
    context->adjacency_offsets = NULL;
    context->adjacency = NULL;
    offsets = (uint32_t *)calloc((size_t)context->code_count + 1u,
        sizeof(uint32_t));
    if (offsets == NULL) {
        return DS_ERR_OUT_OF_MEMORY;
    }
    context->adjacency_offsets = offsets;
    for (index = 0u; index < pair_count; index++) {
        uint32_t first = context->pairs.data[2u * index];
        uint32_t second = context->pairs.data[2u * index + 1u];

        if (first == second) {
            continue;
        }
        offsets[first + 1u]++;
        offsets[second + 1u]++;
    }
    for (code = 0u; code < context->code_count; code++) {
        offsets[code + 1u] += offsets[code];
    }
    context->adjacency = (uint32_t *)malloc(
        ((size_t)offsets[context->code_count] + 1u) * sizeof(uint32_t));
    fill = (uint32_t *)calloc((size_t)context->code_count + 1u,
        sizeof(uint32_t));
    if (context->adjacency == NULL || fill == NULL) {
        free(fill);
        return DS_ERR_OUT_OF_MEMORY;
    }
    for (index = 0u; index < pair_count; index++) {
        uint32_t first = context->pairs.data[2u * index];
        uint32_t second = context->pairs.data[2u * index + 1u];

        if (first == second) {
            continue;
        }
        context->adjacency[offsets[first] + fill[first]] = second;
        fill[first]++;
        context->adjacency[offsets[second] + fill[second]] = first;
        fill[second]++;
    }
    /* Deduplicate each row with a stamp, compacting in place. Rows keep
     * insertion order, which is deterministic; nothing needs them sorted
     * (clique growth orders its candidates itself). */
    write = 0u;
    for (code = 0u; code < context->code_count; code++) {
        uint32_t begin = offsets[code];
        uint32_t end = begin + fill[code];
        uint32_t row_start = write;
        uint32_t position;

        (void)next_stamp(context);
        for (position = begin; position < end; position++) {
            uint32_t neighbor = context->adjacency[position];

            if (context->mark[neighbor] == context->stamp) {
                continue;
            }
            context->mark[neighbor] = context->stamp;
            context->adjacency[write] = neighbor;
            write++;
        }
        offsets[code] = row_start;
        distinct += write - row_start;
        context->ops += end - begin;
    }
    offsets[context->code_count] = write;
    report->conflict_edges = distinct / 2u;
    free(fill);
    return DS_OK;
}

/* ------------------------------------------------------------------ */
/* groups and matching                                                 */
/* ------------------------------------------------------------------ */

static bool joinable(const hall_context *context, uint32_t code,
    bool transversal)
{
    return context->owner[code] != 0u && context->refuted[code] == 0u
        && context->group_of[code] == 0u
        && (!transversal || context->owner_mark[context->owner[code] - 1u]
            != context->group_serial);
}

static void add_member(hall_context *context, uint32_t code,
    uint32_t group, bool transversal)
{
    context->group_of[code] = group;
    if (transversal) {
        context->owner_mark[context->owner[code] - 1u] =
            context->group_serial;
    }
}

/* Greedy clique partition of the demand literals. Returns the number of
 * groups. Members of a group are pairwise adjacent: the candidate set is
 * the running intersection of the members' neighborhoods. */
static ds_status build_groups(hall_context *context, const uint32_t *demands,
    uint32_t demand_count, bool transversal, uint32_t *out_groups)
{
    uint32_t groups = 0u;
    uint32_t index;

    for (index = 0u; index < demand_count; index++) {
        uint32_t candidate = demands[index];
        const uint32_t *codes = context->candidate_codes
            + context->candidate_begin[candidate];
        uint32_t size = context->candidate_size[candidate];
        uint32_t position;

        for (position = 0u; position < size; position++) {
            uint32_t seed = codes[position];
            uint32_t edge;
            uint32_t scan;
            uint32_t current;

            if (context->refuted[seed] != 0u
                || context->group_of[seed] != 0u) {
                continue;
            }
            groups++;
            context->group_serial++;
            add_member(context, seed, groups, transversal);
            current = next_stamp(context);
            context->candidates.count = 0u;
            for (edge = context->adjacency_offsets[seed];
                edge < context->adjacency_offsets[seed + 1u]; edge++) {
                uint32_t neighbor = context->adjacency[edge];

                context->ops++;
                if (joinable(context, neighbor, transversal)) {
                    context->keys[context->candidates.count] =
                        ((uint64_t)context->rank[neighbor] << 32)
                        | neighbor;
                    context->candidates.count++;
                    context->mark[neighbor] = current;
                }
            }
            /* Grow in demand order: in a pigeonhole formula that puts
             * the other pigeons of this hole ahead of auxiliary literals,
             * whatever the variable numbering. */
            if (context->candidates.count > 1u) {
                qsort(context->keys, context->candidates.count,
                    sizeof(uint64_t), compare_u64);
            }
            for (scan = 0u; scan < context->candidates.count; scan++) {
                context->candidates.data[scan] =
                    (uint32_t)(context->keys[scan] & UINT32_MAX);
            }
            context->ops += context->candidates.count;
            for (scan = 0u; scan < context->candidates.count; scan++) {
                uint32_t member = context->candidates.data[scan];
                uint32_t next;

                if (context->mark[member] != current
                    || !joinable(context, member, transversal)) {
                    continue;
                }
                add_member(context, member, groups, transversal);
                next = next_stamp(context);
                for (edge = context->adjacency_offsets[member];
                    edge < context->adjacency_offsets[member + 1u];
                    edge++) {
                    uint32_t neighbor = context->adjacency[edge];

                    context->ops++;
                    if (context->mark[neighbor] == current) {
                        context->mark[neighbor] = next;
                    }
                }
                current = next;
            }
        }
    }
    *out_groups = groups;
    return DS_OK;
}

typedef struct bipartite {
    uint32_t left_count;
    uint32_t right_count;         /* groups are 1 .. right_count */
    uint32_t *offsets;            /* left_count + 1 */
    uint32_t *edges;              /* group ids */
    uint32_t *match_left;         /* group or HALL_NONE */
    uint32_t *match_right;        /* left or HALL_NONE, by group id */
    uint32_t *distance;
    uint32_t *iterator;
    uint32_t *stack;
    uint32_t *queue;
} bipartite;

static void bipartite_free(bipartite *graph)
{
    free(graph->offsets);
    free(graph->edges);
    free(graph->match_left);
    free(graph->match_right);
    free(graph->distance);
    free(graph->iterator);
    free(graph->stack);
    free(graph->queue);
    memset(graph, 0, sizeof(*graph));
}

static ds_status bipartite_build(hall_context *context,
    const uint32_t *demands, uint32_t demand_count, uint32_t groups,
    bipartite *graph)
{
    uint32_t *seen = NULL; /* by group: demand index + 1 */
    uint32_t total = 0u;
    uint32_t index;
    size_t left_slots = (size_t)demand_count + 1u;
    size_t right_slots = (size_t)groups + 1u;

    memset(graph, 0, sizeof(*graph));
    graph->left_count = demand_count;
    graph->right_count = groups;
    for (index = 0u; index < demand_count; index++) {
        total += context->candidate_size[demands[index]];
    }
    graph->offsets = (uint32_t *)calloc(left_slots, sizeof(uint32_t));
    graph->edges = (uint32_t *)malloc(((size_t)total + 1u)
        * sizeof(uint32_t));
    graph->match_left = (uint32_t *)malloc(left_slots * sizeof(uint32_t));
    graph->match_right = (uint32_t *)malloc(right_slots
        * sizeof(uint32_t));
    graph->distance = (uint32_t *)malloc(left_slots * sizeof(uint32_t));
    graph->iterator = (uint32_t *)malloc(left_slots * sizeof(uint32_t));
    graph->stack = (uint32_t *)malloc(left_slots * sizeof(uint32_t));
    graph->queue = (uint32_t *)malloc(left_slots * sizeof(uint32_t));
    seen = (uint32_t *)calloc(right_slots, sizeof(uint32_t));
    if (graph->offsets == NULL || graph->edges == NULL
        || graph->match_left == NULL || graph->match_right == NULL
        || graph->distance == NULL || graph->iterator == NULL
        || graph->stack == NULL || graph->queue == NULL || seen == NULL) {
        free(seen);
        bipartite_free(graph);
        return DS_ERR_OUT_OF_MEMORY;
    }
    total = 0u;
    for (index = 0u; index < demand_count; index++) {
        uint32_t candidate = demands[index];
        const uint32_t *codes = context->candidate_codes
            + context->candidate_begin[candidate];
        uint32_t size = context->candidate_size[candidate];
        uint32_t position;

        graph->offsets[index] = total;
        for (position = 0u; position < size; position++) {
            uint32_t group = context->group_of[codes[position]];

            context->ops++;
            if (group == 0u || seen[group] == index + 1u) {
                continue; /* refuted literal, or group already listed */
            }
            seen[group] = index + 1u;
            graph->edges[total] = group;
            total++;
        }
    }
    graph->offsets[demand_count] = total;
    for (index = 0u; index < demand_count; index++) {
        graph->match_left[index] = HALL_NONE;
    }
    for (index = 0u; index <= groups; index++) {
        graph->match_right[index] = HALL_NONE;
    }
    free(seen);
    return DS_OK;
}

/* Hopcroft-Karp with an explicit stack. Returns the matching size. */
static uint32_t bipartite_match(bipartite *graph, uint64_t *ops)
{
    uint32_t matched = 0u;
    uint32_t left;

    /* Greedy start. */
    for (left = 0u; left < graph->left_count; left++) {
        uint32_t edge;

        for (edge = graph->offsets[left]; edge < graph->offsets[left + 1u];
            edge++) {
            uint32_t group = graph->edges[edge];

            (*ops)++;
            if (graph->match_right[group] == HALL_NONE) {
                graph->match_right[group] = left;
                graph->match_left[left] = group;
                matched++;
                break;
            }
        }
    }
    for (;;) {
        uint32_t queue_head = 0u;
        uint32_t queue_tail = 0u;
        bool reachable_free = false;

        for (left = 0u; left < graph->left_count; left++) {
            if (graph->match_left[left] == HALL_NONE) {
                graph->distance[left] = 0u;
                graph->queue[queue_tail] = left;
                queue_tail++;
            } else {
                graph->distance[left] = HALL_NONE;
            }
        }
        while (queue_head < queue_tail) {
            uint32_t current = graph->queue[queue_head];
            uint32_t edge;

            queue_head++;
            for (edge = graph->offsets[current];
                edge < graph->offsets[current + 1u]; edge++) {
                uint32_t partner = graph->match_right[graph->edges[edge]];

                (*ops)++;
                if (partner == HALL_NONE) {
                    reachable_free = true;
                } else if (graph->distance[partner] == HALL_NONE) {
                    graph->distance[partner] =
                        graph->distance[current] + 1u;
                    graph->queue[queue_tail] = partner;
                    queue_tail++;
                }
            }
        }
        if (!reachable_free) {
            break;
        }
        for (left = 0u; left < graph->left_count; left++) {
            graph->iterator[left] = graph->offsets[left];
        }
        for (left = 0u; left < graph->left_count; left++) {
            uint32_t depth;

            if (graph->match_left[left] != HALL_NONE
                || graph->distance[left] != 0u) {
                continue;
            }
            depth = 0u;
            graph->stack[depth] = left;
            depth++;
            while (depth > 0u) {
                uint32_t current = graph->stack[depth - 1u];
                uint32_t group;
                uint32_t partner;

                if (graph->iterator[current]
                    == graph->offsets[current + 1u]) {
                    graph->distance[current] = HALL_NONE;
                    depth--;
                    continue;
                }
                group = graph->edges[graph->iterator[current]];
                partner = graph->match_right[group];
                (*ops)++;
                if (partner == HALL_NONE) {
                    /* Augment along the stack: each stacked node takes
                     * the group its iterator points at. */
                    while (depth > 0u) {
                        uint32_t node = graph->stack[depth - 1u];
                        uint32_t taken =
                            graph->edges[graph->iterator[node]];

                        graph->match_left[node] = taken;
                        graph->match_right[taken] = node;
                        depth--;
                    }
                    matched++;
                    break;
                }
                if (graph->distance[partner] != HALL_NONE
                    && graph->distance[partner]
                        == graph->distance[current] + 1u) {
                    graph->stack[depth] = partner;
                    depth++;
                } else {
                    graph->iterator[current]++;
                }
            }
        }
    }
    return matched;
}

/* ------------------------------------------------------------------ */
/* certificate                                                         */
/* ------------------------------------------------------------------ */

/* From a maximum matching that leaves demands uncovered: the demands and
 * groups reachable by alternating paths from the uncovered demands. All
 * reached groups are matched to reached demands, so the reached demands
 * outnumber the reached groups by the uncovered count. */
static ds_status extract_violator(hall_context *context,
    const uint32_t *demands, bipartite *graph, ds_hall_report *report,
    ds_count_certificate *out_certificate)
{
    uint8_t *left_seen = NULL;
    uint32_t *group_slot = NULL; /* by group id: certificate slot + 1 */
    uint32_t *fill = NULL;
    ds_count_certificate certificate;
    uint32_t queue_tail = 0u;
    uint32_t queue_head = 0u;
    uint32_t left_reached = 0u;
    uint32_t right_reached = 0u;
    uint32_t left;
    ds_status status = DS_OK;

    memset(&certificate, 0, sizeof(certificate));
    left_seen = (uint8_t *)calloc((size_t)graph->left_count + 1u, 1u);
    group_slot = (uint32_t *)calloc((size_t)graph->right_count + 1u,
        sizeof(uint32_t));
    if (left_seen == NULL || group_slot == NULL) {
        status = DS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (left = 0u; left < graph->left_count; left++) {
        if (graph->match_left[left] == HALL_NONE) {
            left_seen[left] = 1u;
            graph->queue[queue_tail] = left;
            queue_tail++;
        }
    }
    while (queue_head < queue_tail) {
        uint32_t current = graph->queue[queue_head];
        uint32_t edge;

        queue_head++;
        left_reached++;
        for (edge = graph->offsets[current];
            edge < graph->offsets[current + 1u]; edge++) {
            uint32_t group = graph->edges[edge];
            uint32_t partner;

            if (group_slot[group] != 0u) {
                continue;
            }
            right_reached++;
            group_slot[group] = right_reached;
            partner = graph->match_right[group];
            if (partner != HALL_NONE && left_seen[partner] == 0u) {
                left_seen[partner] = 1u;
                graph->queue[queue_tail] = partner;
                queue_tail++;
            }
        }
    }
    report->violator_demands = left_reached;
    report->violator_groups = right_reached;
    if (out_certificate == NULL) {
        goto cleanup;
    }

    /* Clauses in BFS order; group members restricted to their literals. */
    certificate.clause_count = left_reached;
    certificate.group_count = right_reached;
    certificate.clauses = (uint32_t *)malloc(((size_t)left_reached + 1u)
        * sizeof(uint32_t));
    certificate.group_offsets = (uint32_t *)calloc(
        (size_t)right_reached + 2u, sizeof(uint32_t));
    fill = (uint32_t *)calloc((size_t)right_reached + 1u, sizeof(uint32_t));
    if (certificate.clauses == NULL || certificate.group_offsets == NULL
        || fill == NULL) {
        status = DS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    {
        uint32_t member_total = 0u;
        uint32_t refuted_total = 0u;
        uint32_t index;

        for (index = 0u; index < left_reached; index++) {
            uint32_t candidate = demands[graph->queue[index]];
            const uint32_t *codes = context->candidate_codes
                + context->candidate_begin[candidate];
            uint32_t size = context->candidate_size[candidate];
            uint32_t position;

            certificate.clauses[index] =
                context->candidate_source[candidate];
            for (position = 0u; position < size; position++) {
                uint32_t group = context->group_of[codes[position]];

                if (group == 0u) {
                    refuted_total++;
                } else {
                    certificate.group_offsets[group_slot[group]]++;
                    member_total++;
                }
            }
        }
        /* group_offsets[s + 1] holds the size of 0-based slot s, so the
         * prefix sum leaves the start of slot s in group_offsets[s]. */
        for (index = 1u; index <= right_reached; index++) {
            certificate.group_offsets[index] +=
                certificate.group_offsets[index - 1u];
        }
        certificate.group_literals = (ds_literal *)malloc(
            ((size_t)member_total + 1u) * sizeof(ds_literal));
        certificate.refuted = (ds_literal *)malloc(
            ((size_t)refuted_total + 1u) * sizeof(ds_literal));
        if (certificate.group_literals == NULL
            || certificate.refuted == NULL) {
            status = DS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        for (index = 0u; index < left_reached; index++) {
            uint32_t candidate = demands[graph->queue[index]];
            const uint32_t *codes = context->candidate_codes
                + context->candidate_begin[candidate];
            uint32_t size = context->candidate_size[candidate];
            uint32_t position;

            for (position = 0u; position < size; position++) {
                uint32_t group = context->group_of[codes[position]];

                if (group == 0u) {
                    certificate.refuted[certificate.refuted_count] =
                        literal_from_code(codes[position]);
                    certificate.refuted_count++;
                } else {
                    uint32_t slot = group_slot[group] - 1u;

                    certificate.group_literals[
                        certificate.group_offsets[slot] + fill[slot]] =
                        literal_from_code(codes[position]);
                    fill[slot]++;
                }
            }
        }
    }
    *out_certificate = certificate;
    memset(&certificate, 0, sizeof(certificate));
cleanup:
    ds_count_certificate_destroy(&certificate);
    free(left_seen);
    free(group_slot);
    free(fill);
    return status;
}

/* True when some literal of the demand has no conflict edge to a literal
 * of another selected demand. */
static bool has_private_literal(hall_context *context, uint32_t candidate)
{
    const uint32_t *codes = context->candidate_codes
        + context->candidate_begin[candidate];
    uint32_t size = context->candidate_size[candidate];
    uint32_t position;

    for (position = 0u; position < size; position++) {
        uint32_t code = codes[position];
        uint32_t edge;
        bool shared = false;

        if (context->refuted[code] != 0u) {
            continue;
        }
        for (edge = context->adjacency_offsets[code];
            edge < context->adjacency_offsets[code + 1u]; edge++) {
            uint32_t neighbor = context->adjacency[edge];

            context->ops++;
            if (context->owner[neighbor] != 0u
                && context->owner[neighbor] != context->owner[code]) {
                shared = true;
                break;
            }
        }
        if (!shared) {
            return true;
        }
    }
    return false;
}

/*
 * Drop demands that cannot be in a Hall violator. A literal with no edge
 * to another demand's literals can only share a group with literals of
 * its own demand, so that group is the demand's alone, and a set of
 * demands touching fewer groups than its size stays such a set without
 * that demand. Dropping one can strand literals of others, so repeat a
 * few rounds. Dropped literals leave the selection (owner 0).
 */
static uint32_t prune_demands(hall_context *context, const uint32_t *demands,
    uint32_t demand_count, uint32_t *kept)
{
    uint32_t kept_count = demand_count;
    uint32_t round;

    memcpy(kept, demands, (size_t)demand_count * sizeof(uint32_t));
    for (round = 0u; round < 4u; round++) {
        uint32_t write = 0u;
        uint32_t index;

        for (index = 0u; index < kept_count; index++) {
            uint32_t candidate = kept[index];

            if (has_private_literal(context, candidate)) {
                const uint32_t *codes = context->candidate_codes
                    + context->candidate_begin[candidate];
                uint32_t position;

                for (position = 0u;
                    position < context->candidate_size[candidate];
                    position++) {
                    context->owner[codes[position]] = 0u;
                }
                continue;
            }
            kept[write] = candidate;
            write++;
        }
        if (write == kept_count) {
            break;
        }
        kept_count = write;
    }
    /* Owners and ranks follow the kept order. */
    {
        uint32_t literal_rank = 0u;
        uint32_t index;

        for (index = 0u; index < kept_count; index++) {
            const uint32_t *codes = context->candidate_codes
                + context->candidate_begin[kept[index]];
            uint32_t position;

            for (position = 0u;
                position < context->candidate_size[kept[index]];
                position++) {
                context->owner[codes[position]] = index + 1u;
                context->rank[codes[position]] = literal_rank;
                literal_rank++;
            }
            context->owner_mark[index] = 0u;
        }
    }
    return kept_count;
}

/* One selection, one grouping mode. Sets *out_fired on a violation. */
static ds_status run_attempt(hall_context *context, uint32_t policy,
    bool transversal, ds_hall_report *report,
    ds_count_certificate *out_certificate, bool *out_fired)
{
    const uint32_t *demands = context->selection[policy];
    uint32_t demand_count = context->selection_count[policy];
    bipartite graph;
    uint32_t *kept = NULL;
    uint32_t kept_count = 0u;
    uint32_t groups = 0u;
    uint32_t matched;
    uint32_t index;
    ds_status status;

    *out_fired = false;
    {
        uint32_t literal_rank = 0u;

        for (index = 0u; index < demand_count; index++) {
            const uint32_t *codes = context->candidate_codes
                + context->candidate_begin[demands[index]];
            uint32_t size = context->candidate_size[demands[index]];
            uint32_t position;

            for (position = 0u; position < size; position++) {
                context->owner[codes[position]] = index + 1u;
                context->group_of[codes[position]] = 0u;
                context->rank[codes[position]] = literal_rank;
                literal_rank++;
            }
            context->owner_mark[index] = 0u;
        }
        /* Candidates are demand literals, so this bounds every list. */
        free(context->keys);
        context->keys = (uint64_t *)malloc(((size_t)literal_rank + 1u)
            * sizeof(uint64_t));
        if (context->keys == NULL) {
            return DS_ERR_OUT_OF_MEMORY;
        }
        if (context->candidates.capacity < literal_rank + 1u) {
            uint32_t *replacement = (uint32_t *)realloc(
                context->candidates.data,
                ((size_t)literal_rank + 1u) * sizeof(uint32_t));

            if (replacement == NULL) {
                return DS_ERR_OUT_OF_MEMORY;
            }
            context->candidates.data = replacement;
            context->candidates.capacity = literal_rank + 1u;
        }
    }
    kept = (uint32_t *)malloc(((size_t)demand_count + 1u)
        * sizeof(uint32_t));
    if (kept == NULL) {
        status = DS_ERR_OUT_OF_MEMORY;
        goto reset;
    }
    kept_count = prune_demands(context, demands, demand_count, kept);
    status = build_groups(context, kept, kept_count, transversal, &groups);
    if (status == DS_OK && kept_count > 0u) {
        status = bipartite_build(context, kept, kept_count, groups, &graph);
        if (status == DS_OK) {
            matched = bipartite_match(&graph, &context->ops);
            if (matched < kept_count) {
                report->demands = kept_count;
                report->groups = groups;
                report->matched = matched;
                status = extract_violator(context, kept, &graph, report,
                    out_certificate);
                *out_fired = (status == DS_OK);
            }
            bipartite_free(&graph);
        }
    }
reset:
    free(kept);
    for (index = 0u; index < demand_count; index++) {
        const uint32_t *codes = context->candidate_codes
            + context->candidate_begin[demands[index]];
        uint32_t size = context->candidate_size[demands[index]];
        uint32_t position;

        for (position = 0u; position < size; position++) {
            context->owner[codes[position]] = 0u;
            context->group_of[codes[position]] = 0u;
        }
    }
    return status;
}

static ds_status run_attempts(hall_context *context, uint32_t phase,
    ds_hall_report *report, ds_count_certificate *out_certificate)
{
    uint32_t attempt;

    for (attempt = 0u; attempt < 2u * HALL_POLICY_COUNT; attempt++) {
        uint32_t policy = attempt / 2u;
        bool transversal = (attempt % 2u) == 0u;
        bool fired = false;
        bool repeated = false;
        uint32_t earlier;
        ds_status status;

        /* Skip a selection identical to an earlier one. */
        for (earlier = 0u; earlier < policy; earlier++) {
            if (context->selection_count[earlier]
                    == context->selection_count[policy]
                && memcmp(context->selection[earlier],
                    context->selection[policy],
                    (size_t)context->selection_count[policy]
                        * sizeof(uint32_t)) == 0) {
                repeated = true;
                break;
            }
        }
        if (repeated) {
            continue;
        }
        status = run_attempt(context, policy, transversal, report,
            out_certificate, &fired);
        if (status != DS_OK) {
            return status;
        }
        if (fired) {
            report->fired = true;
            report->phase = phase;
            report->attempt = attempt + 1u;
            return DS_OK;
        }
    }
    return DS_OK;
}

/* Rebuild the graph and rerun the attempts, when anything is new. */
static ds_status checkpoint(hall_context *context, ds_hall_report *report,
    ds_count_certificate *out_certificate, uint32_t *last_pairs,
    uint32_t *last_refuted)
{
    ds_status status;

    if (context->pairs.count == *last_pairs
        && report->refuted_literals == *last_refuted) {
        return DS_OK;
    }
    *last_pairs = context->pairs.count;
    *last_refuted = report->refuted_literals;
    status = build_adjacency(context, report);
    if (status == DS_OK) {
        status = run_attempts(context, 2u, report, out_certificate);
    }
    return status;
}

/*
 * Phase two. Longest-first literals are probed first, with a checkpoint
 * before the first demand shorter than three literals: in a pigeonhole
 * formula the long clauses are the pigeons, and their literals alone
 * recover every hole. Then the other selections' literals, with a
 * checkpoint after each.
 */
static ds_status probe_phase(hall_context *context, ds_hall_report *report,
    ds_count_certificate *out_certificate)
{
    static const uint32_t round_policy[HALL_POLICY_COUNT] = { 1u, 0u, 2u };
    uint32_t last_pairs = context->pairs.count;
    uint32_t last_refuted = 0u;
    uint32_t start_pairs = context->pairs.count;
    uint32_t interesting = 0u;
    uint32_t sample;
    uint32_t round;
    uint32_t code;
    bool stop = false;
    ds_status status = attach_watches(context);

    for (code = 0u; code < context->code_count; code++) {
        interesting += context->interest[code];
    }
    sample = interesting / 32u;
    if (sample < HALL_BARREN_SAMPLE) {
        sample = HALL_BARREN_SAMPLE;
    }

    context->visits = 0u;
    for (round = 0u; round < HALL_POLICY_COUNT && status == DS_OK && !stop;
        round++) {
        uint32_t policy = round_policy[round];
        bool short_checked = (policy != 1u);
        uint32_t index;

        for (index = 0u; index < context->selection_count[policy]
            && status == DS_OK && !stop; index++) {
            uint32_t candidate = context->selection[policy][index];
            const uint32_t *codes = context->candidate_codes
                + context->candidate_begin[candidate];
            uint32_t size = context->candidate_size[candidate];
            uint32_t position;

            if (!short_checked && size < 3u) {
                short_checked = true;
                status = checkpoint(context, report, out_certificate,
                    &last_pairs, &last_refuted);
                if (status != DS_OK || report->fired) {
                    break;
                }
            }
            for (position = 0u; position < size && status == DS_OK && !stop;
                position++) {
                status = probe_literal(context, codes[position], report,
                    &stop);
            }
            if (report->probes >= sample && report->refuted_literals == 0u
                && context->pairs.count == start_pairs) {
                report->barren = true;
                stop = true;
            }
        }
        if (status == DS_OK && !report->fired) {
            status = checkpoint(context, report, out_certificate,
                &last_pairs, &last_refuted);
        }
        if (report->fired) {
            break;
        }
    }
    report->probe_visits = context->visits;
    return status;
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */
/* ------------------------------------------------------------------ */

static void context_free(hall_context *context)
{
    uint32_t code;
    uint32_t policy;

    if (context->watches != NULL) {
        for (code = 0u; code < context->code_count; code++) {
            free(context->watches[code].data);
        }
    }
    free(context->watches);
    free(context->arena);
    free(context->clause_begin);
    free(context->clause_size);
    free(context->units);
    free(context->values);
    free(context->trail);
    free(context->candidate_begin);
    free(context->candidate_size);
    free(context->candidate_source);
    free(context->candidate_codes);
    for (policy = 0u; policy < HALL_POLICY_COUNT; policy++) {
        free(context->selection[policy]);
    }
    free(context->interest);
    free(context->refuted);
    free(context->probed);
    free(context->pairs.data);
    free(context->adjacency_offsets);
    free(context->adjacency);
    free(context->owner);
    free(context->rank);
    free(context->keys);
    free(context->group_of);
    free(context->mark);
    free(context->owner_mark);
    free(context->candidates.data);
}

ds_status ds_reason_hall(const ds_cnf *cnf, uint64_t probe_budget,
    ds_verdict *out_verdict, ds_hall_report *out_report,
    ds_count_certificate *out_certificate)
{
    hall_context context;
    ds_hall_report report;
    size_t code_slots;
    ds_status status = DS_OK;
    uint32_t policy;

    if (cnf == NULL || out_verdict == NULL) {
        return DS_ERR_INVALID_ARGUMENT;
    }
    memset(&report, 0, sizeof(report));
    memset(&context, 0, sizeof(context));
    *out_verdict = DS_VERDICT_UNKNOWN;
    if (cnf->variable_count == 0u || cnf->clause_count == 0u
        || cnf->variable_count >= UINT32_MAX / 2u - 1u) {
        goto conclude;
    }
    /* Without a unit or binary clause, assigning one literal can never
     * leave another clause unit: no probe makes any literal false, no
     * conflict pair exists, and no demand survives pruning. Skip the
     * stage. (A longer clause that repeats a literal is missed here;
     * that only costs completeness.) */
    {
        uint32_t clause_index;
        bool short_clause = false;

        for (clause_index = 0u; clause_index < cnf->clause_count
            && !short_clause; clause_index++) {
            short_clause = cnf->offsets[clause_index + 1u]
                - cnf->offsets[clause_index] <= 2u;
        }
        context.ops += clause_index;
        if (!short_clause) {
            report.skipped = true;
            goto conclude;
        }
    }
    context.cnf = cnf;
    context.stamp = DS_HALL_FIRST_STAMP;
    context.variable_count = cnf->variable_count;
    context.code_count = 2u * (cnf->variable_count + 1u);
    context.adaptive = (probe_budget == 0u);
    if (probe_budget == 0u) {
        probe_budget = HALL_BUDGET_BASE
            + HALL_BUDGET_PER_LITERAL * (uint64_t)cnf->literal_count;
        if (probe_budget > HALL_BUDGET_CAP) {
            probe_budget = HALL_BUDGET_CAP;
        }
    }
    context.budget = probe_budget;
    code_slots = (size_t)context.code_count + 1u;
    context.watches = (u32_vec *)calloc(code_slots, sizeof(u32_vec));
    context.values = (int8_t *)calloc((size_t)cnf->variable_count + 1u,
        sizeof(int8_t));
    context.trail = (uint32_t *)malloc(((size_t)cnf->variable_count + 1u)
        * sizeof(uint32_t));
    context.interest = (uint8_t *)calloc(code_slots, 1u);
    context.refuted = (uint8_t *)calloc(code_slots, 1u);
    context.probed = (uint8_t *)calloc(code_slots, 1u);
    context.owner = (uint32_t *)calloc(code_slots, sizeof(uint32_t));
    context.rank = (uint32_t *)calloc(code_slots, sizeof(uint32_t));
    context.group_of = (uint32_t *)calloc(code_slots, sizeof(uint32_t));
    context.mark = (uint32_t *)calloc(code_slots, sizeof(uint32_t));
    context.owner_mark = (uint32_t *)calloc(
        (size_t)cnf->clause_count + 1u, sizeof(uint32_t));
    if (context.watches == NULL || context.values == NULL
        || context.trail == NULL || context.interest == NULL
        || context.refuted == NULL || context.probed == NULL
        || context.owner == NULL || context.rank == NULL
        || context.group_of == NULL
        || context.mark == NULL || context.owner_mark == NULL) {
        status = DS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    status = load_clauses(&context);
    if (status != DS_OK) {
        goto cleanup;
    }
    {
        bool root_conflict = false;

        status = propagate_root(&context, &root_conflict);
        if (status != DS_OK) {
            goto cleanup;
        }
        if (root_conflict) {
            goto conclude; /* search settles a root conflict at once */
        }
    }
    status = collect_candidates(&context);
    if (status != DS_OK) {
        goto cleanup;
    }
    if (context.candidate_count == 0u) {
        goto conclude;
    }
    for (policy = 0u; policy < HALL_POLICY_COUNT; policy++) {
        status = select_demands(&context, policy);
        if (status != DS_OK) {
            goto cleanup;
        }
    }

    /* Phase one: binary clauses only. */
    status = binary_edges(&context, &report);
    if (status == DS_OK) {
        status = build_adjacency(&context, &report);
    }
    if (status == DS_OK) {
        status = run_attempts(&context, 1u, &report, out_certificate);
    }
    if (status != DS_OK) {
        goto cleanup;
    }
    if (!report.fired && !report.budget_exhausted) {
        /* Phase two: propagation probes (they subsume the binaries). */
        status = probe_phase(&context, &report, out_certificate);
        if (status != DS_OK) {
            goto cleanup;
        }
    }
    if (report.fired) {
        *out_verdict = DS_VERDICT_UNSAT;
    }

conclude:
    report.analysis_ops = context.ops + context.visits;
    if (out_report != NULL) {
        *out_report = report;
    }
cleanup:
    context_free(&context);
    return status;
}
