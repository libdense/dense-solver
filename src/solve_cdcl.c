/* Watched literals, first-UIP learning, nonchronological backjumping,
 * phase saving and Luby restarts. Literal codes are 2v and 2v+1. */

#include <stdlib.h>
#include <string.h>

#include "dense_solver/rng.h"
#include "dense_solver/solve.h"

#define DS_REASON_NONE UINT32_MAX
#define DS_ACTIVITY_RESCALE_LIMIT (1ULL << 48)

static uint32_t code_from_literal(ds_literal literal_value)
{
    uint32_t variable = (uint32_t)((literal_value > 0)
        ? literal_value : -literal_value);

    return (literal_value > 0) ? (variable << 1) : ((variable << 1) | 1u);
}

static uint32_t code_negate(uint32_t code)
{
    return code ^ 1u;
}

static uint32_t code_variable(uint32_t code)
{
    return code >> 1;
}

/* value of a literal code under values[]: +1 true, -1 false, 0 unset */
static int8_t code_value(const int8_t *values, uint32_t code)
{
    int8_t variable_value = values[code >> 1];

    if (variable_value == 0) {
        return 0;
    }
    if ((code & 1u) == 0u) {
        return variable_value;
    }
    return (int8_t)-variable_value;
}

typedef struct watch {
    uint32_t ref, other;
} watch;
typedef struct watch_list {
    watch *items;
    uint32_t count;
    uint32_t capacity;
} watch_list;

typedef struct cdcl_context {
    const ds_cnf *cnf;
    ds_cdcl_config config;
    ds_work_ledger ledger;
    uint32_t variable_count;

    /* Clause pool: pool[ref] = size, pool[ref+1 .. ref+size] = codes.
     * Refs at or beyond first_learned_ref are learned clauses. */
    uint32_t *pool;
    uint32_t pool_count;
    uint32_t pool_capacity;
    uint32_t first_learned_ref;

    ds_status propagation_status;
    watch_list *binary;
    uint32_t *heap, *positions, heap_size, *min_stack, *min_undo;
    watch_list *watches; /* indexed by literal code, 2*(V+1) entries */

    int8_t *values;      /* per variable */
    uint32_t *levels;    /* per variable */
    uint32_t *reasons;   /* per variable, clause ref or DS_REASON_NONE */
    int8_t *saved_phase; /* per variable, +1 or -1 */
    uint64_t *activity;  /* per variable */
    uint8_t *seen;       /* per variable, conflict analysis scratch */

    uint32_t *trail;     /* literal codes in assignment order */
    uint32_t trail_count;
    uint32_t queue_head;
    uint32_t *trail_marks; /* trail_count at each decision level */
    uint32_t decision_level;

    uint32_t *learned_scratch; /* variable_count + 1 codes */
    uint64_t activity_increment;
    ds_rng placebo_rng;

    /* Learned-clause metadata for database reduction. */
    uint32_t *learned_refs;
    uint32_t *learned_lbds;
    uint32_t learned_count;
    uint32_t learned_capacity;
    uint64_t conflicts_since_reduction;
    uint32_t *level_stamp;     /* per level, for LBD counting */
    uint32_t stamp_counter;
} cdcl_context;

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

