#include "slop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
