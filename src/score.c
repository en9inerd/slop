#include "slop.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * Code quality score: sum of per-signal log-likelihood ratios (LLR), then
 * slop = sigmoid(sum / temperature) * 10. Signal inventory and overlap notes:
 * see METRICS.md at repo root. Pipeline below matches that document in order.
 */

/* ── Math helpers ───────────────────────────────────────────── */

static double clampd(double x, double lo, double hi) {
  return x < lo ? lo : x > hi ? hi : x;
}

static double gaussian_llr(double x, const GaussParam *p, double clamp) {
  double th = (x - p->mu_h) * (x - p->mu_h) / (p->sig_h * p->sig_h);
  double ta = (x - p->mu_ai) * (x - p->mu_ai) / (p->sig_ai * p->sig_ai);
  return clampd(0.5 * (th - ta) + log(p->sig_h / p->sig_ai), -clamp, clamp);
}

static double binary_llr(bool present, double p_ai, double p_h, double clamp) {
  if (present)
    return clampd(log(p_ai / p_h), -clamp, clamp);
  return clampd(log((1.0 - p_ai) / (1.0 - p_h)), -clamp, clamp);
}

/* ── Spearman rank correlation for K=4 ──────────────────────── */

static void rank4(const double v[4], double r[4]) {
  int idx[4] = {0, 1, 2, 3};
  for (int i = 0; i < 3; i++)
    for (int j = i + 1; j < 4; j++)
      if (v[idx[i]] > v[idx[j]]) {
        int t = idx[i];
        idx[i] = idx[j];
        idx[j] = t;
      }
  for (int i = 0; i < 4;) {
    int j = i;
    while (j < 3 && v[idx[j + 1]] == v[idx[j]])
      j++;
    double avg = ((double)i + (double)j) / 2.0 + 1.0;
    for (int k = i; k <= j; k++)
      r[idx[k]] = avg;
    i = j + 1;
  }
}

static double spearman4(const double vals[4]) {
  double ranks[4];
  rank4(vals, ranks);
  double sum_d2 = 0;
  for (int i = 0; i < 4; i++) {
    double d = (double)(i + 1) - ranks[i];
    sum_d2 += d * d;
  }
  return 1.0 - 6.0 * sum_d2 / 60.0;
}

/* ── Signal computation ─────────────────────────────────────── */

static void add_detail(ScoreResult *out, const char *group, const char *signal,
                       const char *meas, double llr) {
  if (out->signal_count >= MAX_SIGNAL_DETAILS)
    return;
  SignalDetail *d = &out->details[out->signal_count++];
  d->group = group;
  d->signal = signal;
  snprintf(d->measured, sizeof(d->measured), "%s", meas);
  d->llr = llr;
}

/* ── Group A: regularity (entropy + CV + compression) ───── */

static double score_regularity(ScoreResult *out, const ScanResult *scan,
                               const Calibration *cal, double compression_ratio,
                               double clamp) {
  int nz_count = 0;
  for (int i = 0; i < scan->line_length_count; i++)
    if (scan->line_lengths[i] > 0)
      nz_count++;

  double h_norm = 0;
  if (nz_count > 0) {
    int buckets[LINE_LEN_BUCKETS] = {};
    for (int i = 0; i < scan->line_length_count; i++) {
      if (scan->line_lengths[i] <= 0)
        continue;
      int b = scan->line_lengths[i] / LINE_LEN_BUCKET_W;
      if (b >= LINE_LEN_BUCKETS)
        b = LINE_LEN_BUCKETS - 1;
      buckets[b]++;
    }
    double h = 0;
    int used = 0;
    for (int b = 0; b < LINE_LEN_BUCKETS; b++) {
      if (!buckets[b])
        continue;
      used++;
      double p = (double)buckets[b] / nz_count;
      h -= p * log2(p);
    }
    if (used > 1)
      h_norm = h / log2(used);
  }

  double cv = 0;
  if (nz_count > 1) {
    double sum = 0, sum2 = 0;
    for (int i = 0; i < scan->line_length_count; i++) {
      if (scan->line_lengths[i] <= 0)
        continue;
      const double v = scan->line_lengths[i];
      sum += v;
      sum2 += v * v;
    }
    const double mean = sum / nz_count;
    double var = sum2 / nz_count - mean * mean;
    if (var < 0)
      var = 0;
    if (mean > 0)
      cv = sqrt(var) / mean;
  }

  const double hn =
      1.0 - clampd((h_norm - REG_ENTROPY_LO) / REG_ENTROPY_SPAN, 0, 1);
  const double cn = 1.0 - clampd((cv - REG_CV_LO) / REG_CV_SPAN, 0, 1);
  const bool has_compress = (compression_ratio > 0 && compression_ratio < 1.0);

  double reg;
  if (has_compress) {
    const double rn =
        1.0 -
        clampd((compression_ratio - REG_COMPRESS_LO) / REG_COMPRESS_SPAN, 0, 1);
    reg = (rn + hn + cn) / 3.0;
  } else {
    reg = (hn + cn) / 2.0;
  }

  double llr = gaussian_llr(reg, &cal->regularity, clamp);
  if (!has_compress)
    llr = clampd(llr, -LLR_CLAMP_NOCOMPR, LLR_CLAMP_NOCOMPR);
  if (scan->total_lines < MIN_LINES_DECAY)
    llr = clampd(llr, -LLR_CLAMP_SMALL, LLR_CLAMP_SMALL);

  char mbuf[48];
  snprintf(mbuf, sizeof(mbuf), "%.2f", reg);
  add_detail(out, "regularity", has_compress ? "composite" : "composite*", mbuf,
             llr);
  return llr;
}

