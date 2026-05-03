#include "smell_internal.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((format(printf, 5, 6)))
void add_finding(SmellReport *r, SmellKind kind, SmellSeverity sev, int line,
                 const char *fmt, ...) {
  if (r->count >= MAX_FINDINGS)
    return;
  SmellFinding *f = &r->items[r->count++];
  f->kind = kind;
  f->severity = sev;
  f->line = line;

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(f->message, sizeof(f->message), fmt, ap);
  va_end(ap);
}

/* ── Smell 1: narration comments [DIAGNOSTIC] ──────────── */

static void detect_narration(SmellReport *r, const ScanResult *s) {
  for (int i = 0; i < s->narration_count; i++) {
    const NarrationHit *h = &s->narration[i];
    char trimmed[256];
    snprintf(trimmed, sizeof(trimmed), "%s", h->text);
    int tl = (int)strlen(trimmed);
    while (tl > 0 && (trimmed[tl - 1] == ' ' || trimmed[tl - 1] == '\t' ||
                      trimmed[tl - 1] == '\r'))
      trimmed[--tl] = '\0';
    add_finding(r, SMELL_NARRATION, SEV_DIAGNOSTIC, h->line, "\"%s\"", trimmed);
  }
}

/* ── Smell 2: comment density gradient [DIAGNOSTIC] ────── */

static void detect_comment_gradient(SmellReport *r, const ScanResult *s) {
  if (!has_comment_gradient(s->body_lines_top, s->body_comment_top,
                            s->body_lines_bottom, s->body_comment_bottom))
    return;
  double d_top = s->body_comment_top / (double)s->body_lines_top;
  double d_bot = s->body_comment_bottom / (double)s->body_lines_bottom;
  if (d_bot < 0.001)
    d_bot = 0.001;
  int pct_top = (int)(d_top * 100 + 0.5);
  int pct_bot = (int)(d_bot * 100 + 0.5);
  add_finding(r, SMELL_COMMENT_GRADIENT, SEV_DIAGNOSTIC, 0,
              "top half %d%% comments, bottom half %d%% (gradient %.1fx)",
              pct_top, pct_bot, d_top / d_bot);
}

/* ── Smell 3: within-file redundant re-implementation [CORRELATED] ── */

static void detect_redup(SmellReport *r, const ScanResult *s) {
  if (s->function_count < 2)
    return;

  for (int i = 0; i < s->function_count; i++) {
    const FuncInfo *fi = &s->functions[i];
    if (fi->body_len < NCD_MIN_BODY_BYTES)
      continue;
    if (fi->body_offset + fi->body_len > s->content_len)
      continue;

    for (int j = i + 1; j < s->function_count; j++) {
      const FuncInfo *fj = &s->functions[j];
      if (fj->body_len < NCD_MIN_BODY_BYTES)
        continue;
      if (fj->body_offset + fj->body_len > s->content_len)
        continue;

      size_t mx = fi->body_len > fj->body_len ? fi->body_len : fj->body_len;
      size_t mn = fi->body_len < fj->body_len ? fi->body_len : fj->body_len;
      if (mn == 0 || (double)mx / (double)mn > NCD_SIZE_RATIO_MAX)
        continue;

      double ncd = compress_ncd(s->content + fi->body_offset, fi->body_len,
                                s->content + fj->body_offset, fj->body_len);
      if (ncd < NCD_THRESHOLD) {
        add_finding(r, SMELL_REDUP, SEV_CORRELATED, fi->start_line,
                    "%s() and %s() are near-duplicates (NCD %.2f)",
                    fi->name[0] ? fi->name : "?", fj->name[0] ? fj->name : "?",
                    ncd);
      }
    }
  }
}

/* ── Smell 4: defensive over-wrapping [CORRELATED] ─────── */

typedef struct {
  char var[64];
  int line;
} NullCheck;

static const char *null_kws[] = {"null", "undefined", "nil", "None", nullptr};

static int find_null_keyword(const ScanResult *s, const char *ln, int ll,
                             size_t line_start, int search_from, int *out_nkl) {
  for (int nk = 0; null_kws[nk]; nk++) {
    int nkl = (int)strlen(null_kws[nk]);
    for (int pos = search_from; pos + nkl <= ll; pos++) {
      if (s->byte_kind[line_start + (size_t)pos] != 0)
        continue;
      if (memcmp(ln + pos, null_kws[nk], (size_t)nkl) != 0)
        continue;
      bool wb = (pos == 0 ||
                 (!isalnum((unsigned char)ln[pos - 1]) && ln[pos - 1] != '_'));
      bool wa = (pos + nkl >= ll || (!isalnum((unsigned char)ln[pos + nkl]) &&
                                     ln[pos + nkl] != '_'));
      if (wb && wa) {
        *out_nkl = nkl;
        return pos;
      }
    }
  }
  return -1;
}

