#include "slop.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Common error messages ─────────────────────────────── */

static void err_no_target(void) { fprintf(stderr, "slop: no file or directory specified\n"); }
static void err_cannot_stat(const char *p) { fprintf(stderr, "slop: cannot stat %s\n", p); }
static void err_cannot_read(const char *p) { fprintf(stderr, "slop: cannot read %s\n", p); }
static void err_bad_option(const char *o) { fprintf(stderr, "slop: unknown option %s\n", o); }

static void print_rule(int width) {
    printf("  ");
    for (int i = 0; i < width; i++) putchar('-');
    putchar('\n');
}

enum { RULE_W = 60, RULE_W_WIDE = 66, RULE_W_REPORT = 70, RULE_W_PARAM = 58 };

/* ── Output formatting ──────────────────────────────────── */

static const char *smell_kind_str(SmellKind k) {
    switch (k) {
    case SMELL_NARRATION:
        return "narration";
    case SMELL_COMMENT_GRADIENT:
        return "comment-decay";
    case SMELL_REDUP:
        return "redup";
    case SMELL_OVERWRAP:
        return "over-wrap";
    case SMELL_NAME_BREAK:
        return "naming-break";
    case SMELL_AS_ANY:
        return "as-any";
    case SMELL_TS_DIRECTIVE:
        return "ts-directive";
    case SMELL_ERR_SUPPRESS:
        return "err-suppress";
    case SMELL_ZOMBIE_PARAM:
        return "zombie-param";
    case SMELL_UNUSED_IMPORT:
        return "unused-import";
    case SMELL_MAGIC_STRING:
        return "magic-string";
    case SMELL_DEAD_CODE:
        return "dead-code";
    }
    return "?";
}

static const char *severity_str(SmellSeverity s) {
    switch (s) {
    case SEV_DIAGNOSTIC:
        return "DIAGNOSTIC";
    case SEV_CORRELATED:
        return "CORRELATED";
    case SEV_GENERAL:
        return "GENERAL";
    }
    return "?";
}

static void print_findings(const char *filename, const SmellReport *r) {
    if (r->count == 0) return;
    printf("\n  %s \xe2\x80\x94 %d finding%s\n", filename, r->count, r->count == 1 ? "" : "s");
    SmellSeverity last_sev = (SmellSeverity)-1;
    for (int i = 0; i < r->count; i++) {
        const SmellFinding *f = &r->items[i];
        if (f->severity != last_sev) {
            printf("\n  %s\n", severity_str(f->severity));
            last_sev = f->severity;
        }
        if (f->line > 0) printf("  :%d\t%-16s %s\n", f->line, smell_kind_str(f->kind), f->message);
        else printf("  \xe2\x80\x94\t%-16s %s\n", smell_kind_str(f->kind), f->message);
    }
}

static const char *score_label(double p) {
    if (p >= PROB_FLAGGED) return "likely AI";
    if (p >= PROB_SUSPICIOUS) return "suspicious";
    if (p >= PROB_INCONCLUSIVE) return "inconclusive";
    return "likely human";
}

static void print_score(const char *filename, const ScoreResult *sc) {
    printf("\n  %s \xe2\x80\x94 score %+.2f  P(AI) = %.3f  [%s]\n", filename, sc->raw_score,
           sc->probability, score_label(sc->probability));
    printf("\n  %-14s %-20s %-14s %s\n", "group", "signal", "measured", "LLR");
    print_rule(RULE_W);
    for (int i = 0; i < sc->signal_count; i++) {
        const SignalDetail *d = &sc->details[i];
        printf("  %-14s %-20s %-14s %+.2f\n", d->group, d->signal, d->measured, d->llr);
    }
    print_rule(RULE_W);
    printf("  %36s %+.2f\n", "raw total", sc->raw_score);
    printf("  %36s %+.2f\n", "scaled (/T)", sc->raw_score / sc->temperature);
}

/* ── JSON output helpers ────────────────────────────────── */

static void json_escape(const char *s, char *out, int outsz) {
    int j = 0;
    for (int i = 0; s[i] && j < outsz - 6; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c == '\r') {
            out[j++] = '\\';
            out[j++] = 'r';
        } else if (c == '\t') {
            out[j++] = '\\';
            out[j++] = 't';
        } else if (c < 0x20) {
            j += snprintf(out + j, (size_t)(outsz - j), "\\u%04x", c);
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
}

static void print_score_json(const char *filename, const ScoreResult *sc, int dead_lines) {
    char esc[4096];
    json_escape(filename, esc, sizeof(esc));
    printf("    {\"file\": \"%s\", \"score\": %.4f, \"probability\": %.4f, "
           "\"dead_lines\": %d, \"label\": \"%s\", \"signals\": [",
           esc, sc->raw_score, sc->probability, dead_lines, score_label(sc->probability));
    for (int i = 0; i < sc->signal_count; i++) {
        const SignalDetail *d = &sc->details[i];
        char mesc[64];
        json_escape(d->measured, mesc, sizeof(mesc));
        printf("%s{\"group\": \"%s\", \"signal\": \"%s\", \"measured\": \"%s\", "
               "\"llr\": %.4f}",
               i ? ", " : "", d->group, d->signal, mesc, d->llr);
    }
    printf("]}");
}

/* ── Duplicate collection helper ────────────────────────── */

static void collect_dupes(DupeResult *dr, const FileList *fl) {
    for (int i = 0; i < fl->count; i++) {
        size_t len = 0;
        char *content = util_read_file(fl->paths[i], &len);
        if (!content) continue;
        if (util_should_skip(content, len)) { free(content); continue; }
        dupes_add_file(dr, fl->paths[i], content, len);
        free(content);
    }
    dupes_compute(dr, NCD_THRESHOLD);
}

/* ── Shared scan helper ─────────────────────────────────── */

typedef struct {
    ScanResult scan;
    ScoreResult sc;
    double compression_ratio;
    GitFeatures gf;
    int dead_lines;
} FileScanCtx;

static bool scan_one_file(FileScanCtx *ctx, const char *path, const char *content, size_t len,
                          const Calibration *cal, double dup_ratio, bool use_git,
                          double prior_llr) {
    LangFamily lang = lang_detect(path);
    scan_file(&ctx->scan, path, content, len, lang);
    ctx->compression_ratio = compress_ratio(content, len);

    ctx->gf = (GitFeatures){};
    if (use_git) git_features(&ctx->gf, path);

    score_compute(&ctx->sc, &ctx->scan, cal, ctx->compression_ratio, dup_ratio,
                  use_git ? &ctx->gf : nullptr);
    ctx->sc.raw_score += prior_llr;
    ctx->sc.probability = slop_sigmoid(ctx->sc.raw_score / cal->temperature);
    ctx->dead_lines = smell_count_dead_lines(&ctx->scan);
    return true;
}

static void scan_one_free(FileScanCtx *ctx) { scan_free(&ctx->scan); }

/* ── Smell command ──────────────────────────────────────── */

typedef struct {
    int files_scanned;
    int files_with_findings;
    int total_findings;
    bool has_diagnostic;
    bool has_correlated;
} SmellStats;

static void process_smell(const char *path, bool include_all, SmellStats *stats) {
    size_t len = 0;
    char *content = util_read_file(path, &len);
    if (!content) { err_cannot_read(path); return; }
    if (util_should_skip(content, len)) { free(content); return; }

    LangFamily lang = lang_detect(path);
    ScanResult scan;
    scan_file(&scan, path, content, len, lang);

    SmellReport report;
    smell_detect(&report, &scan, include_all);

    stats->files_scanned++;
    if (report.count > 0) {
        stats->files_with_findings++;
        stats->total_findings += report.count;
        for (int i = 0; i < report.count; i++) {
            if (report.items[i].severity == SEV_DIAGNOSTIC) stats->has_diagnostic = true;
            if (report.items[i].severity == SEV_CORRELATED) stats->has_correlated = true;
        }
        print_findings(path, &report);
    }

    scan_free(&scan);
    free(content);
}

static int cmd_smell(int argc, char **argv) {
    bool include_all = false;
    const char *target = nullptr;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) include_all = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            return -1;
        } else if (argv[i][0] != '-') target = argv[i];
        else {
            err_bad_option(argv[i]);
            return 1;
        }
    }
    if (!target) {
        err_no_target();
        return -1;
    }

    SmellStats stats = {};
    struct stat st;
    if (stat(target, &st) != 0) { err_cannot_stat(target); return 1; }

    if (S_ISDIR(st.st_mode)) {
        FileList fl;
        fl_init(&fl);
        fl_collect(&fl, target);
        for (int i = 0; i < fl.count; i++) process_smell(fl.paths[i], include_all, &stats);
        fl_free(&fl);
    } else {
        process_smell(target, include_all, &stats);
    }

    if (stats.files_scanned > 1)
        printf("\n  %d file%s scanned, %d with findings (%d total)\n", stats.files_scanned,
               stats.files_scanned == 1 ? "" : "s", stats.files_with_findings,
               stats.total_findings);
    if (stats.total_findings == 0 && stats.files_scanned > 0) printf("\n  no smells found\n");
    printf("\n");

    if (stats.has_diagnostic) return 2;
    if (stats.has_correlated) return 1;
    return 0;
}

