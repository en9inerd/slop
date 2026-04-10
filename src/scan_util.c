#include "scan_util.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ── Narration markers ──────────────────────────────────── */

static const char *narration_markers[] = {
    "first we", "first,",  "now we", "now,",       "now let",  "next we",
    "next,",    "then we", "then,",  "finally we", "finally,", "here we",
    "here,",    "let's",   "let me", "we need to", "we can ",  "we'll",
    "we will",  "step 1",  "step 2", "step 3",     nullptr};

/* ── Keywords for filtering ─────────────────────────────── */

static const char *all_keywords[] = {
    "auto",      "break",    "case",       "catch",   "char",
    "class",     "const",    "continue",   "default", "do",
    "double",    "else",     "enum",       "extern",  "false",
    "float",     "for",      "goto",       "if",      "int",
    "long",      "new",      "null",       "private", "protected",
    "public",    "register", "return",     "short",   "signed",
    "sizeof",    "static",   "struct",     "super",   "switch",
    "this",      "throw",    "true",       "try",     "typedef",
    "typeof",    "union",    "unsigned",   "var",     "void",
    "volatile",  "while",    "async",      "await",   "delete",
    "export",    "extends",  "finally",    "from",    "function",
    "import",    "in",       "instanceof", "let",     "of",
    "undefined", "yield",    "chan",       "defer",   "fallthrough",
    "func",      "go",       "interface",  "map",     "package",
    "range",     "select",   "type",       "as",      "crate",
    "dyn",       "fn",       "impl",       "loop",    "match",
    "mod",       "move",     "mut",        "pub",     "ref",
    "self",      "trait",    "unsafe",     "use",     "where",
    "def",       "elif",     "except",     "global",  "lambda",
    "nonlocal",  "pass",     "raise",      "with",    nullptr};

/* ── Generic identifier dictionary ──────────────────────── */

static const char *generic_names[] = {
    "data",      "result",     "response", "handler",   "value",
    "item",      "config",     "options",  "params",    "callback",
    "output",    "input",      "temp",     "tmp",       "ret",
    "res",       "val",        "obj",      "arr",       "args",
    "opts",      "payload",    "instance", "container", "wrapper",
    "helper",    "util",       "manager",  "service",   "controller",
    "processor", "middleware", "utils",    "helpers",   "entries",
    "items",     "values",     "results",  "responses", nullptr};

bool is_generic_name(const char *word) {
  for (int i = 0; generic_names[i]; i++)
    if (strcmp(word, generic_names[i]) == 0)
      return true;
  return false;
}

/* ── TTR hash set ────────────────────────────────────────── */

static uint32_t fnv1a(const char *s, int len) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < len; i++)
    h = (h ^ (uint8_t)s[i]) * 16777619u;
  return h;
}

IdentSet ident_set_new(int cap) {
  IdentSet is = {.cap = cap, .count = 0};
  is.slots = calloc((size_t)cap, sizeof(char *));
  return is;
}

void ident_set_insert(IdentSet *is, const char *word, int wlen) {
  uint32_t h = fnv1a(word, wlen);
  for (int probe = 0; probe < is->cap; probe++) {
    int idx = (int)((h + (uint32_t)probe) % (uint32_t)is->cap);
    if (!is->slots[idx]) {
      char *copy = malloc((size_t)wlen + 1);
      if (!copy)
        return;
      memcpy(copy, word, (size_t)wlen);
      copy[wlen] = '\0';
      is->slots[idx] = copy;
      is->count++;
      return;
    }
    if (strncmp(is->slots[idx], word, (size_t)wlen) == 0 &&
        is->slots[idx][wlen] == '\0')
      return;
  }
}

void ident_set_free(IdentSet *is) {
  for (int i = 0; i < is->cap; i++)
    free(is->slots[i]);
  free(is->slots);
}

/* ── Helpers ────────────────────────────────────────────── */

bool starts_with_ci(const char *s, int slen, const char *prefix) {
  int plen = (int)strlen(prefix);
  if (slen < plen)
    return false;
  for (int i = 0; i < plen; i++) {
    if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
      return false;
  }
  return true;
}

bool is_word_char(char c) {
  return isalnum((unsigned char)c) || c == '_';
}

int extract_word(const char *s, const char *end, char *buf, int bufsz) {
  int i = 0;
  while (s + i < end && i < bufsz - 1 && is_word_char(s[i])) {
    buf[i] = s[i];
    i++;
  }
  buf[i] = '\0';
  return i;
}

