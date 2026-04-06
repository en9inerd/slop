#include "slop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INIT_CAP 256

/* ── Union-Find ─────────────────────────────────────────── */

static bool uf_init(DupeResult *dr, int n) {
  dr->parent = malloc((size_t)n * sizeof(int));
  if (!dr->parent)
    return false;
  for (int i = 0; i < n; i++)
    dr->parent[i] = i;
  return true;
}

static int uf_find(int *parent, int x) {
  while (parent[x] != x) {
    parent[x] = parent[parent[x]];
    x = parent[x];
  }
  return x;
}

static void uf_union(int *parent, int a, int b) {
  a = uf_find(parent, a);
  b = uf_find(parent, b);
  if (a != b)
    parent[b] = a;
}

/* ── Public API ─────────────────────────────────────────── */

void dupes_init(DupeResult *dr) {
  *dr = (DupeResult){};
  dr->func_cap = INIT_CAP;
  dr->funcs = calloc((size_t)dr->func_cap, sizeof(DupeFunc));
  dr->pair_cap = INIT_CAP;
  dr->pairs = calloc((size_t)dr->pair_cap, sizeof(DupePair));
}

void dupes_add_file(DupeResult *dr, const char *filepath, const char *content,
                    size_t len) {
  LangFamily lang = lang_detect(filepath);
  ScanResult scan;
  scan_file(&scan, filepath, content, len, lang);

  for (int f = 0; f < scan.function_count; f++) {
    FuncInfo *fi = &scan.functions[f];
    if (fi->body_len < NCD_MIN_BODY_BYTES)
      continue;
    if (fi->body_offset + fi->body_len > len)
      continue;

    if (dr->func_count >= dr->func_cap) {
      dr->func_cap *= 2;
      dr->funcs = realloc(dr->funcs, (size_t)dr->func_cap * sizeof(DupeFunc));
    }

    DupeFunc *df = &dr->funcs[dr->func_count++];
    *df = (DupeFunc){};
    snprintf(df->filepath, sizeof(df->filepath), "%s", filepath);
    snprintf(df->name, sizeof(df->name), "%s", fi->name);
    df->start_line = fi->start_line;
    df->line_count = fi->end_line - fi->start_line + 1;
    df->lang = lang;
    df->body = malloc(fi->body_len + 1);
    memcpy(df->body, content + fi->body_offset, fi->body_len);
    df->body[fi->body_len] = '\0';
    df->body_len = fi->body_len;
    df->cluster = -1;
  }

  scan_free(&scan);
}

void dupes_compute(DupeResult *dr, double threshold) {
  if (dr->func_count < 2)
    return;

  free(dr->parent);
  dr->parent = nullptr;
  dr->pair_count = 0;
  dr->cluster_count = 0;
  if (!uf_init(dr, dr->func_count))
    return;

  for (int i = 0; i < dr->func_count; i++) {
    for (int j = i + 1; j < dr->func_count; j++) {
      DupeFunc *a = &dr->funcs[i];
      DupeFunc *b = &dr->funcs[j];

      if (a->lang != b->lang)
        continue;

      /* pre-filter: skip if size ratio > 2.0 */
      size_t mx = a->body_len > b->body_len ? a->body_len : b->body_len;
      size_t mn = a->body_len < b->body_len ? a->body_len : b->body_len;
      if (mn == 0 || (double)mx / (double)mn > NCD_SIZE_RATIO_MAX)
        continue;

      double ncd = compress_ncd(a->body, a->body_len, b->body, b->body_len);

      if (ncd < threshold) {
        if (dr->pair_count >= dr->pair_cap) {
          dr->pair_cap *= 2;
          dr->pairs =
              realloc(dr->pairs, (size_t)dr->pair_cap * sizeof(DupePair));
        }
        DupePair *p = &dr->pairs[dr->pair_count++];
        p->a = i;
        p->b = j;
        p->ncd = ncd;
        uf_union(dr->parent, i, j);
      }
    }
  }

  int next_id = 0;
  int *id_map = calloc((size_t)dr->func_count, sizeof(int));
  for (int i = 0; i < dr->func_count; i++)
    id_map[i] = -1;

  dr->cluster_count = 0;
  for (int i = 0; i < dr->func_count; i++) {
    int root = uf_find(dr->parent, i);
    if (id_map[root] < 0)
      id_map[root] = next_id++;
    dr->funcs[i].cluster = id_map[root];
  }

  int *sizes = calloc((size_t)next_id + 1, sizeof(int));
  for (int i = 0; i < dr->func_count; i++)
    if (dr->funcs[i].cluster >= 0)
      sizes[dr->funcs[i].cluster]++;
  for (int c = 0; c < next_id; c++)
    if (sizes[c] > 1)
      dr->cluster_count++;
  free(sizes);
  free(id_map);
}

DupRatioResult dupes_compute_ratio(const DupeResult *dr) {
  DupRatioResult out = {};
  out.total_funcs = dr->func_count;
  if (dr->func_count > 0) {
    int *cl_sz = calloc((size_t)dr->func_count, sizeof(int));
    for (int i = 0; i < dr->func_count; i++)
      if (dr->funcs[i].cluster >= 0)
        cl_sz[dr->funcs[i].cluster]++;
    for (int i = 0; i < dr->func_count; i++)
      if (dr->funcs[i].cluster >= 0 && cl_sz[dr->funcs[i].cluster] > 1)
        out.funcs_in_dup++;
    free(cl_sz);
    out.ratio = (double)out.funcs_in_dup / dr->func_count;
  }
  return out;
}

void dupes_free(DupeResult *dr) {
  for (int i = 0; i < dr->func_count; i++)
    free(dr->funcs[i].body);
  free(dr->funcs);
  free(dr->pairs);
  free(dr->parent);
  *dr = (DupeResult){};
}
