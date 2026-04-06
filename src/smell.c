#include "slop.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

__attribute__((format(printf, 5, 6))) static void
add_finding(SmellReport *r, SmellKind kind, SmellSeverity sev, int line, const char *fmt, ...) {
    if (r->count >= MAX_FINDINGS) return;
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
        while (tl > 0 &&
               (trimmed[tl - 1] == ' ' || trimmed[tl - 1] == '\t' || trimmed[tl - 1] == '\r'))
            trimmed[--tl] = '\0';
        add_finding(r, SMELL_NARRATION, SEV_DIAGNOSTIC, h->line, "\"%s\"", trimmed);
    }
}

/* ── Smell 2: comment density gradient [DIAGNOSTIC] ────── */

static void detect_comment_gradient(SmellReport *r, const ScanResult *s) {
    if (s->body_lines_top < MIN_BODY_LINES || s->body_lines_bottom < MIN_BODY_LINES) return;
    int total_body_comments = s->body_comment_top + s->body_comment_bottom;
    if (total_body_comments < MIN_GRADIENT_CMTS) return;

    double d_top = (double)s->body_comment_top / (double)s->body_lines_top;
    double d_bot = (double)s->body_comment_bottom / (double)s->body_lines_bottom;
    if (d_bot < 0.001) d_bot = 0.001;

    double gradient = d_top / d_bot;
    if (gradient > GRADIENT_THRESHOLD) {
        int pct_top = (int)(d_top * 100 + 0.5);
        int pct_bot = (int)(d_bot * 100 + 0.5);
        add_finding(r, SMELL_COMMENT_GRADIENT, SEV_DIAGNOSTIC, 0,
                    "top half %d%% comments, bottom half %d%% (gradient %.1fx)", pct_top, pct_bot,
                    gradient);
    }
}

/* ── Smell 3: within-file redundant re-implementation [CORRELATED] ── */

