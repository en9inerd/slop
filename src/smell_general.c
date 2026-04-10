#include "smell_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Zombie parameters ───────────────────────────────────── */

static bool is_keyword_param(const char *w) {
  static const char *kws[] = {
      "self",     "cls",       "this",     "super",     "int",      "long",
      "short",    "double",    "float",    "char",      "bool",     "boolean",
      "string",   "number",    "void",     "byte",      "var",      "let",
      "const",    "auto",      "unsigned", "signed",    "static",   "final",
      "readonly", "public",    "private",  "protected", "abstract", "virtual",
      "override", "async",     "func",     "def",       "return",   "nil",
      "null",     "undefined", "true",     "false",     "error",    "err",
      "ctx",      "any",       "object",   "unknown",   "Request",  "Response",
      "Context",  "interface", "struct",   "class",     "enum",     nullptr};
  for (int i = 0; kws[i]; i++)
    if (strcmp(w, kws[i]) == 0)
      return true;
  return false;
}

static int extract_last_ident(const char *seg, int len, char *out, int outsz) {
  int best_s = -1, best_e = -1;
  for (int i = 0; i < len;) {
    if (isalpha((unsigned char)seg[i]) || seg[i] == '_') {
      int s = i;
      while (i < len && (isalnum((unsigned char)seg[i]) || seg[i] == '_'))
        i++;
      best_s = s;
      best_e = i;
    } else {
      i++;
    }
  }
  if (best_s < 0)
    return 0;
  int n = best_e - best_s;
  if (n >= outsz)
    n = outsz - 1;
  memcpy(out, seg + best_s, (size_t)n);
  out[n] = '\0';
  return n;
}

static int extract_params(const char *content, const FuncInfo *fi,
                          SpecificLang spec, char params[][64],
                          int max_params) {
  if (fi->body_offset == 0 || fi->start_line == 0)
    return 0;

  bool c_style = (spec == SPECIFIC_C || spec == SPECIFIC_CPP ||
                  spec == SPECIFIC_JAVA || spec == SPECIFIC_RUST ||
                  spec == SPECIFIC_SWIFT || spec == SPECIFIC_OTHER);

  size_t sig_end = fi->body_offset;
  size_t sig_start = sig_end > 512 ? sig_end - 512 : 0;
  const char *sig = content + sig_start;
  int sig_len = (int)(sig_end - sig_start);

  int last_open = -1, last_close = -1, depth = 0;
  for (int i = sig_len - 1; i >= 0; i--) {
    if (sig[i] == ')' && depth == 0) {
      last_close = i;
      depth = 1;
    } else if (sig[i] == ')')
      depth++;
    else if (sig[i] == '(') {
      depth--;
      if (depth == 0) {
        last_open = i;
        break;
      }
    }
  }
  if (last_open < 0 || last_close <= last_open)
    return 0;

  int count = 0;
  int seg_start = last_open + 1;
  depth = 0;
  for (int i = seg_start; i <= last_close; i++) {
    if (sig[i] == '(' || sig[i] == '<' || sig[i] == '[')
      depth++;
    else if (sig[i] == ')' || sig[i] == '>' || sig[i] == ']')
      depth--;

    if ((sig[i] == ',' && depth == 0) || i == last_close) {
      char name[64] = {};
      int nlen = 0;

      if (c_style) {
        nlen = extract_last_ident(sig + seg_start, i - seg_start, name,
                                  sizeof(name));
      } else {
        int j = seg_start;
        while (j < i && isspace((unsigned char)sig[j]))
          j++;
        while (j < i && sig[j] == '.')
          j++;
        if (j < i && (isalpha((unsigned char)sig[j]) || sig[j] == '_')) {
          int k = 0;
          while (j < i && k < 63 &&
                 (isalnum((unsigned char)sig[j]) || sig[j] == '_'))
            name[k++] = sig[j++];
          name[k] = '\0';
          nlen = k;
        }
      }

      if (nlen > 0) {
        if (spec == SPECIFIC_PYTHON &&
            (strcmp(name, "self") == 0 || strcmp(name, "cls") == 0)) {
        } else if (is_keyword_param(name)) {
        } else if (count < max_params) {
          memcpy(params[count], name, (size_t)nlen + 1);
          count++;
        }
      }
      seg_start = i + 1;
    }
  }
  return count;
}

