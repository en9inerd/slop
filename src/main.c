#include "slop.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Common error messages ─────────────────────────────── */

static void err_no_target(void) {
  fprintf(stderr, "slop: no file or directory specified\n");
}
static void err_cannot_stat(const char *p) {
  fprintf(stderr, "slop: cannot stat %s\n", p);
}
static void err_cannot_read(const char *p) {
  fprintf(stderr, "slop: cannot read %s\n", p);
}
static void err_bad_option(const char *o) {
  fprintf(stderr, "slop: unknown option %s\n", o);
}

static void print_rule(int width) {
  printf("  ");
  for (int i = 0; i < width; i++)
    putchar('-');
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
    return "strong tell";
  case SEV_CORRELATED:
    return "score-related";
  case SEV_GENERAL:
    return "general";
  }
  return "?";
}

static void print_findings(const char *filename, const SmellReport *r) {
  if (r->count == 0)
    return;
  printf("\n  %s \xe2\x80\x94 %d finding%s\n", filename, r->count,
         r->count == 1 ? "" : "s");
  SmellSeverity last_sev = (SmellSeverity)-1;
  for (int i = 0; i < r->count; i++) {
    const SmellFinding *f = &r->items[i];
    if (f->severity != last_sev) {
      printf("\n  %s\n", severity_str(f->severity));
      last_sev = f->severity;
    }
    if (f->line > 0)
      printf("  :%d\t%-16s %s\n", f->line, smell_kind_str(f->kind), f->message);
    else
      printf("  \xe2\x80\x94\t%-16s %s\n", smell_kind_str(f->kind), f->message);
  }
}

static const char *score_label(double p) {
  if (p >= PROB_FLAGGED)
    return "sloppy";
  if (p >= PROB_SUSPICIOUS)
    return "moderate";
  if (p >= PROB_INCONCLUSIVE)
    return "mild";
  return "clean";
}

static double slop_score_10(double prob) { return prob * 10.0; }

static const char *signal_why(const char *group, const char *signal) {
  static const struct {
    const char *group;
    const char *signal;
    const char *why;
  } table[] = {
      {"score", "insufficient code",
       "No executable lines; score stays at the neutral prior."},
      {"regularity", "composite",
       "How uniform line lengths look (entropy, spread, zlib ratio when known)."},
      {"regularity", "composite*",
       "How uniform line lengths look (entropy, spread, zlib ratio when known)."},
      {"comments", "comment-to-code",
       "Share of lines that are comments vs code."},
      {"comments", "narration",
       "Whether comments narrate the obvious ('First, we validate input')."},
      {"structure", "conditional dens.",
       "Density of if/while/for-style keywords per line of code."},
      {"position", "quality decay",
       "Whether comments or defects pile up unevenly across the file."},
      {"patterns", "over-wrapping",
       "Extra defensive checks (null, length) beyond what callers need."},
      {"patterns", "naming breaks",
       "Mixed camelCase vs snake_case inside one function."},
      {"naming", "ident. specificity",
       "Fraction of identifiers that are generic (data, result, handler, ...)."},
      {"structure", "func length CV",
       "How similar function body sizes are (variance of line counts)."},
      {"naming", "token diversity",
       "Unique vs repeated identifiers (reuse of the same names)."},
      {"whitespace", "indent regularity",
       "How regular indentation depth is across lines."},
      {"dupes", "duplicate ratio",
       "Project-wide share of functions that sit in duplicate clusters."},
      {"git", "commit style",
       "Recent git messages vs typical human commit patterns."},
  };
  for (int i = 0; i < (int)(sizeof(table) / sizeof(table[0])); i++) {
    if (strcmp(group, table[i].group) == 0 &&
        strcmp(signal, table[i].signal) == 0)
      return table[i].why;
  }
  return "Positive weight = more concern; negative = cleaner (see METHODOLOGY).";
}