static void detect_redup(SmellReport *r, const ScanResult *s) {
    if (s->function_count < 2) return;

    for (int i = 0; i < s->function_count; i++) {
        const FuncInfo *fi = &s->functions[i];
        if (fi->body_len < NCD_MIN_BODY_BYTES) continue;
        if (fi->body_offset + fi->body_len > s->content_len) continue;

        for (int j = i + 1; j < s->function_count; j++) {
            const FuncInfo *fj = &s->functions[j];
            if (fj->body_len < NCD_MIN_BODY_BYTES) continue;
            if (fj->body_offset + fj->body_len > s->content_len) continue;

            size_t mx = fi->body_len > fj->body_len ? fi->body_len : fj->body_len;
            size_t mn = fi->body_len < fj->body_len ? fi->body_len : fj->body_len;
            if (mn == 0 || (double)mx / (double)mn > NCD_SIZE_RATIO_MAX) continue;

            double ncd = compress_ncd(s->content + fi->body_offset, fi->body_len,
                                      s->content + fj->body_offset, fj->body_len);
            if (ncd < NCD_THRESHOLD) {
                add_finding(r, SMELL_REDUP, SEV_CORRELATED, fi->start_line,
                            "%s() and %s() are near-duplicates (NCD %.2f)",
                            fi->name[0] ? fi->name : "?", fj->name[0] ? fj->name : "?", ncd);
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

static void check_null_line(SmellReport *r, const ScanResult *s, const char *ln, int ll, int line,
                            size_t line_start, NullCheck *recent, int *rc) {
    int j = 0;
    while (j < ll && (ln[j] == ' ' || ln[j] == '\t')) j++;
    if (j + 2 >= ll || ln[j] != 'i' || ln[j + 1] != 'f') return;
    if (ln[j + 2] != ' ' && ln[j + 2] != '(') return;

    for (int nk = 0; null_kws[nk]; nk++) {
        int nkl = (int)strlen(null_kws[nk]);
        for (int pos = j + 2; pos + nkl <= ll; pos++) {
            if (s->byte_kind[line_start + (size_t)pos] != 0) continue;
            if (memcmp(ln + pos, null_kws[nk], (size_t)nkl) != 0) continue;
            bool wb = (pos == 0 || (!isalnum((unsigned char)ln[pos - 1]) && ln[pos - 1] != '_'));
            bool wa = (pos + nkl >= ll ||
                       (!isalnum((unsigned char)ln[pos + nkl]) && ln[pos + nkl] != '_'));
            if (!wb || !wa) continue;

            int op = pos - 1;
            while (op > j && (ln[op] == ' ' || ln[op] == '=' || ln[op] == '!')) op--;
            for (;;) {
                if (op >= j + 2 && ln[op] == 't' && ln[op - 1] == 'o' && ln[op - 2] == 'n' &&
                    (op - 3 < j || ln[op - 3] == ' ')) {
                    op -= 3;
                    while (op > j && ln[op] == ' ') op--;
                } else if (op >= j + 1 && ln[op] == 's' && ln[op - 1] == 'i' &&
                           (op - 2 < j || ln[op - 2] == ' ')) {
                    op -= 2;
                    while (op > j && ln[op] == ' ') op--;
                } else break;
            }
            if (op <= j) return;
            int ve = op + 1;
            int vs = ve;
            while (vs > j &&
                   (isalnum((unsigned char)ln[vs - 1]) || ln[vs - 1] == '_' || ln[vs - 1] == '.'))
                vs--;
            int vlen = ve - vs;
            if (vlen < 1 || vlen > 63) return;

            char var[64];
            memcpy(var, ln + vs, (size_t)vlen);
            var[vlen] = '\0';

            for (int k = 0; k < *rc; k++) {
                if (strcmp(recent[k].var, var) == 0 && line - recent[k].line <= NULL_CHECK_WINDOW &&
                    line != recent[k].line) {
                    add_finding(r, SMELL_OVERWRAP, SEV_CORRELATED, line,
                                "redundant null check on '%s' (already checked at line %d)", var,
                                recent[k].line);
                    return;
                }
            }
            if (*rc < NULL_CHECK_RING_CAP) {
                snprintf(recent[*rc].var, sizeof(recent[*rc].var), "%s", var);
                recent[*rc].line = line;
                (*rc)++;
            } else {
                memmove(recent, recent + 1, (NULL_CHECK_RING_CAP - 1) * sizeof(NullCheck));
                snprintf(recent[NULL_CHECK_RING_CAP - 1].var, sizeof(recent[0].var), "%s", var);
                recent[NULL_CHECK_RING_CAP - 1].line = line;
            }
            return;
        }
    }
}

static void detect_overwrap(SmellReport *r, const ScanResult *s) {
    for (int i = 0; i < s->overwrap_count; i++) {
        const OverwrapHit *h = &s->overwraps[i];
        add_finding(r, SMELL_OVERWRAP, SEV_CORRELATED, h->line, "try/catch nested %d deep in %s()",
                    h->depth, h->func_name[0] ? h->func_name : "?");
    }

    /* Redundant null/undefined checks on the same variable within 10 lines */
    NullCheck recent[NULL_CHECK_RING_CAP];
    int rc = 0;

    const char *p = s->content;
    size_t len = s->content_len;
    int line = 1;
    size_t line_start = 0;

    for (size_t i = 0; i <= len; i++) {
        if (i < len && p[i] != '\n') continue;
        int ll = (int)(i - line_start);
        const char *ln = p + line_start;

        if (ll > 3 && s->byte_kind) check_null_line(r, s, ln, ll, line, line_start, recent, &rc);

        line++;
        line_start = i + 1;
    }
}

/* ── Smell 5: naming convention break [CORRELATED] ─────── */

static void detect_name_break(SmellReport *r, const ScanResult *s) {
    /* Go uses PascalCase for exports and camelCase internally — not a smell */
    if (s->specific == SPECIFIC_GO) return;
    for (int i = 0; i < s->name_break_count; i++) {
        const NameBreakHit *h = &s->name_breaks[i];
        add_finding(r, SMELL_NAME_BREAK, SEV_CORRELATED, h->line,
                    "mixed naming in %s(): %d%% camelCase, %d%% snake_case",
                    h->func_name[0] ? h->func_name : "?", h->camel_pct, h->snake_pct);
    }
}

/* ── TS: "as any" casts [CORRELATED] ───────────────────── */

static void detect_as_any(SmellReport *r, const ScanResult *s) {
    if (s->specific != SPECIFIC_TS) return;

    const char *p = s->content;
    size_t len = s->content_len;
    int line = 1, count = 0, first_line = 0;

    for (size_t i = 0; i + 6 < len; i++) {
        if (p[i] == '\n') {
            line++;
            continue;
        }
        if (s->byte_kind && s->byte_kind[i] != 0) continue;
        if (p[i] == ' ' && i + 6 < len && memcmp(p + i, " as any", 7) == 0 &&
            (i + 7 >= len || !isalnum((unsigned char)p[i + 7]))) {
            count++;
            if (count == 1) first_line = line;
            if (count <= 3)
                add_finding(r, SMELL_AS_ANY, SEV_CORRELATED, line,
                            "\"as any\" type cast — loses type safety");
        }
    }
    if (count > 3)
        add_finding(r, SMELL_AS_ANY, SEV_CORRELATED, first_line,
                    "%d total \"as any\" casts in file", count);
}

/* ── TS: @ts-ignore / @ts-nocheck directives [CORRELATED] ─ */

static void detect_ts_directives(SmellReport *r, const ScanResult *s) {
    if (s->specific != SPECIFIC_TS && s->specific != SPECIFIC_JS) return;
    if (!s->byte_kind) return;

    const char *p = s->content;
    size_t len = s->content_len;
    int line = 1;

    for (size_t i = 0; i + 10 < len; i++) {
        if (p[i] == '\n') {
            line++;
            continue;
        }
        if (p[i] != '@') continue;
        if (s->byte_kind[i] != 1) continue;

        if (memcmp(p + i, "@ts-ignore", 10) == 0) {
            add_finding(r, SMELL_TS_DIRECTIVE, SEV_CORRELATED, line,
                        "@ts-ignore — suppresses type checking");
        } else if (i + 11 < len && memcmp(p + i, "@ts-nocheck", 11) == 0) {
            add_finding(r, SMELL_TS_DIRECTIVE, SEV_CORRELATED, line,
                        "@ts-nocheck — disables type checking for entire file");
        } else if (i + 16 <= len && memcmp(p + i, "@ts-expect-error", 16) == 0) {
            add_finding(r, SMELL_TS_DIRECTIVE, SEV_CORRELATED, line,
                        "@ts-expect-error — suppresses type error");
        }
    }
}

/* ── Python: type: ignore / noqa directives [CORRELATED] ── */

static void detect_py_directives(SmellReport *r, const ScanResult *s) {
    if (s->specific != SPECIFIC_PYTHON) return;
    if (!s->byte_kind) return;

    const char *p = s->content;
    size_t len = s->content_len;
    int line = 1, count = 0, first_line = 0;

    for (size_t i = 0; i + 4 < len; i++) {
        if (p[i] == '\n') {
            line++;
            continue;
        }
        if (s->byte_kind[i] != 1) continue;

        if (i + 14 <= len && memcmp(p + i, "type: ignore", 12) == 0) {
            count++;
            if (count == 1) first_line = line;
            if (count <= 3)
                add_finding(r, SMELL_TS_DIRECTIVE, SEV_CORRELATED, line,
                            "type: ignore — suppresses type checking");
        } else if (i + 4 <= len && memcmp(p + i, "noqa", 4) == 0 &&
                   (i + 4 >= len || !isalnum((unsigned char)p[i + 4]))) {
            count++;
            if (count == 1) first_line = line;
            if (count <= 3)
                add_finding(r, SMELL_TS_DIRECTIVE, SEV_CORRELATED, line,
                            "noqa — suppresses linter warning");
        }
    }
    if (count > 3)
        add_finding(r, SMELL_TS_DIRECTIVE, SEV_CORRELATED, first_line,
                    "%d total type suppression directives in file", count);
}

/* ── Go: error suppression with _ [CORRELATED] ─────────── */

static void detect_go_err_suppress(SmellReport *r, const ScanResult *s) {
    if (s->specific != SPECIFIC_GO) return;

    const char *p = s->content;
    size_t len = s->content_len;
    int line = 1, count = 0, first_line = 0;

    for (size_t i = 0; i + 3 < len; i++) {
        if (p[i] == '\n') {
            line++;
            continue;
        }
        if (s->byte_kind && s->byte_kind[i] != 0) continue;
        if (p[i] == '_' && i + 2 < len) {
            bool preceded_ok = (i == 0 || p[i - 1] == ' ' || p[i - 1] == '\t' || p[i - 1] == ',' ||
                                p[i - 1] == '\n');
            bool followed_by_assign =
                (p[i + 1] == ' ' && p[i + 2] == '=') ||
                (p[i + 1] == ' ' && i + 3 < len && p[i + 2] == ':' && p[i + 3] == '=');

            if (preceded_ok && followed_by_assign) {
                /* skip single-value _ in range loops: for _, v := range */
                bool is_range = false;
                for (size_t j = i; j < len && j < i + 80 && p[j] != '\n'; j++) {
                    if (j + 5 < len && memcmp(p + j, "range", 5) == 0) {
                        is_range = true;
                        break;
                    }
                }
                if (!is_range) {
                    /*
                     * Flag if: (a) _ is preceded by comma (multi-return
                     * discard — convention is the discarded value is the
                     * error), or (b) standalone `_ = err` where the RHS
                     * contains "err"/"Err".
                     */
                    bool is_multi_return = false;
                    {
                        size_t b = i;
                        while (b > 0 && (p[b - 1] == ' ' || p[b - 1] == '\t')) b--;
                        if (b > 0 && p[b - 1] == ',') is_multi_return = true;
                    }
                    bool is_err = false;
                    if (!is_multi_return) {
                        size_t eq_pos = i + 1;
                        while (eq_pos < len && p[eq_pos] != '=' && p[eq_pos] != '\n') eq_pos++;
                        if (eq_pos < len && p[eq_pos] == '=') {
                            size_t rhs = eq_pos + 1;
                            while (rhs < len && (p[rhs] == ' ' || p[rhs] == ':')) rhs++;
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
                        if (count == 1) first_line = line;
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

/* ── Smell 6: Zombie parameters [GENERAL] ──────────────── */

static bool is_keyword_param(const char *w) {
    static const char *kws[] = {
        "self",      "cls",     "this",      "super",    "int",     "long",     "short",
        "double",    "float",   "char",      "bool",     "boolean", "string",   "number",
        "void",      "byte",    "var",       "let",      "const",   "auto",     "unsigned",
        "signed",    "static",  "final",     "readonly", "public",  "private",  "protected",
        "abstract",  "virtual", "override",  "async",    "func",    "def",      "return",
        "nil",       "null",    "undefined", "true",     "false",   "error",    "err",
        "ctx",       "any",     "object",    "unknown",  "Request", "Response", "Context",
        "interface", "struct",  "class",     "enum",     nullptr};
    for (int i = 0; kws[i]; i++)
        if (strcmp(w, kws[i]) == 0) return true;
    return false;
}

static int extract_last_ident(const char *seg, int len, char *out, int outsz) {
    int best_s = -1, best_e = -1;
    for (int i = 0; i < len;) {
        if (isalpha((unsigned char)seg[i]) || seg[i] == '_') {
            int s = i;
            while (i < len && (isalnum((unsigned char)seg[i]) || seg[i] == '_')) i++;
            best_s = s;
            best_e = i;
        } else {
            i++;
        }
    }
    if (best_s < 0) return 0;
    int n = best_e - best_s;
    if (n >= outsz) n = outsz - 1;
    memcpy(out, seg + best_s, (size_t)n);
    out[n] = '\0';
    return n;
}

static int extract_params(const char *content, const FuncInfo *fi, SpecificLang spec,
                          char params[][64], int max_params) {
    if (fi->body_offset == 0 || fi->start_line == 0) return 0;

    bool c_style = (spec == SPECIFIC_C || spec == SPECIFIC_CPP || spec == SPECIFIC_JAVA ||
                    spec == SPECIFIC_RUST || spec == SPECIFIC_SWIFT || spec == SPECIFIC_OTHER);

    size_t sig_end = fi->body_offset;
    size_t sig_start = sig_end > 512 ? sig_end - 512 : 0;
    const char *sig = content + sig_start;
    int sig_len = (int)(sig_end - sig_start);

    int last_open = -1, last_close = -1, depth = 0;
    for (int i = sig_len - 1; i >= 0; i--) {
        if (sig[i] == ')' && depth == 0) {
            last_close = i;
            depth = 1;
        } else if (sig[i] == ')') depth++;
        else if (sig[i] == '(') {
            depth--;
            if (depth == 0) {
                last_open = i;
                break;
            }
        }
    }
    if (last_open < 0 || last_close <= last_open) return 0;

    int count = 0;
    int seg_start = last_open + 1;
    depth = 0;
    for (int i = seg_start; i <= last_close; i++) {
        if (sig[i] == '(' || sig[i] == '<' || sig[i] == '[') depth++;
        else if (sig[i] == ')' || sig[i] == '>' || sig[i] == ']') depth--;

        if ((sig[i] == ',' && depth == 0) || i == last_close) {
            char name[64] = {};
            int nlen = 0;

            if (c_style) {
                nlen = extract_last_ident(sig + seg_start, i - seg_start, name, sizeof(name));
            } else {
                int j = seg_start;
                while (j < i && isspace((unsigned char)sig[j])) j++;
                while (j < i && sig[j] == '.') j++;
                if (j < i && (isalpha((unsigned char)sig[j]) || sig[j] == '_')) {
                    int k = 0;
                    while (j < i && k < 63 && (isalnum((unsigned char)sig[j]) || sig[j] == '_'))
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

static void detect_zombie_params(SmellReport *r, const ScanResult *s) {
    for (int f = 0; f < s->function_count; f++) {
        const FuncInfo *fi = &s->functions[f];
        if (fi->body_len < 10) continue;
        if (fi->body_offset + fi->body_len > s->content_len) continue;

        char params[16][64];
        int pc = extract_params(s->content, fi, s->specific, params, 16);

        const char *body = s->content + fi->body_offset;
        size_t blen = fi->body_len;

        for (int p = 0; p < pc; p++) {
            int nlen = (int)strlen(params[p]);
            if (nlen < 2) continue;
            bool found = false;
            for (size_t i = 0; i + (size_t)nlen <= blen; i++) {
                if (memcmp(body + i, params[p], (size_t)nlen) != 0) continue;
                bool wb = (i == 0 || (!isalnum((unsigned char)body[i - 1]) && body[i - 1] != '_'));
                size_t end = i + (size_t)nlen;
                bool wa = (end >= blen || (!isalnum((unsigned char)body[end]) && body[end] != '_'));
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

/* ── Smell 7: Unused imports [GENERAL] ─────────────────── */

#define MAX_IMPORTS 128

typedef struct {
    char name[64];
    int line;
} ImportEntry;

/* Extracts named imports from TS/JS: handles { x, y as z }, * as ns, default imports.
   Skips `import type` (TS 3.8+) which doesn't introduce runtime bindings. */
static int collect_imports_ts(const ScanResult *s, ImportEntry *imp) {
    int count = 0;
    const char *p = s->content;
    size_t len = s->content_len;
    int line = 1;

    for (size_t i = 0; i < len && count < MAX_IMPORTS; i++) {
        if (p[i] == '\n') {
            line++;
            continue;
        }
        if (s->byte_kind && s->byte_kind[i] != 0) continue;

        if (i + 7 >= len || memcmp(p + i, "import ", 7) != 0) continue;
        if (i > 0 && isalnum((unsigned char)p[i - 1])) continue;

        size_t j = i + 7;
        while (j < len && isspace((unsigned char)p[j])) j++;

        /* import type ... — skip the 'type' keyword */
        if (j + 5 < len && memcmp(p + j, "type ", 5) == 0 &&
            (j + 5 >= len || p[j + 5] == '{' || isalpha((unsigned char)p[j + 5])))
            j += 5;

        while (j < len && isspace((unsigned char)p[j])) j++;

        if (j < len && p[j] == '{') {
            j++;
            while (j < len && p[j] != '}' && count < MAX_IMPORTS) {
                while (j < len && (isspace((unsigned char)p[j]) || p[j] == ',')) j++;
                if (j >= len || p[j] == '}') break;
                if (j + 5 < len && memcmp(p + j, "type ", 5) == 0) j += 5;
                while (j < len && isspace((unsigned char)p[j])) j++;
                int k = 0;
                size_t j0 = j;
                while (j < len && k < 63 && (isalnum((unsigned char)p[j]) || p[j] == '_'))
                    imp[count].name[k++] = p[j++];
                imp[count].name[k] = '\0';
                if (j == j0) {
                    j++;
                    continue;
                }
                while (j < len && isspace((unsigned char)p[j])) j++;
                if (j + 3 < len && memcmp(p + j, "as ", 3) == 0) {
                    j += 3;
                    while (j < len && isspace((unsigned char)p[j])) j++;
                    k = 0;
                    while (j < len && k < 63 && (isalnum((unsigned char)p[j]) || p[j] == '_'))
                        imp[count].name[k++] = p[j++];
                    imp[count].name[k] = '\0';
                }
                if (k > 0) {
                    imp[count].line = line;
                    count++;
                }
            }
        } else if (j < len && p[j] == '*') {
            j++;
            while (j < len && isspace((unsigned char)p[j])) j++;
            if (j + 3 < len && memcmp(p + j, "as ", 3) == 0) {
                j += 3;
                while (j < len && isspace((unsigned char)p[j])) j++;
                int k = 0;
                while (j < len && k < 63 && (isalnum((unsigned char)p[j]) || p[j] == '_'))
                    imp[count].name[k++] = p[j++];
                imp[count].name[k] = '\0';
                if (k > 0) {
                    imp[count].line = line;
                    count++;
                }
            }
        } else if (j < len && isalpha((unsigned char)p[j])) {
            int k = 0;
            while (j < len && k < 63 && (isalnum((unsigned char)p[j]) || p[j] == '_'))
                imp[count].name[k++] = p[j++];
            imp[count].name[k] = '\0';
            if (k > 0 && strcmp(imp[count].name, "type") != 0) {
                imp[count].line = line;
                count++;
            }
        }

        while (i < len && p[i] != '\n') i++;
        if (i < len) line++;
    }
    return count;
}

/* Go imports: handles grouped `import (...)` and single `import "pkg"`.
   Extracts the last path component as the name, or the alias if present. */
static int collect_imports_go(const ScanResult *s, ImportEntry *imp) {
    int count = 0;
    const char *p = s->content;
    size_t len = s->content_len;
    int line = 1;

    for (size_t i = 0; i < len && count < MAX_IMPORTS; i++) {
        if (p[i] == '\n') {
            line++;
            continue;
        }
        if (s->byte_kind && s->byte_kind[i] != 0) continue;
        if (i + 7 >= len || memcmp(p + i, "import ", 7) != 0) continue;
        if (i > 0 && isalnum((unsigned char)p[i - 1])) continue;

        size_t j = i + 7;
        while (j < len && isspace((unsigned char)p[j])) {
            if (p[j] == '\n') line++;
            j++;
        }

        bool grouped = (j < len && p[j] == '(');
        if (grouped) j++;

        do {
            while (j < len && isspace((unsigned char)p[j])) {
                if (p[j] == '\n') line++;
                j++;
            }
            if (grouped && j < len && p[j] == ')') break;
            if (j >= len) break;

            char alias[64] = {};
            if (j < len && p[j] != '"' && p[j] != ')' && p[j] != '\n') {
                int k = 0;
                while (j < len && k < 63 && !isspace((unsigned char)p[j]) && p[j] != '"')
                    alias[k++] = p[j++];
                alias[k] = '\0';
                while (j < len && isspace((unsigned char)p[j]) && p[j] != '\n') j++;
            }

            if (j < len && p[j] == '"') {
                j++;
                size_t last_slash = j;
                while (j < len && p[j] != '"') {
                    if (p[j] == '/') last_slash = j + 1;
                    j++;
                }
                size_t pkg_end = j;
                if (j < len) j++;

                if (alias[0] != '.' && alias[0] != '_') {
                    int k = 0;
                    if (alias[0] && alias[0] != '.') {
                        snprintf(imp[count].name, sizeof(imp[count].name), "%s", alias);
                    } else {
                        for (size_t x = last_slash; x < pkg_end && k < 63; x++)
                            imp[count].name[k++] = p[x];
                        imp[count].name[k] = '\0';
                    }
                    if (imp[count].name[0] && count < MAX_IMPORTS) {
                        imp[count].line = line;
                        count++;
                    }
                }
            }
            while (j < len && p[j] != '\n' && p[j] != ')') j++;
            if (j < len && p[j] == '\n') line++;
        } while (grouped && j < len);

        i = j;
    }
    return count;
}

/* Python: handles `from mod import x, y` and `import mod`. For `from` imports,
   captures the individual names; for bare `import`, captures the top-level module. */
static int collect_imports_py(const ScanResult *s, ImportEntry *imp) {
    int count = 0;
    const char *p = s->content;
    size_t len = s->content_len;
    int line = 1;

    for (size_t i = 0; i < len && count < MAX_IMPORTS; i++) {
        if (p[i] == '\n') {
            line++;
            continue;
        }
        if (s->byte_kind && s->byte_kind[i] != 0) continue;

        bool is_from = (i + 5 < len && memcmp(p + i, "from ", 5) == 0);
        bool is_import = (i + 7 < len && memcmp(p + i, "import ", 7) == 0);
        if (!is_from && !is_import) continue;
        if (i > 0 && isalnum((unsigned char)p[i - 1])) continue;

        if (is_from) {
            size_t j = i + 5;
            while (j < len && !isspace((unsigned char)p[j])) j++;
            while (j < len && isspace((unsigned char)p[j])) j++;
            if (j + 7 >= len || memcmp(p + j, "import ", 7) != 0) {
                while (i < len && p[i] != '\n') i++;
                if (i < len) line++;
                continue;
            }
            j += 7;
            while (j < len && p[j] != '\n' && p[j] != '#' && count < MAX_IMPORTS) {
                while (j < len &&
                       (isspace((unsigned char)p[j]) || p[j] == ',' || p[j] == '(' || p[j] == ')'))
                    j++;
                if (j >= len || p[j] == '\n' || p[j] == '#') break;
                if (p[j] == '*') {
                    j++;
                    continue;
                }
                int k = 0;
                size_t j0 = j;
                while (j < len && k < 63 && (isalnum((unsigned char)p[j]) || p[j] == '_'))
                    imp[count].name[k++] = p[j++];
                imp[count].name[k] = '\0';
                if (j == j0) {
                    j++;
                    continue;
                }
                while (j < len && isspace((unsigned char)p[j])) j++;
                if (j + 3 < len && memcmp(p + j, "as ", 3) == 0) {
                    j += 3;
                    while (j < len && isspace((unsigned char)p[j])) j++;
                    k = 0;
                    while (j < len && k < 63 && (isalnum((unsigned char)p[j]) || p[j] == '_'))
                        imp[count].name[k++] = p[j++];
                    imp[count].name[k] = '\0';
                }
                if (k > 0) {
                    imp[count].line = line;
                    count++;
                }
            }
        } else {
            size_t j = i + 7;
            while (j < len && isspace((unsigned char)p[j])) j++;
            int k = 0;
            while (j < len && k < 63 && (isalnum((unsigned char)p[j]) || p[j] == '_'))
                imp[count].name[k++] = p[j++];
            imp[count].name[k] = '\0';
            while (j < len && isspace((unsigned char)p[j])) j++;
            if (j + 3 < len && memcmp(p + j, "as ", 3) == 0) {
                j += 3;
                while (j < len && isspace((unsigned char)p[j])) j++;
                k = 0;
                while (j < len && k < 63 && (isalnum((unsigned char)p[j]) || p[j] == '_'))
                    imp[count].name[k++] = p[j++];
                imp[count].name[k] = '\0';
            }
            if (k > 0) {
                imp[count].line = line;
                count++;
            }
        }
        while (i < len && p[i] != '\n') i++;
        if (i < len) line++;
    }
    return count;
}

static bool name_found_in_range(const ScanResult *s, const char *name, int skip_from, int skip_to) {
    int nlen = (int)strlen(name);
    if (nlen == 0) return true;
    const char *p = s->content;
    size_t len = s->content_len;
    int line = 1;

    for (size_t i = 0; i + (size_t)nlen <= len; i++) {
        if (p[i] == '\n') {
            line++;
            continue;
        }
        if (line >= skip_from && line <= skip_to) continue;
        if (s->byte_kind && s->byte_kind[i] != 0) continue;
        if (memcmp(p + i, name, (size_t)nlen) != 0) continue;
        bool wb = (i == 0 || (!isalnum((unsigned char)p[i - 1]) && p[i - 1] != '_'));
        size_t end = i + (size_t)nlen;
        bool wa = (end >= len || (!isalnum((unsigned char)p[end]) && p[end] != '_'));
        if (wb && wa) return true;
    }
    return false;
}

static void detect_unused_imports(SmellReport *r, const ScanResult *s) {
    ImportEntry imp[MAX_IMPORTS];
    int ic = 0;

    if (s->specific == SPECIFIC_TS || s->specific == SPECIFIC_JS) ic = collect_imports_ts(s, imp);
    else if (s->specific == SPECIFIC_GO) ic = collect_imports_go(s, imp);
    else if (s->specific == SPECIFIC_PYTHON) ic = collect_imports_py(s, imp);
    else return;

    for (int i = 0; i < ic; i++) {
        if (!name_found_in_range(s, imp[i].name, imp[i].line, imp[i].line)) {
            add_finding(r, SMELL_UNUSED_IMPORT, SEV_GENERAL, imp[i].line,
                        "imported name '%s' not used in file", imp[i].name);
        }
    }
}

/* ── Smell 8: Magic string repetition [GENERAL] ───────── */

typedef struct {
    char text[128];
    int count;
    int first_line;
} StrHit;

static void detect_magic_strings(SmellReport *r, const ScanResult *s) {
    if (!s->byte_kind) return;

    const char *p = s->content;
    size_t len = s->content_len;

    StrHit *hits = calloc(512, sizeof(StrHit));
    if (!hits) return;
    int nhits = 0;
    int line = 1;

    for (size_t i = 0; i < len; i++) {
        if (p[i] == '\n') {
            line++;
            continue;
        }
        if (s->byte_kind[i] != 2) continue;

        size_t start = i;
        while (i < len && s->byte_kind[i] == 2) i++;
        size_t slen = i - start;
        i--;

        if (slen < 10) continue;
        size_t cs = start + 1;
        size_t ce = start + slen - 1;
        if (ce <= cs) continue;
        int tlen = (int)(ce - cs);
        if (tlen < 8 || tlen > 127) continue;

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

/* ── Smell 9: Dead / unused code [GENERAL] ─────────────── */

static bool is_entry_point(const char *name, SpecificLang spec) {
    if (strcmp(name, "main") == 0) return true;
    if (strcmp(name, "init") == 0) return true;
    if (strcmp(name, "constructor") == 0) return true;
    if (strcmp(name, "render") == 0) return true;
    if (strcmp(name, "setup") == 0) return true;
    if (strcmp(name, "teardown") == 0) return true;
    if (spec == SPECIFIC_GO) {
        if (strncmp(name, "Test", 4) == 0 && name[4] && isupper((unsigned char)name[4]))
            return true;
        if (strncmp(name, "Benchmark", 9) == 0) return true;
        if (strncmp(name, "Example", 7) == 0) return true;
    }
    return false;
}

static bool line_has_keyword(const char *p, size_t start, size_t end, const char *kw) {
    size_t kwlen = strlen(kw);
    for (size_t i = start; i + kwlen <= end; i++) {
        if (memcmp(p + i, kw, kwlen) != 0) continue;
        bool wb = (i == start || (!isalnum((unsigned char)p[i - 1]) && p[i - 1] != '_'));
        bool wa =
            (i + kwlen >= end || (!isalnum((unsigned char)p[i + kwlen]) && p[i + kwlen] != '_'));
        if (wb && wa) return true;
    }
    return false;
}

/* Language-specific export/visibility rules: Go uppercase, TS/JS `export` keyword
   on the same or preceding line, Python leading underscore, C/C++ `static` keyword. */
static bool func_is_exported(const ScanResult *s, const FuncInfo *fi) {
    if (s->specific == SPECIFIC_GO) return fi->name[0] && isupper((unsigned char)fi->name[0]);

    if (s->specific == SPECIFIC_TS || s->specific == SPECIFIC_JS) {
        const char *p = s->content;
        size_t off = fi->body_offset;
        while (off > 0 && p[off - 1] != '\n') off--;
        while (off < fi->body_offset && (p[off] == ' ' || p[off] == '\t')) off++;
        if (off + 6 <= fi->body_offset && memcmp(p + off, "export", 6) == 0 &&
            (p[off + 6] == ' ' || p[off + 6] == '\t'))
            return true;
        if (off > 0) {
            size_t prev = off - 1;
            while (prev > 0 && p[prev - 1] != '\n') prev--;
            while (prev < off && (p[prev] == ' ' || p[prev] == '\t')) prev++;
            if (prev + 6 < off && memcmp(p + prev, "export", 6) == 0 &&
                (p[prev + 6] == ' ' || p[prev + 6] == '\t'))
                return true;
        }
        return false;
    }

    if (s->specific == SPECIFIC_PYTHON) return fi->name[0] != '_';

    if (s->lang == LANG_C_LIKE) {
        const char *p = s->content;
        size_t off = fi->body_offset;
        while (off > 0 && p[off - 1] != '\n') off--;
        size_t line_end = fi->body_offset;
        if (line_has_keyword(p, off, line_end, "static")) return false;
        if (off > 0) {
            size_t prev_end = off - 1;
            size_t prev_start = prev_end;
            while (prev_start > 0 && p[prev_start - 1] != '\n') prev_start--;
            if (line_has_keyword(p, prev_start, prev_end, "static")) return false;
        }
        return true;
    }

    return false;
}

static bool name_used_outside_func(const ScanResult *s, const FuncInfo *fi) {
    return name_found_in_range(s, fi->name, fi->start_line, fi->end_line);
}

static void detect_dead_code(SmellReport *r, const ScanResult *s) {
    int total_dead_lines = 0;
    int dead_funcs = 0;

    for (int f = 0; f < s->function_count; f++) {
        const FuncInfo *fi = &s->functions[f];
        if (fi->name[0] == '\0') continue;
        if (fi->end_line <= fi->start_line) continue;
        if (is_entry_point(fi->name, s->specific)) continue;
        if (func_is_exported(s, fi)) continue;

        if (!name_used_outside_func(s, fi)) {
            int lines = fi->end_line - fi->start_line + 1;
            total_dead_lines += lines;
            dead_funcs++;
            add_finding(r, SMELL_DEAD_CODE, SEV_GENERAL, fi->start_line,
                        "%s() defined but never referenced (%d lines)", fi->name, lines);
        }
    }

    if (dead_funcs > 1) {
        add_finding(r, SMELL_DEAD_CODE, SEV_GENERAL, 0, "%d unused functions, %d dead lines total",
                    dead_funcs, total_dead_lines);
    }
}

/* ── Public API ─────────────────────────────────────────── */

int smell_count_dead_lines(const ScanResult *s) {
    int total = 0;
    for (int f = 0; f < s->function_count; f++) {
        const FuncInfo *fi = &s->functions[f];
        if (fi->name[0] == '\0') continue;
        if (fi->end_line <= fi->start_line) continue;
        if (is_entry_point(fi->name, s->specific)) continue;
        if (func_is_exported(s, fi)) continue;
        if (!name_used_outside_func(s, fi)) total += fi->end_line - fi->start_line + 1;
    }
    return total;
}

void smell_detect(SmellReport *report, const ScanResult *scan, bool include_general) {
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