bool is_keyword(const char *word) {
  for (int i = 0; all_keywords[i]; i++)
    if (strcmp(word, all_keywords[i]) == 0)
      return true;
  return false;
}

bool is_camel_case(const char *s, int len) {
  if (len <= 2 || !islower((unsigned char)s[0]))
    return false;
  for (int i = 1; i < len; i++)
    if (isupper((unsigned char)s[i]))
      return true;
  return false;
}

bool is_snake_case(const char *s, int len) {
  if (len <= 2)
    return false;
  bool has_us = false;
  for (int i = 0; i < len; i++) {
    if (s[i] == '_')
      has_us = true;
    else if (!islower((unsigned char)s[i]) && !isdigit((unsigned char)s[i]))
      return false;
  }
  return has_us;
}

bool is_all_upper(const char *s, int len) {
  for (int i = 0; i < len; i++)
    if (islower((unsigned char)s[i]))
      return false;
  return true;
}

bool line_has_paren(const char *line, int len) {
  for (int i = 0; i < len; i++)
    if (line[i] == '(')
      return true;
  return false;
}

bool line_has_word(const char *line, int len, const char *word) {
  int wl = (int)strlen(word);
  for (int i = 0; i + wl <= len; i++) {
    if (memcmp(line + i, word, (size_t)wl) != 0)
      continue;
    bool wb = (i == 0 || !is_word_char(line[i - 1]));
    bool wa = (i + wl >= len || !is_word_char(line[i + wl]));
    if (wb && wa)
      return true;
  }
  return false;
}

bool line_starts_control(const char *line, int len) {
  int i = 0;
  while (i < len && isspace((unsigned char)line[i]))
    i++;
  const char *kw[] = {"if",    "else", "for",     "while", "switch",
                      "do",    "try",  "catch",   "finally",
                      "defer", "case", "default", nullptr};
  for (int k = 0; kw[k]; k++) {
    int kl = (int)strlen(kw[k]);
    if (i + kl <= len && memcmp(line + i, kw[k], (size_t)kl) == 0 &&
        (i + kl >= len || !is_word_char(line[i + kl])))
      return true;
  }
  return false;
}

bool line_is_type_body(const char *line, int len) {
  static const char *kw[] = {"struct", "enum", "union", "interface",
                             "typedef", nullptr};
  for (int k = 0; kw[k]; k++)
    if (line_has_word(line, len, kw[k]))
      return true;
  return false;
}

/* ── Function name extraction ────────────────────────────── */

void extract_func_name(const char *line, int len, char *out, int outsz) {
  int best_start = -1, best_end = -1;
  for (int p = 0; p < len; p++) {
    if (line[p] != '(')
      continue;
    int e = p - 1;
    while (e >= 0 && isspace((unsigned char)line[e]))
      e--;
    if (e < 0 || !is_word_char(line[e]))
      continue;
    int s = e;
    while (s > 0 && is_word_char(line[s - 1]))
      s--;
    char tmp[128];
    int tl = e - s + 1;
    if (tl > 0 && tl < (int)sizeof(tmp)) {
      memcpy(tmp, line + s, (size_t)tl);
      tmp[tl] = '\0';
      if (strcmp(tmp, "func") == 0 || strcmp(tmp, "if") == 0 ||
          strcmp(tmp, "for") == 0 || strcmp(tmp, "while") == 0 ||
          strcmp(tmp, "switch") == 0)
        continue;
    }
    best_start = s;
    best_end = e;
  }
  if (best_start < 0) {
    out[0] = '\0';
    return;
  }

  int n = best_end - best_start + 1;
  if (n >= outsz)
    n = outsz - 1;
  memcpy(out, line + best_start, (size_t)n);
  out[n] = '\0';
}

bool py_is_def_line(const char *line, int len, int *out_indent) {
  int i = 0;
  while (i < len && line[i] == ' ')
    i++;
  if (i + 4 <= len && memcmp(line + i, "def ", 4) == 0) {
    *out_indent = i;
    return true;
  }
  return false;
}

void py_extract_func_name(const char *line, int len, char *out, int outsz) {
  int i = 0;
  while (i < len && line[i] == ' ')
    i++;
  if (i + 4 <= len && memcmp(line + i, "def ", 4) == 0)
    i += 4;
  else {
    out[0] = '\0';
    return;
  }
  int s = i;
  while (i < len && is_word_char(line[i]))
    i++;
  int n = i - s;
  if (n <= 0) {
    out[0] = '\0';
    return;
  }
  if (n >= outsz)
    n = outsz - 1;
  memcpy(out, line + s, (size_t)n);
  out[n] = '\0';
}

