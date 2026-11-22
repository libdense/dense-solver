/* Count disjoint positive demands against the at-most-one groups they touch.
 * All edges used as conflicts are justified by binary clauses in the input. */
#include <stdlib.h>
#include <string.h>
#include "dense_solver/reason.h"
#include "trace.h"

typedef struct {
    uint32_t *offset, *edge;
} graph;

typedef struct {
    const ds_cnf *cnf;
    uint32_t n, demand_count, *demands, *group;
    uint8_t *positive, *used, *touched;
    ds_count_report report;
    FILE *trace;
    ds_count_certificate *certificate;
    uint32_t *selected;
} counter;

static uint32_t node(ds_literal lit)
{
    return ((uint32_t)abs(lit) << 1) | (uint32_t)(lit < 0);
}

static void destroy_graph(graph *g)
{
    free(g->offset);
    free(g->edge);
}

static bool is_pair(const ds_literal *l, const counter *c)
{
    return l[0] < 0 && l[1] < 0 && l[0] != l[1]
        && c->positive[-l[0]] && c->positive[-l[1]];
}

/* Count row lengths, prefix-sum them, then fill the flat adjacency list. */
static bool make_graph(counter *c, bool implications, graph *g)
{
    const ds_cnf *f = c->cnf;
    size_t rows = implications ? 2 * (size_t)c->n : c->n;
    g->offset = calloc(rows + 1, sizeof(*g->offset));
    if (!g->offset) return false;
    uint32_t *cursor = NULL;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < f->clause_count; ++i) {
            if (f->offsets[i+1] - f->offsets[i] != 2) continue;
            const ds_literal *l = f->literals + f->offsets[i];
            uint32_t a, b;
            if (implications) {
                a = node(l[0]);
                b = node(l[1]);
                if (pass) {
                    g->edge[cursor[a^1]++] = b;
                    g->edge[cursor[b^1]++] = a;
                } else {
                    ++g->offset[(a^1)+1];
                    ++g->offset[(b^1)+1];
                }
            } else if (is_pair(l, c)) {
                a = (uint32_t)-l[0];
                b = (uint32_t)-l[1];
                uint32_t lower = a < b ? a : b;
                if (pass) g->edge[cursor[lower]++] = a < b ? b : a;
                else ++g->offset[lower+1];
            }
        }
        if (!pass) {
            for (size_t k = 0; k < rows; ++k) g->offset[k+1] += g->offset[k];
            if (!g->offset[rows]) return true;
            g->edge = calloc(g->offset[rows], sizeof(*g->edge));
            cursor = calloc(rows, sizeof(*cursor));
            if (!g->edge || !cursor) { free(cursor); return false; }
            memcpy(cursor, g->offset, rows * sizeof(*cursor));
        }
    }
    free(cursor);
    return true;
}

static void start_groups(counter *c, const char *pass)
{
    memset(c->group, 0, c->n * sizeof(*c->group));
    c->report.amo_groups = 0;
    c->report.pass = pass;
}

void ds_count_certificate_destroy(ds_count_certificate *cert)
{
    if (!cert) return;
    free(cert->clauses);
    free(cert->group_offsets);
    free(cert->group_literals);
    free(cert->refuted);
    memset(cert, 0, sizeof(*cert));
}

/* Assign each distinct live variable to its touched group once. Input clauses
 * may repeat a literal. Only selected, forced-false literals need refutation. */
