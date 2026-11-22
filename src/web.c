#define _GNU_SOURCE
#include <ctype.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include "app.h"

typedef struct {
    char *input, *result, *checks, *live;
    char label[96], address[48];
    ds_cdcl_config config;
    bool cdcl;
} session;

static const char *const datasets[] = {"public-v0.0.4", "native-v0.0.3", "native-v0.0.2", "reference", "repeat", "satsuma-v0.0.1", "live"};
static const char *const descriptions[] = {"Public benchmarks v0.0.4", "Native v0.0.3 comparison", "Native v0.0.2 comparison", "v0.0.1 reference", "v0.0.1 repeat",
    "v0.0.1 satsuma comparison", "Live native solve"};
#define DATASET_COUNT (sizeof(datasets) / sizeof(datasets[0]))
static const char *get(const struct evkeyvalq *p, const char *key)
{
    const char *value = evhttp_find_header(p, key);
    return value ? value : "";
}

static void escape(FILE *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '<') fputs("&lt;",out);
        else if (s[i] == '>') fputs("&gt;",out);
        else if (s[i] == '&') fputs("&amp;",out);
        else if (s[i] == '"') fputs("&quot;",out);
        else fputc((unsigned char)s[i],out);
    }
}

static void send_body(struct evhttp_request *req, int code, const char *type, const char *body, size_t len)
{
    struct evbuffer *buffer = evbuffer_new();
    if (!buffer || evbuffer_add(buffer,body,len)) {
        if (buffer) evbuffer_free(buffer);
        evhttp_send_error(req,500,"Memory error");
        return;
    }
    struct evkeyvalq *h = evhttp_request_get_output_headers(req);
    evhttp_add_header(h,"Content-Type",type);
    evhttp_add_header(h,"Cache-Control","no-store");
    evhttp_add_header(h,"X-Content-Type-Options","nosniff");
    evhttp_add_header(h,"Content-Security-Policy","default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'");
    evhttp_send_reply(req,code,NULL,buffer);
    evbuffer_free(buffer);
}

static void notice(struct evhttp_request *r, int code, const char *s)
{
    send_body(r,code,"text/plain; charset=utf-8",s,strlen(s));
}

static void redirect(struct evhttp_request *r, const char *path)
{
    evhttp_add_header(evhttp_request_get_output_headers(r),"Location",path);
    evhttp_send_reply(r,303,NULL,NULL);
}

static void header(FILE *fp, const char *title)
{
    fputs("<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>",fp);
    fprintf(fp,"%s | dense-solver</title>",title);
    fputs("<style>*{box-sizing:border-box}body{margin:0;background:#f5f7f8;color:#18252b;font:16px/1.5 'Inter',system-ui,sans-serif}"
          "header{background:#17343b;color:white;padding:18px max(5vw,20px);display:flex;align-items:center;gap:32px;flex-wrap:wrap}"
          "header strong{font-size:21px}nav{display:flex;gap:20px;flex-wrap:wrap}nav a{color:#dcf4eb}a{color:#126756}"
          "main{max-width:1220px;margin:auto;padding:24px}h1{font-size:30px;margin:0 0 16px}h2{font-size:18px;margin:0 0 10px}"
          "section,details{background:white;border:1px solid #cdd7da;border-radius:8px;padding:17px;margin-bottom:14px;overflow:auto}"
          ".grid{display:grid;grid-template-columns:minmax(240px,1fr) minmax(380px,2fr);gap:16px;align-items:start}"
          "@media(max-width:760px){.grid{display:block}main{padding:14px}}label{display:inline-block;margin:3px 12px 7px 0}"
          "input,select,textarea,button{font:inherit}textarea{width:100%;min-height:170px;font:13px/1.45 monospace}"
          "input[type=number]{width:7em}button{margin:4px 3px 4px 0;background:#175e52;color:white;border:0;border-radius:4px;padding:6px 12px;cursor:pointer}"
          "input,select,textarea{border:1px solid #a6b7ba;border-radius:4px;padding:4px}table{border-collapse:collapse;width:100%;font-size:14px}"
          "th,td{text-align:right;padding:5px 7px;border-bottom:1px solid #e1e8e8}th:first-child,td:first-child{text-align:left}"
          "pre{overflow:auto;max-height:60vh;font:13px/1.55 monospace}.muted{color:#466168}summary{cursor:pointer}meter{max-width:80px}</style>"
          "<header><strong>dense-solver</strong><nav><a href='/'>Solver</a><a href='/structure'>Structure</a>"
          "<a href='/bench'>Benchmarks</a><a href='/verify'>Verification</a></nav></header><main>",fp);
    fprintf(fp,"<h1>%s</h1>",title);
}

