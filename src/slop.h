#ifndef SLOP_H
#define SLOP_H

#include <stddef.h>
#include <stdint.h>

/* ── Probability thresholds ──────────────────────────────── */

#define PROB_FLAGGED 0.85
#define PROB_SUSPICIOUS 0.60
#define PROB_INCONCLUSIVE 0.40

/* ── LLR clamping ────────────────────────────────────────── */

#define LLR_CLAMP 3.0
#define LLR_CLAMP_UNCAL 1.5
#define LLR_CLAMP_NOCOMPR 1.5
#define LLR_CLAMP_SMALL 1.0

/* ── Conditional density ─────────────────────────────────── */

#define MIN_COND_KEYWORDS 3

/* ── Positional quality / attention decay ────────────────── */

#define MIN_LINES_DECAY 60
#define MIN_LINES_GRADIENT 50
#define MIN_BODY_LINES 25
#define MIN_DEFECTS_DECAY 4
#define GRADIENT_THRESHOLD 3.0
#define MIN_GRADIENT_CMTS 6
#define SPEARMAN_THRESHOLD 0.5

/* ── Compression / NCD ───────────────────────────────────── */

#define MIN_COMPRESS_BYTES 500
#define NCD_MIN_BODY_BYTES 200
#define NCD_THRESHOLD 0.30
#define ZLIB_LEVEL 6

/* ── Narration tiers ─────────────────────────────────────── */

#define NARRATION_TIER1 3
#define NARRATION_TIER2 8

/* ── File filtering ──────────────────────────────────────── */

#define MAX_AVG_LINE_LEN 200
#define BINARY_PROBE_BYTES 8192

/* ── Duplicate detection ─────────────────────────────────── */

#define NCD_SIZE_RATIO_MAX 2.0
#define MAX_DUP_CLUSTERS 20

/* ── Null-check detection ────────────────────────────────── */

#define NULL_CHECK_WINDOW 10
#define NULL_CHECK_RING_CAP 32

/* ── Identifier specificity ──────────────────────────────── */

#define MIN_IDENTIFIERS 20

/* ── Token diversity ────────────────────────────────────── */

#define MIN_TTR_TOKENS 50
#define TTR_HASH_BUCKETS 2048

/* ── Indentation regularity ─────────────────────────────── */

#define MIN_INDENT_LINES 15

/* ── Function length variance ───────────────────────────── */

#define MIN_FUNCS_CV 5

/* ── Git normalization ───────────────────────────────────── */

#define GIT_MSG_LEN_MIN 10.0
#define GIT_MSG_LEN_SPAN 190.0

/* ── Regularity normalization ranges ─────────────────────── */

#define REG_COMPRESS_LO 0.10
#define REG_COMPRESS_SPAN 0.35
#define REG_ENTROPY_LO 0.30
#define REG_ENTROPY_SPAN 0.60
#define REG_CV_LO 0.15
#define REG_CV_SPAN 0.75

/* ── Line-length histogram ───────────────────────────────── */

#define LINE_LEN_BUCKET_W 5
#define LINE_LEN_BUCKETS 100

/* ── Calibration ─────────────────────────────────────────── */

#define DEFAULT_PRIOR 0.5
#define DEFAULT_TEMPERATURE 2.0
#define MIN_TEMPERATURE 0.1
#define MIN_GAUSS_SIGMA 0.01
#define MIN_BINARY_PROB 0.01
#define ECE_BIN_COUNT 10
#define TEMP_GRID_MIN 0.5
#define TEMP_GRID_MAX 5.0
#define TEMP_GRID_STEP 0.1
#define MIN_CAL_FILES 10

/* ── Language ────────────────────────────────────────────── */

typedef enum LangFamily : uint8_t {
  LANG_C_LIKE,
  LANG_PYTHON,
  LANG_SHELL,
} LangFamily;

typedef enum SpecificLang : uint8_t {
  SPECIFIC_UNKNOWN,
  SPECIFIC_C,
  SPECIFIC_CPP,
  SPECIFIC_JAVA,
  SPECIFIC_JS,
  SPECIFIC_TS,
  SPECIFIC_GO,
  SPECIFIC_RUST,
  SPECIFIC_SWIFT,
  SPECIFIC_PYTHON,
  SPECIFIC_SHELL,
  SPECIFIC_OTHER,
} SpecificLang;

LangFamily lang_detect(const char *filename);
SpecificLang lang_detect_specific(const char *filename);
const char *lang_family_name(LangFamily f);
bool lang_is_source_ext(const char *filename);

