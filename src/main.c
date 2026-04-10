#include "output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
    printf("\n  %d files scanned, %d with findings (%d total)\n",
           stats.files_scanned, stats.files_with_findings,
           stats.total_findings);
  if (stats.total_findings == 0 && stats.files_scanned > 0)
    printf("\n  no issues found\n");
  printf("\n");

  if (stats.has_diagnostic)
    return 2;
  if (stats.has_correlated)
    return 1;
  return 0;
}

/* ── Scan command ───────────────────────────────────────────── */

typedef struct {
  int files_scanned;
  int files_skipped;
  int sloppy;
  int moderate;
  int clean;
} ScanStats;

#define MAX_SCAN_ENTRIES 8192
static const char OPT_PRIOR[] = "--prior=";

static void classify(ScanStats *ss, double prob) {
  ss->files_scanned++;
  if (prob >= PROB_FLAGGED)
    ss->sloppy++;
  else if (prob >= PROB_SUSPICIOUS)
    ss->moderate++;
  else
    ss->clean++;
}

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

    DupeResult dr;
    dupes_init(&dr);
    collect_dupes(&dr, &fl);
    const DupRatioResult drr = dupes_compute_ratio(&dr);
    double dup_ratio = drr.ratio;

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
        e->code_lines = ctx.scan.code_lines;
        e->sc = ctx.sc;
      }

      scan_one_free(&ctx);
      free(content);
    }

    double weight_sum = 0, prob_sum = 0;
    for (int i = 0; i < entry_count; i++) {
      double w = entries[i].code_lines > 0 ? (double)entries[i].code_lines : 1.0;
      prob_sum += entries[i].prob * w;
      weight_sum += w;
    }
    double project_prob = weight_sum > 0 ? prob_sum / weight_sum : 0;

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
      printf("\n  project score = %.1f / 10   [%s]\n",
             slop_score_10(project_prob), score_label(project_prob));
      printf("  %d sloppy / %d moderate / %d clean\n", ss.sloppy, ss.moderate,
             ss.clean);
      if (total_dead > 0)
        printf("  %d dead lines detected across project\n", total_dead);
    }

    if (json_out) {
      printf("{\n  \"calibrated\": %s,\n  \"temperature\": %.2f,\n",
             cal.loaded ? "true" : "false", cal.temperature);
      printf("  \"project_score\": %.1f,\n", slop_score_10(project_prob));
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
    else if (strcmp(argv[i], "--stdin") == 0) {}
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
    print_score_json(fake_name, &ctx.sc, ctx.dead_lines);
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
  print_dupes_display(&dr, threshold);

  int found = dr.cluster_count;
  dupes_free(&dr);
  printf("\n");
  return found > 0 ? 2 : 0;
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
