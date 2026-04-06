#include "slop.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Calibration defaults ───────────────────────────────────── */

void calibration_default(Calibration *cal) {
  cal->temperature = DEFAULT_TEMPERATURE;
  cal->regularity = (GaussParam){0.50, 0.18, 0.20, 0.13};
  cal->ccr = (GaussParam){0.25, 0.15, 0.04, 0.10};
  cal->cond = (GaussParam){0.18, 0.10, 0.10, 0.10};
  cal->dup = (GaussParam){0.22, 0.12, 0.10, 0.15};
  cal->git = (GaussParam){0.55, 0.20, 0.25, 0.22};
  cal->ident = (GaussParam){0.18, 0.08, 0.07, 0.04};
  cal->func_cv = (GaussParam){0.40, 0.20, 0.90, 0.35};
  cal->ttr = (GaussParam){0.35, 0.15, 0.40, 0.20};
  cal->indent = (GaussParam){0.45, 0.22, 0.70, 0.25};
  cal->narr_p_ai = 0.40;
  cal->narr_p_h = 0.01;
  cal->decay_p_ai = 0.25;
  cal->decay_p_h = 0.05;
  cal->overwrap_p_ai = 0.18;
  cal->overwrap_p_h = 0.01;
  cal->namebrk_p_ai = 0.15;
  cal->namebrk_p_h = 0.02;
  cal->loaded = false;
}

/* ── Calibration JSON I/O ───────────────────────────────────── */

static double json_find_double(const char *json, const char *key) {
  char needle[128];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *p = strstr(json, needle);
  if (!p)
    return 0;
  p += strlen(needle);
  while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n'))
    p++;
  return atof(p);
}

static void json_load_gp(const char *json, const char *name, GaussParam *gp) {
  const char *start = strstr(json, name);
  if (!start)
    return;

  gp->mu_ai = json_find_double(start, "mu_ai");
  gp->sig_ai = json_find_double(start, "sigma_ai");
  gp->mu_h = json_find_double(start, "mu_h");
  gp->sig_h = json_find_double(start, "sigma_h");

  if (gp->sig_ai < MIN_GAUSS_SIGMA)
    gp->sig_ai = MIN_GAUSS_SIGMA;
  if (gp->sig_h < MIN_GAUSS_SIGMA)
    gp->sig_h = MIN_GAUSS_SIGMA;
}

bool calibration_load(Calibration *cal, const char *dir) {
  char path[4096];
  snprintf(path, sizeof(path), "%s/.slop-calibration.json", dir);

  size_t len = 0;
  char *json = util_read_file(path, &len);
  if (!json)
    return false;

  Calibration tmp = {};

  tmp.temperature = json_find_double(json, "temperature");
  if (tmp.temperature < MIN_TEMPERATURE)
    tmp.temperature = DEFAULT_TEMPERATURE;

  json_load_gp(json, "\"regularity\"", &tmp.regularity);
  json_load_gp(json, "\"ccr\"", &tmp.ccr);
  json_load_gp(json, "\"cond\"", &tmp.cond);
  json_load_gp(json, "\"dup\"", &tmp.dup);
  json_load_gp(json, "\"git\"", &tmp.git);
  json_load_gp(json, "\"ident\"", &tmp.ident);
  json_load_gp(json, "\"func_cv\"", &tmp.func_cv);
  json_load_gp(json, "\"ttr\"", &tmp.ttr);
  json_load_gp(json, "\"indent\"", &tmp.indent);

  const char *bp = strstr(json, "\"binary\"");
  if (bp) {
    const char *np = strstr(bp, "\"narration\"");
    if (np) {
      tmp.narr_p_ai = json_find_double(np, "p_ai");
      tmp.narr_p_h = json_find_double(np, "p_h");
    }
    const char *dp = strstr(bp, "\"decay\"");
    if (dp) {
      tmp.decay_p_ai = json_find_double(dp, "p_ai");
      tmp.decay_p_h = json_find_double(dp, "p_h");
    }
  }

  const char *pp = strstr(json, "\"patterns\"");
  if (pp) {
    const char *ow = strstr(pp, "\"overwrap\"");
    if (ow) {
      tmp.overwrap_p_ai = json_find_double(ow, "p_ai");
      tmp.overwrap_p_h = json_find_double(ow, "p_h");
    }
    const char *nb = strstr(pp, "\"namebreak\"");
    if (nb) {
      tmp.namebrk_p_ai = json_find_double(nb, "p_ai");
      tmp.namebrk_p_h = json_find_double(nb, "p_h");
    }
  }

  free(json);

  /*
   * Validate: parse into zeroed tmp, so unfilled fields remain 0.
   * A signal is "present" only if both sigmas were actually parsed
   * with non-trivial values. Require at least 3 of 5 Gaussian
   * signals to be valid — otherwise the file is malformed.
   */
  int valid_signals = 0;
  GaussParam *gps[] = {&tmp.regularity, &tmp.ccr, &tmp.cond,
                       &tmp.dup,        &tmp.git, &tmp.ident,
                       &tmp.func_cv,    &tmp.ttr, &tmp.indent};
  for (int g = 0; g < 9; g++) {
    if (gps[g]->sig_ai > MIN_GAUSS_SIGMA && gps[g]->sig_h > MIN_GAUSS_SIGMA)
      valid_signals++;
    if (gps[g]->sig_ai < MIN_GAUSS_SIGMA)
      gps[g]->sig_ai = MIN_GAUSS_SIGMA;
    if (gps[g]->sig_h < MIN_GAUSS_SIGMA)
      gps[g]->sig_h = MIN_GAUSS_SIGMA;
  }

  if (valid_signals < 3)
    return false;

  double *bins[] = {&tmp.narr_p_ai,    &tmp.narr_p_h,      &tmp.decay_p_ai,
                    &tmp.decay_p_h,    &tmp.overwrap_p_ai, &tmp.overwrap_p_h,
                    &tmp.namebrk_p_ai, &tmp.namebrk_p_h};
  for (int b = 0; b < 8; b++) {
    if (*bins[b] < MIN_BINARY_PROB)
      *bins[b] = MIN_BINARY_PROB;
  }

  tmp.loaded = true;
  *cal = tmp;
  return true;
}

