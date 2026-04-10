#ifndef SCAN_UTIL_H
#define SCAN_UTIL_H

#include "slop.h"

/* ── TTR hash set ────────────────────────────────────────── */

typedef struct {
  char **slots;
  int cap;
  int count;
} IdentSet;

IdentSet ident_set_new(int cap);
void ident_set_insert(IdentSet *is, const char *word, int wlen);
void ident_set_free(IdentSet *is);

/* ── Name classification ─────────────────────────────────── */

bool is_generic_name(const char *word);
bool is_keyword(const char *word);
bool is_word_char(char c);
bool is_camel_case(const char *s, int len);
bool is_snake_case(const char *s, int len);
bool is_all_upper(const char *s, int len);

/* ── String helpers ──────────────────────────────────────── */

bool starts_with_ci(const char *s, int slen, const char *prefix);
int extract_word(const char *s, const char *end, char *buf, int bufsz);

/* ── Line-level queries ──────────────────────────────────── */

bool line_has_paren(const char *line, int len);
bool line_has_word(const char *line, int len, const char *word);
bool line_starts_control(const char *line, int len);
bool line_is_type_body(const char *line, int len);

/* ── Function name extraction ────────────────────────────── */

void extract_func_name(const char *line, int len, char *out, int outsz);
void extract_arrow_name(const char *line, int len, char *out, int outsz);
void extract_field_name(const char *line, int len, char *out, int outsz);
bool py_is_def_line(const char *line, int len, int *out_indent);
void py_extract_func_name(const char *line, int len, char *out, int outsz);
bool sh_is_func_line(const char *line, int len);
bool scan_back_has_paren(const char *content, size_t from, int max_lines,
                         const char **out_line, int *out_len);

/* ── Byte-kind map and post-pass ─────────────────────────── */

void compute_byte_kind(uint8_t *bk, const char *content, size_t len,
                       LangFamily lang);
void scan_finalize(ScanResult *res, const char *content, size_t len,
                   int max_lines, const uint8_t *line_is_cmnt,
                   const int *defect_buf, int defect_n);
void check_narration_comment(ScanResult *res, const char *cmnt_buf,
                             int cmnt_len, int line_num, const char *content,
                             size_t line_start, int ll);
void check_defect_markers(const char *cmnt_buf, int cmnt_len, int *defect_buf,
                          int *defect_n, int max_lines, int line_num);

#endif /* SCAN_UTIL_H */
