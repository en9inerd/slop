#include "slop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void slop_options_default(SlopOptions *opts) {
  *opts = (SlopOptions){
      .include_general_smells = false,
      .use_git = false,
      .prior = DEFAULT_PRIOR,
      .calibration_dir = nullptr,
  };
}

static void load_cal(Calibration *cal, const SlopOptions *opts,
                     const char *fallback) {
  calibration_default(cal);
  if (opts->calibration_dir)
    (void)calibration_load(cal, opts->calibration_dir);
  else if (fallback)
    (void)calibration_load(cal, fallback);
}

static void apply_prior(ScoreResult *sc, const Calibration *cal,
                        double prior_llr) {
  sc->raw_score += prior_llr;
  sc->probability = slop_sigmoid(sc->raw_score / cal->temperature);
}

static void analyze_content(SlopFileResult *r, const char *path,
                            const char *content, size_t len,
                            const Calibration *cal, double dup_ratio,
                            bool use_git, double prior_llr,
                            bool include_general) {
  snprintf(r->filepath, sizeof(r->filepath), "%s", path);

  if (util_should_skip(content, len)) {
    r->skipped = true;
    return;
  }

  LangFamily lang = lang_detect(path);
  ScanResult scan;
  scan_file(&scan, path, content, len, lang);

  const double cr = compress_ratio(content, len);
  GitFeatures gf = {};
  if (use_git)
    git_features(&gf, path);

  score_compute(&r->score, &scan, cal, cr, dup_ratio, use_git ? &gf : nullptr);
  apply_prior(&r->score, cal, prior_llr);

  r->probability = r->score.probability;
  r->raw_score = r->score.raw_score;
  r->dead_lines = smell_count_dead_lines(&scan);
  r->total_lines = scan.total_lines;
  r->code_lines = scan.code_lines;
  r->comment_lines = scan.comment_lines;
  r->blank_lines = scan.blank_lines;
  r->function_count = scan.function_count;
  r->lang = scan.lang;

  smell_detect(&r->smells, &scan, include_general);
  scan_free(&scan);
}

SlopFileResult slop_analyze_file(const char *path, const SlopOptions *opts) {
  SlopFileResult r = {};
  snprintf(r.filepath, sizeof(r.filepath), "%s", path);

  Calibration cal;
  load_cal(&cal, opts, ".");
  const double prior_llr = slop_prior_llr(opts->prior);

  size_t len = 0;
  char *content = util_read_file(path, &len);
  if (!content) {
    r.skipped = true;
    return r;
  }

  analyze_content(&r, path, content, len, &cal, -1, opts->use_git, prior_llr,
                  opts->include_general_smells);
  free(content);
  return r;
}

SlopProjectResult slop_analyze_dir(const char *dirpath,
                                   const SlopOptions *opts) {
  SlopProjectResult pr = {};

  load_cal(&pr.calibration, opts, dirpath);
  const double prior_llr = slop_prior_llr(opts->prior);

  FileList fl;
  fl_init(&fl);
  fl_collect(&fl, dirpath);

  DupeResult dr;
  dupes_init(&dr);
  for (int i = 0; i < fl.count; i++) {
    size_t len = 0;
    char *content = util_read_file(fl.paths[i], &len);
    if (!content)
      continue;
    if (util_should_skip(content, len)) {
      free(content);
      continue;
    }
    dupes_add_file(&dr, fl.paths[i], content, len);
    free(content);
  }
  dupes_compute(&dr, NCD_THRESHOLD);

  const DupRatioResult drr = dupes_compute_ratio(&dr);
  pr.total_funcs = drr.total_funcs;
  pr.funcs_in_dup = drr.funcs_in_dup;
  pr.dup_ratio = drr.ratio;
  pr.dupes = dr;

  pr.files = calloc((size_t)fl.count, sizeof(SlopFileResult));
  pr.file_count = 0;

  for (int i = 0; i < fl.count; i++) {
    size_t len = 0;
    char *content = util_read_file(fl.paths[i], &len);
    if (!content)
      continue;

    SlopFileResult *r = &pr.files[pr.file_count];
    analyze_content(r, fl.paths[i], content, len, &pr.calibration, pr.dup_ratio,
                    opts->use_git, prior_llr, opts->include_general_smells);

    if (r->skipped)
      pr.skipped++;
    else if (r->probability >= PROB_FLAGGED)
      pr.flagged++;
    else if (r->probability >= PROB_SUSPICIOUS)
      pr.suspicious++;
    else
      pr.human++;

    pr.file_count++;
    free(content);
  }

  fl_free(&fl);
  return pr;
}

void slop_file_result_free(SlopFileResult *r) { (void)r; }

void slop_project_result_free(SlopProjectResult *r) {
  dupes_free(&r->dupes);
  free(r->files);
  r->files = nullptr;
  r->file_count = 0;
}