static bool certify(counter *c, const uint8_t *forced)
{
    ds_count_certificate result = {0};
    uint32_t count = c->report.forced_clause ? 1 : c->report.demand;
    uint32_t *slots = calloc((size_t)c->report.amo_groups + 1, sizeof(*slots));
    uint32_t *cursor = NULL;
    result.clauses = calloc(count, sizeof(*result.clauses));
    if (!slots || !result.clauses) goto fail;
    result.clause_count = count;
    if (c->report.forced_clause) result.clauses[0] = c->report.forced_clause - 1;
    else memcpy(result.clauses, c->selected, count * sizeof(*result.clauses));

    memset(c->used, 0, c->n);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t k = result.clauses[i];
        for (uint32_t j = c->cnf->offsets[k]; j < c->cnf->offsets[k+1]; ++j)
            c->used[c->cnf->literals[j]] = 1;
    }
    uint32_t live = 0;
    for (uint32_t v = 1; v < c->n; ++v) {
        if (!c->used[v]) continue;
        if (forced && forced[v]) { ++result.refuted_count; continue; }
        ++live;
        if (!slots[c->group[v]]) slots[c->group[v]] = ++result.group_count;
    }
    result.group_offsets = calloc((size_t)result.group_count + 1, sizeof(*result.group_offsets));
    result.group_literals = calloc((size_t)live + 1, sizeof(*result.group_literals));
    result.refuted = calloc((size_t)result.refuted_count + 1, sizeof(*result.refuted));
    cursor = calloc((size_t)result.group_count + 1, sizeof(*cursor));
    if (!result.group_offsets || !result.group_literals || !result.refuted || !cursor) goto fail;
    uint32_t dead = 0;
    for (uint32_t v = 1; v < c->n; ++v) {
        if (!c->used[v]) continue;
        if (forced && forced[v]) result.refuted[dead++] = (ds_literal)v;
        else ++result.group_offsets[slots[c->group[v]]];
    }
    for (uint32_t i = 0; i < result.group_count; ++i) {
        result.group_offsets[i+1] += result.group_offsets[i];
        cursor[i] = result.group_offsets[i];
    }
    for (uint32_t v = 1; v < c->n; ++v) {
        if (c->used[v] && !(forced && forced[v])) {
            uint32_t slot = slots[c->group[v]] - 1;
            result.group_literals[cursor[slot]++] = (ds_literal)v;
        }
    }
    free(cursor);
    free(slots);
    *c->certificate = result;
    return true;
fail:
    free(cursor);
    free(slots);
    ds_count_certificate_destroy(&result);
    return false;
}

static bool evaluate(counter *c, const uint8_t *forced)
{
    ds_count_report *r = &c->report;
    memset(c->used, 0, c->n);
    memset(c->touched, 0, c->n);
    r->demand = r->capacity = r->alo_selected = 0;
    if (c->trace) fprintf(c->trace, "c counting pass %s\n", r->pass);
    for (uint32_t i = 0; i < c->demand_count; ++i) {
        uint32_t k = c->demands[i], begin = c->cnf->offsets[k], end = c->cnf->offsets[k+1];
        bool eligible = true;
        uint32_t active = 0;
        for (uint32_t j = begin; j < end; ++j) {
            uint32_t v = (uint32_t)c->cnf->literals[j];
            ++r->analysis_ops;
            if (forced && forced[v]) continue;
            if (!c->group[v] || c->used[v]) { eligible = false; break; }
            ++active;
        }
        if (!eligible) continue;
        if (c->selected) c->selected[r->alo_selected] = k;
        ++r->alo_selected;
        ++r->demand;
        if (!active) {
            /* Every literal in this demand implies its own negation. */
            r->forced_clause = k + 1;
            r->fired = true;
            break;
        }
        if (c->trace && r->alo_selected <= 128)
            fprintf(c->trace, "c demand-clause %u\n", k+1);
        for (uint32_t j = begin; j < end; ++j) {
            uint32_t v = (uint32_t)c->cnf->literals[j];
            if (forced && forced[v]) continue;
            c->used[v] = 1;
            uint32_t group = c->group[v];
            if (!c->touched[group]) { c->touched[group] = 1; ++r->capacity; }
        }
    }
    r->fired |= r->demand > r->capacity;
    if (c->trace) {
        /* Print each sampled group in a single linear pass over the variables. */
        for (uint32_t v = 1; v < c->n; ++v) {
            if (c->group[v] && c->group[v] <= 128)
                fprintf(c->trace, "c group %u member %u\n", c->group[v], v);
        }
        for (uint32_t g = 1; g <= r->amo_groups && g <= 128; ++g)
            if (c->touched[g]) fprintf(c->trace, "c touched-group %u\n", g);
    }
    return !r->fired || !c->certificate || certify(c, forced);
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static bool cliques(counter *c, graph *g, const uint8_t *forced)
{
    uint32_t *candidates = calloc(c->n, sizeof(*candidates));
    uint32_t *mark = calloc(c->n, sizeof(*mark));
    if (!candidates || !mark) { free(candidates); free(mark); return false; }
    for (uint32_t v = 1; v < c->n; ++v) {
        if (c->group[v] || (forced && forced[v])) continue;
        uint32_t count = 0;
        bool sorted = true;
        for (uint32_t e = g->offset[v]; e < g->offset[v+1]; ++e) {
            uint32_t w = g->edge[e];
            ++c->report.analysis_ops;
            if (c->group[w] || (forced && forced[w]) || mark[w] == v) continue;
            mark[w] = v;
            if (count && candidates[count-1] > w) sorted = false;
            candidates[count++] = w;
        }
        if (!count) continue;
        if (!sorted) qsort(candidates, count, sizeof(*candidates), compare_u32);
        uint32_t group = ++c->report.amo_groups;
        c->group[v] = group;
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t member = candidates[k];
            c->group[member] = group;
            if (k+1 == count) break;
            /* Each selected vertex is visited once. Use it as the stamp for
             * its upward neighbors, then intersect with surviving candidates. */
            for (uint32_t e = g->offset[member]; e < g->offset[member+1]; ++e)
                mark[g->edge[e]] = member;
            c->report.analysis_ops += g->offset[member+1] - g->offset[member];
            uint32_t keep = k+1;
            for (uint32_t j = k+1; j < count; ++j)
                if (mark[candidates[j]] == member) candidates[keep++] = candidates[j];
            count = keep;
        }
    }
    free(candidates);
    free(mark);
    return evaluate(c, forced);
}

