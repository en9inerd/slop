#include "scan_util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Types ──────────────────────────────────────────────── */

typedef enum { ST_CODE, ST_LINE_COMMENT, ST_BLOCK_COMMENT, ST_STRING } State;

typedef enum { BLK_OTHER, BLK_TRY, BLK_CATCH } BlockKind;

typedef struct {
  BlockKind kind;
  int start_line;
  int code_lines;
} BlockEntry;

#define MAX_BLOCK_DEPTH 64

/* ── Function close helper ──────────────────────────────── */

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
  if (!*ctx->in_func)
    return;
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

/* ── Main scanner ──────────────────────────────────────────
 *
 * Single-pass character-by-character scanner. Structure:
 *   1. Byte-kind map (code/comment/string per offset)
 *   2. Main loop:
 *      a. Newline → classify line, record metrics, detect functions
 *      b. State machine (code / line-comment / block-comment / string)
 *      c. In code: brace tracking, function detection, identifier extraction
 *   3. Finalize (line counts, comment gradient, defect quartiles)
 */

void scan_file(ScanResult *res, const char *filename, const char *content,
               size_t len, LangFamily lang) {
  *res = (ScanResult){};
  res->filename = filename;
  res->content = content;
  res->content_len = len;
  res->lang = lang;
  res->specific = lang_detect_specific(filename);

  if (len == 0)
    return;

  res->byte_kind = calloc(len + 1, 1);
  compute_byte_kind(res->byte_kind, content, len, lang);

  int max_lines = 1;
  for (size_t i = 0; i < len; i++)
    if (content[i] == '\n')
      max_lines++;

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

    /* ── (a) Newline: classify line, record metrics ─── */
    if (c == '\n') {
      int ll = (int)(i - line_start);

      if (!line_has_nonws) {
        res->blank_lines++;
      } else if (first_nonws_cmnt && !line_has_code) {
        res->comment_lines++;
        if (line_num <= max_lines)
          line_is_cmnt[line_num] = 1;
      } else {
        res->code_lines++;
      }

      if (res->line_length_count < max_lines)
        res->line_lengths[res->line_length_count++] = ll;

      if (line_has_code) {
        int ws = 0;
        for (size_t wi = line_start; wi < line_start + (size_t)ll; wi++) {
          if (content[wi] == ' ')
            ws++;
          else if (content[wi] == '\t')
            ws += 4;
          else
            break;
        }
        res->indent_depths[res->indent_count++] = ws;
      }

      if (cmnt_len > 0) {
        check_narration_comment(res, cmnt_buf, cmnt_len, line_num, content,
                                line_start, ll);
        check_defect_markers(cmnt_buf, cmnt_len, defect_buf, &defect_n,
                             max_lines, line_num);
      }

      if (line_has_code && ll < (int)sizeof(prev_line)) {
        memcpy(prev_line, content + line_start, (size_t)ll);
        prev_line_len = ll;
      }

      if (bstack_top > 0 && line_has_code)
        bstack[bstack_top - 1].code_lines++;

      if (lang == LANG_PYTHON && in_func && py_func_indent >= 0 &&
          line_has_nonws) {
        int indent = 0;
        while (indent < ll && content[line_start + (size_t)indent] == ' ')
          indent++;
        if (indent <= py_func_indent && !first_nonws_cmnt) {
          FuncCloseCtx fcc = {res,         &in_func,        &func_camel,
                              &func_snake, func_start_line, cur_func_name,
                              line_start,  line_num - 1};
          close_current_function(&fcc);
          py_func_indent = -1;
        }
      }

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
            py_extract_func_name(content + line_start, ll, fi->name,
                                 sizeof(fi->name));
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

      if (lang == LANG_SHELL && !first_nonws_cmnt) {
        if (sh_is_func_line(content + line_start, ll)) {
          if (!found_body) {
            res->code_body_start = line_num;
            found_body = true;
          }
        }
      }

      line_num++;
      line_start = i + 1;
      line_has_nonws = false;
      first_nonws_cmnt = false;
      line_has_code = false;
      cmnt_len = 0;
      if (state == ST_LINE_COMMENT)
        state = ST_CODE;
      continue;
    }

    /* ── (b) Block-comment line-start detection ─────── */
    if (!line_has_nonws && !isspace((unsigned char)c) &&
        state == ST_BLOCK_COMMENT) {
      first_nonws_cmnt = true;
      line_has_nonws = true;
    }

    /* ── (c) State machine ─────────────────────────── */
    switch (state) {
    case ST_CODE: {
      if (isspace((unsigned char)c))
        break;

      if (!line_has_nonws) {
        line_has_nonws = true;

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

      if (c == '`' && lang == LANG_C_LIKE) {
        str_quote = '`';
        state = ST_STRING;
        continue;
      }

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
          if (brace_depth == 0 || (brace_depth == 1 && !in_func)) {
            int cl_len = (int)(i - line_start);
            const char *cl = content + line_start;

            int fw = 0;
            while (fw < cl_len && isspace((unsigned char)cl[fw]))
              fw++;
            bool line_starts_close = (fw < cl_len && cl[fw] == '}');

            bool has_sig = !line_starts_close &&
                           (line_has_paren(cl, cl_len) ||
                            line_has_paren(prev_line, prev_line_len));
            const char *sig_line = line_has_paren(cl, cl_len) ? cl : prev_line;
            int sig_len = line_has_paren(cl, cl_len) ? cl_len : prev_line_len;

            if (!has_sig && !line_starts_close) {
              const char *back_line;
              int back_len;
              if (scan_back_has_paren(content, line_start, 4, &back_line,
                                      &back_len)) {
                has_sig = true;
                sig_line = back_line;
                sig_len = back_len;
              }
            }

            if (line_is_type_body(cl, cl_len))
              has_sig = false;

            if (has_sig && !line_starts_control(sig_line, sig_len)) {
              if (in_func) {
                FuncCloseCtx fcc = {res,         &in_func,        &func_camel,
                                    &func_snake, func_start_line, cur_func_name,
                                    i,           line_num - 1};
                close_current_function(&fcc);
              }

              if (res->function_count < MAX_FUNCTIONS) {
                FuncInfo *fi = &res->functions[res->function_count];
                fi->body_offset = i + 1;
                int sig_back = 0;
                if (sig_line >= content && sig_line < content + line_start) {
                  for (const char *q = sig_line; q < content + line_start; q++)
                    if (*q == '\n')
                      sig_back++;
                }
                fi->start_line = line_num - sig_back;
                extract_func_name(sig_line, sig_len, fi->name,
                                  sizeof(fi->name));
                if (fi->name[0] == '\0') {
                  extract_arrow_name(cl, cl_len, fi->name, sizeof(fi->name));
                  if (fi->name[0] == '\0')
                    extract_arrow_name(prev_line, prev_line_len, fi->name,
                                       sizeof(fi->name));
                }
                if (fi->name[0] == '\0' && brace_depth == 1)
                  extract_field_name(cl, cl_len, fi->name, sizeof(fi->name));
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
                snprintf(oh->func_name, sizeof(oh->func_name), "%s",
                         cur_func_name);
              }
            }
            bstack_top++;
          }
          pending_blk = BLK_OTHER;
        }

        if (c == '}') {
          brace_depth--;
          if (brace_depth < 0)
            brace_depth = 0;

          if (in_func) {
            if (bstack_top > 0) {
              bstack_top--;
              if (bstack[bstack_top].kind == BLK_TRY)
                try_nest--;
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

      /* ── Keyword / identifier extraction ──────────── */
      if (isalpha((unsigned char)c) || c == '_') {
        int wlen = extract_word(content + i, content + len, word, sizeof(word));
        if (wlen > 0) {
          if ((strcmp(word, "if") == 0) || (strcmp(word, "else") == 0) ||
              (strcmp(word, "elif") == 0) || (strcmp(word, "switch") == 0) ||
              (strcmp(word, "case") == 0) || (strcmp(word, "match") == 0))
            res->conditional_count++;
          if (strcmp(word, "try") == 0)
            pending_blk = BLK_TRY;
          else if (strcmp(word, "catch") == 0 || strcmp(word, "except") == 0) {
            pending_blk = BLK_CATCH;
            if (lang == LANG_PYTHON && strcmp(word, "except") == 0) {
              size_t e = i + (size_t)wlen;
              while (e < len && (content[e] == ' ' || content[e] == '\t'))
                e++;
              if (e < len && content[e] == ':' && defect_n < max_lines)
                defect_buf[defect_n++] = line_num;
            }
          }
          if (in_func && !is_keyword(word) && !is_all_upper(word, wlen)) {
            if (is_camel_case(word, wlen))
              func_camel++;
            else if (is_snake_case(word, wlen))
              func_snake++;
          }

          if (!is_keyword(word) && wlen >= 2 && !is_all_upper(word, wlen)) {
            res->total_identifiers++;
            if (is_generic_name(word))
              res->generic_identifiers++;
            ident_set_insert(&iset, word, wlen);
          }

          i += (size_t)wlen - 1;
        }
      }
      break;
    }

    case ST_LINE_COMMENT:
      if (cmnt_len < (int)sizeof(cmnt_buf) - 1)
        cmnt_buf[cmnt_len++] = c;
      break;

    case ST_BLOCK_COMMENT:
      if (lang == LANG_C_LIKE && c == '*' && i + 1 < len &&
          content[i + 1] == '/') {
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

  if (lang == LANG_PYTHON && in_func) {
    FuncCloseCtx fcc = {res,         &in_func,        &func_camel,
                        &func_snake, func_start_line, cur_func_name,
                        len,         line_num - 1};
    close_current_function(&fcc);
  }

  if (!found_body)
    res->code_body_start = 1;

  res->unique_identifiers = iset.count;
  ident_set_free(&iset);

  scan_finalize(res, content, len, max_lines, line_is_cmnt, defect_buf,
                defect_n);

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