static bool extract_checked_var(const char *ln, int if_start, int null_pos,
                                char *var, int varsz) {
  int op = null_pos - 1;
  while (op > if_start &&
         (ln[op] == ' ' || ln[op] == '=' || ln[op] == '!'))
    op--;
  for (;;) {
    if (op >= if_start + 2 && ln[op] == 't' && ln[op - 1] == 'o' &&
        ln[op - 2] == 'n' && (op - 3 < if_start || ln[op - 3] == ' ')) {
      op -= 3;
      while (op > if_start && ln[op] == ' ')
        op--;
    } else if (op >= if_start + 1 && ln[op] == 's' && ln[op - 1] == 'i' &&
               (op - 2 < if_start || ln[op - 2] == ' ')) {
      op -= 2;
      while (op > if_start && ln[op] == ' ')
        op--;
    } else
      break;
  }
  if (op <= if_start)
    return false;
  int ve = op + 1;
  int vs = ve;
  while (vs > if_start && (isalnum((unsigned char)ln[vs - 1]) ||
                           ln[vs - 1] == '_' || ln[vs - 1] == '.'))
    vs--;
  int vlen = ve - vs;
  if (vlen < 1 || vlen >= varsz)
    return false;
  memcpy(var, ln + vs, (size_t)vlen);
  var[vlen] = '\0';
  return true;
}

static void record_null_check(NullCheck *recent, int *rc, const char *var,
                              int line) {
  if (*rc < NULL_CHECK_RING_CAP) {
    snprintf(recent[*rc].var, sizeof(recent[*rc].var), "%s", var);
    recent[*rc].line = line;
    (*rc)++;
  } else {
    memmove(recent, recent + 1,
            (NULL_CHECK_RING_CAP - 1) * sizeof(NullCheck));
    snprintf(recent[NULL_CHECK_RING_CAP - 1].var, sizeof(recent[0].var), "%s",
             var);
    recent[NULL_CHECK_RING_CAP - 1].line = line;
  }
}

static void check_null_line(SmellReport *r, const ScanResult *s, const char *ln,
                            int ll, int line, size_t line_start,
                            NullCheck *recent, int *rc) {
  int j = 0;
  while (j < ll && (ln[j] == ' ' || ln[j] == '\t'))
    j++;
  if (j + 2 >= ll || ln[j] != 'i' || ln[j + 1] != 'f')
    return;
  if (ln[j + 2] != ' ' && ln[j + 2] != '(')
    return;

  int nkl = 0;
  int pos = find_null_keyword(s, ln, ll, line_start, j + 2, &nkl);
  if (pos < 0)
    return;

  char var[64];
  if (!extract_checked_var(ln, j, pos, var, sizeof(var)))
    return;

  for (int k = 0; k < *rc; k++) {
    if (strcmp(recent[k].var, var) == 0 &&
        line - recent[k].line <= NULL_CHECK_WINDOW &&
        line != recent[k].line) {
      add_finding(r, SMELL_OVERWRAP, SEV_CORRELATED, line,
                  "redundant null check on '%s' (already checked at line %d)",
                  var, recent[k].line);
      return;
    }
  }
  record_null_check(recent, rc, var, line);
}

static void detect_overwrap(SmellReport *r, const ScanResult *s) {
  for (int i = 0; i < s->overwrap_count; i++) {
    const OverwrapHit *h = &s->overwraps[i];
    add_finding(r, SMELL_OVERWRAP, SEV_CORRELATED, h->line,
                "try/catch nested %d deep in %s()", h->depth,
                h->func_name[0] ? h->func_name : "?");
  }

  NullCheck recent[NULL_CHECK_RING_CAP];
  int rc = 0;

  const char *p = s->content;
  size_t len = s->content_len;
  int line = 1;
  size_t line_start = 0;

  for (size_t i = 0; i <= len; i++) {
    if (i < len && p[i] != '\n')
      continue;
    int ll = (int)(i - line_start);
    const char *ln = p + line_start;

    if (ll > 3 && s->byte_kind)
      check_null_line(r, s, ln, ll, line, line_start, recent, &rc);

    line++;
    line_start = i + 1;
  }
}