/* ── Scanner ─────────────────────────────────────────────── */

#define MAX_NARRATION 128
#define MAX_FUNCTIONS 4096
#define MAX_OVERWRAPS 64
#define MAX_NAME_BREAKS 64

typedef struct {
  int line;
  char text[256];
} NarrationHit;

typedef struct {
  int start_line;
  int end_line;
  size_t body_offset;
  size_t body_len;
  char name[128];
} FuncInfo;

typedef struct {
  int line;
  int depth;
  char func_name[128];
} OverwrapHit;

typedef struct {
  int line;
  char func_name[128];
  int camel_pct;
  int snake_pct;
} NameBreakHit;

typedef struct {
  const char *filename;
  const char *content;
  size_t content_len;
  LangFamily lang;
  SpecificLang specific;

  int total_lines;
  int blank_lines;
  int comment_lines;
  int code_lines;

  NarrationHit narration[MAX_NARRATION];
  int narration_count;

  int code_body_start;
  int body_comment_top;
  int body_lines_top;
  int body_comment_bottom;
  int body_lines_bottom;

  FuncInfo functions[MAX_FUNCTIONS];
  int function_count;

  int conditional_count;

  OverwrapHit overwraps[MAX_OVERWRAPS];
  int overwrap_count;

  NameBreakHit name_breaks[MAX_NAME_BREAKS];
  int name_break_count;

  int *line_lengths;
  int line_length_count;

  int defects_per_quartile[4];
  int total_defects;

  int total_identifiers;
  int generic_identifiers;
  int unique_identifiers;

  int *indent_depths;
  int indent_count;

  uint8_t *byte_kind; /* 0=code, 1=line-comment, 2=string, 3=block-comment */
} ScanResult;

void scan_file(ScanResult *res, const char *filename, const char *content,
               size_t len, LangFamily lang);
void scan_free(ScanResult *res);

/* ── Smells ──────────────────────────────────────────────── */

typedef enum SmellSeverity : uint8_t {
  SEV_DIAGNOSTIC,
  SEV_CORRELATED,
  SEV_GENERAL,
} SmellSeverity;

typedef enum SmellKind : uint8_t {
  SMELL_NARRATION,
  SMELL_COMMENT_GRADIENT,
  SMELL_REDUP,
  SMELL_OVERWRAP,
  SMELL_NAME_BREAK,
  SMELL_AS_ANY,
  SMELL_TS_DIRECTIVE,
  SMELL_ERR_SUPPRESS,
  SMELL_ZOMBIE_PARAM,
  SMELL_UNUSED_IMPORT,
  SMELL_MAGIC_STRING,
  SMELL_DEAD_CODE,
} SmellKind;

typedef struct {
  SmellKind kind;
  SmellSeverity severity;
  int line;
  char message[512];
} SmellFinding;

#define MAX_FINDINGS 512

typedef struct {
  SmellFinding items[MAX_FINDINGS];
  int count;
} SmellReport;

void smell_detect(SmellReport *report, const ScanResult *scan,
                  bool include_general);
int smell_count_dead_lines(const ScanResult *scan);

/* ── Git Features ────────────────────────────────────────── */

typedef struct {
  double multiline_ratio;
  double conventional_ratio;
  double avg_msg_length;
  double composite;
  bool available;
} GitFeatures;

void git_features(GitFeatures *out, const char *filepath);

/* ── Calibration ─────────────────────────────────────────── */

typedef struct {
  double mu_ai, sig_ai, mu_h, sig_h;
} GaussParam;

typedef struct {
  double temperature;
  GaussParam regularity;
  GaussParam ccr;
  GaussParam cond;
  GaussParam dup;
  GaussParam git;
  GaussParam ident;
  GaussParam func_cv;
  GaussParam ttr;
  GaussParam indent;
  double narr_p_ai, narr_p_h;
  double decay_p_ai, decay_p_h;
  double overwrap_p_ai, overwrap_p_h;
  double namebrk_p_ai, namebrk_p_h;
  bool loaded;
} Calibration;

void calibration_default(Calibration *cal);
[[nodiscard]] bool calibration_load(Calibration *cal, const char *dir);
[[nodiscard]] bool calibration_save(const Calibration *cal, const char *dir);

/* ── Score ───────────────────────────────────────────────── */

#define MAX_SIGNAL_DETAILS 16

typedef struct {
  const char *group;
  const char *signal;
  char measured[48];
  double llr;
} SignalDetail;