static void write_gp(FILE *fp, const char *name, const GaussParam *g) {
  fprintf(fp,
          "    \"%s\": { \"mu_ai\": %.6f, \"sigma_ai\": %.6f, "
          "\"mu_h\": %.6f, \"sigma_h\": %.6f }",
          name, g->mu_ai, g->sig_ai, g->mu_h, g->sig_h);
}

bool calibration_save(const Calibration *cal, const char *dir) {
  char path[4096];
  snprintf(path, sizeof(path), "%s/.slop-calibration.json", dir);

  FILE *f = fopen(path, "w");
  if (!f)
    return false;

  fprintf(f, "{\n");
  fprintf(f, "  \"version\": 1,\n");
  fprintf(f, "  \"temperature\": %.6f,\n", cal->temperature);
  fprintf(f, "  \"signals\": {\n");

  write_gp(f, "regularity", &cal->regularity);
  fprintf(f, ",\n");
  write_gp(f, "ccr", &cal->ccr);
  fprintf(f, ",\n");
  write_gp(f, "cond", &cal->cond);
  fprintf(f, ",\n");
  write_gp(f, "dup", &cal->dup);
  fprintf(f, ",\n");
  write_gp(f, "git", &cal->git);
  fprintf(f, ",\n");
  write_gp(f, "ident", &cal->ident);
  fprintf(f, ",\n");
  write_gp(f, "func_cv", &cal->func_cv);
  fprintf(f, ",\n");
  write_gp(f, "ttr", &cal->ttr);
  fprintf(f, ",\n");
  write_gp(f, "indent", &cal->indent);
  fprintf(f, "\n");

  fprintf(f, "  },\n");
  fprintf(f, "  \"binary\": {\n");
  fprintf(f, "    \"narration\": { \"p_ai\": %.6f, \"p_h\": %.6f },\n",
          cal->narr_p_ai, cal->narr_p_h);
  fprintf(f, "    \"decay\": { \"p_ai\": %.6f, \"p_h\": %.6f }\n",
          cal->decay_p_ai, cal->decay_p_h);
  fprintf(f, "  },\n");
  fprintf(f, "  \"patterns\": {\n");
  fprintf(f, "    \"overwrap\": { \"p_ai\": %.6f, \"p_h\": %.6f },\n",
          cal->overwrap_p_ai, cal->overwrap_p_h);
  fprintf(f, "    \"namebreak\": { \"p_ai\": %.6f, \"p_h\": %.6f }\n",
          cal->namebrk_p_ai, cal->namebrk_p_h);
  fprintf(f, "  }\n");
  fprintf(f, "}\n");

  fclose(f);
  return true;
}

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
  double pos[4] = {1, 2, 3, 4};
  double sum_d2 = 0;
  for (int i = 0; i < 4; i++) {
    double d = pos[i] - ranks[i];
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

  out->m_regularity = reg;

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
  if (scan->body_lines_top >= MIN_BODY_LINES &&
      scan->body_lines_bottom >= MIN_BODY_LINES) {
    const double dt = (double)scan->body_comment_top / scan->body_lines_top;
    double db = (double)scan->body_comment_bottom / scan->body_lines_bottom;
    if (db < 0.001)
      db = 0.001;
    const int total_c = scan->body_comment_top + scan->body_comment_bottom;
    if (dt / db > GRADIENT_THRESHOLD && total_c >= MIN_GRADIENT_CMTS)
      return true;
  }
  return false;
}

/* ── Score computation ──────────────────────────────────── */