static void print_signal_table(const ScoreResult *sc, bool with_why) {
  printf("\n  %-40s %-16s %8s\n", "Check (category / name)", "Value", "weight");
  print_rule(RULE_W_WIDE);
  for (int i = 0; i < sc->signal_count; i++) {
    const SignalDetail *d = &sc->details[i];
    char label[72];
    snprintf(label, sizeof(label), "%s / %s", d->group, d->signal);
    printf("  %-40s %-16s %+7.2f\n", label, d->measured, d->llr);
    if (with_why)
      printf("    %s\n", signal_why(d->group, d->signal));
  }
  print_rule(RULE_W_WIDE);
}

static void print_score(const char *filename, const ScoreResult *sc) {
  printf("\n  %s \xe2\x80\x94 slop %.1f/10  [%s]\n", filename,
         slop_score_10(sc->probability), score_label(sc->probability));
  printf("\n  \"weight\" is log-odds evidence per check; they sum to the raw score.\n");
  print_signal_table(sc, true);
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

static void print_score_json(const char *filename, const ScoreResult *sc,
                             int dead_lines) {
  char esc[4096];
  json_escape(filename, esc, sizeof(esc));
  printf("    {\"file\": \"%s\", \"raw_score\": %.4f, \"slop_score\": %.1f, "
         "\"dead_lines\": %d, \"label\": \"%s\", \"signals\": [",
         esc, sc->raw_score, slop_score_10(sc->probability), dead_lines,
         score_label(sc->probability));
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
    if (!content)
      continue;
    if (util_should_skip(content, len)) {
      free(content);
      continue;
    }
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

static bool scan_one_file(FileScanCtx *ctx, const char *path,
                          const char *content, size_t len,
                          const Calibration *cal, double dup_ratio,
                          bool use_git, double prior_llr) {
  LangFamily lang = lang_detect(path);
  scan_file(&ctx->scan, path, content, len, lang);
  ctx->compression_ratio = compress_ratio(content, len);

  ctx->gf = (GitFeatures){};
  if (use_git)
    git_features(&ctx->gf, path);

  score_compute(&ctx->sc, &ctx->scan, cal, ctx->compression_ratio, dup_ratio,
                use_git ? &ctx->gf : nullptr);
  ctx->sc.raw_score += prior_llr;
  ctx->sc.probability = slop_sigmoid(ctx->sc.raw_score / cal->temperature);
  ctx->dead_lines = smell_count_dead_lines(&ctx->scan);
  return true;
}

static void scan_one_free(FileScanCtx *ctx) { scan_free(&ctx->scan); }

/* ── Check command (alias: smell) ─────────────────────────── */

typedef struct {
  int files_scanned;
  int files_with_findings;
  int total_findings;
  bool has_diagnostic;
  bool has_correlated;
} SmellStats;

static void process_smell(const char *path, bool include_all,
                          SmellStats *stats) {
  size_t len = 0;
  char *content = util_read_file(path, &len);
  if (!content) {
    err_cannot_read(path);
    return;
  }
  if (util_should_skip(content, len)) {
    free(content);
    return;
  }

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
      if (report.items[i].severity == SEV_DIAGNOSTIC)
        stats->has_diagnostic = true;
      if (report.items[i].severity == SEV_CORRELATED)
        stats->has_correlated = true;
    }
    print_findings(path, &report);
  }

  scan_free(&scan);
  free(content);
}

static int cmd_check(int argc, char **argv) {
  bool include_all = false;
  const char *target = nullptr;

  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--all") == 0)
      include_all = true;
    else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      return -1;
    } else if (argv[i][0] != '-')
      target = argv[i];
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
  if (stat(target, &st) != 0) {
    err_cannot_stat(target);
    return 1;
  }

  if (S_ISDIR(st.st_mode)) {
    FileList fl;
    fl_init(&fl);
    fl_collect(&fl, target);
    for (int i = 0; i < fl.count; i++)
      process_smell(fl.paths[i], include_all, &stats);
    fl_free(&fl);
  } else {
    process_smell(target, include_all, &stats);
  }

  if (stats.files_scanned > 1)
    printf("\n  %d file%s scanned, %d with findings (%d total)\n",
           stats.files_scanned, stats.files_scanned == 1 ? "" : "s",
           stats.files_with_findings, stats.total_findings);
  if (stats.total_findings == 0 && stats.files_scanned > 0)
    printf("\n  no issues found\n");
  printf("\n");

  if (stats.has_diagnostic)
    return 2;
  if (stats.has_correlated)
    return 1;
  return 0;
}