static bool replace_input(session *s, char *input, const char *label)
{
    if (!input) return false;
    free(s->input); free(s->result);
    s->input = input; s->result = NULL;
    snprintf(s->label,sizeof(s->label),"%s",label);
    return true;
}

static bool load_formula(session *s, ds_cnf *f, const char *label)
{
    char *buf = NULL;
    size_t n = 0;
    FILE *fp = open_memstream(&buf,&n);
    if (!fp) return false;
    ds_status status = ds_cnf_write_dimacs(f,fp);
    if (fclose(fp) || status != DS_OK) { free(buf); return false; }
    return replace_input(s,buf,label);
}

static bool load_preset(session *s, size_t i)
{
    ds_cnf f = {0};
    bool ok = ds_preset(&f,i) == DS_OK && load_formula(s,&f,ds_presets[i]);
    ds_cnf_destroy(&f);
    return ok;
}

static void option(FILE *fp, const char *value, const char *label, bool selected)
{
    fprintf(fp,"<option value='%s'%s>%s</option>",value,selected ? " selected" : "",label);
}

static void configure_form(FILE *fp, const session *s)
{
    const ds_cdcl_config *c = &s->config;
    fputs("<label>Engine <select name=engine>",fp);
    option(fp,"reasoned","Reasoned",!s->cdcl); option(fp,"cdcl","CDCL alone",s->cdcl);
    fputs("</select></label><label>Branching <select name=heuristic>",fp);
    option(fp,"activity","Activity",c->heuristic==DS_BRANCH_ACTIVITY);
    option(fp,"fixed","Fixed",c->heuristic==DS_BRANCH_FIXED);
    option(fp,"placebo","Placebo",c->heuristic==DS_BRANCH_PLACEBO);
    fputs("</select></label><details><summary>Search settings</summary>",fp);
    fprintf(fp,"<label><input type=checkbox name=restarts value=on %s> Restarts</label>"
        "<label><input type=checkbox name=reduce value=on %s> Reduce learned clauses</label><br>",
        c->enable_restarts?"checked":"",c->enable_db_reduction?"checked":"");
    const char *keys[] = {"luby","interval","decisions","conflicts","seed"};
    const char *labels[] = {"Luby unit","Reduction interval","Decision budget","Conflict budget","Placebo seed"};
    uint64_t values[] = {c->luby_unit,c->db_reduction_interval,c->decision_budget,c->conflict_budget,c->placebo_seed};
    for (size_t i = 0; i < 5; ++i)
        fprintf(fp,"<label>%s <input name=%s inputmode=numeric value='%llu'></label>",labels[i],keys[i],(unsigned long long)values[i]);
    fputs("<p class=muted>A zero decision or conflict budget allows unlimited search.</p></details>",fp);
}