/* ── Scan command ───────────────────────────────────────── */

typedef struct {
    char path[4096];
    double score;
    double prob;
    int dead_lines;
    ScoreResult sc;
} ScanEntry;

typedef struct {
    int files_scanned;
    int files_skipped;
    int flagged;
    int suspicious;
    int human;
} ScanStats;

#define MAX_SCAN_ENTRIES 8192
static const char OPT_PRIOR[] = "--prior=";

static int entry_cmp(const void *a, const void *b) {
    const double pa = ((const typeof(ScanEntry) *)a)->prob;
    const double pb = ((const typeof(ScanEntry) *)b)->prob;
    return (pa < pb) ? 1 : (pa > pb) ? -1 : 0;
}

static void classify(ScanStats *ss, double prob) {
    ss->files_scanned++;
    if (prob >= PROB_FLAGGED) ss->flagged++;
    else if (prob >= PROB_SUSPICIOUS) ss->suspicious++;
    else ss->human++;
}

static int cmd_scan(int argc, char **argv) {
    const char *target = nullptr;
    bool verbose = false, use_git = false, json_out = false;
    double prior = DEFAULT_PRIOR;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) verbose = true;
        else if (strcmp(argv[i], "--git") == 0) use_git = true;
        else if (strcmp(argv[i], "--json") == 0) json_out = true;
        else if (strncmp(argv[i], OPT_PRIOR, sizeof(OPT_PRIOR) - 1) == 0) prior = atof(argv[i] + 8);
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) return -1;
        else if (argv[i][0] != '-') target = argv[i];
        else {
            err_bad_option(argv[i]);
            return 1;
        }
    }
    if (!target) {
        err_no_target();
        return -1;
    }

    Calibration cal;
    calibration_default(&cal);
    (void)calibration_load(&cal, ".");

    const double prior_llr = slop_prior_llr(prior);

    struct stat st;
    if (stat(target, &st) != 0) { err_cannot_stat(target); return 1; }

    if (!json_out && !cal.loaded)
        printf("\n  \xe2\x9a\xa0 UNCALIBRATED (T=%.1f)"
               " \xe2\x80\x94 run 'slop calibrate' for precision\n",
               cal.temperature);

    ScanStats ss = {};

    if (S_ISDIR(st.st_mode)) {
        FileList fl;
        fl_init(&fl);
        fl_collect(&fl, target);

        /*
         * Two-pass scan:
         *   Pass 1 — collect functions from every file for cross-file
         *            duplicate detection (Group E).
         *   Pass 2 — score every file with the real project-wide dup_ratio.
         */

        /* ── Pass 1: collect dupes ────────────────────────── */
        DupeResult dr;
        dupes_init(&dr);
        collect_dupes(&dr, &fl);
        const DupRatioResult drr = dupes_compute_ratio(&dr);
        double dup_ratio = drr.ratio;

        /* ── Pass 2: score each file with real dup_ratio ── */
        ScanEntry *entries = malloc(MAX_SCAN_ENTRIES * sizeof(ScanEntry));
        int entry_count = 0;

        for (int i = 0; i < fl.count; i++) {
            size_t len = 0;
            char *content = util_read_file(fl.paths[i], &len);
            if (!content) continue;
            if (util_should_skip(content, len)) {
                ss.files_skipped++;
                free(content);
                continue;
            }

            FileScanCtx ctx;
            scan_one_file(&ctx, fl.paths[i], content, len, &cal, dup_ratio, use_git, prior_llr);
            classify(&ss, ctx.sc.probability);

            if (verbose) {
                print_score(fl.paths[i], &ctx.sc);
                if (ctx.dead_lines > 0) printf("  dead_lines        = %d\n", ctx.dead_lines);
            } else if (entry_count < MAX_SCAN_ENTRIES) {
                ScanEntry *e = &entries[entry_count++];
                snprintf(e->path, sizeof(e->path), "%s", fl.paths[i]);
                e->score = ctx.sc.raw_score;
                e->prob = ctx.sc.probability;
                e->dead_lines = ctx.dead_lines;
                e->sc = ctx.sc;
            }

            scan_one_free(&ctx);
            free(content);
        }

        /* ── Output ───────────────────────────────────────── */
        if (!verbose && !json_out && entry_count > 0) {
            qsort(entries, (size_t)entry_count, sizeof(ScanEntry), entry_cmp);
            printf("\n  scanned %d file%s", ss.files_scanned, ss.files_scanned == 1 ? "" : "s");
            if (ss.files_skipped > 0) printf(" (%d skipped: minified/generated)", ss.files_skipped);
            if (drr.total_funcs > 0)
                printf("\n  dup_ratio = %.2f (%d/%d functions)", drr.ratio, drr.funcs_in_dup,
                       drr.total_funcs);
            int total_dead = 0;
            for (int i = 0; i < entry_count; i++) total_dead += entries[i].dead_lines;

            printf("\n\n  %-7s %-7s %-5s %s\n", "P(AI)", "score", "dead", "file");
            print_rule(RULE_W);
            for (int i = 0; i < entry_count; i++) {
                const ScanEntry *e = &entries[i];
                if (e->dead_lines > 0)
                    printf("  %-7.3f %+6.2f  %-5d %s\n", e->prob, e->score, e->dead_lines, e->path);
                else printf("  %-7.3f %+6.2f  %-5s %s\n", e->prob, e->score, "-", e->path);
            }
            print_rule(RULE_W);
            printf("\n  %d flagged (P > %.2f) / %d suspicious (%.2f-%.2f)"
                   " / %d likely human\n",
                   ss.flagged, PROB_FLAGGED, ss.suspicious, PROB_SUSPICIOUS, PROB_FLAGGED,
                   ss.human);
            if (total_dead > 0) printf("  %d dead lines detected across project\n", total_dead);
        }

        if (json_out) {
            printf("{\n  \"calibrated\": %s,\n  \"temperature\": %.2f,\n",
                   cal.loaded ? "true" : "false", cal.temperature);
            printf("  \"dup_ratio\": %.4f,\n  \"files\": [\n", dup_ratio);
            for (int i = 0; i < entry_count; i++) {
                print_score_json(entries[i].path, &entries[i].sc, entries[i].dead_lines);
                printf("%s\n", i + 1 < entry_count ? "," : "");
            }
            printf("  ]\n}\n");
        }

        free(entries);
        dupes_free(&dr);
        fl_free(&fl);
    } else {
        size_t len = 0;
        char *content = util_read_file(target, &len);
        if (!content) {
            err_cannot_read(target);
            return 1;
        }
        if (util_should_skip(content, len)) {
            printf("\n  %s — skipped (binary/minified/generated)\n\n", target);
            free(content);
            return 0;
        }

        FileScanCtx ctx;
        scan_one_file(&ctx, target, content, len, &cal, -1, use_git, prior_llr);
        classify(&ss, ctx.sc.probability);

        if (json_out) {
            printf("[\n");
            print_score_json(target, &ctx.sc, ctx.dead_lines);
            printf("\n]\n");
        } else {
            print_score(target, &ctx.sc);
            if (ctx.dead_lines > 0) printf("  dead_lines        = %d\n", ctx.dead_lines);
        }

        scan_one_free(&ctx);
        free(content);
    }

    printf("\n");
    if (ss.flagged > 0) return 2;
    if (ss.suspicious > 0) return 1;
    return 0;
}

