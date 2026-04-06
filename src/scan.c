#include "slop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Types ──────────────────────────────────────────────── */

typedef enum { ST_CODE, ST_LINE_COMMENT, ST_BLOCK_COMMENT, ST_STRING } State;

typedef enum { BLK_OTHER, BLK_TRY, BLK_CATCH } BlockKind;

typedef struct {
    BlockKind kind;
    int start_line;
    int code_lines;
} BlockEntry;

#define MAX_BLOCK_DEPTH 64

/* ── Narration markers ──────────────────────────────────── */

static const char *narration_markers[] = {
    "first we", "first,",     "now we",   "now,",    "now let", "next we", "next,",  "then we",
    "then,",    "finally we", "finally,", "here we", "here,",   "let's",   "let me", "we need to",
    "we can ",  "we'll",      "we will",  "step 1",  "step 2",  "step 3",  nullptr};

/* ── Keywords for filtering ─────────────────────────────── */

static const char *all_keywords[] = {
    "auto",     "break",       "case",    "catch",     "char",      "class",    "const",
    "continue", "default",     "do",      "double",    "else",      "enum",     "extern",
    "false",    "float",       "for",     "goto",      "if",        "int",      "long",
    "new",      "null",        "private", "protected", "public",    "register", "return",
    "short",    "signed",      "sizeof",  "static",    "struct",    "super",    "switch",
    "this",     "throw",       "true",    "try",       "typedef",   "typeof",   "union",
    "unsigned", "var",         "void",    "volatile",  "while",     "async",    "await",
    "delete",   "export",      "extends", "finally",   "from",      "function", "import",
    "in",       "instanceof",  "let",     "of",        "undefined", "yield",    "chan",
    "defer",    "fallthrough", "func",    "go",        "interface", "map",      "package",
    "range",    "select",      "type",    "as",        "crate",     "dyn",      "fn",
    "impl",     "loop",        "match",   "mod",       "move",      "mut",      "pub",
    "ref",      "self",        "trait",   "unsafe",    "use",       "where",    "def",
    "elif",     "except",      "global",  "lambda",    "nonlocal",  "pass",     "raise",
    "with",     nullptr};

/* ── Generic identifier dictionary (AI naming signal) ───── */

static const char *generic_names[] = {
    "data",      "result",     "response",   "handler",    "value",      "item",
    "config",    "options",    "params",     "callback",   "output",     "input",
    "temp",      "tmp",        "ret",        "res",        "val",        "obj",
    "arr",       "args",       "opts",       "payload",    "instance",   "container",
    "wrapper",   "helper",     "util",       "manager",    "service",    "controller",
    "processor", "middleware", "utils",      "helpers",    "entries",    "items",
    "values",    "results",    "responses",  nullptr};

static bool is_generic_name(const char *word) {
    for (int i = 0; generic_names[i]; i++)
        if (strcmp(word, generic_names[i]) == 0) return true;
    return false;
}

/* ── Simple hash set for unique identifier counting (TTR) ── */

typedef struct {
    char **slots;
    int cap;
    int count;
} IdentSet;

static uint32_t fnv1a(const char *s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static IdentSet ident_set_new(int cap) {
    IdentSet is = { .cap = cap, .count = 0 };
    is.slots = calloc((size_t)cap, sizeof(char *));
    return is;
}

static void ident_set_insert(IdentSet *is, const char *word, int wlen) {
    uint32_t h = fnv1a(word, wlen);
    for (int probe = 0; probe < is->cap; probe++) {
        int idx = (int)((h + (uint32_t)probe) % (uint32_t)is->cap);
        if (!is->slots[idx]) {
            is->slots[idx] = strndup(word, (size_t)wlen);
            is->count++;
            return;
        }
        if (strncmp(is->slots[idx], word, (size_t)wlen) == 0 && is->slots[idx][wlen] == '\0')
            return;
    }
}

static void ident_set_free(IdentSet *is) {
    for (int i = 0; i < is->cap; i++)
        free(is->slots[i]);
    free(is->slots);
}

/* ── Function close helper (used by scanner in 4 places) ── */

typedef struct {
    ScanResult *res;
    bool *in_func;
    int *func_camel;
    int *func_snake;
    int func_start_line;
    char *cur_func_name;
    size_t close_offset;
    int close_line;
} FuncCloseCtx;

static void close_current_function(FuncCloseCtx *ctx) {
    ScanResult *res = ctx->res;
    if (!*ctx->in_func) return;
    if (res->function_count < MAX_FUNCTIONS) {
        FuncInfo *fi = &res->functions[res->function_count];
        fi->end_line = ctx->close_line;
        fi->body_len = ctx->close_offset - fi->body_offset;
        res->function_count++;
    }
    int total = *ctx->func_camel + *ctx->func_snake;
    if (total >= 4) {
        int cp = *ctx->func_camel * 100 / total;
        int sp = *ctx->func_snake * 100 / total;
        if (cp > 25 && sp > 25 && res->name_break_count < MAX_NAME_BREAKS) {
            NameBreakHit *nb = &res->name_breaks[res->name_break_count++];
            nb->line = ctx->func_start_line;
            snprintf(nb->func_name, sizeof(nb->func_name), "%s", ctx->cur_func_name);
            nb->camel_pct = cp;
            nb->snake_pct = sp;
        }
    }
    *ctx->in_func = false;
}

/* ── Helpers ────────────────────────────────────────────── */

static bool starts_with_ci(const char *s, int slen, const char *prefix) {
    int plen = (int)strlen(prefix);
    if (slen < plen) return false;
    for (int i = 0; i < plen; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i])) return false;
    }
    return true;
}