static ds_status pool_reserve(cdcl_context *context, uint32_t extra)
{
    uint32_t needed = context->pool_count + extra;
    uint32_t replacement_capacity;
    uint32_t *replacement;

    if (needed < context->pool_count) {
        return DS_ERR_CAPACITY;
    }
    if (context->pool_capacity >= needed) {
        return DS_OK;
    }
    replacement_capacity =
        (context->pool_capacity == 0u) ? 1024u : context->pool_capacity;
    while (replacement_capacity < needed) {
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
    if (context->pool != NULL) {
        memcpy(replacement, context->pool,
            (size_t)context->pool_count * sizeof(uint32_t));
        free(context->pool);
    }
    context->pool = replacement;
    context->pool_capacity = replacement_capacity;
    return DS_OK;
}

static ds_status watch_push(watch_list *list, uint32_t ref, uint32_t other)
{
    if (list->count == list->capacity) {
        if (list->capacity > UINT32_MAX/2) return DS_ERR_CAPACITY;
        uint32_t cap = list->capacity ? 2*list->capacity : 4;
        watch *items = realloc(list->items, (size_t)cap * sizeof(*items));
        if (!items) return DS_ERR_OUT_OF_MEMORY;
        list->items = items;
        list->capacity = cap;
    }
    list->items[list->count++] = (watch){ref, other};
    return DS_OK;
}

static ds_status attach(cdcl_context *c, const uint32_t *codes, uint32_t size, uint32_t ref)
{
    watch_list *lists = size == 2 ? c->binary : c->watches;
    ds_status status = watch_push(&lists[codes[0]], ref, codes[1]);
    if (status == DS_OK) status = watch_push(&lists[codes[1]], ref, codes[0]);
    return status;
}

static bool higher(const cdcl_context *c, uint32_t a, uint32_t b)
{
    return c->activity[a] > c->activity[b] || (c->activity[a] == c->activity[b] && a < b);
}

static void heap_set(cdcl_context *c, uint32_t i, uint32_t v)
{
    c->heap[i] = v;
    c->positions[v] = i;
}

static void sift_up(cdcl_context *c, uint32_t i, uint32_t v)
{
    while (i && higher(c, v, c->heap[(i-1)/2])) {
        uint32_t parent = (i-1)/2;
        heap_set(c, i, c->heap[parent]);
        i = parent;
    }
    heap_set(c, i, v);
}

static void sift_down(cdcl_context *c, uint32_t i, uint32_t v)
{
    while (i < c->heap_size / 2) {
        uint32_t child = 2*i+1;
        if (child+1 < c->heap_size && higher(c, c->heap[child+1], c->heap[child])) ++child;
        if (!higher(c, c->heap[child], v)) break;
        heap_set(c, i, c->heap[child]);
        i = child;
    }
    heap_set(c, i, v);
}

static void enqueue(cdcl_context *context, uint32_t code, uint32_t reason)
{
    uint32_t variable = code_variable(code);

    context->values[variable] =
        ((code & 1u) == 0u) ? (int8_t)1 : (int8_t)-1;
    context->levels[variable] = context->decision_level;
    context->reasons[variable] = reason;
    context->trail[context->trail_count] = code;
    context->trail_count++;
}

static void cancel_until(cdcl_context *context, uint32_t target_level)
{
    uint32_t keep;

    if (context->decision_level <= target_level) {
        return;
    }
    /* trail_marks[L] is the trail size at the moment level L was
     * created, so keeping levels 0..target means keeping
     * trail_marks[target + 1] entries. */
    keep = context->trail_marks[target_level + 1u];
    while (context->trail_count > keep) {
        uint32_t code;
        uint32_t variable;

        context->trail_count--;
        code = context->trail[context->trail_count];
        variable = code_variable(code);
        context->saved_phase[variable] = context->values[variable];
        context->values[variable] = 0;
        if (context->config.heuristic != DS_BRANCH_FIXED && context->positions[variable] == UINT32_MAX)
            sift_up(context, context->heap_size++, variable);
        context->reasons[variable] = DS_REASON_NONE;
    }
    context->queue_head = context->trail_count;
    context->decision_level = target_level;
}

/* ------------------------------------------------------------------ */
/* propagation: visit only clauses that can have become unit           */
/* ------------------------------------------------------------------ */

/* Returns the conflicting clause ref, or DS_REASON_NONE. */
static uint32_t propagate(cdcl_context *context)
{
    while (context->queue_head < context->trail_count) {
        uint32_t assigned_code = context->trail[context->queue_head];
        uint32_t false_code = code_negate(assigned_code);
        watch_list *list = &context->watches[false_code];
        uint32_t read_index = 0u;
        uint32_t write_index = 0u;

        context->queue_head++;
        watch_list *binary = &context->binary[false_code];
        for (uint32_t i = 0; i < binary->count; ++i) {
            watch w = binary->items[i];
            ++context->ledger.clause_visits;
            int8_t value = code_value(context->values, w.other);
            if (value > 0) continue;
            if (value < 0) { context->queue_head = context->trail_count; return w.ref; }
            enqueue(context, w.other, w.ref);
            ++context->ledger.propagations;
            if (w.ref >= context->first_learned_ref) ++context->ledger.learned_propagations;
        }
        while (read_index < list->count) {
            watch w = list->items[read_index];
            ++context->ledger.clause_visits;
            if (code_value(context->values, w.other) == 1) {
                list->items[write_index++] = w;
                ++read_index;
                continue;
            }
            uint32_t ref = w.ref;
            uint32_t size = context->pool[ref];
            uint32_t *codes = context->pool + ref + 1u;
            uint32_t other;
            uint32_t scan;
            bool moved = false;

            /* Keep the false literal in slot 1. */
            if (codes[0] == false_code) {
                codes[0] = codes[1];
                codes[1] = false_code;
            }
            other = codes[0];
            if (code_value(context->values, other) == 1) {
                /* Clause already satisfied; keep watching. */
                list->items[write_index] = (watch){ref, other};
                write_index++;
                read_index++;
                continue;
            }
            for (scan = 2u; scan < size; scan++) {
                if (code_value(context->values, codes[scan]) != -1) {
                    /* New watch found; migrate the clause. */
                    codes[1] = codes[scan];
                    codes[scan] = false_code;
                    context->propagation_status = watch_push(&context->watches[codes[1]], ref, other);
                    if (context->propagation_status != DS_OK) return DS_REASON_NONE;
                    moved = true;
                    break;
                }
            }
            if (moved) {
                read_index++;
                continue;
            }
            /* Clause is unit or conflicting on codes[0]. */
            list->items[write_index] = (watch){ref, other};
            write_index++;
            read_index++;
            if (code_value(context->values, other) == -1) {
                /* Conflict: keep the remaining watchers, stop. */
                while (read_index < list->count) {
                    list->items[write_index] = list->items[read_index];
                    write_index++;
                    read_index++;
                }
                list->count = write_index;
                context->queue_head = context->trail_count;
                return ref;
            }
            enqueue(context, other, ref);
            context->ledger.propagations++;
            if (ref >= context->first_learned_ref) {
                context->ledger.learned_propagations++;
            }
        }
        list->count = write_index;
    }
    return DS_REASON_NONE;
}

/* ------------------------------------------------------------------ */
/* activity                                                            */
/* ------------------------------------------------------------------ */

static void rescale_activity(cdcl_context *context)
{
    uint32_t variable;

    for (variable = 1u; variable <= context->variable_count; variable++) {
        context->activity[variable] >>= 32;
    }
    context->activity_increment >>= 32;
    if (context->activity_increment == 0u) {
        context->activity_increment = 1u;
    }
    /* Rounding can create ties; restore the variable-number tie break. */
    for (uint32_t i = context->heap_size/2; i > 0; --i)
        sift_down(context, i-1, context->heap[i-1]);
}

static void bump_variable(cdcl_context *context, uint32_t variable)
{
    context->activity[variable] += context->activity_increment;
    if (context->activity[variable] > DS_ACTIVITY_RESCALE_LIMIT) {
        rescale_activity(context);
    }
    if (context->positions[variable] != UINT32_MAX)
        sift_up(context, context->positions[variable], variable);
}

/* Growing the increment is equivalent to decaying all activities:
 * newer conflicts weigh more, in pure integer arithmetic. */
static void decay_activity(cdcl_context *context)
{
    context->activity_increment +=
        (context->activity_increment >> 4) + 1u;
    if (context->activity_increment > DS_ACTIVITY_RESCALE_LIMIT) {
        rescale_activity(context);
    }
}

/* The placebo bumps the same number of variables with the same
 * increment schedule, chosen by a decoupled stream: identical
 * machinery, meaningless signal. */
static void placebo_bump(cdcl_context *context, uint32_t bump_count)
{
    uint32_t index;

    for (index = 0u; index < bump_count; index++) {
        uint32_t variable = (uint32_t)ds_rng_below(&context->placebo_rng,
            context->variable_count) + 1u;

        bump_variable(context, variable);
    }
}

/* ------------------------------------------------------------------ */
/* conflict analysis: first UIP                                        */
/* ------------------------------------------------------------------ */

typedef struct analysis_result {
    uint32_t learned_size;
    uint32_t backjump_level;
} analysis_result;

/* A learned literal may be removed if every non-root antecedent is
 * already covered or has a reason recursively covered by the clause. */
static bool redundant(cdcl_context *c, uint32_t root, uint32_t levels)
{
    uint32_t top = 0, changes = 0;
    bool ok = true;
    c->min_stack[top++] = root;
    while (top && ok) {
        uint32_t v = c->min_stack[--top];
        uint32_t ref = c->reasons[v];
        for (uint32_t i = 1; i <= c->pool[ref]; ++i) {
            uint32_t w = code_variable(c->pool[ref+i]), level = c->levels[w];
            if (w == v || !level || c->seen[w]) continue;
            if (c->reasons[w] == DS_REASON_NONE || !(levels & (1u << (level & 31)))) {
                ok = false;
                break;
            }
            c->seen[w] = 1;
            c->min_undo[changes++] = w;
            c->min_stack[top++] = w;
        }
    }
    while (changes) c->seen[c->min_undo[--changes]] = 0;
    return ok;
}

static void analyze_conflict(cdcl_context *context, uint32_t conflict_ref,
    analysis_result *out_result)
{
    uint32_t *learned = context->learned_scratch;
    uint32_t learned_size = 1u; /* slot 0 reserved for the UIP */
    uint32_t counter = 0u;
    uint32_t clause_ref = conflict_ref;
    uint32_t trail_index = context->trail_count;
    uint32_t uip_code = 0u;
    uint32_t bumped = 0u;
    bool first_round = true;
    uint32_t index;

    for (;;) {
        uint32_t size = context->pool[clause_ref];
        const uint32_t *codes = context->pool + clause_ref + 1u;

        for (index = 0u; index < size; index++) {
            uint32_t code = codes[index];
            uint32_t variable = code_variable(code);

            if (!first_round && code == uip_code) {
                continue; /* the resolved-away literal */
            }
            if (context->seen[variable] != 0u
                || context->levels[variable] == 0u) {
                continue;
            }
            context->seen[variable] = 1u;
            if (context->config.heuristic == DS_BRANCH_ACTIVITY) {
                bump_variable(context, variable);
            }
            bumped++;
            if (context->levels[variable] == context->decision_level) {
                counter++;
            } else {
                learned[learned_size] = code;
                learned_size++;
            }
        }
        /* Walk back to the next marked trail literal. */
        do {
            trail_index--;
        } while (context->seen[
            code_variable(context->trail[trail_index])] == 0u);
        uip_code = context->trail[trail_index];
        context->seen[code_variable(uip_code)] = 0u;
        counter--;
        first_round = false;
        if (counter == 0u) {
            break;
        }
        clause_ref = context->reasons[code_variable(uip_code)];
    }
    learned[0] = code_negate(uip_code);

    if (context->config.heuristic == DS_BRANCH_PLACEBO) {
        placebo_bump(context, bumped);
    }

    uint32_t levels = 0, kept = 1;
    for (index = 1; index < learned_size; ++index)
        levels |= 1u << (context->levels[code_variable(learned[index])] & 31);
    for (index = 1; index < learned_size; ++index) {
        uint32_t v = code_variable(learned[index]);
        if (context->reasons[v] != DS_REASON_NONE && redundant(context, v, levels)) {
            context->seen[v] = 0;
            continue;
        }
        learned[kept++] = learned[index];
    }
    learned_size = kept;
    /* Clear remaining seen flags and find the backjump level; keep the
     * deepest non-UIP literal in slot 1 so the learned clause is unit
     * on slot 0 immediately after the jump. */
    out_result->backjump_level = 0u;
    for (index = 1u; index < learned_size; index++) {
        uint32_t variable = code_variable(learned[index]);
        uint32_t level = context->levels[variable];

        context->seen[variable] = 0u;
        if (level > out_result->backjump_level) {
            uint32_t swap = learned[1];

            out_result->backjump_level = level;
            learned[1] = learned[index];
            learned[index] = swap;
        }
    }
    out_result->learned_size = learned_size;
}

/* ------------------------------------------------------------------ */
/* learned-clause database reduction                      */
/* ------------------------------------------------------------------ */

/* Literal block distance of the clause in learned_scratch: the number
 * of distinct decision levels it touches. Low LBD = "glue". */
static uint32_t compute_lbd(cdcl_context *context, const uint32_t *codes,
    uint32_t size)
{
    uint32_t distinct = 0u;
    uint32_t index;

    if (++context->stamp_counter == 0) {
        memset(context->level_stamp, 0, ((size_t)context->variable_count+2)*sizeof(uint32_t));
        context->stamp_counter = 1;
    }
    for (index = 0u; index < size; index++) {
        uint32_t level = context->levels[code_variable(codes[index])];

        if (context->level_stamp[level] != context->stamp_counter) {
            context->level_stamp[level] = context->stamp_counter;
            distinct++;
        }
    }
    return distinct;
}

static ds_status learned_meta_push(cdcl_context *context, uint32_t ref,
    uint32_t lbd)
{
    if (context->learned_count == context->learned_capacity) {
        uint32_t replacement_capacity =
            (context->learned_capacity == 0u) ? 256u
                : context->learned_capacity * 2u;
        uint32_t *replacement_refs = (uint32_t *)malloc(
            (size_t)replacement_capacity * sizeof(uint32_t));
        uint32_t *replacement_lbds = (uint32_t *)malloc(
            (size_t)replacement_capacity * sizeof(uint32_t));

        if (replacement_refs == NULL || replacement_lbds == NULL) {
            free(replacement_refs);
            free(replacement_lbds);
            return DS_ERR_OUT_OF_MEMORY;
        }
        if (context->learned_refs != NULL) {
            memcpy(replacement_refs, context->learned_refs,
                (size_t)context->learned_count * sizeof(uint32_t));
            memcpy(replacement_lbds, context->learned_lbds,
                (size_t)context->learned_count * sizeof(uint32_t));
            free(context->learned_refs);
            free(context->learned_lbds);
        }
        context->learned_refs = replacement_refs;
        context->learned_lbds = replacement_lbds;
        context->learned_capacity = replacement_capacity;
    }
    context->learned_refs[context->learned_count] = ref;
    context->learned_lbds[context->learned_count] = lbd;
    context->learned_count++;
    return DS_OK;
}

typedef struct reduce_candidate {
    uint32_t lbd;
    uint32_t meta_index;
} reduce_candidate;

static int compare_candidates(const void *left, const void *right)
{
    const reduce_candidate *first = (const reduce_candidate *)left;
    const reduce_candidate *second = (const reduce_candidate *)right;

    if (first->lbd != second->lbd) {
        return (first->lbd < second->lbd) ? -1 : 1;
    }
    if (first->meta_index != second->meta_index) {
        return (first->meta_index < second->meta_index) ? -1 : 1;
    }
    return 0;
}

/*
 * Reduce the learned database. Called only at decision level 0 with
 * propagation at fixpoint, which makes the operation simple and safe:
 * level-0 reasons are never dereferenced again (conflict analysis
 * skips level-0 literals), so no clause is locked; any learned clause
 * satisfied by a root fact is permanently dead; and deleting learned
 * clauses is always sound.
 *
 * Shape follows the skills-pack pure-cache rule: no incremental watch
 * surgery, the watch lists are invalidated and rebuilt whole, so no
 * stale state is possible.
 */
static ds_status reduce_database(cdcl_context *context)
{
    reduce_candidate *candidates = NULL;
    uint8_t *drop = NULL; /* per meta index */
    uint32_t candidate_count = 0u;
    uint32_t survivor_count = 0u;
    uint32_t deleted = 0u;
    uint32_t *new_pool = NULL;
    uint32_t new_pool_count;
    uint32_t meta_index;
    uint32_t code;
    ds_status status = DS_OK;

    if (context->learned_count == 0u) {
        return DS_OK;
    }
    candidates = (reduce_candidate *)malloc(
        (size_t)context->learned_count * sizeof(reduce_candidate));
    drop = (uint8_t *)calloc(context->learned_count, sizeof(uint8_t));
    if (candidates == NULL || drop == NULL) {
        status = DS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    /* Classify. */
    for (meta_index = 0u; meta_index < context->learned_count;
        meta_index++) {
        uint32_t ref = context->learned_refs[meta_index];
        uint32_t size = context->pool[ref];
        const uint32_t *codes = context->pool + ref + 1u;
        bool satisfied_at_root = false;
        uint32_t index;

        for (index = 0u; index < size; index++) {
            if (code_value(context->values, codes[index]) == 1) {
                satisfied_at_root = true;
                break;
            }
        }
        if (satisfied_at_root) {
            drop[meta_index] = 1u; /* permanently dead */
            continue;
        }
        if (size <= 2u || context->learned_lbds[meta_index] <= 3u) {
            continue; /* protected: binary and glue clauses */
        }
        candidates[candidate_count].lbd =
            context->learned_lbds[meta_index];
        candidates[candidate_count].meta_index = meta_index;
        candidate_count++;
    }
    /* Delete the worst half of the candidates by LBD. */
    qsort(candidates, candidate_count, sizeof(reduce_candidate),
        compare_candidates);
    for (meta_index = candidate_count / 2u; meta_index < candidate_count;
        meta_index++) {
        drop[candidates[meta_index].meta_index] = 1u;
    }

    /* Rebuild the pool: originals verbatim, survivors compacted. */
    new_pool = (uint32_t *)malloc(
        (size_t)context->pool_capacity * sizeof(uint32_t));
    if (new_pool == NULL) {
        status = DS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    memcpy(new_pool, context->pool,
        (size_t)context->first_learned_ref * sizeof(uint32_t));
    new_pool_count = context->first_learned_ref;
    for (meta_index = 0u; meta_index < context->learned_count;
        meta_index++) {
        uint32_t ref;
        uint32_t size;

        if (drop[meta_index] != 0u) {
            deleted++;
            continue;
        }
        ref = context->learned_refs[meta_index];
        size = context->pool[ref];
        memcpy(new_pool + new_pool_count, context->pool + ref,
            ((size_t)size + 1u) * sizeof(uint32_t));
        context->learned_refs[survivor_count] = new_pool_count;
        context->learned_lbds[survivor_count] =
            context->learned_lbds[meta_index];
        survivor_count++;
        new_pool_count += size + 1u;
    }
    free(context->pool);
    context->pool = new_pool;
    new_pool = NULL;
    context->pool_count = new_pool_count;
    context->learned_count = survivor_count;

    /* Level-0 reasons are permanently settled facts; clear them so no
     * stale ref can survive the compaction. */
    {
        uint32_t trail_index;

        for (trail_index = 0u; trail_index < context->trail_count;
            trail_index++) {
            context->reasons[code_variable(
                context->trail[trail_index])] = DS_REASON_NONE;
        }
    }

    /* Rebuild every watch list from scratch (pure-cache rule). */
    for (code = 0u; code < 2u * (context->variable_count + 1u); code++) {
        context->watches[code].count = 0u;
        context->binary[code].count = 0u;
    }
    {
        uint32_t ref = 0u;

        while (ref < context->pool_count) {
            uint32_t size = context->pool[ref];
            uint32_t *codes = context->pool + ref + 1u;
            uint32_t write_slot = 0u;
            uint32_t index;

            /* Partition non-false literals to the front so the watch
             * invariant holds under the current root assignment. */
            for (index = 0u; index < size && write_slot < 2u; index++) {
                if (code_value(context->values, codes[index]) != -1) {
                    uint32_t held = codes[write_slot];

                    codes[write_slot] = codes[index];
                    codes[index] = held;
                    write_slot++;
                }
            }
            status = attach(context, codes, size, ref);
            if (status != DS_OK) goto cleanup;
            ref += size + 1u;
        }
    }
    context->ledger.db_reductions++;
    context->ledger.learned_deleted += deleted;
    context->conflicts_since_reduction = 0u;
cleanup:
    free(candidates);
    free(drop);
    free(new_pool);
    return status;
}

/* ------------------------------------------------------------------ */
/* clause ingestion                                                    */
/* ------------------------------------------------------------------ */

static ds_status add_clause_to_pool(cdcl_context *context,
    const uint32_t *codes, uint32_t size, uint32_t *out_ref)
{
    ds_status status = pool_reserve(context, size + 1u);
    uint32_t ref;
    uint32_t index;

    if (status != DS_OK) {
        return status;
    }
    ref = context->pool_count;
    context->pool[context->pool_count] = size;
    context->pool_count++;
    for (index = 0u; index < size; index++) {
        context->pool[context->pool_count] = codes[index];
        context->pool_count++;
    }
    status = attach(context, codes, size, ref);
    if (status != DS_OK) return status;
    *out_ref = ref;
    return DS_OK;
}

/* Load original clauses: dedupe literals, drop tautologies, enqueue
 * units at level 0. Returns DS_OK, or DS_ERR_STATE if the formula is
 * trivially unsatisfiable (empty clause or contradicting units). */
static ds_status load_original_clauses(cdcl_context *context,
    bool *out_unsat)
{
    const ds_cnf *cnf = context->cnf;
    uint32_t *scratch = context->learned_scratch;
    uint32_t clause_index;

    *out_unsat = false;
    for (clause_index = 0u; clause_index < cnf->clause_count;
        clause_index++) {
        uint32_t begin = cnf->offsets[clause_index];
        uint32_t end = cnf->offsets[clause_index + 1u];
        uint32_t size = 0u;
        bool tautology = false;
        uint32_t position;
        uint32_t check;

        for (position = begin; position < end && !tautology; position++) {
            uint32_t code = code_from_literal(cnf->literals[position]);
            bool duplicate = false;

            for (check = 0u; check < size; check++) {
                if (scratch[check] == code) {
                    duplicate = true;
                    break;
                }
                if (scratch[check] == code_negate(code)) {
                    tautology = true;
                    break;
                }
            }
            if (!duplicate && !tautology) {
                scratch[size] = code;
                size++;
            }
        }
        if (tautology) {
            continue;
        }
        if (size == 0u) {
            *out_unsat = true;
            return DS_OK;
        }
        if (size == 1u) {
            int8_t current = code_value(context->values, scratch[0]);

            if (current == -1) {
                *out_unsat = true;
                return DS_OK;
            }
            if (current == 0) {
                enqueue(context, scratch[0], DS_REASON_NONE);
                context->ledger.propagations++;
            }
            continue;
        }
        {
            uint32_t ref;
            ds_status status = add_clause_to_pool(context, scratch,
                size, &ref);

            if (status != DS_OK) {
                return status;
            }
        }
    }
    return DS_OK;
}

/* ------------------------------------------------------------------ */
/* decisions and restarts                                              */
/* ------------------------------------------------------------------ */

static uint32_t pick_branch_variable(cdcl_context *context)
{
    uint32_t variable;

    if (context->config.heuristic == DS_BRANCH_FIXED) {
        for (variable = 1u; variable <= context->variable_count;
            variable++) {
            if (context->values[variable] == 0) {
                return variable;
            }
        }
        return 0u;
    }
    while (context->heap_size) {
        uint32_t v = context->heap[0];
        --context->heap_size;
        if (context->heap_size) sift_down(context, 0, context->heap[context->heap_size]);
        context->positions[v] = UINT32_MAX;
        if (!context->values[v]) return v;
    }
    return 0;
}

static uint64_t luby_value(uint64_t sequence_index)
{
    /* Standard Luby computation, 1-based internally. */
    uint64_t target = sequence_index + 1u;
    uint64_t size = 1u;
    uint64_t exponent = 0u;

    while (size < target) {
        exponent++;
        size = 2u * size + 1u;
    }
    while (size - 1u != sequence_index) {
        size = (size - 1u) / 2u;
        exponent--;
        sequence_index = sequence_index % size;
    }
    return 1ULL << exponent;
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */
/* ------------------------------------------------------------------ */

ds_cdcl_config ds_cdcl_config_default(void)
{
    ds_cdcl_config config;

    config.heuristic = DS_BRANCH_ACTIVITY;
    config.placebo_seed = 0x9E3779B97F4A7C15ULL;
    config.enable_restarts = true;
    config.luby_unit = 64u;
    config.enable_db_reduction = false; /* lands dark; cutover later */
    config.db_reduction_interval = 2000u;
    config.decision_budget = 0u;
    config.conflict_budget = 0u;
    return config;
}

static void context_free(cdcl_context *context)
{
    uint32_t code;

    if (context->watches != NULL) {
        for (code = 0u; code < 2u * (context->variable_count + 1u);
            code++) {
            free(context->watches[code].items);
        }
    }
    if (context->binary) {
        for (size_t i = 0; i < 2*((size_t)context->variable_count+1); ++i)
            free(context->binary[i].items);
    }
    free(context->heap); free(context->positions);
    free(context->min_stack); free(context->min_undo);
    free(context->binary);
    free(context->pool);
    free(context->watches);
    free(context->values);
    free(context->levels);
    free(context->reasons);
    free(context->saved_phase);
    free(context->activity);
    free(context->seen);
    free(context->trail);
    free(context->trail_marks);
    free(context->learned_scratch);
    free(context->learned_refs);
    free(context->learned_lbds);
    free(context->level_stamp);
}

ds_status ds_solve_cdcl(const ds_cnf *cnf, const ds_cdcl_config *config,
    ds_verdict *out_verdict, int8_t *out_model, ds_work_ledger *out_ledger)
{
    cdcl_context context;
    size_t variable_slots;
    bool trivially_unsat = false;
    ds_verdict verdict = DS_VERDICT_UNKNOWN;
    uint64_t restart_epoch = 0u;
    uint64_t conflicts_this_epoch = 0u;
    uint64_t restart_limit;
    ds_status status = DS_OK;

    if (cnf == NULL || out_verdict == NULL) {
        return DS_ERR_INVALID_ARGUMENT;
    }
    memset(&context, 0, sizeof(context));
    context.cnf = cnf;
    context.config = (config != NULL) ? *config
        : ds_cdcl_config_default();
    if (context.config.luby_unit == 0u) {
        context.config.luby_unit = 64u;
    }
    context.variable_count = cnf->variable_count;
    context.activity_increment = 1u;
    ds_rng_seed(&context.placebo_rng, context.config.placebo_seed);

    variable_slots = (size_t)context.variable_count + 1u;
    context.watches = (watch_list *)calloc(2u * variable_slots,
        sizeof(watch_list));
    context.values = (int8_t *)calloc(variable_slots, sizeof(int8_t));
    context.levels = (uint32_t *)calloc(variable_slots, sizeof(uint32_t));
    context.reasons = (uint32_t *)malloc(
        variable_slots * sizeof(uint32_t));
    context.saved_phase = (int8_t *)malloc(
        variable_slots * sizeof(int8_t));
    context.activity = (uint64_t *)calloc(variable_slots,
        sizeof(uint64_t));
    context.seen = (uint8_t *)calloc(variable_slots, sizeof(uint8_t));
    context.trail = (uint32_t *)calloc(variable_slots, sizeof(uint32_t));
    context.trail_marks = (uint32_t *)calloc(variable_slots + 1u,
        sizeof(uint32_t));
    context.learned_scratch = (uint32_t *)calloc(variable_slots + 1u,
        sizeof(uint32_t));
    context.level_stamp = (uint32_t *)calloc(variable_slots + 1u,
        sizeof(uint32_t));
    context.binary = calloc(2*variable_slots, sizeof(watch_list));
    context.heap = calloc(variable_slots, sizeof(uint32_t));
    context.positions = calloc(variable_slots, sizeof(uint32_t));
    context.min_stack = calloc(variable_slots, sizeof(uint32_t));
    context.min_undo = calloc(variable_slots, sizeof(uint32_t));
    if (!context.binary || !context.heap || !context.positions || !context.min_stack || !context.min_undo
        || context.watches == NULL || context.values == NULL
        || context.levels == NULL || context.reasons == NULL
        || context.saved_phase == NULL || context.activity == NULL
        || context.seen == NULL || context.trail == NULL
        || context.trail_marks == NULL
        || context.learned_scratch == NULL
        || context.level_stamp == NULL) {
        context_free(&context);
        return DS_ERR_OUT_OF_MEMORY;
    }
    for (uint32_t v = 1; v <= context.variable_count; ++v) heap_set(&context, v-1, v);
    context.heap_size = context.variable_count;
    memset(context.reasons, 0xFF, variable_slots * sizeof(uint32_t));
    memset(context.saved_phase, 1, variable_slots * sizeof(int8_t));
    context.first_learned_ref = UINT32_MAX;

    status = load_original_clauses(&context, &trivially_unsat);
    if (status != DS_OK) {
        context_free(&context);
        return status;
    }
    context.first_learned_ref = context.pool_count;
    restart_limit = luby_value(restart_epoch)
        * (uint64_t)context.config.luby_unit;

    if (trivially_unsat) {
        verdict = DS_VERDICT_UNSAT;
    } else {
        for (;;) {
            uint32_t conflict_ref = propagate(&context);
            if (context.propagation_status != DS_OK) { status = context.propagation_status; break; }

            if (conflict_ref != DS_REASON_NONE) {
                analysis_result analysis;

                context.ledger.conflicts++;
                conflicts_this_epoch++;
                if (context.decision_level == 0u) {
                    verdict = DS_VERDICT_UNSAT;
                    break;
                }
                if (context.config.conflict_budget != 0u
                    && context.ledger.conflicts
                        >= context.config.conflict_budget) {
                    verdict = DS_VERDICT_UNKNOWN;
                    break;
                }
                analyze_conflict(&context, conflict_ref, &analysis);
                decay_activity(&context);
                context.conflicts_since_reduction++;
                if (analysis.learned_size == 1u) {
                    cancel_until(&context, analysis.backjump_level);
                    context.ledger.backtracks++;
                    enqueue(&context, context.learned_scratch[0],
                        DS_REASON_NONE);
                } else {
                    uint32_t ref;
                    /* LBD needs the levels as they stand at conflict
                     * time, before the backjump clears them. */
                    uint32_t lbd = compute_lbd(&context,
                        context.learned_scratch, analysis.learned_size);

                    cancel_until(&context, analysis.backjump_level);
                    context.ledger.backtracks++;
                    status = add_clause_to_pool(&context,
                        context.learned_scratch, analysis.learned_size,
                        &ref);
                    if (status != DS_OK) {
                        break;
                    }
                    status = learned_meta_push(&context, ref, lbd);
                    if (status != DS_OK) {
                        break;
                    }
                    enqueue(&context, context.learned_scratch[0], ref);
                }
                context.ledger.propagations++;
                context.ledger.learned_clauses++;
                context.ledger.learned_literals += analysis.learned_size;
                continue;
            }
            if (context.trail_count == context.variable_count) {
                verdict = DS_VERDICT_SAT;
                break;
            }
            if (context.config.enable_db_reduction
                && context.decision_level == 0u
                && context.conflicts_since_reduction
                    >= (uint64_t)context.config.db_reduction_interval) {
                status = reduce_database(&context);
                if (status != DS_OK) {
                    break;
                }
            }
            if (context.config.enable_restarts
                && conflicts_this_epoch >= restart_limit) {
                restart_epoch++;
                conflicts_this_epoch = 0u;
                restart_limit = luby_value(restart_epoch)
                    * (uint64_t)context.config.luby_unit;
                context.ledger.restarts++;
                cancel_until(&context, 0u);
                continue;
            }
            if (context.config.decision_budget != 0u
                && context.ledger.decisions
                    >= context.config.decision_budget) {
                verdict = DS_VERDICT_UNKNOWN;
                break;
            }
            {
                uint32_t variable = pick_branch_variable(&context);
                uint32_t code;

                if (variable == 0u) {
                    /* All assigned; handled above, defensive. */
                    verdict = DS_VERDICT_SAT;
                    break;
                }
                context.ledger.decisions++;
                context.decision_level++;
                context.trail_marks[context.decision_level] =
                    context.trail_count;
                if (context.decision_level > context.ledger.max_depth) {
                    context.ledger.max_depth = context.decision_level;
                }
                code = (context.saved_phase[variable] == -1)
                    ? ((variable << 1) | 1u) : (variable << 1);
                enqueue(&context, code, DS_REASON_NONE);
            }
        }
    }

    context.ledger.branches_avoided =
        context.ledger.propagations + context.ledger.pure_eliminations;
    *out_verdict = verdict;
    if (verdict == DS_VERDICT_SAT && out_model != NULL) {
        uint32_t variable;

        out_model[0] = 0;
        for (variable = 1u; variable <= context.variable_count;
            variable++) {
            int8_t value = context.values[variable];

            out_model[variable] = (value != 0) ? value : (int8_t)1;
        }
    }
    if (out_ledger != NULL) {
        *out_ledger = context.ledger;
    }
    context_free(&context);
    return status;
}