/* ── Shared helpers for scan / report commands ─────────── */

static void scoring_init(Calibration *cal, double prior,
                         double *out_prior_llr) {
  calibration_default(cal);
  (void)calibration_load(cal, ".");
  *out_prior_llr = slop_prior_llr(prior);
}

typedef struct {
  char path[4096];
  double prob;
  double raw_score;
  int dead_lines;
  ScoreResult sc;
} FileEntry;

typedef struct {
  int files_scanned;
  int files_skipped;
  int sloppy;
  int moderate;
  int clean;
} ScanStats;

#define MAX_SCAN_ENTRIES 8192
static const char OPT_PRIOR[] = "--prior=";

static int entry_cmp(const void *a, const void *b) {
  const double pa = ((const FileEntry *)a)->prob;
  const double pb = ((const FileEntry *)b)->prob;
  return (pa < pb) ? 1 : (pa > pb) ? -1 : 0;
}

static void classify(ScanStats *ss, double prob) {
  ss->files_scanned++;
  if (prob >= PROB_FLAGGED)
    ss->sloppy++;
  else if (prob >= PROB_SUSPICIOUS)
    ss->moderate++;
  else
    ss->clean++;
}

static void print_file_table(const FileEntry *entries, int count) {
  printf("\n  %-7s %-7s %-5s %s\n", "slop", "raw", "dead", "file");
  print_rule(RULE_W);
  for (int i = 0; i < count; i++) {
    const FileEntry *e = &entries[i];
    if (e->dead_lines > 0)
      printf("  %-7.1f %+6.2f  %-5d %s\n", slop_score_10(e->prob),
             e->raw_score, e->dead_lines, e->path);
    else
      printf("  %-7.1f %+6.2f  %-5s %s\n", slop_score_10(e->prob),
             e->raw_score, "-", e->path);
  }
  print_rule(RULE_W);
}

/* ── Scan command ───────────────────────────────────────── */