static bool is_word_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static int extract_word(const char *s, const char *end, char *buf, int bufsz) {
    int i = 0;
    while (s + i < end && i < bufsz - 1 && is_word_char(s[i])) {
        buf[i] = s[i];
        i++;
    }
    buf[i] = '\0';
    return i;
}

static bool is_keyword(const char *word) {
    for (int i = 0; all_keywords[i]; i++)
        if (strcmp(word, all_keywords[i]) == 0) return true;
    return false;
}

static bool is_camel_case(const char *s, int len) {
    if (len <= 2 || !islower((unsigned char)s[0])) return false;
    for (int i = 1; i < len; i++)
        if (isupper((unsigned char)s[i])) return true;
    return false;
}

static bool is_snake_case(const char *s, int len) {
    if (len <= 2) return false;
    bool has_us = false;
    for (int i = 0; i < len; i++) {
        if (s[i] == '_') has_us = true;
        else if (!islower((unsigned char)s[i]) && !isdigit((unsigned char)s[i])) return false;
    }
    return has_us;
}

static bool is_all_upper(const char *s, int len) {
    for (int i = 0; i < len; i++)
        if (islower((unsigned char)s[i])) return false;
    return true;
}

static bool line_has_paren(const char *line, int len) {
    for (int i = 0; i < len; i++)
        if (line[i] == '(') return true;
    return false;
}

static bool line_starts_control(const char *line, int len) {
    int i = 0;
    while (i < len && isspace((unsigned char)line[i])) i++;
    const char *kw[] = {"if", "else", "for", "while", "switch", "do", nullptr};
    for (int k = 0; kw[k]; k++) {
        int kl = (int)strlen(kw[k]);
        if (i + kl <= len && memcmp(line + i, kw[k], (size_t)kl) == 0 &&
            (i + kl >= len || !is_word_char(line[i + kl])))
            return true;
    }
    return false;
}

