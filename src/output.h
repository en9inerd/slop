#ifndef OUTPUT_H
#define OUTPUT_H

#include "slop.h"

/* ── Output formatting ───────────────────────────────────── */

enum { RULE_W = 60, RULE_W_WIDE = 66, RULE_W_REPORT = 70, RULE_W_PARAM = 58 };

void print_rule(int width);
const char *smell_kind_str(SmellKind k);
const char *severity_str(SmellSeverity s);
void print_findings(const char *filename, const SmellReport *r);
const char *score_label(double p);
double slop_score_10(double prob);
void print_signal_table(const ScoreResult *sc, bool with_why);
void print_score(const char *filename, const ScoreResult *sc);

/* ── JSON helpers ────────────────────────────────────────── */

void json_escape(const char *s, char *out, int outsz);
void print_score_json(const char *filename, const ScoreResult *sc,
                      int dead_lines);

/* ── File table ──────────────────────────────────────────── */

typedef struct {
  char path[4096];
  double prob;
  double raw_score;
  int dead_lines;
  ScoreResult sc;
} FileEntry;

int entry_cmp(const void *a, const void *b);
void print_file_table(const FileEntry *entries, int count);

/* ── Duplicate display ───────────────────────────────────── */

void print_dupes_display(const DupeResult *dr, double threshold);

/* ── Report display helpers ──────────────────────────────── */

void report_divider(void);
void report_header(const char *title);
void report_methodology(const Calibration *cal);

/* ── Shared CLI helpers ──────────────────────────────────── */

typedef struct {
  ScanResult scan;
  ScoreResult sc;
  double compression_ratio;
  GitFeatures gf;
  int dead_lines;
} FileScanCtx;

void scan_one_file(FileScanCtx *ctx, const char *path, const char *content,
                   size_t len, const Calibration *cal, double dup_ratio,
                   bool use_git, double prior_llr);
void scan_one_free(FileScanCtx *ctx);
void scoring_init(Calibration *cal, double prior, double *out_prior_llr);
void collect_dupes(DupeResult *dr, const FileList *fl);

/* ── Error helpers ───────────────────────────────────────── */

void err_no_target(void);
void err_cannot_stat(const char *p);
void err_cannot_read(const char *p);
void err_bad_option(const char *o);

/* ── Report command ──────────────────────────────────────── */

int cmd_report(int argc, char **argv);

#endif /* OUTPUT_H */