void score_compute(ScoreResult *out, const ScanResult *scan,
                   const Calibration *cal, double compression_ratio,
                   double dup_ratio, const GitFeatures *git) {
  *out = (ScoreResult){};
  out->temperature = cal->temperature;
  out->m_dup_ratio = dup_ratio;
  out->m_git_composite = (git && git->available) ? git->composite : -1;

  if (scan->code_lines == 0) {
    out->probability = DEFAULT_PRIOR;
    add_detail(out, "—", "insufficient code", "0 lines", 0);
    return;
  }

  const int code = scan->code_lines;
  double S = 0;
  char mbuf[48];
  const double clamp = cal->loaded ? LLR_CLAMP : LLR_CLAMP_UNCAL;

  S += score_regularity(out, scan, cal, compression_ratio, clamp);

  const double ccr = (double)scan->comment_lines / code;
  out->m_ccr = ccr;
  double llr = gaussian_llr(ccr, &cal->ccr, clamp);
  snprintf(mbuf, sizeof(mbuf), "%.2f", ccr);
  add_detail(out, "comments", "comment-to-code", mbuf, llr);
  S += llr;

  const int nc = scan->narration_count;
  out->m_narration_count = nc;
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
  out->m_cond_density = cd;
  if (scan->conditional_count >= MIN_COND_KEYWORDS) {
    llr = gaussian_llr(cd, &cal->cond, clamp);
    snprintf(mbuf, sizeof(mbuf), "%.2f", cd);
  } else {
    llr = 0;
    snprintf(mbuf, sizeof(mbuf), "n/a (%d kw)", scan->conditional_count);
  }
  add_detail(out, "structure", "conditional dens.", mbuf, llr);
  S += llr;

  const bool decay = detect_decay(scan);
  out->m_decay = decay;
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

  /* identifier specificity: generic_names / total identifiers */
  if (scan->total_identifiers >= MIN_IDENTIFIERS) {
    double ispec = (double)scan->generic_identifiers / scan->total_identifiers;
    out->m_ident_spec = ispec;
    llr = gaussian_llr(ispec, &cal->ident, clamp);
    snprintf(mbuf, sizeof(mbuf), "%.2f (%d/%d)", ispec,
             scan->generic_identifiers, scan->total_identifiers);
  } else {
    out->m_ident_spec = -1;
    llr = 0;
    snprintf(mbuf, sizeof(mbuf), "n/a (%d idents)", scan->total_identifiers);
  }
  add_detail(out, "naming", "ident. specificity", mbuf, llr);
  S += llr;

  /* function length CV: stdev(line counts) / mean(line counts) */
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
      out->m_func_cv = cv;
      llr = gaussian_llr(cv, &cal->func_cv, clamp);
      snprintf(mbuf, sizeof(mbuf), "%.2f (%d funcs)", cv, nf);
    } else {
      out->m_func_cv = -1;
      llr = 0;
      snprintf(mbuf, sizeof(mbuf), "n/a (%d funcs)", nf);
    }
  } else {
    out->m_func_cv = -1;
    llr = 0;
    snprintf(mbuf, sizeof(mbuf), "n/a (%d funcs)", scan->function_count);
  }
  add_detail(out, "structure", "func length CV", mbuf, llr);
  S += llr;

  /* token diversity: unique / total identifiers */
  if (scan->total_identifiers >= MIN_TTR_TOKENS) {
    double ttr = (double)scan->unique_identifiers / scan->total_identifiers;
    out->m_ttr = ttr;
    llr = gaussian_llr(ttr, &cal->ttr, clamp);
    snprintf(mbuf, sizeof(mbuf), "%.2f (%d/%d)", ttr, scan->unique_identifiers,
             scan->total_identifiers);
  } else {
    out->m_ttr = -1;
    llr = 0;
    snprintf(mbuf, sizeof(mbuf), "n/a (%d tokens)", scan->total_identifiers);
  }
  add_detail(out, "naming", "token diversity", mbuf, llr);
  S += llr;

  /* indentation regularity: CV of leading-whitespace depths */
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
    out->m_indent_reg = icv;
    llr = gaussian_llr(icv, &cal->indent, clamp);
    snprintf(mbuf, sizeof(mbuf), "%.2f (%d lines)", icv, scan->indent_count);
  } else {
    out->m_indent_reg = -1;
    llr = 0;
    snprintf(mbuf, sizeof(mbuf), "n/a (%d lines)", scan->indent_count);
  }
  add_detail(out, "whitespace", "indent regularity", mbuf, llr);
  S += llr;

  if (dup_ratio >= 0) {
    llr = gaussian_llr(dup_ratio, &cal->dup, clamp);
    snprintf(mbuf, sizeof(mbuf), "%.2f", dup_ratio);
    add_detail(out, "dupes", "duplicate ratio", mbuf, llr);
    S += llr;
  }

  if (git && git->available) {
    llr = gaussian_llr(git->composite, &cal->git, clamp);
    snprintf(mbuf, sizeof(mbuf), "%.2f", git->composite);
    add_detail(out, "git", "commit style", mbuf, llr);
    S += llr;
  }

  out->raw_score = S;
  out->probability = slop_sigmoid(S / cal->temperature);
}