void extract_arrow_name(const char *line, int len, char *out, int outsz) {
  const char *kws[] = {"const ", "let ", "var ", nullptr};
  for (int k = 0; kws[k]; k++) {
    int kwlen = (int)strlen(kws[k]);
    for (int p = 0; p + kwlen < len; p++) {
      if (p > 0 && is_word_char(line[p - 1]))
        continue;
      if (memcmp(line + p, kws[k], (size_t)kwlen) != 0)
        continue;
      int s = p + kwlen;
      if (s >= len || !isalpha((unsigned char)line[s]))
        continue;
      int e = s;
      while (e < len && is_word_char(line[e]))
        e++;
      int n = e - s;
      if (n > 0 && n < outsz) {
        memcpy(out, line + s, (size_t)n);
        out[n] = '\0';
        return;
      }
    }
  }
  out[0] = '\0';
}

void extract_field_name(const char *line, int len, char *out, int outsz) {
  int i = 0;
  while (i < len && isspace((unsigned char)line[i]))
    i++;
  if (i >= len || !isalpha((unsigned char)line[i])) {
    out[0] = '\0';
    return;
  }
  int s = i;
  while (i < len && is_word_char(line[i]))
    i++;
  int nlen = i - s;
  while (i < len && isspace((unsigned char)line[i]))
    i++;
  if (i >= len || line[i] != '=' || (i + 1 < len && line[i + 1] == '=')) {
    out[0] = '\0';
    return;
  }
  bool has_callable = false;
  for (int j = i + 1; j < len && !has_callable; j++) {
    if (line[j] == '(' || (j + 1 < len && line[j] == '=' && line[j + 1] == '>'))
      has_callable = true;
    if (j + 8 <= len && memcmp(line + j, "function", 8) == 0)
      has_callable = true;
  }
  if (!has_callable || nlen <= 0 || nlen >= outsz) {
    out[0] = '\0';
    return;
  }
  memcpy(out, line + s, (size_t)nlen);
  out[nlen] = '\0';
}

bool scan_back_has_paren(const char *content, size_t from, int max_lines,
                         const char **out_line, int *out_len) {
  int newlines = 0;
  size_t pos = from;
  while (pos > 0 && newlines < max_lines) {
    pos--;
    if (content[pos] == '\n') {
      newlines++;
      continue;
    }
    if (content[pos] == ';' || content[pos] == '}')
      return false;
    if (content[pos] == '(') {
      size_t ls = pos;
      while (ls > 0 && content[ls - 1] != '\n')
        ls--;
      size_t le = pos;
      while (content[le] && content[le] != '\n')
        le++;
      *out_line = content + ls;
      *out_len = (int)(le - ls);
      return true;
    }
  }
  return false;
}

bool sh_is_func_line(const char *line, int len) {
  int i = 0;
  while (i < len && isspace((unsigned char)line[i]))
    i++;
  if (i + 9 <= len && memcmp(line + i, "function ", 9) == 0)
    return true;
  for (int j = i; j < len - 1; j++) {
    if (line[j] == '(' && line[j + 1] == ')')
      return true;
  }
  return false;
}

/* ── Byte-kind map ───────────────────────────────────────── */