typedef struct {
  double raw_score;
  double probability;
  double temperature;
  int signal_count;
  SignalDetail details[MAX_SIGNAL_DETAILS];

  double m_regularity;
  double m_ccr;
  int m_narration_count;
  double m_cond_density;
  bool m_decay;
  double m_dup_ratio;
  double m_git_composite;
  double m_ident_spec;
  double m_func_cv;
  double m_ttr;
  double m_indent_reg;
} ScoreResult;

void score_compute(ScoreResult *out, const ScanResult *scan,
                   const Calibration *cal, double compression_ratio,
                   double dup_ratio, const GitFeatures *git);

/* ── Compression / NCD ───────────────────────────────────── */

[[nodiscard]] double compress_ratio(const char *data, size_t len);
[[nodiscard]] double compress_ncd(const char *x, size_t xlen, const char *y,
                                  size_t ylen);

/* ── Duplicates ──────────────────────────────────────────── */

typedef struct {
  char filepath[4096];
  char name[128];
  int start_line;
  int line_count;
  char *body;
  size_t body_len;
  LangFamily lang;
  int cluster;
} DupeFunc;

typedef struct {
  int a, b;
  double ncd;
} DupePair;

typedef struct {
  DupeFunc *funcs;
  int func_count;
  int func_cap;

  DupePair *pairs;
  int pair_count;
  int pair_cap;

  int *parent;
  int cluster_count;
} DupeResult;

void dupes_init(DupeResult *dr);
void dupes_add_file(DupeResult *dr, const char *filepath, const char *content,
                    size_t len);
void dupes_compute(DupeResult *dr, double threshold);
void dupes_free(DupeResult *dr);

/* ── File Walking ────────────────────────────────────────── */

typedef struct {
  char **paths;
  int count;
  int cap;
} FileList;

void fl_init(FileList *fl);
void fl_add(FileList *fl, const char *path);
void fl_free(FileList *fl);
void fl_collect(FileList *fl, const char *dirpath);

/* ── Util ────────────────────────────────────────────────── */

[[nodiscard]] char *util_read_file(const char *path, size_t *out_len);
bool util_is_binary(const char *content, size_t len);
bool util_is_minified(const char *content, size_t len);
bool util_should_skip(const char *content, size_t len);

/* ── High-Level Library API ──────────────────────────────── */

typedef struct {
  bool include_general_smells; /* include GENERAL-severity smells (default:
                                  false) */
  bool use_git; /* include git commit pattern signal (default: false) */
  double prior; /* P(AI) prior, 0.5 = uninformative (default: 0.5) */
  const char *calibration_dir; /* directory containing .slop-calibration.json,
                                  or nullptr */
} SlopOptions;

typedef struct {
  char filepath[4096];
  double probability;
  double raw_score;
  int dead_lines;
  ScoreResult score;
  SmellReport smells;
  bool skipped; /* true if file was binary/minified/generated */

  int total_lines;
  int code_lines;
  int comment_lines;
  int blank_lines;
  int function_count;
  LangFamily lang;
} SlopFileResult;

#define SLOP_MAX_PROJECT_FILES 8192

typedef struct {
  SlopFileResult *files;
  int file_count;

  DupeResult dupes;
  double dup_ratio;
  int funcs_in_dup;
  int total_funcs;

  Calibration calibration;

  int flagged;
  int suspicious;
  int human;
  int skipped;
} SlopProjectResult;

void slop_options_default(SlopOptions *opts);

[[nodiscard]] SlopFileResult slop_analyze_file(const char *path,
                                               const SlopOptions *opts);
[[nodiscard]] SlopProjectResult slop_analyze_dir(const char *dirpath,
                                                 const SlopOptions *opts);

void slop_file_result_free(SlopFileResult *r);
void slop_project_result_free(SlopProjectResult *r);

/* ── Shared math (inline, used by main.c and api.c) ──────── */

#include <math.h>

static inline double slop_sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }

static inline double slop_prior_llr(double prior) {
  if (prior > 0.01 && prior < 0.99 && fabs(prior - 0.5) > 0.001)
    return log(prior / (1.0 - prior));
  return 0;
}

/* ── Duplicate ratio (shared between main.c and api.c) ───── */

typedef struct {
  double ratio;
  int funcs_in_dup;
  int total_funcs;
} DupRatioResult;

[[nodiscard]] DupRatioResult dupes_compute_ratio(const DupeResult *dr);

#endif /* SLOP_H */