static void extract_func_name(const char *line, int len, char *out, int outsz) {
    /*
     * Find the LAST '(' that has an identifier before it.
     * This handles Go method receivers: func (s *Server) Handle(req)
     * where the first '(' is the receiver and the last is the real signature.
     */
    int best_start = -1, best_end = -1;
    for (int p = 0; p < len; p++) {
        if (line[p] != '(') continue;
        int e = p - 1;
        while (e >= 0 && isspace((unsigned char)line[e])) e--;
        if (e < 0 || !is_word_char(line[e])) continue;
        int s = e;
        while (s > 0 && is_word_char(line[s - 1])) s--;
        /* skip if the "name" is a keyword like func/if/for */
        char tmp[128];
        int tl = e - s + 1;
        if (tl > 0 && tl < (int)sizeof(tmp)) {
            memcpy(tmp, line + s, (size_t)tl);
            tmp[tl] = '\0';
            if (strcmp(tmp, "func") == 0 || strcmp(tmp, "if") == 0 || strcmp(tmp, "for") == 0 ||
                strcmp(tmp, "while") == 0 || strcmp(tmp, "switch") == 0)
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
    if (n <= 0) {
        out[0] = '\0';
        return;
    }
    if (n >= outsz) n = outsz - 1;
    memcpy(out, line + best_start, (size_t)n);
    out[n] = '\0';
}

/* ── Python function detection helpers ──────────────────── */

static bool py_is_def_line(const char *line, int len, int *out_indent) {
    int i = 0;
    while (i < len && line[i] == ' ') i++;
    if (i + 4 <= len && memcmp(line + i, "def ", 4) == 0) {
        *out_indent = i;
        return true;
    }
    return false;
}

static void py_extract_func_name(const char *line, int len, char *out, int outsz) {
    int i = 0;
    while (i < len && line[i] == ' ') i++;
    /* skip past "def " which py_is_def_line already verified */
    if (i + 4 <= len && memcmp(line + i, "def ", 4) == 0) i += 4;
    else {
        out[0] = '\0';
        return;
    }
    int s = i;
    while (i < len && is_word_char(line[i])) i++;
    int n = i - s;
    if (n <= 0) {
        out[0] = '\0';
        return;
    }
    if (n >= outsz) n = outsz - 1;
    memcpy(out, line + s, (size_t)n);
    out[n] = '\0';
}

/* ── Arrow function name extraction (TS/JS) ────────────── */

static void extract_arrow_name(const char *line, int len, char *out, int outsz) {
    const char *kws[] = {"const ", "let ", "var ", nullptr};
    for (int k = 0; kws[k]; k++) {
        int kwlen = (int)strlen(kws[k]);
        for (int p = 0; p + kwlen < len; p++) {
            if (p > 0 && is_word_char(line[p - 1])) continue;
            if (memcmp(line + p, kws[k], (size_t)kwlen) != 0) continue;
            int s = p + kwlen;
            if (s >= len || !isalpha((unsigned char)line[s])) continue;
            int e = s;
            while (e < len && is_word_char(line[e])) e++;
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

/* ── Shell function detection helpers ───────────────────── */

static bool sh_is_func_line(const char *line, int len) {
    int i = 0;
    while (i < len && isspace((unsigned char)line[i])) i++;
    if (i + 9 <= len && memcmp(line + i, "function ", 9) == 0) return true;
    for (int j = i; j < len - 1; j++) {
        if (line[j] == '(' && line[j + 1] == ')') return true;
    }
    return false;
}

/* ── Byte-kind map (code/comment/string per byte offset) ── */

static void compute_byte_kind(uint8_t *bk, const char *content, size_t len, LangFamily lang) {
    enum { S_CODE, S_LINE_CMT, S_BLOCK_CMT, S_STRING } state = S_CODE;
    char sq = 0; /* string quote */
    char bq = 0; /* block-comment quote (Python triple-quote char) */

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
                } else if ((c == '"' || c == '\'') && i + 2 < len && content[i + 1] == c &&
                           content[i + 2] == c) {
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
            if (c == '\n') state = S_CODE;
            break;

        case S_BLOCK_CMT:
            bk[i] = 3;
            if (lang == LANG_C_LIKE && c == '*' && i + 1 < len && content[i + 1] == '/') {
                bk[++i] = 3;
                state = S_CODE;
            } else if (lang == LANG_PYTHON && c == bq && i + 2 < len && content[i + 1] == bq &&
                       content[i + 2] == bq) {
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

/* ── Post-pass: total lines, comment gradient, defect quartiles ── */

static void scan_finalize(ScanResult *res, const char *content, size_t len, int max_lines,
                          const uint8_t *line_is_cmnt, const int *defect_buf, int defect_n) {
    /* Count real lines (not synthetic) */
    int real_newlines = 0;
    for (size_t idx = 0; idx < len; idx++)
        if (content[idx] == '\n') real_newlines++;
    if (real_newlines > 0 && content[len - 1] == '\n') res->total_lines = real_newlines;
    else res->total_lines = real_newlines + 1;

    if (res->line_length_count > res->total_lines) res->line_length_count = res->total_lines;

    /* Comment gradient within code body */
    const int body_lines = res->total_lines - res->code_body_start + 1;
    if (body_lines >= MIN_LINES_GRADIENT) {
        const int half = body_lines / 2;
        const int mid = res->code_body_start + half;
        for (int l = res->code_body_start; l <= res->total_lines && l <= max_lines; l++) {
            if (l < mid) {
                res->body_lines_top++;
                if (line_is_cmnt[l]) res->body_comment_top++;
            } else {
                res->body_lines_bottom++;
                if (line_is_cmnt[l]) res->body_comment_bottom++;
            }
        }
    }

    /* Defects per quartile for Spearman rank correlation */
    res->total_defects = defect_n;
    if (res->total_lines >= 4 && defect_n > 0) {
        int q_sz = res->total_lines / 4;
        if (q_sz < 1) q_sz = 1;
        for (int d = 0; d < defect_n; d++) {
            int q = (defect_buf[d] - 1) / q_sz;
            if (q >= 4) q = 3;
            res->defects_per_quartile[q]++;
        }
    }
}

/* ── Main scanner ───────────────────────────────────────── */

void scan_file(ScanResult *res, const char *filename, const char *content, size_t len,
               LangFamily lang) {
    *res = (ScanResult){};
    res->filename = filename;
    res->content = content;
    res->content_len = len;
    res->lang = lang;
    res->specific = lang_detect_specific(filename);

    if (len == 0) return;

    res->byte_kind = calloc(len + 1, 1);
    compute_byte_kind(res->byte_kind, content, len, lang);

    int max_lines = 1;
    for (size_t i = 0; i < len; i++)
        if (content[i] == '\n') max_lines++;

    res->line_lengths = calloc((size_t)max_lines + 1, sizeof(int));
    res->indent_depths = calloc((size_t)max_lines + 1, sizeof(int));
    res->indent_count = 0;
    uint8_t *line_is_cmnt = calloc((size_t)max_lines + 2, 1);

    int *defect_buf = malloc((size_t)max_lines * sizeof(int));
    int defect_n = 0;

    IdentSet iset = ident_set_new(TTR_HASH_BUCKETS);

    State state = ST_CODE;
    char str_quote = 0;

    int line_num = 1;
    size_t line_start = 0;
    bool line_has_nonws = false;
    bool first_nonws_cmnt = false;
    bool line_has_code = false;

    char prev_line[2048];
    int prev_line_len = 0;
    int brace_depth = 0;

    bool in_func = false;
    int func_start_line = 0;
    int func_start_brace = 0;
    char cur_func_name[128] = {};
    int py_func_indent = -1;

    BlockEntry bstack[MAX_BLOCK_DEPTH];
    int bstack_top = 0;
    int try_nest = 0;
    BlockKind pending_blk = BLK_OTHER;

    int func_camel = 0, func_snake = 0;
    bool found_body = false;

    char cmnt_buf[512];
    int cmnt_len = 0;
    char word[256];

    for (size_t i = 0; i <= len; i++) {
        char c = (i < len) ? content[i] : '\n';

        /* ── newline: finalise line ─────────────────────── */
        if (c == '\n') {
            int ll = (int)(i - line_start);

            if (!line_has_nonws) {
                res->blank_lines++;
            } else if (first_nonws_cmnt && !line_has_code) {
                res->comment_lines++;
                if (line_num <= max_lines) line_is_cmnt[line_num] = 1;
            } else {
                res->code_lines++;
            }

            if (res->line_length_count < max_lines)
                res->line_lengths[res->line_length_count++] = ll;

            if (line_has_code) {
                int ws = 0;
                for (size_t wi = line_start; wi < line_start + (size_t)ll; wi++) {
                    if (content[wi] == ' ') ws++;
                    else if (content[wi] == '\t') ws += 4;
                    else break;
                }
                res->indent_depths[res->indent_count++] = ws;
            }

            if (cmnt_len > 0) {
                int ci = 0;
                while (ci < cmnt_len && isspace((unsigned char)cmnt_buf[ci])) ci++;
                int tl = cmnt_len - ci;
                for (int m = 0; narration_markers[m]; m++) {
                    if (starts_with_ci(cmnt_buf + ci, tl, narration_markers[m])) {
                        if (res->narration_count < MAX_NARRATION) {
                            NarrationHit *h = &res->narration[res->narration_count++];
                            h->line = line_num;
                            int cp = ll;
                            if (cp >= (int)sizeof(h->text)) cp = (int)sizeof(h->text) - 1;
                            memcpy(h->text, content + line_start, (size_t)cp);
                            h->text[cp] = '\0';
                        }
                        break;
                    }
                }

                /* TODO / FIXME detection in comment text */
                for (int j = 0; j < cmnt_len - 3; j++) {
                    if (starts_with_ci(cmnt_buf + j, cmnt_len - j, "todo") ||
                        starts_with_ci(cmnt_buf + j, cmnt_len - j, "fixme")) {
                        if (defect_n < max_lines) defect_buf[defect_n++] = line_num;
                        break;
                    }
                }
            }

            /* store prev line for C-like function detection (only code lines) */
            if (line_has_code && ll < (int)sizeof(prev_line)) {
                memcpy(prev_line, content + line_start, (size_t)ll);
                prev_line_len = ll;
            }

            /* track code in current block (empty catch detection) */
            if (bstack_top > 0 && line_has_code) bstack[bstack_top - 1].code_lines++;

            /* Python function boundary by indentation */
            if (lang == LANG_PYTHON && in_func && py_func_indent >= 0 && line_has_nonws) {
                int indent = 0;
                while (indent < ll && content[line_start + (size_t)indent] == ' ') indent++;
                if (indent <= py_func_indent && !first_nonws_cmnt) {
                    FuncCloseCtx fcc = {res,         &in_func,        &func_camel,
                                        &func_snake, func_start_line, cur_func_name,
                                        line_start,  line_num - 1};
                    close_current_function(&fcc);
                    py_func_indent = -1;
                }
            }

            /* Python: detect new def */
            if (lang == LANG_PYTHON && !first_nonws_cmnt) {
                int pyi = 0;
                if (py_is_def_line(content + line_start, ll, &pyi)) {
                    if (in_func) {
                        FuncCloseCtx fcc = {res,         &in_func,        &func_camel,
                                            &func_snake, func_start_line, cur_func_name,
                                            line_start,  line_num - 1};
                        close_current_function(&fcc);
                    }
                    if (res->function_count < MAX_FUNCTIONS) {
                        FuncInfo *fi = &res->functions[res->function_count];
                        fi->start_line = line_num;
                        fi->body_offset = i + 1;
                        py_extract_func_name(content + line_start, ll, fi->name, sizeof(fi->name));
                        snprintf(cur_func_name, sizeof(cur_func_name), "%s", fi->name);
                    }
                    in_func = true;
                    func_start_line = line_num;
                    py_func_indent = pyi;
                    func_camel = 0;
                    func_snake = 0;
                    try_nest = 0;
                    bstack_top = 0;
                    if (!found_body) {
                        res->code_body_start = line_num;
                        found_body = true;
                    }
                }
            }

            /* Shell: detect function lines */
            if (lang == LANG_SHELL && !first_nonws_cmnt) {
                if (sh_is_func_line(content + line_start, ll)) {
                    if (!found_body) {
                        res->code_body_start = line_num;
                        found_body = true;
                    }
                }
            }

            /* reset per-line */
            line_num++;
            line_start = i + 1;
            line_has_nonws = false;
            first_nonws_cmnt = false;
            line_has_code = false;
            cmnt_len = 0;
            if (state == ST_LINE_COMMENT) state = ST_CODE;
            continue;
        }

        /* ── block-comment line start detection ─────────── */
        if (!line_has_nonws && !isspace((unsigned char)c) && state == ST_BLOCK_COMMENT) {
            first_nonws_cmnt = true;
            line_has_nonws = true;
        }

        /* ── state machine ──────────────────────────────── */
        switch (state) {
        case ST_CODE: {
            if (isspace((unsigned char)c)) break;

            if (!line_has_nonws) {
                line_has_nonws = true;

                /* C-like: // or block comment */
                if (lang == LANG_C_LIKE) {
                    if (c == '/' && i + 1 < len && content[i + 1] == '/') {
                        first_nonws_cmnt = true;
                        state = ST_LINE_COMMENT;
                        i++;
                        continue;
                    }
                    if (c == '/' && i + 1 < len && content[i + 1] == '*') {
                        first_nonws_cmnt = true;
                        state = ST_BLOCK_COMMENT;
                        i++;
                        continue;
                    }
                }
                /* Python: # or triple-quote */
                if (lang == LANG_PYTHON) {
                    if (c == '#') {
                        first_nonws_cmnt = true;
                        state = ST_LINE_COMMENT;
                        continue;
                    }
                    if ((c == '"' || c == '\'') && i + 2 < len && content[i + 1] == c &&
                        content[i + 2] == c) {
                        first_nonws_cmnt = true;
                        str_quote = c;
                        state = ST_BLOCK_COMMENT;
                        i += 2;
                        continue;
                    }
                }
                /* Shell: # */
                if (lang == LANG_SHELL) {
                    if (c == '#') {
                        first_nonws_cmnt = true;
                        state = ST_LINE_COMMENT;
                        continue;
                    }
                }
            }

            line_has_code = true;

            if (c == '"' || c == '\'') {
                if (lang == LANG_PYTHON && i + 2 < len && content[i + 1] == c &&
                    content[i + 2] == c) {
                    str_quote = c;
                    state = ST_BLOCK_COMMENT;
                    i += 2;
                    continue;
                }
                str_quote = c;
                state = ST_STRING;
                continue;
            }

            /* backtick template literals (JS/TS) */
            if (c == '`' && lang == LANG_C_LIKE) {
                str_quote = '`';
                state = ST_STRING;
                continue;
            }

            /* inline comment (not first token on line) */
            if (lang == LANG_C_LIKE && c == '/' && i + 1 < len) {
                if (content[i + 1] == '/') {
                    state = ST_LINE_COMMENT;
                    i++;
                    continue;
                }
                if (content[i + 1] == '*') {
                    state = ST_BLOCK_COMMENT;
                    i++;
                    continue;
                }
            }
            if ((lang == LANG_PYTHON || lang == LANG_SHELL) && c == '#') {
                state = ST_LINE_COMMENT;
                continue;
            }

            /* ── C-like brace tracking & function detection ── */
            if (lang == LANG_C_LIKE) {
                if (c == '{') {
                    /*
                     * Detect functions at depth 0 (top-level) and depth 1
                     * (class methods) but NOT depth 1 when already inside
                     * a function (those are control blocks / object literals).
                     */
                    if (brace_depth == 0 || (brace_depth == 1 && !in_func)) {
                        int cl_len = (int)(i - line_start);
                        const char *cl = content + line_start;

                        /* reject "} else {" / "} catch {" / "} finally {" */
                        int fw = 0;
                        while (fw < cl_len && isspace((unsigned char)cl[fw])) fw++;
                        bool line_starts_close = (fw < cl_len && cl[fw] == '}');

                        bool has_sig =
                            !line_starts_close && (line_has_paren(cl, cl_len) ||
                                                   line_has_paren(prev_line, prev_line_len));
                        const char *sig_line = line_has_paren(cl, cl_len) ? cl : prev_line;
                        int sig_len = line_has_paren(cl, cl_len) ? cl_len : prev_line_len;

                        if (has_sig && !line_starts_control(sig_line, sig_len)) {
                            if (in_func) {
                                FuncCloseCtx fcc = {res,         &in_func,        &func_camel,
                                                    &func_snake, func_start_line, cur_func_name,
                                                    i,           line_num - 1};
                                close_current_function(&fcc);
                            }

                            if (res->function_count < MAX_FUNCTIONS) {
                                FuncInfo *fi = &res->functions[res->function_count];
                                fi->start_line = line_num;
                                fi->body_offset = i + 1;
                                extract_func_name(sig_line, sig_len, fi->name, sizeof(fi->name));
                                if (fi->name[0] == '\0') {
                                    extract_arrow_name(cl, cl_len, fi->name, sizeof(fi->name));
                                    if (fi->name[0] == '\0')
                                        extract_arrow_name(prev_line, prev_line_len, fi->name,
                                                           sizeof(fi->name));
                                }
                                snprintf(cur_func_name, sizeof(cur_func_name), "%s", fi->name);
                                in_func = true;
                            } else {
                                in_func = false;
                            }
                            func_start_line = line_num;
                            func_start_brace = brace_depth;
                            func_camel = 0;
                            func_snake = 0;
                            try_nest = 0;
                            bstack_top = 0;
                            if (!found_body) {
                                res->code_body_start = line_num;
                                found_body = true;
                            }
                        }
                    }
                    brace_depth++;

                    if (in_func && bstack_top < MAX_BLOCK_DEPTH) {
                        bstack[bstack_top].kind = pending_blk;
                        bstack[bstack_top].start_line = line_num;
                        bstack[bstack_top].code_lines = 0;
                        if (pending_blk == BLK_TRY) {
                            try_nest++;
                            if (try_nest > 2 && res->overwrap_count < MAX_OVERWRAPS) {
                                OverwrapHit *oh = &res->overwraps[res->overwrap_count++];
                                oh->line = line_num;
                                oh->depth = try_nest;
                                snprintf(oh->func_name, sizeof(oh->func_name), "%s", cur_func_name);
                            }
                        }
                        bstack_top++;
                    }
                    pending_blk = BLK_OTHER;
                }

                if (c == '}') {
                    brace_depth--;
                    if (brace_depth < 0) brace_depth = 0;

                    if (in_func) {
                        if (bstack_top > 0) {
                            bstack_top--;
                            if (bstack[bstack_top].kind == BLK_TRY) try_nest--;
                            if (bstack[bstack_top].kind == BLK_CATCH &&
                                bstack[bstack_top].code_lines == 0 && defect_n < max_lines)
                                defect_buf[defect_n++] = bstack[bstack_top].start_line;
                        }

                        if (brace_depth == func_start_brace) {
                            FuncCloseCtx fcc = {res,         &in_func,        &func_camel,
                                                &func_snake, func_start_line, cur_func_name,
                                                i,           line_num};
                            close_current_function(&fcc);
                        }
                    }
                }
            }

            /* ── keyword / identifier extraction ──────────── */
            if (isalpha((unsigned char)c) || c == '_') {
                int wlen = extract_word(content + i, content + len, word, sizeof(word));
                if (wlen > 0) {
                    /* conditional keywords */
                    if ((strcmp(word, "if") == 0) || (strcmp(word, "else") == 0) ||
                        (strcmp(word, "elif") == 0) || (strcmp(word, "switch") == 0) ||
                        (strcmp(word, "case") == 0) || (strcmp(word, "match") == 0))
                        res->conditional_count++;

                    /* try/catch/except → pending block type */
                    if (strcmp(word, "try") == 0) pending_blk = BLK_TRY;
                    else if (strcmp(word, "catch") == 0 || strcmp(word, "except") == 0) {
                        pending_blk = BLK_CATCH;
                        /* Python bare except: "except:" with no exception type */
                        if (lang == LANG_PYTHON && strcmp(word, "except") == 0) {
                            size_t e = i + (size_t)wlen;
                            while (e < len && (content[e] == ' ' || content[e] == '\t')) e++;
                            if (e < len && content[e] == ':' && defect_n < max_lines)
                                defect_buf[defect_n++] = line_num;
                        }
                    }

                    /* naming convention tracking inside functions */
                    if (in_func && !is_keyword(word) && !is_all_upper(word, wlen)) {
                        if (is_camel_case(word, wlen)) func_camel++;
                        else if (is_snake_case(word, wlen)) func_snake++;
                    }

                    if (!is_keyword(word) && wlen >= 2 && !is_all_upper(word, wlen)) {
                        res->total_identifiers++;
                        if (is_generic_name(word)) res->generic_identifiers++;
                        ident_set_insert(&iset, word, wlen);
                    }

                    i += (size_t)wlen - 1;
                }
            }
            break;
        }

        case ST_LINE_COMMENT:
            if (cmnt_len < (int)sizeof(cmnt_buf) - 1) cmnt_buf[cmnt_len++] = c;
            break;

        case ST_BLOCK_COMMENT:
            if (lang == LANG_C_LIKE && c == '*' && i + 1 < len && content[i + 1] == '/') {
                state = ST_CODE;
                i++;
            } else if (lang == LANG_PYTHON && c == str_quote && i + 2 < len &&
                       content[i + 1] == str_quote && content[i + 2] == str_quote) {
                state = ST_CODE;
                i += 2;
            }
            break;

        case ST_STRING:
            if (c == '\\' && i + 1 < len) {
                i++;
            } else if (c == str_quote) {
                state = ST_CODE;
            }
            break;
        }
    }

    /* close any open Python function */
    if (lang == LANG_PYTHON && in_func) {
        FuncCloseCtx fcc = {res,           &in_func, &func_camel, &func_snake, func_start_line,
                            cur_func_name, len,      line_num - 1};
        close_current_function(&fcc);
    }

    if (!found_body) res->code_body_start = 1;

    res->unique_identifiers = iset.count;
    ident_set_free(&iset);

    scan_finalize(res, content, len, max_lines, line_is_cmnt, defect_buf, defect_n);

    free(line_is_cmnt);
    free(defect_buf);
}

void scan_free(ScanResult *res) {
    free(res->line_lengths);
    res->line_lengths = nullptr;
    free(res->indent_depths);
    res->indent_depths = nullptr;
    free(res->byte_kind);
    res->byte_kind = nullptr;
}