void compute_byte_kind(uint8_t *bk, const char *content, size_t len,
                       LangFamily lang) {
  enum { S_CODE, S_LINE_CMT, S_BLOCK_CMT, S_STRING } state = S_CODE;
  char sq = 0;
  char bq = 0;

  for (size_t i = 0; i < len; i++) {
    char c = content[i];

    switch (state) {
    case S_CODE:
      bk[i] = 0;
      if (lang == LANG_C_LIKE) {
        if (c == '/' && i + 1 < len && content[i + 1] == '/') {
          bk[i] = 1;
          bk[++i] = 1;
          state = S_LINE_CMT;
        } else if (c == '/' && i + 1 < len && content[i + 1] == '*') {
          bk[i] = 3;
          bk[++i] = 3;
          state = S_BLOCK_CMT;
        } else if (c == '"' || c == '\'' || c == '`') {
          bk[i] = 2;
          sq = c;
          state = S_STRING;
        }
      } else if (lang == LANG_PYTHON) {
        if (c == '#') {
          bk[i] = 1;
          state = S_LINE_CMT;
        } else if ((c == '"' || c == '\'') && i + 2 < len &&
                   content[i + 1] == c && content[i + 2] == c) {
          bk[i] = 3;
          bk[++i] = 3;
          bk[++i] = 3;
          bq = c;
          state = S_BLOCK_CMT;
        } else if (c == '"' || c == '\'') {
          bk[i] = 2;
          sq = c;
          state = S_STRING;
        }
      } else if (lang == LANG_SHELL) {
        if (c == '#') {
          bk[i] = 1;
          state = S_LINE_CMT;
        } else if (c == '"' || c == '\'') {
          bk[i] = 2;
          sq = c;
          state = S_STRING;
        }
      }
      break;

    case S_LINE_CMT:
      bk[i] = 1;
      if (c == '\n')
        state = S_CODE;
      break;

    case S_BLOCK_CMT:
      bk[i] = 3;
      if (lang == LANG_C_LIKE && c == '*' && i + 1 < len &&
          content[i + 1] == '/') {
        bk[++i] = 3;
        state = S_CODE;
      } else if (lang == LANG_PYTHON && c == bq && i + 2 < len &&
                 content[i + 1] == bq && content[i + 2] == bq) {
        bk[++i] = 3;
        bk[++i] = 3;
        state = S_CODE;
      }
      break;

    case S_STRING:
      bk[i] = 2;
      if (c == '\\' && i + 1 < len) {
        bk[++i] = 2;
      } else if (c == sq) {
        state = S_CODE;
      } else if (c == '\n' && sq != '`') {
        state = S_CODE;
      }
      break;
    }
  }
}

/* ── Post-pass finalization ──────────────────────────────── */

void scan_finalize(ScanResult *res, const char *content, size_t len,
                   int max_lines, const uint8_t *line_is_cmnt,
                   const int *defect_buf, int defect_n) {
  int real_newlines = 0;
  for (size_t idx = 0; idx < len; idx++)
    if (content[idx] == '\n')
      real_newlines++;
  if (real_newlines > 0 && content[len - 1] == '\n')
    res->total_lines = real_newlines;
  else
    res->total_lines = real_newlines + 1;

  if (res->line_length_count > res->total_lines)
    res->line_length_count = res->total_lines;

  const int body_lines = res->total_lines - res->code_body_start + 1;
  if (body_lines >= MIN_LINES_GRADIENT) {
    const int half = body_lines / 2;
    const int mid = res->code_body_start + half;
    for (int l = res->code_body_start; l <= res->total_lines && l <= max_lines;
         l++) {
      if (l < mid) {
        res->body_lines_top++;
        if (line_is_cmnt[l])
          res->body_comment_top++;
      } else {
        res->body_lines_bottom++;
        if (line_is_cmnt[l])
          res->body_comment_bottom++;
      }
    }
  }

  res->total_defects = defect_n;
  if (res->total_lines >= 4 && defect_n > 0) {
    int q_sz = res->total_lines / 4;
    if (q_sz < 1)
      q_sz = 1;
    for (int d = 0; d < defect_n; d++) {
      int q = (defect_buf[d] - 1) / q_sz;
      if (q >= 4)
        q = 3;
      res->defects_per_quartile[q]++;
    }
  }
}

/* ── Narration / defect checks ───────────────────────────── */

void check_narration_comment(ScanResult *res, const char *cmnt_buf,
                             int cmnt_len, int line_num, const char *content,
                             size_t line_start, int ll) {
  int ci = 0;
  while (ci < cmnt_len && isspace((unsigned char)cmnt_buf[ci]))
    ci++;
  int tl = cmnt_len - ci;
  for (int m = 0; narration_markers[m]; m++) {
    if (starts_with_ci(cmnt_buf + ci, tl, narration_markers[m])) {
      if (res->narration_count < MAX_NARRATION) {
        NarrationHit *h = &res->narration[res->narration_count++];
        h->line = line_num;
        int cp = ll;
        if (cp >= (int)sizeof(h->text))
          cp = (int)sizeof(h->text) - 1;
        memcpy(h->text, content + line_start, (size_t)cp);
        h->text[cp] = '\0';
      }
      return;
    }
  }
}

void check_defect_markers(const char *cmnt_buf, int cmnt_len, int *defect_buf,
                          int *defect_n, int max_lines, int line_num) {
  for (int j = 0; j < cmnt_len - 3; j++) {
    if (starts_with_ci(cmnt_buf + j, cmnt_len - j, "todo") ||
        starts_with_ci(cmnt_buf + j, cmnt_len - j, "fixme")) {
      if (*defect_n < max_lines)
        defect_buf[(*defect_n)++] = line_num;
      return;
    }
  }
}