/* ── Smell 5: naming convention break [CORRELATED] ─────── */

static void detect_name_break(SmellReport *r, const ScanResult *s) {
  if (s->specific == SPECIFIC_GO)
    return;
  for (int i = 0; i < s->name_break_count; i++) {
    const NameBreakHit *h = &s->name_breaks[i];
    add_finding(r, SMELL_NAME_BREAK, SEV_CORRELATED, h->line,
                "mixed naming in %s(): %d%% camelCase, %d%% snake_case",
                h->func_name[0] ? h->func_name : "?", h->camel_pct,
                h->snake_pct);
  }
}

/* ── TS: "as any" casts [CORRELATED] ───────────────────── */

static void detect_as_any(SmellReport *r, const ScanResult *s) {
  if (s->specific != SPECIFIC_TS)
    return;

  const char *p = s->content;
  size_t len = s->content_len;
  int line = 1, count = 0, first_line = 0;

  for (size_t i = 0; i + 6 < len; i++) {
    if (p[i] == '\n') {
      line++;
      continue;
    }
    if (s->byte_kind && s->byte_kind[i] != 0)
      continue;
    if (p[i] == ' ' && i + 6 < len && memcmp(p + i, " as any", 7) == 0 &&
        (i + 7 >= len || !isalnum((unsigned char)p[i + 7]))) {
      count++;
      if (count == 1)
        first_line = line;
      if (count <= 3)
        add_finding(r, SMELL_AS_ANY, SEV_CORRELATED, line,
                    "\"as any\" type cast - loses type safety");
    }
  }
  if (count > 3)
    add_finding(r, SMELL_AS_ANY, SEV_CORRELATED, first_line,
                "%d total \"as any\" casts in file", count);
}

/* ── TS: @ts-ignore / @ts-nocheck directives [CORRELATED] ─ */

static void detect_ts_directives(SmellReport *r, const ScanResult *s) {
  if (s->specific != SPECIFIC_TS && s->specific != SPECIFIC_JS)
    return;
  if (!s->byte_kind)
    return;

  const char *p = s->content;
  size_t len = s->content_len;
  int line = 1;

  for (size_t i = 0; i + 10 < len; i++) {
    if (p[i] == '\n') {
      line++;
      continue;
    }
    if (p[i] != '@')
      continue;
    if (s->byte_kind[i] != 1)
      continue;

    if (memcmp(p + i, "@ts-ignore", 10) == 0) {
      add_finding(r, SMELL_TS_DIRECTIVE, SEV_CORRELATED, line,
                  "@ts-ignore - suppresses type checking");
    } else if (i + 11 < len && memcmp(p + i, "@ts-nocheck", 11) == 0) {
      add_finding(r, SMELL_TS_DIRECTIVE, SEV_CORRELATED, line,
                  "@ts-nocheck - disables type checking for entire file");
    } else if (i + 16 <= len && memcmp(p + i, "@ts-expect-error", 16) == 0) {
      add_finding(r, SMELL_TS_DIRECTIVE, SEV_CORRELATED, line,
                  "@ts-expect-error - suppresses type error");
    }
  }
}

/* ── Python: type: ignore / noqa directives [CORRELATED] ── */

static void detect_py_directives(SmellReport *r, const ScanResult *s) {
  if (s->specific != SPECIFIC_PYTHON)
    return;
  if (!s->byte_kind)
    return;

  const char *p = s->content;
  size_t len = s->content_len;
  int line = 1, count = 0, first_line = 0;

  for (size_t i = 0; i + 4 < len; i++) {
    if (p[i] == '\n') {
      line++;
      continue;
    }
    if (s->byte_kind[i] != 1)
      continue;

    if (i + 14 <= len && memcmp(p + i, "type: ignore", 12) == 0) {
      count++;
      if (count == 1)
        first_line = line;
      if (count <= 3)
        add_finding(r, SMELL_TS_DIRECTIVE, SEV_CORRELATED, line,
                    "type: ignore - suppresses type checking");
    } else if (i + 4 <= len && memcmp(p + i, "noqa", 4) == 0 &&
               (i + 4 >= len || !isalnum((unsigned char)p[i + 4]))) {
      count++;
      if (count == 1)
        first_line = line;
      if (count <= 3)
        add_finding(r, SMELL_TS_DIRECTIVE, SEV_CORRELATED, line,
                    "noqa - suppresses linter warning");
    }
  }
  if (count > 3)
    add_finding(r, SMELL_TS_DIRECTIVE, SEV_CORRELATED, first_line,
                "%d total type suppression directives in file", count);
}