/* ── Scan stdin ─────────────────────────────────────────── */

static int cmd_scan_stdin(int argc, char **argv) {
    const char *lang_name = nullptr;
    bool json_out = false;
    double prior = DEFAULT_PRIOR;

    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--lang=", 7) == 0) lang_name = argv[i] + 7;
        else if (strcmp(argv[i], "--json") == 0) json_out = true;
        else if (strncmp(argv[i], OPT_PRIOR, sizeof(OPT_PRIOR) - 1) == 0)
            prior = atof(argv[i] + sizeof(OPT_PRIOR) - 1);
        else if (strcmp(argv[i], "--stdin") == 0) { /* already handled */ }
    }

    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fprintf(stderr, "slop: out of memory\n"); return 1; }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += n;
        if (len >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    buf[len] = '\0';

    char fake_name[64] = "<stdin>";
    LangFamily lang = LANG_C_LIKE;
    if (lang_name) {
        if (strcmp(lang_name, "python") == 0 || strcmp(lang_name, "py") == 0) lang = LANG_PYTHON;
        else if (strcmp(lang_name, "shell") == 0 || strcmp(lang_name, "sh") == 0 ||
                 strcmp(lang_name, "bash") == 0)
            lang = LANG_SHELL;
        snprintf(fake_name, sizeof(fake_name), "<stdin>.%s", lang_name);
    }

    Calibration cal;
    calibration_default(&cal);
    (void)calibration_load(&cal, ".");

    const double prior_llr = slop_prior_llr(prior);

    ScanResult scan;
    scan_file(&scan, fake_name, buf, len, lang);
    double cr = compress_ratio(buf, len);

    GitFeatures gf = {};
    ScoreResult sc;
    score_compute(&sc, &scan, &cal, cr, -1, &gf);
    sc.raw_score += prior_llr;
    sc.probability = slop_sigmoid(sc.raw_score / cal.temperature);

    if (json_out) {
        printf("[\n");
        print_score_json(fake_name, &sc, 0);
        printf("\n]\n");
    } else {
        if (!cal.loaded) printf("\n  \xe2\x9a\xa0 UNCALIBRATED (T=%.1f)\n", cal.temperature);
        print_score(fake_name, &sc);
    }
    printf("\n");

    scan_free(&scan);
    free(buf);
    return sc.probability >= PROB_FLAGGED ? 2 : sc.probability >= PROB_SUSPICIOUS ? 1 : 0;
}

/* ── Dupes command ──────────────────────────────────────── */

static void print_dupes(const DupeResult *dr, double threshold) {
    if (dr->cluster_count == 0) {
        printf("\n  no duplicate functions found (NCD < %.2f, min body %d bytes)\n", threshold,
               NCD_MIN_BODY_BYTES);
        return;
    }
    printf("\n  %d cluster%s (NCD < %.2f, min body %d bytes)\n", dr->cluster_count,
           dr->cluster_count == 1 ? "" : "s", threshold, NCD_MIN_BODY_BYTES);

    int max_cluster = 0;
    for (int i = 0; i < dr->func_count; i++)
        if (dr->funcs[i].cluster > max_cluster) max_cluster = dr->funcs[i].cluster;

    int *sizes = calloc((size_t)max_cluster + 2, sizeof(int));
    for (int i = 0; i < dr->func_count; i++)
        if (dr->funcs[i].cluster >= 0) sizes[dr->funcs[i].cluster]++;

    double *min_ncd = malloc(((size_t)max_cluster + 2) * sizeof(double));
    for (int c = 0; c <= max_cluster; c++) min_ncd[c] = 1.0;
    for (int p = 0; p < dr->pair_count; p++) {
        int cl = dr->funcs[dr->pairs[p].a].cluster;
        if (dr->pairs[p].ncd < min_ncd[cl]) min_ncd[cl] = dr->pairs[p].ncd;
    }

    int *order = malloc(((size_t)max_cluster + 2) * sizeof(int));
    int nclusters = 0;
    for (int c = 0; c <= max_cluster; c++)
        if (sizes[c] >= 2) order[nclusters++] = c;
    for (int i = 0; i < nclusters - 1; i++)
        for (int j = i + 1; j < nclusters; j++)
            if (sizes[order[i]] < sizes[order[j]]) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }

    int printed = 0;
    for (int ci = 0; ci < nclusters && printed < MAX_DUP_CLUSTERS; ci++) {
        int c = order[ci];
        printed++;
        printf("\n  cluster %d \xe2\x80\x94 NCD %.2f (%s)\n", printed, min_ncd[c],
               min_ncd[c] < 0.10   ? "nearly identical"
               : min_ncd[c] < 0.20 ? "very similar"
                                   : "similar");
        for (int i = 0; i < dr->func_count; i++) {
            if (dr->funcs[i].cluster != c) continue;
            printf("    %s:%d\t%s()\t[%d lines]\n", dr->funcs[i].filepath, dr->funcs[i].start_line,
                   dr->funcs[i].name[0] ? dr->funcs[i].name : "?", dr->funcs[i].line_count);
        }
    }
    free(order);
    free(sizes);
    free(min_ncd);
}