static void solver_page(FILE *fp, session *s, bool edit)
{
    header(fp,"Solver");
    fputs("<div class=grid><aside><section><h2>Benchmark instances</h2><form method=post action='/preset'><select name=id>",fp);
    for (size_t i = 0; i < ds_preset_count; ++i)
        fprintf(fp,"<option value='%zu'%s>%s</option>",i,!strcmp(s->label,ds_presets[i])?" selected":"",ds_presets[i]);
    fputs("</select> <button>Load</button></form></section>"
        "<section><h2>Pigeonhole generator</h2><form method=post action='/generate'>"
        "<label>Pigeons <input type=number min=1 max=100 name=p value=9 required></label>"
        "<label>Holes <input type=number min=1 max=100 name=h value=8 required></label>"
        "<label>Encoding <select name=encoding><option>pairwise</option><option>sequential</option></select></label>"
        "<button>Generate</button></form></section><section><h2>Open a DIMACS file</h2>"
        "<form method=post action='/upload' enctype='multipart/form-data'><input type=file name=file required><button>Load file</button></form>"
        "</section></aside><div><section><h2>",fp);
    escape(fp,s->label,strlen(s->label));
    fputs("</h2><form method=post action='/solve'>",fp);
    size_t n = strlen(s->input);
    if (n <= 120000 || edit) {
        fputs("<label for=cnf>DIMACS input</label><textarea id=cnf name=cnf spellcheck=false>",fp);
        escape(fp,s->input,n); fputs("</textarea>",fp);
    } else {
        fprintf(fp,"<p>%zu bytes. <a href='/?edit=1'>Edit the full input</a></p><pre>",n);
        escape(fp,s->input,1500); fputs("\n...\n",fp); escape(fp,s->input+n-500,500); fputs("</pre>",fp);
    }
    configure_form(fp,s);
    fputs("<button>Solve</button> <button formaction='/save'>Download DIMACS</button></form></section>",fp);
    if (s->result) {
        fputs("<section><h2>Solver output</h2><pre>",fp);
        for (const char *p=s->result; *p;) {
            size_t len=strcspn(p,"\n");
            if (!strncmp(p,"s ",2) || !strncmp(p,"v ",2) || !strncmp(p,"c method",8) ||
                !strncmp(p,"c demand ",9) || !strncmp(p,"c parse_ns ",11) || !strncmp(p,"c solver error",14))
                { escape(fp,p,len); fputc('\n',fp); }
            p += len + (p[len] == '\n');
        }
        fputs("</pre><a href='/structure'>Inspect the reasoning</a> &nbsp; <a href='/output'>Download output</a></section>",fp);
    }
    fputs("</div></div>",fp);
}

static FILE *open_data(const session *s, size_t i)
{
    if (!strcmp(datasets[i], "live")) return s->live ? fmemopen(s->live,strlen(s->live),"r") : NULL;
    char name[128];
    snprintf(name,sizeof(name),"results/%s.csv",datasets[i]);
    return fopen(name,"r");
}