static int cmd_scan(int argc, char **argv) {
  const char *target = nullptr;
  bool verbose = false, use_git = false, json_out = false;
  double prior = DEFAULT_PRIOR;

  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
      verbose = true;
    else if (strcmp(argv[i], "--git") == 0)
      use_git = true;
    else if (strcmp(argv[i], "--json") == 0)
      json_out = true;
    else if (strncmp(argv[i], OPT_PRIOR, sizeof(OPT_PRIOR) - 1) == 0)
      prior = atof(argv[i] + 8);
    else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      return -1;
    else if (argv[i][0] != '-')
      target = argv[i];
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
  double prior_llr;
  scoring_init(&cal, prior, &prior_llr);

  struct stat st;
  if (stat(target, &st) != 0) {
    err_cannot_stat(target);
    return 1;
  }

  ScanStats ss = {};

  if (S_ISDIR(st.st_mode)) {
    FileList fl;
    fl_init(&fl);
    fl_collect(&fl, target);

    /* Pass 1: cross-file duplicate detection (Group E) */
    DupeResult dr;
    dupes_init(&dr);
    collect_dupes(&dr, &fl);
    const DupRatioResult drr = dupes_compute_ratio(&dr);
    double dup_ratio = drr.ratio;

    /* Pass 2: score each file with real dup_ratio */
    FileEntry *entries = malloc(MAX_SCAN_ENTRIES * sizeof(FileEntry));
    int entry_count = 0;

    for (int i = 0; i < fl.count; i++) {
      size_t len = 0;
      char *content = util_read_file(fl.paths[i], &len);
      if (!content)
        continue;
      if (util_should_skip(content, len)) {
        ss.files_skipped++;
        free(content);
        continue;
      }

      FileScanCtx ctx;
      scan_one_file(&ctx, fl.paths[i], content, len, &cal, dup_ratio, use_git,
                    prior_llr);
      classify(&ss, ctx.sc.probability);

      if (verbose) {
        print_score(fl.paths[i], &ctx.sc);
        if (ctx.dead_lines > 0)
          printf("  dead_lines        = %d\n", ctx.dead_lines);
      } else if (entry_count < MAX_SCAN_ENTRIES) {
        FileEntry *e = &entries[entry_count++];
        snprintf(e->path, sizeof(e->path), "%s", fl.paths[i]);
        e->prob = ctx.sc.probability;
        e->raw_score = ctx.sc.raw_score;
        e->dead_lines = ctx.dead_lines;
        e->sc = ctx.sc;
      }

      scan_one_free(&ctx);
      free(content);
    }

    /* ── Output ───────────────────────────────────────── */
    if (!verbose && !json_out && entry_count > 0) {
      qsort(entries, (size_t)entry_count, sizeof(FileEntry), entry_cmp);
      printf("\n  scanned %d file%s", ss.files_scanned,
             ss.files_scanned == 1 ? "" : "s");
      if (ss.files_skipped > 0)
        printf(" (%d skipped: minified/generated)", ss.files_skipped);
      if (drr.total_funcs > 0)
        printf("\n  dup_ratio = %.2f (%d/%d functions)", drr.ratio,
               drr.funcs_in_dup, drr.total_funcs);
      int total_dead = 0;
      for (int i = 0; i < entry_count; i++)
        total_dead += entries[i].dead_lines;

      print_file_table(entries, entry_count);
      printf("\n  %d sloppy / %d moderate / %d clean\n", ss.sloppy, ss.moderate,
             ss.clean);
      if (total_dead > 0)
        printf("  %d dead lines detected across project\n", total_dead);
    }

    if (json_out) {
      printf("{\n  \"calibrated\": %s,\n  \"temperature\": %.2f,\n",
             cal.loaded ? "true" : "false", cal.temperature);
      printf("  \"dup_ratio\": %.4f,\n  \"files\": [\n", dup_ratio);
      for (int i = 0; i < entry_count; i++) {
        print_score_json(entries[i].path, &entries[i].sc,
                         entries[i].dead_lines);
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
      if (ctx.dead_lines > 0)
        printf("  dead_lines        = %d\n", ctx.dead_lines);
    }

    scan_one_free(&ctx);
    free(content);
  }

  printf("\n");
  if (ss.sloppy > 0)
    return 2;
  if (ss.moderate > 0)
    return 1;
  return 0;
}

/* ── Scan stdin ─────────────────────────────────────────── */

static int cmd_scan_stdin(int argc, char **argv) {
  const char *lang_name = nullptr;
  bool json_out = false;
  double prior = DEFAULT_PRIOR;

  for (int i = 2; i < argc; i++) {
    if (strncmp(argv[i], "--lang=", 7) == 0)
      lang_name = argv[i] + 7;
    else if (strcmp(argv[i], "--json") == 0)
      json_out = true;
    else if (strncmp(argv[i], OPT_PRIOR, sizeof(OPT_PRIOR) - 1) == 0)
      prior = atof(argv[i] + sizeof(OPT_PRIOR) - 1);
    else if (strcmp(argv[i], "--stdin") == 0) { /* already handled */
    }
  }

  size_t cap = 65536, len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    fprintf(stderr, "slop: out of memory\n");
    return 1;
  }

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
  if (lang_name)
    snprintf(fake_name, sizeof(fake_name), "<stdin>.%s", lang_name);

  Calibration cal;
  double prior_llr;
  scoring_init(&cal, prior, &prior_llr);

  FileScanCtx ctx;
  scan_one_file(&ctx, fake_name, buf, len, &cal, -1, false, prior_llr);

  if (json_out) {
    printf("[\n");
    print_score_json(fake_name, &ctx.sc, 0);
    printf("\n]\n");
  } else {
    print_score(fake_name, &ctx.sc);
  }
  printf("\n");

  double prob = ctx.sc.probability;
  scan_one_free(&ctx);
  free(buf);
  return prob >= PROB_FLAGGED ? 2 : prob >= PROB_SUSPICIOUS ? 1 : 0;
}