static int cmd_dupes(int argc, char **argv) {
    double threshold = NCD_THRESHOLD;
    const char *target = nullptr;
    bool cross_lang = false;

    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--threshold=", 12) == 0) threshold = atof(argv[i] + 12);
        else if (strcmp(argv[i], "--cross-lang") == 0) cross_lang = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) return -1;
        else if (argv[i][0] != '-') target = argv[i];
        else {
            err_bad_option(argv[i]);
            return 1;
        }
    }
    if (!target) {
        err_no_target();
        return -1;
    }

    struct stat st;
    if (stat(target, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "slop: %s is not a directory\n", target);
        return 1;
    }

    FileList fl;
    fl_init(&fl);
    fl_collect(&fl, target);

    DupeResult dr;
    dupes_init(&dr);
    for (int i = 0; i < fl.count; i++) {
        size_t len = 0;
        char *content = util_read_file(fl.paths[i], &len);
        if (!content) continue;
        if (!util_should_skip(content, len)) dupes_add_file(&dr, fl.paths[i], content, len);
        free(content);
    }
    fl_free(&fl);

    printf("\n  collected %d functions (>= %d bytes) from %s\n", dr.func_count, NCD_MIN_BODY_BYTES,
           target);

    if (cross_lang) {
        for (int i = 0; i < dr.func_count; i++) dr.funcs[i].lang = LANG_C_LIKE;
    }

    dupes_compute(&dr, threshold);
    print_dupes(&dr, threshold);

    int found = dr.cluster_count;
    dupes_free(&dr);
    printf("\n");
    return found > 0 ? 2 : 0;
}

/* ── Calibrate command ──────────────────────────────────── */

typedef struct {
    double *vals;
    int count;
    int cap;
} DVec;

static void dv_init(DVec *v) {
    v->count = 0;
    v->cap = 256;
    v->vals = malloc(256 * sizeof(double));
}

static void dv_push(DVec *v, double x) {
    if (v->count >= v->cap) {
        v->cap *= 2;
        v->vals = realloc(v->vals, (size_t)v->cap * sizeof(double));
    }
    v->vals[v->count++] = x;
}

static void dv_free(DVec *v) {
    free(v->vals);
    v->count = 0;
}

static double dv_mean(const DVec *v) {
    if (v->count == 0) return 0;
    double s = 0;
    for (int i = 0; i < v->count; i++) s += v->vals[i];
    return s / v->count;
}

static double dv_stdev(const DVec *v) {
    if (v->count < 2) return MIN_GAUSS_SIGMA;
    double m = dv_mean(v);
    double s = 0;
    for (int i = 0; i < v->count; i++) {
        double d = v->vals[i] - m;
        s += d * d;
    }
    double sd = sqrt(s / v->count);
    return sd < MIN_GAUSS_SIGMA ? MIN_GAUSS_SIGMA : sd;
}

typedef struct {
    DVec reg, ccr, narr, cond, decay, overwrap, namebrk, ident, func_cv, ttr, indent;
} MeasurementVecs;

static void mv_init(MeasurementVecs *mv) {
    dv_init(&mv->reg);
    dv_init(&mv->ccr);
    dv_init(&mv->narr);
    dv_init(&mv->cond);
    dv_init(&mv->decay);
    dv_init(&mv->overwrap);
    dv_init(&mv->namebrk);
    dv_init(&mv->ident);
    dv_init(&mv->func_cv);
    dv_init(&mv->ttr);
    dv_init(&mv->indent);
}

static void mv_free(MeasurementVecs *mv) {
    dv_free(&mv->reg);
    dv_free(&mv->ccr);
    dv_free(&mv->narr);
    dv_free(&mv->cond);
    dv_free(&mv->decay);
    dv_free(&mv->overwrap);
    dv_free(&mv->namebrk);
    dv_free(&mv->ident);
    dv_free(&mv->func_cv);
    dv_free(&mv->ttr);
    dv_free(&mv->indent);
}