/* ── Group D: positional quality decay ───────────────────── */

static bool detect_decay(const ScanResult *scan) {
  if (scan->total_lines >= MIN_LINES_DECAY &&
      scan->total_defects >= MIN_DEFECTS_DECAY) {
    int q_sz = scan->total_lines / 4;
    if (q_sz < 1)
      q_sz = 1;
    double rates[4];
    for (int q = 0; q < 4; q++)
      rates[q] = (double)scan->defects_per_quartile[q] / q_sz;
    if (spearman4(rates) > SPEARMAN_THRESHOLD)
      return true;
  }
  return has_comment_gradient(scan->body_lines_top, scan->body_comment_top,
                              scan->body_lines_bottom,
                              scan->body_comment_bottom);
}

static double accum_comment_signals(ScoreResult *out, const ScanResult *scan,
                                    const Calibration *cal, int code,
                                    double clamp) {
  char mbuf[48];
  double S = 0;

  const double ccr = (double)scan->comment_lines / code;
  double llr = gaussian_llr(ccr, &cal->ccr, clamp);
  snprintf(mbuf, sizeof(mbuf), "%.2f", ccr);
  add_detail(out, "comments", "comment-to-code", mbuf, llr);
  S += llr;

  const int nc = scan->narration_count;
  if (nc < NARRATION_TIER1)
    llr = binary_llr(false, cal->narr_p_ai, cal->narr_p_h, clamp);
  else if (nc < NARRATION_TIER2)
    llr = binary_llr(true, cal->narr_p_ai, cal->narr_p_h, clamp);
  else
    llr = clamp;
  snprintf(mbuf, sizeof(mbuf), "%s (%d hits)",
           nc >= NARRATION_TIER1 ? "yes" : "no", nc);
  add_detail(out, "comments", "narration", mbuf, llr);
  S += llr;

  const double cd = (double)scan->conditional_count / code;
  if (scan->conditional_count >= MIN_COND_KEYWORDS) {
    llr = gaussian_llr(cd, &cal->cond, clamp);
    snprintf(mbuf, sizeof(mbuf), "%.2f", cd);
  } else {
    llr = 0;
    snprintf(mbuf, sizeof(mbuf), "n/a (%d kw)", scan->conditional_count);
  }
  add_detail(out, "structure", "conditional dens.", mbuf, llr);
  S += llr;

  return S;
}

static double accum_position_and_patterns(ScoreResult *out,
                                          const ScanResult *scan,
                                          const Calibration *cal,
                                          double clamp) {
  char mbuf[48];
  double S = 0;

  const bool decay = detect_decay(scan);
  double llr;
  if (scan->total_lines < MIN_LINES_DECAY) {
    llr = 0;
    snprintf(mbuf, sizeof(mbuf), "n/a (<%d lines)", MIN_LINES_DECAY);
  } else {
    llr = binary_llr(decay, cal->decay_p_ai, cal->decay_p_h, clamp);
    snprintf(mbuf, sizeof(mbuf), "%s", decay ? "detected" : "none");
  }
  add_detail(out, "position", "quality decay", mbuf, llr);
  S += llr;

  if (scan->overwrap_count > 0) {
    llr = binary_llr(true, cal->overwrap_p_ai, cal->overwrap_p_h, clamp);
    snprintf(mbuf, sizeof(mbuf), "%d hit%s", scan->overwrap_count,
             scan->overwrap_count == 1 ? "" : "s");
  } else {
    llr = binary_llr(false, cal->overwrap_p_ai, cal->overwrap_p_h, clamp);
    snprintf(mbuf, sizeof(mbuf), "none");
  }
  add_detail(out, "patterns", "over-wrapping", mbuf, llr);
  S += llr;

  if (scan->name_break_count > 0) {
    llr = binary_llr(true, cal->namebrk_p_ai, cal->namebrk_p_h, clamp);
    snprintf(mbuf, sizeof(mbuf), "%d func%s", scan->name_break_count,
             scan->name_break_count == 1 ? "" : "s");
  } else {
    llr = binary_llr(false, cal->namebrk_p_ai, cal->namebrk_p_h, clamp);
    snprintf(mbuf, sizeof(mbuf), "none");
  }
  add_detail(out, "patterns", "naming breaks", mbuf, llr);
  S += llr;

  return S;
}

