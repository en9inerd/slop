#include "output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Single-file report ──────────────────────────────────── */

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

/* ── Report command ──────────────────────────────────────── */

int cmd_report(int argc, char **argv) {
  const char *target = nullptr;
  bool use_git = false;
  double prior = DEFAULT_PRIOR;
  bool json_out = false;

  static const char OPT_PRIOR[] = "--prior=";

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

    DupeResult dr;
    dupes_init(&dr);
    collect_dupes(&dr, &fl);
    const DupRatioResult drr = dupes_compute_ratio(&dr);
    double dup_ratio = drr.ratio;

    report_header("PROJECT OVERVIEW");
    printf("\n  target:    %s\n", target);
    printf("  files:     %d source files\n", fl.count);
    printf("  functions: %d (>= %d bytes body)\n", drr.total_funcs,
           NCD_MIN_BODY_BYTES);
    printf("  dup_ratio: %.2f (%d/%d functions in duplicate clusters)\n",
           drr.ratio, drr.funcs_in_dup, drr.total_funcs);

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

    report_header("SUMMARY");
    print_file_table(rows, row_count);
    printf("\n  %d sloppy / %d moderate / %d clean\n", sloppy, moderate, clean);
    printf("  %d total findings / %d dead lines across project\n", total_smells,
           total_dead);
    if (drr.ratio > 0)
      printf("  dup_ratio = %.2f (%d/%d functions)\n", drr.ratio,
             drr.funcs_in_dup, drr.total_funcs);

    if (dr.cluster_count > 0) {
      report_header("DUPLICATE FUNCTIONS");
      print_dupes_display(&dr, NCD_THRESHOLD);
    }

    free(rows);
    dupes_free(&dr);
    fl_free(&fl);

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