static void collect_measurements(const char *dirpath, MeasurementVecs *mv) {
    FileList fl;
    fl_init(&fl);

    struct stat st;
    if (stat(dirpath, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) fl_collect(&fl, dirpath);
    else fl_add(&fl, dirpath);

    Calibration cal;
    calibration_default(&cal);

    for (int i = 0; i < fl.count; i++) {
        size_t len = 0;
        char *content = util_read_file(fl.paths[i], &len);
        if (!content) continue;
        if (util_should_skip(content, len)) { free(content); continue; }

        FileScanCtx ctx;
        scan_one_file(&ctx, fl.paths[i], content, len, &cal, -1, false, 0);

        dv_push(&mv->reg, ctx.sc.m_regularity);
        dv_push(&mv->ccr, ctx.sc.m_ccr);
        dv_push(&mv->narr, ctx.sc.m_narration_count >= NARRATION_TIER1 ? 1.0 : 0.0);
        dv_push(&mv->cond, ctx.sc.m_cond_density);
        dv_push(&mv->decay, ctx.sc.m_decay ? 1.0 : 0.0);
        dv_push(&mv->overwrap, ctx.scan.overwrap_count > 0 ? 1.0 : 0.0);
        dv_push(&mv->namebrk, ctx.scan.name_break_count > 0 ? 1.0 : 0.0);
        if (ctx.sc.m_ident_spec >= 0) dv_push(&mv->ident, ctx.sc.m_ident_spec);
        if (ctx.sc.m_func_cv >= 0) dv_push(&mv->func_cv, ctx.sc.m_func_cv);
        if (ctx.sc.m_ttr >= 0) dv_push(&mv->ttr, ctx.sc.m_ttr);
        if (ctx.sc.m_indent_reg >= 0) dv_push(&mv->indent, ctx.sc.m_indent_reg);

        scan_one_free(&ctx);
        free(content);
    }
    fl_free(&fl);
}

static double compute_ece(const double *probs, const int *labels, int n, int bins) {
    if (n == 0) return 1.0;
    double ece = 0;
    for (int b = 0; b < bins; b++) {
        double lo = (double)b / bins, hi = (double)(b + 1) / bins;
        int cnt = 0;
        double acc_sum = 0, conf_sum = 0;
        for (int i = 0; i < n; i++) {
            if (probs[i] >= lo && probs[i] < hi) {
                cnt++;
                acc_sum += labels[i];
                conf_sum += probs[i];
            }
        }
        if (cnt > 0) {
            double acc = acc_sum / cnt;
            double conf = conf_sum / cnt;
            ece += ((double)cnt / n) * fabs(acc - conf);
        }
    }
    return ece;
}

static double clampd_cal(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

static double gauss_llr_cal(double x, double mu_a, double s_a, double mu_h, double s_h) {
    double zh = (x - mu_h) / s_h;
    double za = (x - mu_a) / s_a;
    return clampd_cal(0.5 * (zh * zh - za * za) + log(s_h / s_a), -LLR_CLAMP, LLR_CLAMP);
}

static double binary_llr_cal(bool present, double p_ai, double p_h) {
    if (present) return clampd_cal(log(p_ai / p_h), -LLR_CLAMP, LLR_CLAMP);
    return clampd_cal(log((1.0 - p_ai) / (1.0 - p_h)), -LLR_CLAMP, LLR_CLAMP);
}

static double prob_from_measurements(const Calibration *cal, double reg, double ccr, bool narr,
                                     double cond, bool decay, bool overwrap, bool namebrk,
                                     double ident, double func_cv, double ttr, double indent) {
    double s = 0;
    s += gauss_llr_cal(reg, cal->regularity.mu_ai, cal->regularity.sig_ai, cal->regularity.mu_h,
                       cal->regularity.sig_h);
    s += gauss_llr_cal(ccr, cal->ccr.mu_ai, cal->ccr.sig_ai, cal->ccr.mu_h, cal->ccr.sig_h);
    s += binary_llr_cal(narr, cal->narr_p_ai, cal->narr_p_h);
    s += gauss_llr_cal(cond, cal->cond.mu_ai, cal->cond.sig_ai, cal->cond.mu_h, cal->cond.sig_h);
    s += binary_llr_cal(decay, cal->decay_p_ai, cal->decay_p_h);
    s += binary_llr_cal(overwrap, cal->overwrap_p_ai, cal->overwrap_p_h);
    s += binary_llr_cal(namebrk, cal->namebrk_p_ai, cal->namebrk_p_h);
    if (ident >= 0)
        s += gauss_llr_cal(ident, cal->ident.mu_ai, cal->ident.sig_ai, cal->ident.mu_h,
                           cal->ident.sig_h);
    if (func_cv >= 0)
        s += gauss_llr_cal(func_cv, cal->func_cv.mu_ai, cal->func_cv.sig_ai, cal->func_cv.mu_h,
                           cal->func_cv.sig_h);
    if (ttr >= 0)
        s += gauss_llr_cal(ttr, cal->ttr.mu_ai, cal->ttr.sig_ai, cal->ttr.mu_h, cal->ttr.sig_h);
    if (indent >= 0)
        s += gauss_llr_cal(indent, cal->indent.mu_ai, cal->indent.sig_ai, cal->indent.mu_h,
                           cal->indent.sig_h);
    return slop_sigmoid(s / cal->temperature);
}

static int cmd_calibrate(int argc, char **argv) {
    const char *ai_dir = nullptr, *human_dir = nullptr, *out_dir = ".";

    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--ai=", 5) == 0) ai_dir = argv[i] + 5;
        else if (strcmp(argv[i], "--ai") == 0 && i + 1 < argc) ai_dir = argv[++i];
        else if (strncmp(argv[i], "--human=", 8) == 0) human_dir = argv[i] + 8;
        else if (strcmp(argv[i], "--human") == 0 && i + 1 < argc) human_dir = argv[++i];
        else if (strncmp(argv[i], "--out=", 6) == 0) out_dir = argv[i] + 6;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) return -1;
    }

    if (!ai_dir || !human_dir) {
        fprintf(stderr, "slop calibrate --ai <dir> --human <dir> [--out=<dir>]\n");
        return 1;
    }

    MeasurementVecs ai, hu;
    mv_init(&ai);
    mv_init(&hu);

    printf("  scanning AI files from %s...\n", ai_dir);
    collect_measurements(ai_dir, &ai);
    printf("  scanning human files from %s...\n", human_dir);
    collect_measurements(human_dir, &hu);

    printf("  AI files: %d, Human files: %d\n", ai.reg.count, hu.reg.count);
    if (ai.reg.count < MIN_CAL_FILES || hu.reg.count < MIN_CAL_FILES) {
        fprintf(stderr, "slop: need at least %d files per class\n", MIN_CAL_FILES);
        mv_free(&ai);
        mv_free(&hu);
        return 1;
    }

    Calibration cal;
    cal.regularity =
        (GaussParam){dv_mean(&ai.reg), dv_stdev(&ai.reg), dv_mean(&hu.reg), dv_stdev(&hu.reg)};
    cal.ccr =
        (GaussParam){dv_mean(&ai.ccr), dv_stdev(&ai.ccr), dv_mean(&hu.ccr), dv_stdev(&hu.ccr)};
    cal.cond =
        (GaussParam){dv_mean(&ai.cond), dv_stdev(&ai.cond), dv_mean(&hu.cond), dv_stdev(&hu.cond)};
    cal.narr_p_ai = dv_mean(&ai.narr);
    cal.narr_p_h = dv_mean(&hu.narr);
    cal.decay_p_ai = dv_mean(&ai.decay);
    cal.decay_p_h = dv_mean(&hu.decay);
    cal.overwrap_p_ai = dv_mean(&ai.overwrap);
    cal.overwrap_p_h = dv_mean(&hu.overwrap);
    cal.namebrk_p_ai = dv_mean(&ai.namebrk);
    cal.namebrk_p_h = dv_mean(&hu.namebrk);
    if (cal.narr_p_ai < MIN_BINARY_PROB) cal.narr_p_ai = MIN_BINARY_PROB;
    if (cal.narr_p_h < MIN_BINARY_PROB) cal.narr_p_h = MIN_BINARY_PROB;
    if (cal.decay_p_ai < MIN_BINARY_PROB) cal.decay_p_ai = MIN_BINARY_PROB;
    if (cal.decay_p_h < MIN_BINARY_PROB) cal.decay_p_h = MIN_BINARY_PROB;
    if (cal.overwrap_p_ai < MIN_BINARY_PROB) cal.overwrap_p_ai = MIN_BINARY_PROB;
    if (cal.overwrap_p_h < MIN_BINARY_PROB) cal.overwrap_p_h = MIN_BINARY_PROB;
    if (cal.namebrk_p_ai < MIN_BINARY_PROB) cal.namebrk_p_ai = MIN_BINARY_PROB;
    if (cal.namebrk_p_h < MIN_BINARY_PROB) cal.namebrk_p_h = MIN_BINARY_PROB;
    if (ai.ident.count > 0 && hu.ident.count > 0)
        cal.ident = (GaussParam){dv_mean(&ai.ident), dv_stdev(&ai.ident), dv_mean(&hu.ident),
                                 dv_stdev(&hu.ident)};
    else {
        Calibration d;
        calibration_default(&d);
        cal.ident = d.ident;
    }
    if (ai.func_cv.count > 0 && hu.func_cv.count > 0)
        cal.func_cv = (GaussParam){dv_mean(&ai.func_cv), dv_stdev(&ai.func_cv),
                                   dv_mean(&hu.func_cv), dv_stdev(&hu.func_cv)};
    else {
        Calibration d;
        calibration_default(&d);
        cal.func_cv = d.func_cv;
    }
    if (ai.ttr.count > 0 && hu.ttr.count > 0)
        cal.ttr = (GaussParam){dv_mean(&ai.ttr), dv_stdev(&ai.ttr), dv_mean(&hu.ttr),
                               dv_stdev(&hu.ttr)};
    else {
        Calibration d;
        calibration_default(&d);
        cal.ttr = d.ttr;
    }
    if (ai.indent.count > 0 && hu.indent.count > 0)
        cal.indent = (GaussParam){dv_mean(&ai.indent), dv_stdev(&ai.indent), dv_mean(&hu.indent),
                                  dv_stdev(&hu.indent)};
    else {
        Calibration d;
        calibration_default(&d);
        cal.indent = d.indent;
    }
    Calibration defaults;
    calibration_default(&defaults);
    cal.dup = defaults.dup;
    cal.git = defaults.git;

    /* Grid search temperature for minimum ECE, reusing score_compute */
    int total_n = ai.reg.count + hu.reg.count;
    double *probs = malloc((size_t)total_n * sizeof(double));
    int *labels = malloc((size_t)total_n * sizeof(int));

    double best_t = DEFAULT_TEMPERATURE, best_ece = 1e9;
    for (double t = TEMP_GRID_MIN; t <= TEMP_GRID_MAX; t += TEMP_GRID_STEP) {
        cal.temperature = t;
        int idx = 0;

        for (int j = 0; j < ai.reg.count; j++) {
            double id = (j < ai.ident.count) ? ai.ident.vals[j] : -1;
            double fc = (j < ai.func_cv.count) ? ai.func_cv.vals[j] : -1;
            double tt = (j < ai.ttr.count) ? ai.ttr.vals[j] : -1;
            double ind = (j < ai.indent.count) ? ai.indent.vals[j] : -1;
            probs[idx] = prob_from_measurements(
                &cal, ai.reg.vals[j], ai.ccr.vals[j], ai.narr.vals[j] > 0.5, ai.cond.vals[j],
                ai.decay.vals[j] > 0.5, ai.overwrap.vals[j] > 0.5, ai.namebrk.vals[j] > 0.5,
                id, fc, tt, ind);
            labels[idx] = 1;
            idx++;
        }
        for (int j = 0; j < hu.reg.count; j++) {
            double id = (j < hu.ident.count) ? hu.ident.vals[j] : -1;
            double fc = (j < hu.func_cv.count) ? hu.func_cv.vals[j] : -1;
            double tt = (j < hu.ttr.count) ? hu.ttr.vals[j] : -1;
            double ind = (j < hu.indent.count) ? hu.indent.vals[j] : -1;
            probs[idx] = prob_from_measurements(
                &cal, hu.reg.vals[j], hu.ccr.vals[j], hu.narr.vals[j] > 0.5, hu.cond.vals[j],
                hu.decay.vals[j] > 0.5, hu.overwrap.vals[j] > 0.5, hu.namebrk.vals[j] > 0.5,
                id, fc, tt, ind);
            labels[idx] = 0;
            idx++;
        }

        double ece = compute_ece(probs, labels, total_n, ECE_BIN_COUNT);
        if (ece < best_ece) {
            best_ece = ece;
            best_t = t;
        }
    }

    free(probs);
    free(labels);

    cal.temperature = best_t;
    cal.loaded = true;

    printf("\n  calibration results:\n");
    printf("  temperature = %.2f (ECE = %.4f)\n", best_t, best_ece);
    printf("  regularity: AI(%.3f \xc2\xb1 %.3f) Human(%.3f \xc2\xb1 %.3f)\n", cal.regularity.mu_ai,
           cal.regularity.sig_ai, cal.regularity.mu_h, cal.regularity.sig_h);
    printf("  ccr:        AI(%.3f \xc2\xb1 %.3f) Human(%.3f \xc2\xb1 %.3f)\n", cal.ccr.mu_ai,
           cal.ccr.sig_ai, cal.ccr.mu_h, cal.ccr.sig_h);
    printf("  cond:       AI(%.3f \xc2\xb1 %.3f) Human(%.3f \xc2\xb1 %.3f)\n", cal.cond.mu_ai,
           cal.cond.sig_ai, cal.cond.mu_h, cal.cond.sig_h);
    printf("  ident_spec: AI(%.3f \xc2\xb1 %.3f) Human(%.3f \xc2\xb1 %.3f)\n", cal.ident.mu_ai,
           cal.ident.sig_ai, cal.ident.mu_h, cal.ident.sig_h);
    printf("  func_cv:    AI(%.3f \xc2\xb1 %.3f) Human(%.3f \xc2\xb1 %.3f)\n", cal.func_cv.mu_ai,
           cal.func_cv.sig_ai, cal.func_cv.mu_h, cal.func_cv.sig_h);
    printf("  ttr:        AI(%.3f \xc2\xb1 %.3f) Human(%.3f \xc2\xb1 %.3f)\n", cal.ttr.mu_ai,
           cal.ttr.sig_ai, cal.ttr.mu_h, cal.ttr.sig_h);
    printf("  indent:     AI(%.3f \xc2\xb1 %.3f) Human(%.3f \xc2\xb1 %.3f)\n", cal.indent.mu_ai,
           cal.indent.sig_ai, cal.indent.mu_h, cal.indent.sig_h);
    printf("  narration:  p_ai=%.3f p_h=%.3f\n", cal.narr_p_ai, cal.narr_p_h);
    printf("  decay:      p_ai=%.3f p_h=%.3f\n", cal.decay_p_ai, cal.decay_p_h);
    printf("  overwrap:   p_ai=%.3f p_h=%.3f\n", cal.overwrap_p_ai, cal.overwrap_p_h);
    printf("  namebreak:  p_ai=%.3f p_h=%.3f\n", cal.namebrk_p_ai, cal.namebrk_p_h);

    if (calibration_save(&cal, out_dir))
        printf("\n  saved to %s/.slop-calibration.json\n", out_dir);
    else fprintf(stderr, "  error: could not save calibration file\n");

    mv_free(&ai);
    mv_free(&hu);

    printf("\n");
    return 0;
}