void detect_zombie_params(SmellReport *r, const ScanResult *s) {
  for (int f = 0; f < s->function_count; f++) {
    const FuncInfo *fi = &s->functions[f];
    if (fi->body_len < 10)
      continue;
    if (fi->body_offset + fi->body_len > s->content_len)
      continue;

    char params[16][64];
    int pc = extract_params(s->content, fi, s->specific, params, 16);

    const char *body = s->content + fi->body_offset;
    size_t blen = fi->body_len;

    for (int p = 0; p < pc; p++) {
      int nlen = (int)strlen(params[p]);
      if (nlen < 2)
        continue;
      bool found = false;
      for (size_t i = 0; i + (size_t)nlen <= blen; i++) {
        if (memcmp(body + i, params[p], (size_t)nlen) != 0)
          continue;
        bool wb = (i == 0 || (!isalnum((unsigned char)body[i - 1]) &&
                              body[i - 1] != '_'));
        size_t end = i + (size_t)nlen;
        bool wa = (end >= blen ||
                   (!isalnum((unsigned char)body[end]) && body[end] != '_'));
        if (wb && wa) {
          found = true;
          break;
        }
      }
      if (!found) {
        add_finding(r, SMELL_ZOMBIE_PARAM, SEV_GENERAL, fi->start_line,
                    "parameter '%s' in %s() is never used", params[p],
                    fi->name[0] ? fi->name : "?");
      }
    }
  }
}

/* ── Unused imports ──────────────────────────────────────── */

void detect_unused_imports(SmellReport *r, const ScanResult *s) {
  ImportEntry imp[MAX_IMPORTS];
  int ic = 0;

  if (s->specific == SPECIFIC_TS || s->specific == SPECIFIC_JS)
    ic = collect_imports_ts(s, imp);
  else if (s->specific == SPECIFIC_GO)
    ic = collect_imports_go(s, imp);
  else if (s->specific == SPECIFIC_PYTHON)
    ic = collect_imports_py(s, imp);
  else
    return;

  for (int i = 0; i < ic; i++) {
    if (!name_found_in_range(s, imp[i].name, imp[i].line, imp[i].line)) {
      add_finding(r, SMELL_UNUSED_IMPORT, SEV_GENERAL, imp[i].line,
                  "imported name '%s' not used in file", imp[i].name);
    }
  }
}

/* ── Magic string repetition ─────────────────────────────── */

typedef struct {
  char text[128];
  int count;
  int first_line;
} StrHit;

void detect_magic_strings(SmellReport *r, const ScanResult *s) {
  if (!s->byte_kind)
    return;

  const char *p = s->content;
  size_t len = s->content_len;

  StrHit *hits = calloc(512, sizeof(StrHit));
  if (!hits)
    return;
  int nhits = 0;
  int line = 1;

  for (size_t i = 0; i < len; i++) {
    if (p[i] == '\n') {
      line++;
      continue;
    }
    if (s->byte_kind[i] != 2)
      continue;

    size_t start = i;
    while (i < len && s->byte_kind[i] == 2)
      i++;
    size_t slen = i - start;
    i--;

    if (slen < 10)
      continue;
    size_t cs = start + 1;
    size_t ce = start + slen - 1;
    if (ce <= cs)
      continue;
    int tlen = (int)(ce - cs);
    if (tlen < 8 || tlen > 127)
      continue;

    char buf[128];
    memcpy(buf, p + cs, (size_t)tlen);
    buf[tlen] = '\0';

    bool found = false;
    for (int h = 0; h < nhits; h++) {
      if (strcmp(hits[h].text, buf) == 0) {
        hits[h].count++;
        found = true;
        break;
      }
    }
    if (!found && nhits < 512) {
      memcpy(hits[nhits].text, buf, (size_t)tlen + 1);
      hits[nhits].count = 1;
      hits[nhits].first_line = line;
      nhits++;
    }
  }

  for (int h = 0; h < nhits; h++) {
    if (hits[h].count >= 3) {
      char trunc[52];
      snprintf(trunc, sizeof(trunc), "%.48s%s", hits[h].text,
               strlen(hits[h].text) > 48 ? "..." : "");
      add_finding(r, SMELL_MAGIC_STRING, SEV_GENERAL, hits[h].first_line,
                  "string \"%s\" repeated %d times", trunc, hits[h].count);
    }
  }
  free(hits);
}

/* ── Dead / unused code ──────────────────────────────────── */