static double accum_naming_and_layout(ScoreResult *out, const ScanResult *scan,
                                     const Calibration *cal, double clamp) {
  char mbuf[48];
  double S = 0;
  double llr;

  if (scan->total_identifiers >= MIN_IDENTIFIERS) {
    double ispec = (double)scan->generic_identifiers / scan->total_identifiers;
    llr = gaussian_llr(ispec, &cal->ident, clamp);
    snprintf(mbuf, sizeof(mbuf), "%.2f (%d/%d)", ispec,
             scan->generic_identifiers, scan->total_identifiers);
  } else {
    llr = 0;
    snprintf(mbuf, sizeof(mbuf), "n/a (%d idents)", scan->total_identifiers);
  }
  add_detail(out, "naming", "ident. specificity", mbuf, llr);
  S += llr;

  if (scan->function_count >= MIN_FUNCS_CV) {
    double sum = 0, sum2 = 0;
    int nf = 0;
    for (int f = 0; f < scan->function_count; f++) {
      int lines =
          scan->functions[f].end_line - scan->functions[f].start_line + 1;
      if (lines < 1)
        continue;
      double v = (double)lines;
      sum += v;
      sum2 += v * v;
      nf++;
    }
    if (nf >= MIN_FUNCS_CV) {
      double mean = sum / nf;
      double var = sum2 / nf - mean * mean;
      if (var < 0)
        var = 0;
      double cv = (mean > 0) ? sqrt(var) / mean : 0;
      llr = gaussian_llr(cv, &cal->func_cv, clamp);
      snprintf(mbuf, sizeof(mbuf), "%.2f (%d funcs)", cv, nf);
    } else {
      llr = 0;
      snprintf(mbuf, sizeof(mbuf), "n/a (%d funcs)", nf);
    }
  } else {
    llr = 0;
    snprintf(mbuf, sizeof(mbuf), "n/a (%d funcs)", scan->function_count);
  }
  add_detail(out, "structure", "func length CV", mbuf, llr);
  S += llr;

  if (scan->total_identifiers >= MIN_TTR_TOKENS) {
    double ttr = (double)scan->unique_identifiers / scan->total_identifiers;
    llr = gaussian_llr(ttr, &cal->ttr, clamp);
    snprintf(mbuf, sizeof(mbuf), "%.2f (%d/%d)", ttr, scan->unique_identifiers,
             scan->total_identifiers);
  } else {
    llr = 0;
    snprintf(mbuf, sizeof(mbuf), "n/a (%d tokens)", scan->total_identifiers);
  }
  add_detail(out, "naming", "token diversity", mbuf, llr);
  S += llr;

  if (scan->indent_count >= MIN_INDENT_LINES) {
    double sum = 0, sum2 = 0;
    for (int k = 0; k < scan->indent_count; k++) {
      double v = (double)scan->indent_depths[k];
      sum += v;
      sum2 += v * v;
    }
    double mean = sum / scan->indent_count;
    double var = sum2 / scan->indent_count - mean * mean;
    if (var < 0)
      var = 0;
    double icv = (mean > 0) ? sqrt(var) / mean : 0;
    llr = gaussian_llr(icv, &cal->indent, clamp);
    snprintf(mbuf, sizeof(mbuf), "%.2f (%d lines)", icv, scan->indent_count);
  } else {
    llr = 0;
    snprintf(mbuf, sizeof(mbuf), "n/a (%d lines)", scan->indent_count);
  }
  add_detail(out, "whitespace", "indent regularity", mbuf, llr);
  S += llr;

  return S;
}

static double accum_project_signals(ScoreResult *out, const Calibration *cal,
                                    double dup_ratio, const GitFeatures *git,
                                    double clamp) {
  char mbuf[48];
  double S = 0;

  if (dup_ratio >= 0) {
    double llr = gaussian_llr(dup_ratio, &cal->dup, clamp);
    snprintf(mbuf, sizeof(mbuf), "%.2f", dup_ratio);
    add_detail(out, "dupes", "duplicate ratio", mbuf, llr);
    S += llr;
  }

  if (git && git->available) {
    double llr = gaussian_llr(git->composite, &cal->git, clamp);
    snprintf(mbuf, sizeof(mbuf), "%.2f", git->composite);
    add_detail(out, "git", "commit style", mbuf, llr);
    S += llr;
  }

  return S;
}

void score_compute(ScoreResult *out, const ScanResult *scan,
                   const Calibration *cal, double compression_ratio,
                   double dup_ratio, const GitFeatures *git) {
  *out = (ScoreResult){};
  out->temperature = cal->temperature;
  if (scan->code_lines == 0) {
    out->probability = DEFAULT_PRIOR;
    add_detail(out, "score", "insufficient code", "0 code lines", 0);
    return;
  }

  const int code = scan->code_lines;
  const double clamp = cal->loaded ? LLR_CLAMP : LLR_CLAMP_UNCAL;

  double S = score_regularity(out, scan, cal, compression_ratio, clamp);
  S += accum_comment_signals(out, scan, cal, code, clamp);
  S += accum_position_and_patterns(out, scan, cal, clamp);
  S += accum_naming_and_layout(out, scan, cal, clamp);
  S += accum_project_signals(out, cal, dup_ratio, git, clamp);

  out->raw_score = S;
  out->probability = slop_sigmoid(S / cal->temperature);
}