/* ── Dupes command ──────────────────────────────────────── */

typedef struct {
  int cluster;
  int size;
} ClusterOrder;

static int cluster_order_cmp(const void *a, const void *b) {
  return ((const ClusterOrder *)b)->size - ((const ClusterOrder *)a)->size;
}

static void print_dupes(const DupeResult *dr, double threshold) {
  if (dr->cluster_count == 0) {
    printf("\n  no duplicate functions found (NCD < %.2f, min body %d bytes)\n",
           threshold, NCD_MIN_BODY_BYTES);
    return;
  }
  printf("\n  %d cluster%s (NCD < %.2f, min body %d bytes)\n",
         dr->cluster_count, dr->cluster_count == 1 ? "" : "s", threshold,
         NCD_MIN_BODY_BYTES);

  int max_cluster = 0;
  for (int i = 0; i < dr->func_count; i++)
    if (dr->funcs[i].cluster > max_cluster)
      max_cluster = dr->funcs[i].cluster;

  int *sizes = calloc((size_t)max_cluster + 2, sizeof(int));
  for (int i = 0; i < dr->func_count; i++)
    if (dr->funcs[i].cluster >= 0)
      sizes[dr->funcs[i].cluster]++;

  double *min_ncd = malloc(((size_t)max_cluster + 2) * sizeof(double));
  for (int c = 0; c <= max_cluster; c++)
    min_ncd[c] = 1.0;
  for (int p = 0; p < dr->pair_count; p++) {
    int cl = dr->funcs[dr->pairs[p].a].cluster;
    if (dr->pairs[p].ncd < min_ncd[cl])
      min_ncd[cl] = dr->pairs[p].ncd;
  }

  ClusterOrder *order = malloc(((size_t)max_cluster + 2) * sizeof(ClusterOrder));
  int nclusters = 0;
  for (int c = 0; c <= max_cluster; c++)
    if (sizes[c] >= 2) {
      order[nclusters].cluster = c;
      order[nclusters].size = sizes[c];
      nclusters++;
    }
  qsort(order, (size_t)nclusters, sizeof(ClusterOrder), cluster_order_cmp);

  int printed = 0;
  for (int ci = 0; ci < nclusters && printed < MAX_DUP_CLUSTERS; ci++) {
    int c = order[ci].cluster;
    printed++;
    printf("\n  cluster %d \xe2\x80\x94 NCD %.2f (%s)\n", printed, min_ncd[c],
           min_ncd[c] < 0.10   ? "nearly identical"
           : min_ncd[c] < 0.20 ? "very similar"
                               : "similar");
    for (int i = 0; i < dr->func_count; i++) {
      if (dr->funcs[i].cluster != c)
        continue;
      printf("    %s:%d\t%s()\t[%d lines]\n", dr->funcs[i].filepath,
             dr->funcs[i].start_line,
             dr->funcs[i].name[0] ? dr->funcs[i].name : "?",
             dr->funcs[i].line_count);
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
    if (strncmp(argv[i], "--threshold=", 12) == 0)
      threshold = atof(argv[i] + 12);
    else if (strcmp(argv[i], "--cross-lang") == 0)
      cross_lang = true;
    else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      return -1;
    else if (argv[i][0] != '-')
      target = argv[i];
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
    if (!content)
      continue;
    if (!util_should_skip(content, len))
      dupes_add_file(&dr, fl.paths[i], content, len);
    free(content);
  }
  fl_free(&fl);

  printf("\n  collected %d functions (>= %d bytes) from %s\n", dr.func_count,
         NCD_MIN_BODY_BYTES, target);

  if (cross_lang) {
    for (int i = 0; i < dr.func_count; i++)
      dr.funcs[i].lang = LANG_C_LIKE;
  }

  dupes_compute(&dr, threshold);
  print_dupes(&dr, threshold);

  int found = dr.cluster_count;
  dupes_free(&dr);
  printf("\n");
  return found > 0 ? 2 : 0;
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
  printf("    %-14s %.3f +/- %-16.3f %.3f +/- %.3f\n", name, g->mu_ai,
         g->sig_ai, g->mu_h, g->sig_h);
}

static void report_methodology(const Calibration *cal) {
  report_header("METHODOLOGY");
  printf(
      "\n"
      "  What you are seeing\n\n"
      "    The slop score (0-10) is built from many small checks on code quality\n"
      "    (comments, naming, duplicates, layout, optional git history). Each\n"
      "    check adds a positive or negative statistical weight. Small files\n"
      "    often show \"n/a\" for some rows until there is enough code to measure.\n\n"
      "    Full signal list and overlap notes: METRICS.md in the repo.\n\n"
      "  How the math works\n\n"
      "    Each row weight is a log-likelihood ratio (LLR):\n\n"
      "      Gaussian (continuous) measurements:\n"
      "        LLR = 0.5*(z_h^2 - z_a^2) + ln(sig_h/sig_a)\n\n"
      "      Binary (present / absent) patterns:\n"
      "        LLR = ln(p_slop/p_clean) if present\n\n"
      "    Weights are clamped to [%.1f, +%.1f] so one quirky file cannot dominate%s.\n"
      "    Raw score S = sum of weights, then:\n\n"
      "      slop = sigmoid(S / T) * 10\n\n"
      "    T = %.2f%s.\n\n"
      "    Labels: >= %.0f sloppy, >= %.0f moderate, >= %.0f mild, else clean.\n",
      cal->loaded ? LLR_CLAMP : LLR_CLAMP_UNCAL,
      cal->loaded ? LLR_CLAMP : LLR_CLAMP_UNCAL,
      cal->loaded ? "" : " (default cap)", cal->temperature,
      cal->loaded ? " (calibrated)" : " (default)",
      PROB_FLAGGED * 10, PROB_SUSPICIOUS * 10, PROB_INCONCLUSIVE * 10);

  printf("\n  Signal parameters:\n");
  printf("    %-14s %-22s %-22s\n", "signal", "Sloppy (mu +/- sig)",
         "Clean (mu +/- sig)");
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

  static const char *bp_names[] = {"narration", "decay", "over-wrapping",
                                   "naming breaks"};
  const double bp_slop[] = {cal->narr_p_ai, cal->decay_p_ai,
                            cal->overwrap_p_ai, cal->namebrk_p_ai};
  const double bp_clean[] = {cal->narr_p_h, cal->decay_p_h, cal->overwrap_p_h,
                             cal->namebrk_p_h};
  for (int k = 0; k < 4; k++)
    printf("    %-14s p_slop=%.3f            p_clean=%.3f\n", bp_names[k],
           bp_slop[k], bp_clean[k]);
}

static double report_single_file(const char *path, const Calibration *cal,
                                 bool use_git, double prior_llr) {
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
  printf("  lines:   %d total, %d code, %d comment, %d blank\n",
         ctx.scan.total_lines, ctx.scan.code_lines, ctx.scan.comment_lines,
         ctx.scan.blank_lines);
  printf("  funcs:   %d detected\n", ctx.scan.function_count);
  printf("  lang:    %s\n\n", lang_family_name(ctx.scan.lang));

  printf("  slop score = %.1f / 10   [%s]\n",
         slop_score_10(ctx.sc.probability), score_label(ctx.sc.probability));
  printf("  raw = %+.2f   temperature = %.2f   scaled = %+.2f\n",
         ctx.sc.raw_score, ctx.sc.temperature,
         ctx.sc.raw_score / ctx.sc.temperature);

  report_header("SIGNAL BREAKDOWN");
  printf("\n  Positive weight = more concern; negative = cleaner.  Rows explain "
         "themselves below.\n");
  print_signal_table(&ctx.sc, true);
  printf("  %36s = %+.2f / %.2f = %+.2f\n", "scaled (S/T)", ctx.sc.raw_score,
         ctx.sc.temperature, ctx.sc.raw_score / ctx.sc.temperature);
  printf("  %36s = %.1f\n", "slop score", slop_score_10(ctx.sc.probability));

  /* ── Smell findings ───────────────────────────────────── */
  SmellReport smell;
  smell_detect(&smell, &ctx.scan, true);

  report_header("FINDINGS");
  if (smell.count == 0) {
    printf("\n  no findings\n");
  } else {
    int strong = 0, related = 0, general = 0;
    for (int i = 0; i < smell.count; i++) {
      switch (smell.items[i].severity) {
      case SEV_DIAGNOSTIC:
        strong++;
        break;
      case SEV_CORRELATED:
        related++;
        break;
      case SEV_GENERAL:
        general++;
        break;
      }
    }
    printf("\n  %d finding%s (%d strong, %d score-related, %d general)\n",
           smell.count, smell.count == 1 ? "" : "s", strong, related, general);
    SmellSeverity last_sev = (SmellSeverity)-1;
    for (int i = 0; i < smell.count; i++) {
      const SmellFinding *f = &smell.items[i];
      if (f->severity != last_sev) {
        printf("\n  [%s]\n", severity_str(f->severity));
        last_sev = f->severity;
      }
      if (f->line > 0)
        printf("    line %-5d %-16s %s\n", f->line, smell_kind_str(f->kind),
               f->message);
      else
        printf("    %-10s %-16s %s\n", "", smell_kind_str(f->kind), f->message);
    }
  }

  /* ── Dead code ────────────────────────────────────────── */
  if (ctx.dead_lines > 0) {
    report_header("DEAD CODE");
    printf("\n  %d dead/unused lines detected\n", ctx.dead_lines);
    for (int i = 0; i < smell.count; i++) {
      if (smell.items[i].kind == SMELL_DEAD_CODE && smell.items[i].line > 0)
        printf("    line %-5d %s\n", smell.items[i].line,
               smell.items[i].message);
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
    if (strcmp(argv[i], "--git") == 0)
      use_git = true;
    else if (strcmp(argv[i], "--json") == 0)
      json_out = true;
    else if (strncmp(argv[i], OPT_PRIOR, sizeof(OPT_PRIOR) - 1) == 0)
      prior = atof(argv[i] + sizeof(OPT_PRIOR) - 1);
    else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      return -1;
    else if (argv[i][0] != '-')
      target = argv[i];
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
  double prior_llr;
  scoring_init(&cal, prior, &prior_llr);

  struct stat st;
  if (stat(target, &st) != 0) {
    err_cannot_stat(target);
    return 1;
  }

  printf("\n  ============================================================\n");
  printf("  SLOP REPORT \xe2\x80\x94 code quality analysis\n");
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
    printf("  functions: %d (>= %d bytes body)\n", drr.total_funcs,
           NCD_MIN_BODY_BYTES);
    printf("  dup_ratio: %.2f (%d/%d functions in duplicate clusters)\n",
           drr.ratio, drr.funcs_in_dup, drr.total_funcs);

    /* ── Per-file reports ── */
    report_header("PER-FILE ANALYSIS");

    FileEntry *rows = malloc((size_t)fl.count * sizeof(FileEntry));
    int row_count = 0;
    int total_dead = 0;
    int total_smells = 0;
    int sloppy = 0, moderate = 0, clean = 0;

    for (int i = 0; i < fl.count; i++) {
      size_t len = 0;
      char *content = util_read_file(fl.paths[i], &len);
      if (!content)
        continue;
      if (util_should_skip(content, len)) {
        free(content);
        continue;
      }

      FileScanCtx ctx;
      scan_one_file(&ctx, fl.paths[i], content, len, &cal, dup_ratio, use_git,
                    prior_llr);

      SmellReport smell;
      smell_detect(&smell, &ctx.scan, true);

      if (row_count < fl.count) {
        FileEntry *r = &rows[row_count++];
        snprintf(r->path, sizeof(r->path), "%s", fl.paths[i]);
        r->prob = ctx.sc.probability;
        r->raw_score = ctx.sc.raw_score;
        r->dead_lines = ctx.dead_lines;
      }
      total_dead += ctx.dead_lines;
      total_smells += smell.count;

      if (ctx.sc.probability >= PROB_FLAGGED)
        sloppy++;
      else if (ctx.sc.probability >= PROB_SUSPICIOUS)
        moderate++;
      else
        clean++;

      if (!json_out) {
        printf("\n  %s\n", fl.paths[i]);
        printf("    slop %.1f/10  [%s]  raw = %+.2f",
               slop_score_10(ctx.sc.probability),
               score_label(ctx.sc.probability), ctx.sc.raw_score);
        if (ctx.dead_lines > 0)
          printf("  dead = %d", ctx.dead_lines);
        if (smell.count > 0)
          printf("  findings = %d", smell.count);
        putchar('\n');

        print_signal_table(&ctx.sc, false);

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
    print_file_table(rows, row_count);
    printf("\n  %d sloppy / %d moderate / %d clean\n", sloppy, moderate, clean);
    printf("  %d total findings / %d dead lines across project\n", total_smells,
           total_dead);
    if (drr.ratio > 0)
      printf("  dup_ratio = %.2f (%d/%d functions)\n", drr.ratio,
             drr.funcs_in_dup, drr.total_funcs);

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

    if (sloppy > 0)
      return 2;
    if (moderate > 0)
      return 1;
    return 0;

  } else {
    double prob = report_single_file(target, &cal, use_git, prior_llr);
    report_methodology(&cal);
    printf("\n");

    if (prob < 0)
      return 0;
    return prob >= PROB_FLAGGED ? 2 : prob >= PROB_SUSPICIOUS ? 1 : 0;
  }
}

/* ── Usage ──────────────────────────────────────────────── */

static void usage(void) {
  fprintf(
      stderr,
      "slop \xe2\x80\x94 code quality scanner\n\n"
      "  Catches patterns linters miss: narration comments, dead code,\n"
      "  naming drift, cross-file duplicates, and more.\n\n"
      "Usage:\n"
      "  slop check  [--all] <file|dir>   Find code quality issues (primary)\n"
      "  slop scan   [options] <file|dir>  Score files (slop 0-10)\n"
      "  slop report [options] <file|dir>  Full quality report\n"
      "  slop dupes  [options] <dir>       Find duplicate functions (NCD)\n\n"
      "Check options:\n"
      "  --all    Include general issues (zombie params, unused imports, dead "
      "code)\n\n"
      "Scan options:\n"
      "  --verbose       Show per-file signal breakdown\n"
      "  --json          JSON output\n"
      "  --git           Include git commit patterns\n"
      "  --stdin --lang=LANG  Read from stdin\n\n"
      "Report options:\n"
      "  --git           Include git commit patterns\n\n"
      "Dupes options:\n"
      "  --threshold=N   NCD threshold (default 0.30)\n"
      "  --cross-lang    Compare across language families\n\n"
      "Exit codes:\n"
      "  0  clean\n"
      "  1  warnings (moderate findings or slop >= 6)\n"
      "  2  sloppy (strong tells, dupes found, or slop >= 8.5)\n\n"
      "Aliases: 'smell' = 'check'\n\n"
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
    if (strcmp(argv[i], "--stdin") == 0)
      has_stdin = true;

  if (strcmp(cmd, "check") == 0 || strcmp(cmd, "smell") == 0) {
    rc = cmd_check(argc, argv);
  } else if (strcmp(cmd, "scan") == 0) {
    rc = has_stdin ? cmd_scan_stdin(argc, argv) : cmd_scan(argc, argv);
  } else if (strcmp(cmd, "report") == 0) {
    rc = cmd_report(argc, argv);
  } else if (strcmp(cmd, "dupes") == 0) {
    rc = cmd_dupes(argc, argv);
  } else if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
    printf("slop %s\n", SLOP_VERSION);
    return 0;
  } else if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 ||
             strcmp(cmd, "help") == 0) {
    usage();
    return 0;
  } else {
    fprintf(stderr, "slop: unknown command '%s'\n", cmd);
    fprintf(stderr, "Commands: check, scan, report, dupes\n");
    return 1;
  }

  if (rc == -1) {
    usage();
    return 0;
  }
  return rc;
}