/* ── Report command ─────────────────────────────────────── */

static void report_divider(void) { print_rule(RULE_W_REPORT); }

static void report_header(const char *title) {
    printf("\n");
    report_divider();
    printf("  %s\n", title);
    report_divider();
}

static void show_gp(const char *name, const GaussParam *g) {
    printf("    %-14s %.3f +/- %-16.3f %.3f +/- %.3f\n", name, g->mu_ai, g->sig_ai, g->mu_h,
           g->sig_h);
}

static void report_methodology(const Calibration *cal) {
    report_header("METHODOLOGY");
    printf("\n"
           "  The AI slop score uses Bayesian hypothesis testing with\n"
           "  log-likelihood ratios (LLR). Each signal contributes an LLR:\n\n"
           "    Gaussian signals:  LLR = 0.5*(z_h^2 - z_a^2) + ln(sig_h/sig_a)\n"
           "      where z_h = (x - mu_h)/sig_h, z_a = (x - mu_ai)/sig_ai\n\n"
           "    Binary signals:    LLR = ln(p_ai / p_h)    if present\n"
           "                       LLR = ln((1-p_ai)/(1-p_h)) otherwise\n\n"
           "  All LLRs are clamped to [%.1f, +%.1f] to limit any single signal%s.\n"
           "  The total raw score S = sum of all LLRs.\n\n"
           "  Temperature scaling (Guo et al. 2017) calibrates the output:\n\n"
           "    P(AI) = sigmoid(S / T) = 1 / (1 + exp(-S/T))\n\n"
           "  Temperature T = %.2f%s.\n\n"
           "  Thresholds: P >= %.2f flagged, P >= %.2f suspicious, P < %.2f likely human\n",
           cal->loaded ? LLR_CLAMP : LLR_CLAMP_UNCAL, cal->loaded ? LLR_CLAMP : LLR_CLAMP_UNCAL,
           cal->loaded ? "" : " (conservative uncalibrated limit)", cal->temperature,
           cal->loaded ? " (calibrated)" : " (default)", PROB_FLAGGED, PROB_SUSPICIOUS,
           PROB_SUSPICIOUS);

    printf("\n  Signal parameters:\n");
    printf("    %-14s %-22s %-22s\n", "signal", "AI (mu +/- sig)", "Human (mu +/- sig)");
    printf("  ");
    print_rule(RULE_W_PARAM);
    show_gp("regularity", &cal->regularity);
    show_gp("ccr", &cal->ccr);
    show_gp("cond_density", &cal->cond);
    show_gp("ident_spec", &cal->ident);
    show_gp("func_cv", &cal->func_cv);
    show_gp("ttr", &cal->ttr);
    show_gp("indent_reg", &cal->indent);
    show_gp("dup_ratio", &cal->dup);
    show_gp("git_style", &cal->git);

    static const struct {
        const char *name;
        double p_ai;
        double p_h;
    } bp[] = {
        {"narration", 0, 0},
        {"decay", 0, 0},
        {"over-wrapping", 0, 0},
        {"naming breaks", 0, 0},
    };
    const double bp_ai[] = {cal->narr_p_ai, cal->decay_p_ai, cal->overwrap_p_ai, cal->namebrk_p_ai};
    const double bp_h[] = {cal->narr_p_h, cal->decay_p_h, cal->overwrap_p_h, cal->namebrk_p_h};
    for (int k = 0; k < 4; k++)
        printf("    %-14s p_ai=%.3f              p_h=%.3f\n", bp[k].name, bp_ai[k], bp_h[k]);
}