static bool chains(counter *c, const graph *g)
{
    size_t nodes = (size_t)c->n * 2;
    uint8_t *incoming = calloc(nodes, 1);
    uint32_t *visited = calloc(nodes, sizeof(*visited));
    uint32_t *in_chain = calloc(c->n, sizeof(*in_chain));
    uint32_t *members = calloc(c->n, sizeof(*members));
    uint64_t *conflict = calloc(c->n, sizeof(*conflict));
    bool ok = incoming && visited && in_chain && members && conflict;
    if (!ok) goto done;
    for (size_t u = 2; u < nodes; ++u) {
        uint8_t flag = (!(u & 1) && c->positive[u/2]) ? 1 : 2;
        for (uint32_t e = g->offset[u]; e < g->offset[u+1]; ++e)
            incoming[g->edge[e]] |= flag;
    }
    uint64_t stamp = 0;
    for (uint32_t root = 2; root < nodes; ++root) {
        if (incoming[root] != 1) continue;
        uint32_t current = root, count = 0, reverse = root^1;
        for (uint32_t e = g->offset[reverse]; e < g->offset[reverse+1]; ++e) {
            uint32_t lit = g->edge[e], v = lit/2;
            if ((lit & 1) && c->positive[v] && !c->group[v]) {
                members[count++] = v; in_chain[v] = root; break;
            }
        }
        if (!count) continue;
        /* All accepted members imply current. A new member must conflict
         * with current and imply the next node, preserving that invariant. */
        for (;;) {
            visited[current] = root;
            ++stamp;
            uint32_t next = 0, terminal = 0;
            for (uint32_t e = g->offset[current]; e < g->offset[current+1]; ++e) {
                uint32_t lit = g->edge[e], v = lit/2;
                ++c->report.analysis_ops;
                if ((lit & 1) && c->positive[v]) {
                    if (!c->group[v] && in_chain[v] != root) {
                        conflict[v] = stamp;
                        if (!terminal) terminal = v;
                    }
                } else if (!next && lit != (current^1) && visited[lit] != root) {
                    next = lit;
                }
            }
            if (!next) {
                if (terminal) members[count++] = terminal;
                break;
            }
            reverse = next^1;
            for (uint32_t e = g->offset[reverse]; e < g->offset[reverse+1]; ++e) {
                uint32_t lit = g->edge[e], v = lit/2;
                ++c->report.analysis_ops;
                if ((lit & 1) && conflict[v] == stamp) {
                    members[count++] = v; in_chain[v] = root; break;
                }
            }
            current = next;
        }
        if (count >= 2) {
            uint32_t group = ++c->report.amo_groups;
            for (uint32_t j = 0; j < count; ++j) c->group[members[j]] = group;
        }
    }
    ok = evaluate(c, NULL);
done:
    free(incoming); free(visited); free(in_chain); free(members); free(conflict);
    return ok;
}