/* ── Go: error suppression with _ [CORRELATED] ─────────── */

static void detect_go_err_suppress(SmellReport *r, const ScanResult *s) {
  if (s->specific != SPECIFIC_GO)
    return;

  const char *p = s->content;
  size_t len = s->content_len;
  int line = 1, count = 0, first_line = 0;

  for (size_t i = 0; i + 3 < len; i++) {
    if (p[i] == '\n') {
      line++;
      continue;
    }
    if (s->byte_kind && s->byte_kind[i] != 0)
      continue;
    if (p[i] == '_' && i + 2 < len) {
      bool preceded_ok = (i == 0 || p[i - 1] == ' ' || p[i - 1] == '\t' ||
                          p[i - 1] == ',' || p[i - 1] == '\n');
      bool followed_by_assign = (p[i + 1] == ' ' && p[i + 2] == '=') ||
                                (p[i + 1] == ' ' && i + 3 < len &&
                                 p[i + 2] == ':' && p[i + 3] == '=');

      if (preceded_ok && followed_by_assign) {
        bool is_range = false;
        for (size_t j = i; j < len && j < i + 80 && p[j] != '\n'; j++) {
          if (j + 5 < len && memcmp(p + j, "range", 5) == 0) {
            is_range = true;
            break;
          }
        }
        if (!is_range) {
          bool is_multi_return = false;
          {
            size_t b = i;
            while (b > 0 && (p[b - 1] == ' ' || p[b - 1] == '\t'))
              b--;
            if (b > 0 && p[b - 1] == ',')
              is_multi_return = true;
          }
          bool is_err = false;
          if (!is_multi_return) {
            size_t eq_pos = i + 1;
            while (eq_pos < len && p[eq_pos] != '=' && p[eq_pos] != '\n')
              eq_pos++;
            if (eq_pos < len && p[eq_pos] == '=') {
              size_t rhs = eq_pos + 1;
              while (rhs < len && (p[rhs] == ' ' || p[rhs] == ':'))
                rhs++;
              for (size_t k = rhs; k + 2 < len && p[k] != '\n'; k++) {
                if ((p[k] == 'e' || p[k] == 'E') &&
                    (p[k + 1] == 'r' || p[k + 1] == 'R') &&
                    (p[k + 2] == 'r' || p[k + 2] == 'R')) {
                  is_err = true;
                  break;
                }
              }
            }
          }
          if (is_multi_return || is_err) {
            count++;
            if (count == 1)
              first_line = line;
            if (count <= 3)
              add_finding(r, SMELL_ERR_SUPPRESS, SEV_CORRELATED, line,
                          "error return value discarded with _");
          }
        }
      }
    }
  }
  if (count > 3)
    add_finding(r, SMELL_ERR_SUPPRESS, SEV_CORRELATED, first_line,
                "%d total suppressed error returns in file", count);
}

/* ── Public API ─────────────────────────────────────────── */

int smell_count_dead_lines(const ScanResult *s) {
  int total = 0;
  for (int f = 0; f < s->function_count; f++) {
    const FuncInfo *fi = &s->functions[f];
    if (fi->name[0] == '\0')
      continue;
    if (fi->end_line <= fi->start_line)
      continue;
    if (is_entry_point(fi->name, s->specific))
      continue;
    if (func_is_exported(s, fi))
      continue;
    if (!name_used_outside_func(s, fi))
      total += fi->end_line - fi->start_line + 1;
  }
  return total;
}

void smell_detect(SmellReport *report, const ScanResult *scan,
                  bool include_general) {
  *report = (SmellReport){};

  detect_narration(report, scan);
  detect_comment_gradient(report, scan);
  detect_redup(report, scan);
  detect_overwrap(report, scan);
  detect_name_break(report, scan);

  detect_as_any(report, scan);
  detect_ts_directives(report, scan);
  detect_py_directives(report, scan);
  detect_go_err_suppress(report, scan);

  if (include_general) {
    detect_zombie_params(report, scan);
    detect_unused_imports(report, scan);
    detect_magic_strings(report, scan);
    detect_dead_code(report, scan);
  }
}