static double report_single_file(const char *path, const Calibration *cal, bool use_git,
                                 double prior_llr) {
    size_t len = 0;
    char *content = util_read_file(path, &len);
    if (!content) {
        err_cannot_read(path);
        return -1;
    }
    if (util_should_skip(content, len)) {
        printf("  %s — skipped (binary/minified/generated)\n", path);
        free(content);
        return -1;
    }

    FileScanCtx ctx;
    scan_one_file(&ctx, path, content, len, cal, -1, use_git, prior_llr);

    /* ── Verdict ──────────────────────────────────────────── */
    report_header("SCORE");
    printf("\n  file:    %s\n", path);
    printf("  lines:   %d total, %d code, %d comment, %d blank\n", ctx.scan.total_lines,
           ctx.scan.code_lines, ctx.scan.comment_lines, ctx.scan.blank_lines);
    printf("  funcs:   %d detected\n", ctx.scan.function_count);
    printf("  lang:    %s\n\n", lang_family_name(ctx.scan.lang));

    printf("  P(AI) = %.3f   [%s]\n", ctx.sc.probability, score_label(ctx.sc.probability));
    printf("  raw score = %+.2f   temperature = %.2f   scaled = %+.2f\n", ctx.sc.raw_score,
           ctx.sc.temperature, ctx.sc.raw_score / ctx.sc.temperature);

    /* ── Signal breakdown ─────────────────────────────────── */
    report_header("SIGNAL BREAKDOWN");
    printf("\n  Each signal contributes a log-likelihood ratio (LLR).\n"
           "  Positive LLR = evidence for AI.  Negative = evidence for human.\n\n");
    printf("  %-14s %-20s %-14s %-8s %s\n", "group", "signal", "measured", "LLR", "contribution");
    print_rule(RULE_W_WIDE);

    double total = 0;
    for (int i = 0; i < ctx.sc.signal_count; i++) {
        const SignalDetail *d = &ctx.sc.details[i];
        total += d->llr;
        const char *arrow = d->llr > 0.5 ? "<< AI" : d->llr < -0.5 ? ">> human" : "";
        printf("  %-14s %-20s %-14s %+6.2f   %s\n", d->group, d->signal, d->measured, d->llr,
               arrow);
    }
    print_rule(RULE_W_WIDE);
    printf("  %36s = %+.2f\n", "sum(LLR)", total);
    printf("  %36s = %+.2f / %.2f = %+.2f\n", "scaled", total, ctx.sc.temperature,
           total / ctx.sc.temperature);
    printf("  %36s = sigmoid(%+.2f) = %.3f\n", "P(AI)", total / ctx.sc.temperature,
           ctx.sc.probability);

    /* ── Smell findings ───────────────────────────────────── */
    SmellReport smell;
    smell_detect(&smell, &ctx.scan, true);

    report_header("SMELL FINDINGS");
    if (smell.count == 0) {
        printf("\n  no smells detected\n");
    } else {
        int diag = 0, corr = 0, gen = 0;
        for (int i = 0; i < smell.count; i++) {
            switch (smell.items[i].severity) {
            case SEV_DIAGNOSTIC:
                diag++;
                break;
            case SEV_CORRELATED:
                corr++;
                break;
            case SEV_GENERAL:
                gen++;
                break;
            }
        }
        printf("\n  %d finding%s (%d diagnostic, %d correlated, %d general)\n", smell.count,
               smell.count == 1 ? "" : "s", diag, corr, gen);
        SmellSeverity last_sev = (SmellSeverity)-1;
        for (int i = 0; i < smell.count; i++) {
            const SmellFinding *f = &smell.items[i];
            if (f->severity != last_sev) {
                printf("\n  [%s]\n", severity_str(f->severity));
                last_sev = f->severity;
            }
            if (f->line > 0)
                printf("    line %-5d %-16s %s\n", f->line, smell_kind_str(f->kind), f->message);
            else printf("    %-10s %-16s %s\n", "", smell_kind_str(f->kind), f->message);
        }
    }

    /* ── Dead code ────────────────────────────────────────── */
    if (ctx.dead_lines > 0) {
        report_header("DEAD CODE");
        printf("\n  %d dead/unused lines detected\n", ctx.dead_lines);
        for (int i = 0; i < smell.count; i++) {
            if (smell.items[i].kind == SMELL_DEAD_CODE && smell.items[i].line > 0)
                printf("    line %-5d %s\n", smell.items[i].line, smell.items[i].message);
        }
    }

    double prob = ctx.sc.probability;
    scan_one_free(&ctx);
    free(content);
    return prob;
}