static bool implied(counter *c, const graph *g)
{
    size_t nodes = (size_t)c->n * 2;
    uint32_t *visited = calloc(nodes, sizeof(*visited));
    uint32_t *queue = calloc(nodes, sizeof(*queue));
    uint8_t *forced = calloc(c->n, 1);
    graph conflicts = { .offset = calloc((size_t)c->n+1, sizeof(uint32_t)) };
    bool ok = visited && queue && forced && conflicts.offset;
    if (!ok) goto done;
    uint32_t length = 0, capacity = 0, budget = 4000000;
    for (uint32_t v = 1; v < c->n; ++v) {
        conflicts.offset[v+1] = length;
        if (!c->positive[v]) continue;
        uint32_t head = 0, tail = 1;
        queue[0] = 2*v;
        visited[2*v] = v;
        while (head < tail) {
            uint32_t u = queue[head++];
            for (uint32_t e = g->offset[u]; e < g->offset[u+1]; ++e) {
                if (!budget) goto done; /* no verdict from an incomplete traversal */
                --budget;
                ++c->report.analysis_ops;
                uint32_t lit = g->edge[e], w = lit/2;
                if (visited[lit] == v) continue;
                visited[lit] = v;
                queue[tail++] = lit;
                if (!(lit & 1)) continue;
                if (v == w) forced[v] = 1;
                if (w <= v || !c->positive[w]) continue;
                if (length == capacity) {
                    capacity = capacity ? 2*capacity : 128;
                    uint32_t *p = realloc(conflicts.edge, (size_t)capacity * sizeof(*p));
                    if (!p) { ok = false; goto done; }
                    conflicts.edge = p;
                }
                conflicts.edge[length++] = w;
            }
        }
        conflicts.offset[v+1] = length;
    }
    start_groups(c, "implied");
    ok = cliques(c, &conflicts, forced);
done:
    destroy_graph(&conflicts); free(visited); free(queue); free(forced);
    return ok;
}

ds_status ds_count_traced(const ds_cnf *f, ds_verdict *out, ds_count_report *report, FILE *trace, ds_count_certificate *certificate)
{
    if (!f || !out) return DS_ERR_INVALID_ARGUMENT;
    counter c = { .cnf = f, .n = f->variable_count+1, .trace = trace, .certificate = certificate };
    graph direct = {0}, big = {0};
    bool ok = true;
    if (f->variable_count > INT32_MAX-1) return DS_ERR_CAPACITY;
    *out = DS_VERDICT_UNKNOWN;
    if (!f->clause_count || !f->variable_count) goto done;
    c.demands = calloc(f->clause_count, sizeof(*c.demands));
    c.positive = calloc(c.n, 1);
    c.group = calloc(c.n, sizeof(*c.group));
    c.used = calloc(c.n, 1);
    c.touched = calloc(c.n, 1);
    ok = c.demands && c.positive && c.group && c.used && c.touched;
    if (!ok) goto done;
    bool direct_pairs = false, mixed = false;
    for (uint32_t i = 0; i < f->clause_count; ++i) {
        uint32_t begin = f->offsets[i], end = f->offsets[i+1], j = begin;
        if (end-begin == 2) {
            ds_literal a = f->literals[begin], b = f->literals[begin+1];
            if (a < 0 && b < 0) direct_pairs |= a != b;
            else mixed = true;
        }
        while (j < end && f->literals[j] > 0) ++j;
        if (begin == end || j != end) continue;
        c.demands[c.demand_count++] = i;
        for (j = begin; j < end; ++j) c.positive[f->literals[j]] = 1;
    }
    c.report.analysis_ops += f->clause_count;
    if (!c.demand_count) goto done;
    if (certificate) {
        c.selected = calloc(c.demand_count, sizeof(*c.selected));
        if (!c.selected) { ok = false; goto done; }
    }
    if (direct_pairs) {
        start_groups(&c, "direct");
        ok = make_graph(&c, false, &direct) && cliques(&c, &direct, NULL);
        destroy_graph(&direct); direct = (graph){0};
        if (!ok || c.report.fired) goto done;
    }
    if (mixed) {
        start_groups(&c, "chain");
        ok = make_graph(&c, true, &big) && chains(&c, &big);
        if (ok && !c.report.fired) ok = implied(&c, &big);
    }
done:
    if (ok && c.report.fired) *out = DS_VERDICT_UNSAT;
    if (report) *report = c.report;
    destroy_graph(&direct); destroy_graph(&big);
    free(c.selected); free(c.demands); free(c.positive); free(c.used); free(c.group); free(c.touched);
    return ok ? DS_OK : DS_ERR_OUT_OF_MEMORY;
}

ds_status ds_reason_count(const ds_cnf *cnf, ds_verdict *out, ds_count_report *report)
{
    return ds_count_traced(cnf, out, report, NULL, NULL);
}

ds_status ds_reason_count_certified(const ds_cnf *cnf, ds_verdict *out,
    ds_count_report *report, ds_count_certificate *certificate)
{
    return ds_count_traced(cnf, out, report, NULL, certificate);
}