bool is_entry_point(const char *name, SpecificLang spec) {
  if (strcmp(name, "main") == 0 || strcmp(name, "init") == 0)
    return true;
  if (strcmp(name, "constructor") == 0 || strcmp(name, "render") == 0)
    return true;
  if (strcmp(name, "setup") == 0 || strcmp(name, "teardown") == 0)
    return true;

  if (spec == SPECIFIC_TS || spec == SPECIFIC_JS) {
    static const char *const lifecycle[] = {
        "componentDidMount",       "componentDidUpdate",
        "componentWillUnmount",    "shouldComponentUpdate",
        "getDerivedStateFromProps", "getSnapshotBeforeUpdate",
        "ngOnInit",                "ngOnDestroy",
        "ngOnChanges",             "ngAfterViewInit",
        "ngAfterContentInit",      "ngDoCheck",
        "mounted",                 "created",
        "beforeMount",             "beforeDestroy",
        "updated",                 "unmounted",
        "getServerSideProps",      "getStaticProps",
        "getStaticPaths",          "getInitialProps",
        "loader",                  "action",
        nullptr,
    };
    for (int i = 0; lifecycle[i]; i++)
      if (strcmp(name, lifecycle[i]) == 0)
        return true;
  }

  if (spec == SPECIFIC_GO) {
    if (strncmp(name, "Test", 4) == 0 && name[4] &&
        isupper((unsigned char)name[4]))
      return true;
    if (strncmp(name, "Benchmark", 9) == 0)
      return true;
    if (strncmp(name, "Example", 7) == 0)
      return true;
    if (strncmp(name, "Fuzz", 4) == 0 && name[4] &&
        isupper((unsigned char)name[4]))
      return true;
    if (strcmp(name, "TestMain") == 0)
      return true;
    static const char *const gorm_hooks[] = {
        "BeforeCreate", "AfterCreate", "BeforeUpdate", "AfterUpdate",
        "BeforeSave",   "AfterSave",   "BeforeDelete", "AfterDelete",
        nullptr,
    };
    for (int i = 0; gorm_hooks[i]; i++)
      if (strcmp(name, gorm_hooks[i]) == 0)
        return true;
  }

  return false;
}

static bool line_has_keyword(const char *p, size_t start, size_t end,
                             const char *kw) {
  size_t kwlen = strlen(kw);
  for (size_t i = start; i + kwlen <= end; i++) {
    if (memcmp(p + i, kw, kwlen) != 0)
      continue;
    bool wb =
        (i == start || (!isalnum((unsigned char)p[i - 1]) && p[i - 1] != '_'));
    bool wa = (i + kwlen >= end ||
               (!isalnum((unsigned char)p[i + kwlen]) && p[i + kwlen] != '_'));
    if (wb && wa)
      return true;
  }
  return false;
}

static size_t line_trimmed_start(const char *p, size_t pos) {
  size_t s = pos;
  while (s > 0 && p[s - 1] != '\n')
    s--;
  size_t base = s;
  while (base < pos && (p[base] == ' ' || p[base] == '\t'))
    base++;
  return base;
}

static bool line_starts_with(const char *p, size_t start, size_t end,
                             const char *kw) {
  size_t klen = strlen(kw);
  if (start + klen > end)
    return false;
  return memcmp(p + start, kw, klen) == 0 &&
         (start + klen >= end || p[start + klen] == ' ' ||
          p[start + klen] == '\t');
}

bool func_is_exported(const ScanResult *s, const FuncInfo *fi) {
  if (s->specific == SPECIFIC_GO)
    return fi->name[0] && isupper((unsigned char)fi->name[0]);

  if (s->specific == SPECIFIC_TS || s->specific == SPECIFIC_JS) {
    const char *p = s->content;
    size_t cur = line_trimmed_start(p, fi->body_offset);
    if (line_starts_with(p, cur, fi->body_offset, "export"))
      return true;
    if (cur > 0) {
      size_t prev = line_trimmed_start(p, cur - 1);
      if (line_starts_with(p, prev, cur, "export"))
        return true;
      if (prev < cur && p[prev] == '@')
        return true;
    }
    return false;
  }

  if (s->specific == SPECIFIC_PYTHON)
    return fi->name[0] != '_';

  if (s->lang == LANG_C_LIKE) {
    const char *p = s->content;
    size_t line_end = fi->body_offset;

    size_t scan_start = line_end;
    int lines_back = 0;
    while (scan_start > 0 && lines_back < 5) {
      scan_start--;
      if (p[scan_start] == '\n')
        lines_back++;
    }

    bool is_static = line_has_keyword(p, scan_start, line_end, "static");

    if (is_static) {
      return line_has_keyword(p, scan_start, line_end, "__attribute__");
    }
    return true;
  }

  return false;
}

bool name_used_outside_func(const ScanResult *s, const FuncInfo *fi) {
  return name_found_in_range(s, fi->name, fi->start_line, fi->end_line);
}

void detect_dead_code(SmellReport *r, const ScanResult *s) {
  int total_dead_lines = 0;
  int dead_funcs = 0;

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

    if (!name_used_outside_func(s, fi)) {
      int lines = fi->end_line - fi->start_line + 1;
      total_dead_lines += lines;
      dead_funcs++;
      add_finding(r, SMELL_DEAD_CODE, SEV_GENERAL, fi->start_line,
                  "%s() defined but never referenced (%d lines)", fi->name,
                  lines);
    }
  }

  if (dead_funcs > 1) {
    add_finding(r, SMELL_DEAD_CODE, SEV_GENERAL, 0,
                "%d unused functions, %d dead lines total", dead_funcs,
                total_dead_lines);
  }
}