static int cmd_report(int argc, char **argv) {
    const char *target = nullptr;
    bool use_git = false;
    double prior = DEFAULT_PRIOR;
    bool json_out = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--git") == 0) use_git = true;
        else if (strcmp(argv[i], "--json") == 0) json_out = true;
        else if (strncmp(argv[i], OPT_PRIOR, sizeof(OPT_PRIOR) - 1) == 0)
            prior = atof(argv[i] + sizeof(OPT_PRIOR) - 1);
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) return -1;
        else if (argv[i][0] != '-') target = argv[i];
        else {
            err_bad_option(argv[i]);
            return 1;
        }
    }
    if (!target) {
        err_no_target();
        return -1;
    }

    Calibration cal;
    calibration_default(&cal);
    (void)calibration_load(&cal, ".");

    const double prior_llr = slop_prior_llr(prior);

    struct stat st;
    if (stat(target, &st) != 0) { err_cannot_stat(target); return 1; }

    if (!cal.loaded)
        printf("\n  \xe2\x9a\xa0 UNCALIBRATED (T=%.1f) \xe2\x80\x94 run 'slop calibrate' for "
               "precision\n",
               cal.temperature);

    printf("\n  ============================================================\n");
    printf("  SLOP REPORT \xe2\x80\x94 AI Slop Detector\n");
    printf("  ============================================================\n");

    if (S_ISDIR(st.st_mode)) {
        FileList fl;
        fl_init(&fl);
        fl_collect(&fl, target);

        /* ── Dupes pass ── */
        DupeResult dr;
        dupes_init(&dr);
        collect_dupes(&dr, &fl);
        const DupRatioResult drr = dupes_compute_ratio(&dr);
        double dup_ratio = drr.ratio;

        /* ── Project overview ── */
        report_header("PROJECT OVERVIEW");
        printf("\n  target:    %s\n", target);
        printf("  files:     %d source files\n", fl.count);
        printf("  functions: %d (>= %d bytes body)\n", drr.total_funcs, NCD_MIN_BODY_BYTES);
        printf("  dup_ratio: %.2f (%d/%d functions in duplicate clusters)\n", drr.ratio,
               drr.funcs_in_dup, drr.total_funcs);

        /* ── Per-file reports ── */
        report_header("PER-FILE ANALYSIS");

        typedef struct {
            char path[4096];
            double prob;
            double score;
            int dead;
        } RE;
        RE *rows = malloc((size_t)fl.count * sizeof(RE));
        int row_count = 0;
        int total_dead = 0;
        int total_smells = 0;
        int flagged = 0, suspicious = 0, human = 0;

        for (int i = 0; i < fl.count; i++) {
            size_t len = 0;
            char *content = util_read_file(fl.paths[i], &len);
            if (!content) continue;
            if (util_should_skip(content, len)) { free(content); continue; }

            FileScanCtx ctx;
            scan_one_file(&ctx, fl.paths[i], content, len, &cal, dup_ratio, use_git, prior_llr);

            SmellReport smell;
            smell_detect(&smell, &ctx.scan, true);

            if (row_count < fl.count) {
                RE *r = &rows[row_count++];
                snprintf(r->path, sizeof(r->path), "%s", fl.paths[i]);
                r->prob = ctx.sc.probability;
                r->score = ctx.sc.raw_score;
                r->dead = ctx.dead_lines;
            }
            total_dead += ctx.dead_lines;
            total_smells += smell.count;

            if (ctx.sc.probability >= PROB_FLAGGED) flagged++;
            else if (ctx.sc.probability >= PROB_SUSPICIOUS) suspicious++;
            else human++;

            if (!json_out) {
                printf("\n  %s\n", fl.paths[i]);
                printf("    P(AI) = %.3f  [%s]  score = %+.2f", ctx.sc.probability,
                       score_label(ctx.sc.probability), ctx.sc.raw_score);
                if (ctx.dead_lines > 0) printf("  dead = %d", ctx.dead_lines);
                if (smell.count > 0) printf("  smells = %d", smell.count);
                putchar('\n');

                for (int s = 0; s < ctx.sc.signal_count; s++) {
                    const SignalDetail *d = &ctx.sc.details[s];
                    printf("      %-14s %-20s %-12s %+.2f\n", d->group, d->signal, d->measured,
                           d->llr);
                }

                if (smell.count > 0) {
                    for (int s = 0; s < smell.count; s++) {
                        const SmellFinding *f = &smell.items[s];
                        if (f->line > 0)
                            printf("      [%s] line %d: %s — %s\n", severity_str(f->severity),
                                   f->line, smell_kind_str(f->kind), f->message);
                    }
                }
            }

            scan_one_free(&ctx);
            free(content);
        }

        /* ── Summary table ── */
        report_header("SUMMARY");
        printf("\n  %-7s %-7s %-5s %s\n", "P(AI)", "score", "dead", "file");
        print_rule(RULE_W);
        for (int i = 0; i < row_count; i++) {
            const RE *r = &rows[i];
            if (r->dead > 0)
                printf("  %-7.3f %+6.2f  %-5d %s\n", r->prob, r->score, r->dead, r->path);
            else printf("  %-7.3f %+6.2f  %-5s %s\n", r->prob, r->score, "-", r->path);
        }
        print_rule(RULE_W);
        printf("\n  %d flagged / %d suspicious / %d likely human\n", flagged, suspicious, human);
        printf("  %d total smells / %d dead lines across project\n", total_smells, total_dead);
        if (drr.ratio > 0)
            printf("  dup_ratio = %.2f (%d/%d functions)\n", drr.ratio, drr.funcs_in_dup,
                   drr.total_funcs);

        /* ── Duplicates detail ── */
        if (dr.cluster_count > 0) {
            report_header("DUPLICATE FUNCTIONS");
            print_dupes(&dr, NCD_THRESHOLD);
        }

        free(rows);
        dupes_free(&dr);
        fl_free(&fl);

        /* ── Methodology ── */
        report_methodology(&cal);
        printf("\n");

        if (flagged > 0) return 2;
        if (suspicious > 0) return 1;
        return 0;

    } else {
        /* single file */
        double prob = report_single_file(target, &cal, use_git, prior_llr);
        report_methodology(&cal);
        printf("\n");

        if (prob < 0) return 0;
        return prob >= PROB_FLAGGED ? 2 : prob >= PROB_SUSPICIOUS ? 1 : 0;
    }
}

/* ── Usage ──────────────────────────────────────────────── */

static void usage(void) {
    fprintf(stderr,
            "slop \xe2\x80\x94 AI slop detector\n\n"
            "Usage:\n"
            "  slop smell  [--all] <file|dir>   Find AI-diagnostic code smells\n"
            "  slop scan   [options] <file|dir>  Estimate AI likelihood (score)\n"
            "  slop report [options] <file|dir>  Full report with score methodology\n"
            "  slop dupes  [options] <dir>       Find duplicate functions (NCD)\n"
            "  slop calibrate --ai <dir> --human <dir>\n\n"
            "Smell options:\n"
            "  --all    Include GENERAL smells (zombie params, unused imports, dead code)\n\n"
            "Scan options:\n"
            "  --verbose       Show per-file signal breakdown\n"
            "  --json          JSON output\n"
            "  --git           Include git commit patterns (Group F)\n"
            "  --prior=N       Prior P(AI), default 0.5\n"
            "  --stdin --lang=LANG  Read from stdin\n\n"
            "Report options:\n"
            "  --git           Include git commit patterns\n"
            "  --prior=N       Prior P(AI), default 0.5\n\n"
            "Dupes options:\n"
            "  --threshold=N   NCD threshold (default 0.30)\n"
            "  --cross-lang    Compare across language families\n\n"
            "Calibrate:\n"
            "  --ai <dir>      Directory of known AI-generated files\n"
            "  --human <dir>   Directory of known human-written files\n"
            "  --out=<dir>     Output directory (default: .)\n\n"
            "Exit codes:\n"
            "  0  clean\n"
            "  1  warnings (correlated smells or P > 0.60)\n"
            "  2  flagged (diagnostic smells, dupes found, or P > 0.85)\n\n"
            "File filtering:\n"
            "  Respects .gitignore (when in a git repo) and .slopignore\n");
}

/* ── Main ───────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];
    int rc;

    bool has_stdin = false;
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--stdin") == 0) has_stdin = true;

    if (strcmp(cmd, "smell") == 0) {
        rc = cmd_smell(argc, argv);
    } else if (strcmp(cmd, "scan") == 0) {
        rc = has_stdin ? cmd_scan_stdin(argc, argv) : cmd_scan(argc, argv);
    } else if (strcmp(cmd, "report") == 0) {
        rc = cmd_report(argc, argv);
    } else if (strcmp(cmd, "dupes") == 0) {
        rc = cmd_dupes(argc, argv);
    } else if (strcmp(cmd, "calibrate") == 0) {
        rc = cmd_calibrate(argc, argv);
    } else if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        usage();
        return 0;
    } else {
        fprintf(stderr, "slop: unknown command '%s'\n", cmd);
        fprintf(stderr, "Commands: smell, scan, report, dupes, calibrate\n");
        return 1;
    }

    if (rc == -1) {
        usage();
        return 0;
    }
    return rc;
}