static void public_input_options(FILE *out, const char *selected)
{
    FILE *manifest = fopen("bench/public-inputs.csv", "r");
    if (!manifest) return;
    char *line = NULL;
    size_t size = 0;
    /* Input IDs are the unquoted first column of our manifest. */
    if (getline(&line, &size, manifest) >= 0) {
        while (getline(&line, &size, manifest) >= 0) {
            size_t n = strspn(line, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-");
            if (n == 0 || n >= 100 || line[n] != ',') continue;
            line[n] = 0;
            option(out, line, line, !strcmp(line, selected));
        }
    }
    free(line);
    fclose(manifest);
}

static void bench_page(FILE *fp, session *s, const struct evkeyvalq *q)
{
    header(fp,"Benchmarks");
    const char *filter = get(q,"scope"), *instance = get(q,"instance");
    size_t data = 0;
    for (size_t i=0; i<DATASET_COUNT; ++i) if (!strcmp(get(q,"data"),datasets[i])) data=i;
    if (!strcmp(filter,"all") || (!strcmp(datasets[data],"public-v0.0.4") && !strcmp(filter,"solve"))) filter="";
    if (!strcmp(instance,"all")) instance="";
    fputs("<section><form action='/bench'><label>Dataset <select name=data>",fp);
    for(size_t i=0;i<DATASET_COUNT;++i) option(fp,datasets[i],descriptions[i],i==data);
    fputs("</select></label><label>Clock <select name=scope>",fp);
    option(fp,"all","All clocks",!*filter);
    if (strcmp(datasets[data],"public-v0.0.4")) option(fp,"solve","Solve only",!strcmp(filter,"solve"));
    option(fp,"process","Full process",!strcmp(filter,"process"));
    fputs("</select></label><label>Instance <select name=instance>",fp);
    option(fp,"all","All instances",!*instance);
    if (!strcmp(datasets[data], "public-v0.0.4")) {
        public_input_options(fp, instance);
    } else {
        for (size_t i=0;i<6;++i) option(fp,ds_presets[i],ds_presets[i],!strcmp(instance,ds_presets[i]));
    }
    if (!strcmp(datasets[data], "native-v0.0.3")) {
        static const char *const extra_inputs[] = {
            "chessboard12", "chessboard16", "chessboard32",
            "php8x8-pairwise", "php8x8-sequential", "php9x8-bitwise",
            "php9x8-cardnetwrk", "php9x8-kmtotalizer", "php9x8-ladder",
            "php9x8-mtotalizer", "php9x8-pairwise-functional",
            "php9x8-sequential-renamed", "php9x8-sortnetwrk", "php9x8-totalizer"
        };
        for (size_t i = 0; i < sizeof(extra_inputs) / sizeof(extra_inputs[0]); ++i)
            option(fp, extra_inputs[i], extra_inputs[i], !strcmp(instance, extra_inputs[i]));
    }
    fputs("</select></label><button>Apply</button></form><form method=post action='/live'>"
        "<label>Trials <input type=number name=trials min=1 max=100 value=7></label><button>Run native benchmark</button></form></section>",fp);
    fprintf(fp,"<section><h2>%s</h2>",descriptions[data]);
    FILE *csv = open_data(s,data);
    if (csv) {
        if (ds_report(fp,csv,*filter?filter:NULL,*instance?instance:NULL,true)) fputs("<p>Invalid benchmark CSV.</p>",fp);
        fclose(csv);
        fprintf(fp,"<p><a href='/csv?data=%s'>Download trials</a></p>",datasets[data]);
    } else fputs(!strcmp(datasets[data], "live")?"<p>Run the native benchmark to collect timings.</p>":"<p>Run the application from the release directory to read this dataset.</p>",fp);
    fputs("<p class=muted>Solve time starts after input loading. Full-process time includes startup, parsing and exit. Live trials use one running process.</p></section>",fp);
}

static bool solver_config(session *s, const struct evkeyvalq *q)
{
    ds_cdcl_config cfg = s->config;
    const char *mode = get(q,"engine");
    if (strcmp(mode,"reasoned") && strcmp(mode,"cdcl")) return false;
    const char *keys[] = {"heuristic","luby","interval","decisions","conflicts","seed"};
    for (size_t i=0; i<sizeof(keys)/sizeof(*keys); ++i)
        if (!ds_configure(&cfg,keys[i],get(q,keys[i]))) return false;
    cfg.enable_restarts = !strcmp(get(q,"restarts"),"on");
    cfg.enable_db_reduction = !strcmp(get(q,"reduce"),"on");
    s->config = cfg; s->cdcl = !strcmp(mode,"cdcl");
    return true;
}

static bool run_session(session *s)
{
    ds_cnf f = {0};
    uint64_t start = ds_now_ns();
    FILE *input = fmemopen(s->input,strlen(s->input),"r");
    if (!input) return false;
    ds_status status = ds_cnf_parse_dimacs(&f,input);
    fclose(input);
    uint64_t elapsed = ds_now_ns()-start;
    char *buf = NULL; size_t length = 0;
    FILE *fp = open_memstream(&buf,&length);
    if (!fp) { ds_cnf_destroy(&f); return false; }
    if (status != DS_OK) fprintf(fp,"c solver error DIMACS input (%s)\n",ds_status_string(status));
    else if (ds_run(fp,&f,&s->config,s->cdcl,false,true,true,false,elapsed)==1)
        fputs("c solver error Could not complete this solve.\n",fp);
    ds_cnf_destroy(&f);
    if (fclose(fp)) { free(buf); return false; }
    free(s->result); s->result = buf;
    return true;
}

/* This endpoint accepts the single file field of the upload form. Use the
 * opening delimiter to locate the closing boundary. The filename is never
 * interpreted as a path. */
static bool upload(session *s, const char *body, size_t n)
{
    if (n < 8 || strncmp(body,"--",2)) return false;
    const char *first = strstr(body,"\r\n");
    if (!first || first-body > 72 || first == body+2) return false;
    const char *data = strstr(first+2,"\r\n\r\n");
    if (!data || data-first > 16384) return false;
    data += 4;
    char boundary[80];
    snprintf(boundary,sizeof(boundary),"\r\n%.*s--",(int)(first-body),body);
    const char *end = strstr(data,boundary);
    if (!end || (end[strlen(boundary)] && strcmp(end+strlen(boundary),"\r\n"))) return false;
    return replace_input(s,strndup(data,(size_t)(end-data)),"Uploaded DIMACS");
}

static void post(struct evhttp_request *r, const char *path, session *s)
{
    struct evbuffer *buffer = evhttp_request_get_input_buffer(r);
    size_t n = evbuffer_get_length(buffer);
    char *body = malloc(n+1);
    if (!body) { notice(r,500,"Out of memory"); return; }
    evbuffer_copyout(buffer,body,n); body[n]=0;
    if (memchr(body,0,n)) { free(body); notice(r,400,"Invalid text"); return; }
    const char *type = get(evhttp_request_get_input_headers(r),"Content-Type");
    if (!strcmp(path,"/upload")) {
        bool ok = !strncmp(type,"multipart/form-data",19) && upload(s,body,n);
        free(body);
        if (ok) redirect(r,"/"); else notice(r,400,"Invalid file upload");
        return;
    }
    struct evkeyvalq q = {0};
    int invalid = strncmp(type,"application/x-www-form-urlencoded",33) || strstr(body,"%00") != NULL;
    if (invalid || evhttp_parse_query_str(body, &q)) {
        free(body); notice(r,400,"Invalid form"); return;
    }
    const char *next = "/", *text = evhttp_find_header(&q,"cnf");
    bool ok = true;
    if (!strcmp(path,"/preset")) {
        uint64_t i;
        ok = ds_number(get(&q,"id"),ds_preset_count-1,&i) && load_preset(s,(size_t)i) && run_session(s);
    } else if (!strcmp(path,"/generate")) {
        uint64_t p,h; ds_cnf f = {0};
        const char *e = get(&q,"encoding"); bool seq = !strcmp(e,"sequential");
        ok = (seq || !strcmp(e,"pairwise")) && ds_number(get(&q,"p"),100,&p) && ds_number(get(&q,"h"),100,&h)
            && ds_generate(&f,(unsigned)p,(unsigned)h,seq)==DS_OK;
        if (ok) {
            char label[64]; snprintf(label,sizeof(label),"php%ux%u-%s",(unsigned)p,(unsigned)h,e);
            ok = load_formula(s,&f,label);
        }
        ds_cnf_destroy(&f);
    } else if (!strcmp(path,"/solve") || !strcmp(path,"/save")) {
        if (!strcmp(path,"/save")) {
            const char *download = text ? text : s->input;
            evhttp_add_header(evhttp_request_get_output_headers(r),"Content-Disposition","attachment; filename=formula.cnf");
            send_body(r,200,"text/plain; charset=utf-8",download,strlen(download)); next=NULL;
        } else {
            ok = solver_config(s,&q);
            if (ok && text && strcmp(text,s->input)) ok = replace_input(s,strdup(text),"Edited DIMACS");
            if (ok) ok = run_session(s);
        }
    } else if (!strcmp(path,"/live") || !strcmp(path,"/verify")) {
        bool live = !strcmp(path,"/live"); uint64_t trials = 7;
        ok = !live || (ds_number(get(&q,"trials"),100,&trials) && trials);
        if (ok) {
            char *buf=NULL; size_t length=0; FILE *fp=open_memstream(&buf,&length);
            if (fp) {
                int rc = live ? ds_benchmark(fp,(unsigned)trials) : ds_verify(fp);
                ok = !fclose(fp) && (!live || !rc);
                if (ok) { char **slot=live?&s->live:&s->checks; free(*slot); *slot=buf; }
                else free(buf);
            } else ok=false;
        }
        next = live ? "/bench?data=live&scope=solve" : "/verify";
    } else { next=NULL; notice(r,404,"Unknown action"); }
    if (!ok) notice(r,400,"Invalid settings or unavailable input. Go back and try again.");
    else if (next) redirect(r,next);
    evhttp_clear_headers(&q); free(body);
}

typedef struct {
    uint32_t shown, count, values[12];
    bool touched;
} sample_group;

static void structure(FILE *fp, session *s)
{
    header(fp,"Structure");
    if (!s->result) { fputs("<section>Solve a formula to inspect its structure.</section>",fp); return; }
    ds_cnf f={0};
    FILE *input = fmemopen(s->input,strlen(s->input),"r");
    bool parsed = input && ds_cnf_parse_dimacs(&f,input)==DS_OK;
    if (input) fclose(input);
    sample_group groups[128]={0};
    uint32_t demands[128], selected=0, total=0, capacity=0, group_count=0, sample_count=0;
    char pass[24]="", method[24]="";
    uint32_t hall_demands = 0, hall_groups = 0;
    char *lines=strdup(s->result);
    if (!lines) { ds_cnf_destroy(&f); fputs("<section>Out of memory.</section>",fp); return; }
    char *state=NULL;
    for (char *l=strtok_r(lines,"\n",&state); l; l=strtok_r(NULL,"\n",&state)) {
        unsigned a,b;
        if (sscanf(l,"c counting pass %23s",pass)==1) {
            memset(groups,0,sizeof(groups)); selected=0; sample_count=0;
        } else if (sscanf(l,"c group %u member %u",&a,&b)==2 && a>=1 && a<=128) {
            sample_group *g=&groups[a-1];
            ++g->count;
            if (g->shown<12) g->values[g->shown++]=b;
            if (a>sample_count) sample_count=a;
        } else if (sscanf(l,"c touched-group %u",&a)==1 && a>=1 && a<=128) {
            groups[a-1].touched=true;
        } else if (sscanf(l,"c demand-clause %u",&a)==1 && selected<128) {
            demands[selected++]=a;
        } else if (sscanf(l,"c groups %u selected %*u demand %u capacity %u",&group_count,&total,&capacity)==3) {
            continue;
        } else if (sscanf(l,"c demand %u capacity %u",&hall_demands,&hall_groups)==2) {
            continue;
        } else if (sscanf(l,"c method %23s",method)==1) {
            continue;
        }
    }
    if (!strcmp(method, "hall")) {
        fprintf(fp,"<section><h2>Hall matching</h2><p>%u demands compete for %u groups.</p>",
            hall_demands,hall_groups);
        selected = sample_count = 0;
    } else fprintf(fp,"<section><h2>Demand and capacity</h2><p>Solver method <b>%s</b>. Counting pass <b>%s</b>.</p>"
        "<p>Disjoint demands <b>%u</b> &nbsp; Available groups <b>%u</b> &nbsp; Groups found <b>%u</b></p>",
        *method?method:"unknown",*pass?pass:"none",total,capacity,group_count);
    if (sample_count) {
        fputs("<details open><summary>At-most-one groups</summary><table><tr><th>Group</th><th>Variables (first 12)</th><th>Size</th><th>Used</th></tr>",fp);
        for (uint32_t i=0;i<sample_count;++i) {
            const sample_group *g=&groups[i];
            fprintf(fp,"<tr><td>%u</td><td>",i+1);
            for (uint32_t j=0;j<g->shown;++j) fprintf(fp,"%sx%u",j?", ":"",g->values[j]);
            fprintf(fp,"</td><td>%u</td><td>%s</td></tr>",g->count,g->touched?"yes":"");
        }
        fputs("</table></details>",fp);
    }
    if (selected && parsed) {
        fputs("<details><summary>Selected demand clauses</summary><table><tr><th>Clause</th><th>Variables (first 12)</th></tr>",fp);
        for (uint32_t i=0;i<selected;++i) {
            uint32_t id=demands[i];
            if (!id || id>f.clause_count) continue;
            fprintf(fp,"<tr><td>%u</td><td>",id);
            uint32_t begin=f.offsets[id-1], end=f.offsets[id];
            for (uint32_t j=begin;j<end && j-begin<12;++j) fprintf(fp,"%sx%d",j>begin?", ":"",f.literals[j]);
            fputs("</td></tr>",fp);
        }
        fputs("</table></details>",fp);
    }
    fputs("</section><section><h2>Parity rows and search work</h2><pre>",fp);
    for (const char *p=s->result; *p;) {
        size_t n=strcspn(p,"\n");
        if (strncmp(p,"c group ",8) && strncmp(p,"c touched-group ",16) && strncmp(p,"c demand-clause ",16)
            && strncmp(p,"v ",2)) { escape(fp,p,n); fputc('\n',fp); }
        p+=n+(p[n]=='\n');
    }
    fputs("</pre><a href='/output'>Download the full reasoning trace</a></section>",fp);
    free(lines); ds_cnf_destroy(&f);
}

static void handle(struct evhttp_request *r, void *arg)
{
    session *s=arg;
    const struct evhttp_uri *uri=evhttp_request_get_evhttp_uri(r);
    const char *path=uri?evhttp_uri_get_path(uri):NULL;
    struct evkeyvalq *headers=evhttp_request_get_input_headers(r);
    if (!path || strcmp(get(headers,"Host"),s->address)) { notice(r,400,"Invalid host"); return; }
    if (evhttp_request_get_command(r)==EVHTTP_REQ_POST) {
        char origin[72]; snprintf(origin,sizeof(origin),"http://%s",s->address);
        const char *sent=get(headers,"Origin");
        if ((*sent && strcmp(sent,origin)) || !strcmp(get(headers,"Sec-Fetch-Site"),"cross-site"))
            { notice(r,403,"Local requests only"); return; }
        post(r,path,s); return;
    }
    struct evkeyvalq q = {0};
    if (evhttp_parse_query_str(evhttp_uri_get_query(uri) ? evhttp_uri_get_query(uri) : "", &q))
        { notice(r,400,"Invalid query"); return; }
    char *buf=NULL; size_t length=0;
    FILE *fp=open_memstream(&buf,&length);
    const char *type="text/html; charset=utf-8";
    int code=200;
    if (!fp) { evhttp_clear_headers(&q); notice(r,500,"Out of memory"); return; }
    if (!strcmp(path,"/")) solver_page(fp,s,!strcmp(get(&q,"edit"),"1"));
    else if (!strcmp(path,"/bench")) bench_page(fp,s,&q);
    else if (!strcmp(path,"/verify")) {
        header(fp,"Verification");
        fputs("<section><form method=post action='/verify'><button>Run all checks</button></form>",fp);
        if (s->checks) { fputs("<pre>",fp); escape(fp,s->checks,strlen(s->checks)); fputs("</pre>",fp); }
        fputs("</section>",fp);
    } else if (!strcmp(path,"/structure")) {
        structure(fp,s);
    } else if (!strcmp(path,"/output")) {
        type="text/plain; charset=utf-8";
        evhttp_add_header(evhttp_request_get_output_headers(r),"Content-Disposition","attachment; filename=solution.txt");
        if (s->result) fputs(s->result,fp); else code=404;
    } else if (!strcmp(path,"/csv")) {
        type="text/csv; charset=utf-8"; code=404;
        for (size_t i=0;i<DATASET_COUNT;++i) if (!strcmp(datasets[i],get(&q,"data"))) {
            FILE *csv=open_data(s,i);
            if (csv) {
                char block[4096]; size_t used; code=200;
                while ((used=fread(block,1,sizeof(block),csv))) fwrite(block,1,used,fp);
                fclose(csv);
            }
        }
    } else { type="text/plain"; code=404; fputs("No such page",fp); }
    if (!strncmp(type,"text/html",9)) fputs("</main></html>",fp);
    if (fclose(fp)) notice(r,500,"Unable to render page");
    else send_body(r,code,type,buf,length);
    free(buf); evhttp_clear_headers(&q);
}

int main(int argc, char **argv)
{
    uint64_t port=8080;
    if (argc>2 || (argc==2 && (!ds_number(argv[1],65535,&port) || !port))) {
        fputs("Usage: dense-solver-web [PORT]\n",stderr); return 1;
    }
    session s={.config=ds_cdcl_config_default()};
    s.config.enable_db_reduction=true;
    snprintf(s.address,sizeof(s.address),"127.0.0.1:%u",(unsigned)port);
    if (!load_preset(&s,0) || !run_session(&s)) return 1;
    struct event_base *base=event_base_new();
    struct evhttp *http=base?evhttp_new(base):NULL;
    if (!http || evhttp_bind_socket(http,"127.0.0.1",(unsigned short)port)) {
        fputs("Could not bind the local workbench. Try a different port.\n",stderr);
        return 1;
    }
    evhttp_set_max_body_size(http,64*1024*1024);
    evhttp_set_timeout(http,30);
    evhttp_set_allowed_methods(http,EVHTTP_REQ_GET|EVHTTP_REQ_POST);
    evhttp_set_gencb(http,handle,&s);
    signal(SIGPIPE,SIG_IGN);
    printf("dense-solver at http://%s/\n",s.address); fflush(stdout);
    int rc=event_base_dispatch(base);
    evhttp_free(http); event_base_free(base);
    free(s.input); free(s.result); free(s.live); free(s.checks);
    return rc<0 ? 1 : 0;
}
