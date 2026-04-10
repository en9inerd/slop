#include "output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Error helpers ───────────────────────────────────────── */

void err_no_target(void) {
  fprintf(stderr, "slop: no file or directory specified\n");
}
void err_cannot_stat(const char *p) {
  fprintf(stderr, "slop: cannot stat %s\n", p);
}
void err_cannot_read(const char *p) {
  fprintf(stderr, "slop: cannot read %s\n", p);
}
void err_bad_option(const char *o) {
  fprintf(stderr, "slop: unknown option %s\n", o);
}

/* ── Output formatting ───────────────────────────────────── */

void print_rule(int width) {
  printf("  ");
  for (int i = 0; i < width; i++)
    putchar('-');
  putchar('\n');
}

const char *smell_kind_str(SmellKind k) {
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

const char *severity_str(SmellSeverity s) {
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

void print_findings(const char *filename, const SmellReport *r) {
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

const char *score_label(double p) {
  if (p >= PROB_FLAGGED)
    return "sloppy";
  if (p >= PROB_SUSPICIOUS)
    return "moderate";
  if (p >= PROB_INCONCLUSIVE)
    return "mild";
  return "clean";
}

double slop_score_10(double prob) { return prob * 10.0; }

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

void print_signal_table(const ScoreResult *sc, bool with_why) {
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

void print_score(const char *filename, const ScoreResult *sc) {
  printf("\n  %s \xe2\x80\x94 slop %.1f/10  [%s]\n", filename,
         slop_score_10(sc->probability), score_label(sc->probability));
  printf("\n  \"weight\" is log-odds evidence per check; they sum to the raw score.\n");
  print_signal_table(sc, true);
  printf("  %36s %+.2f\n", "raw total", sc->raw_score);
  printf("  %36s %+.2f\n", "scaled (/T)", sc->raw_score / sc->temperature);
}

/* ── JSON output ─────────────────────────────────────────── */

void json_escape(const char *s, char *out, int outsz) {
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

void print_score_json(const char *filename, const ScoreResult *sc,
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

/* ── File table ──────────────────────────────────────────── */

int entry_cmp(const void *a, const void *b) {
  const double pa = ((const FileEntry *)a)->prob;
  const double pb = ((const FileEntry *)b)->prob;
  return (pa < pb) ? 1 : (pa > pb) ? -1 : 0;
}

void print_file_table(const FileEntry *entries, int count) {
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

/* ── Duplicate display ───────────────────────────────────── */

typedef struct {
  int cluster;
  int size;
} ClusterOrder;

static int cluster_order_cmp(const void *a, const void *b) {
  return ((const ClusterOrder *)b)->size - ((const ClusterOrder *)a)->size;
}

void print_dupes_display(const DupeResult *dr, double threshold) {
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

/* ── Report display helpers ──────────────────────────────── */

void report_divider(void) { print_rule(RULE_W_REPORT); }

void report_header(const char *title) {
  printf("\n");
  report_divider();
  printf("  %s\n", title);
  report_divider();
}

static void show_gp(const char *name, const GaussParam *g) {
  printf("    %-14s %.3f +/- %-16.3f %.3f +/- %.3f\n", name, g->mu_ai,
         g->sig_ai, g->mu_h, g->sig_h);
}

void report_methodology(const Calibration *cal) {
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

/* ── Shared CLI helpers ──────────────────────────────────── */

void collect_dupes(DupeResult *dr, const FileList *fl) {
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

void scan_one_file(FileScanCtx *ctx, const char *path, const char *content,
                   size_t len, const Calibration *cal, double dup_ratio,
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
}

void scan_one_free(FileScanCtx *ctx) { scan_free(&ctx->scan); }

void scoring_init(Calibration *cal, double prior, double *out_prior_llr) {
  calibration_default(cal);
  (void)calibration_load(cal, ".");
  *out_prior_llr = slop_prior_llr(prior);
}
