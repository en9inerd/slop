# slop — code quality scanner

Fast CLI tool that catches code quality patterns linters miss: narration
comments, dead code, naming drift, cross-file duplicates, and more.
No neural networks, no API calls, no cloud. Single static binary.

Written in C23. Built with Zig. Zero dependencies at runtime.

## Table of Contents

- [Install](#install)
  - [Homebrew](#homebrew-macos--linux)
  - [Debian / Ubuntu](#debian--ubuntu-deb)
  - [Quick Install](#quick-install-any-platform)
  - [Build from Source](#build-from-source)
- [Quick Start](#quick-start)
- [Commands](#commands)
  - [slop check](#slop-check)
  - [slop scan](#slop-scan)
  - [slop report](#slop-report)
  - [slop dupes](#slop-dupes)
- [Detection Algorithms](#detection-algorithms)
  - [Narration Comments](#narration-comments)
  - [Comment Density Gradient](#comment-density-gradient)
  - [Defensive Over-Wrapping](#defensive-over-wrapping)
  - [Naming Convention Break](#naming-convention-break)
  - [Redundant Re-Implementation](#redundant-re-implementation)
- [Scoring Algorithm](#scoring-algorithm)
  - [Bayesian Framework](#bayesian-framework)
  - [Signal Groups (A–J)](#signal-groups-aj)
  - [Temperature Scaling](#temperature-scaling)
  - [Worked Example](#worked-example)
- [Duplicate Detection Algorithm](#duplicate-detection-algorithm)
  - [NCD Math](#ncd-math)
  - [NCD Limitations](#ncd-limitations)
  - [Dupe Pipeline](#dupe-pipeline)
- [Scanner Internals](#scanner-internals)
- [Language Support](#language-support)
- [File Filtering](#file-filtering)
- [Exit Codes](#exit-codes)
- [CI Integration](#ci-integration)
- [Library API](#library-api)
- [Cross-Compilation](#cross-compilation)
- [Tuning](#tuning)
  - [Default Parameter Reference](#default-parameter-reference)
- [Honest Assessment](#honest-assessment)
- [Project Structure](#project-structure)
- [References](#references)

## Install

### Homebrew (macOS / Linux)

```bash
brew install en9inerd/tap/slop
```

### Debian / Ubuntu (.deb)

```bash
# Download the latest .deb for your architecture (amd64 or arm64)
curl -sLO "https://github.com/en9inerd/slop/releases/latest/download/slop_VERSION_amd64.deb"
sudo dpkg -i slop_*.deb
```

### Quick install (any platform)

```bash
curl -sL https://raw.githubusercontent.com/en9inerd/slop/master/scripts/install.sh | bash
```

### Build from source

Requires [Zig](https://ziglang.org/download/) (0.14+) and zlib (system library,
pre-installed on macOS and most Linux distros).

```bash
make              # debug build
make release      # optimized build
make test         # run 86 integration tests
make install      # install to /usr/local (binary + library + header)
```

Or use Zig directly:

```bash
zig build                                 # debug
zig build -Doptimize=ReleaseFast          # release
zig build test                            # run tests
zig build run -- scan ./src/              # run with arguments
```

The Makefile is a thin wrapper — every target delegates to `zig build`.

## Quick Start

```bash
# Find code quality issues (primary command)
slop check ./src/

# Find duplicate functions across a project
slop dupes ./src/

# Score files by overall code quality (0-10 slop score)
slop scan ./src/

# Full quality report with methodology explanation
slop report ./src/handler.ts
```

## Commands

```
slop check  [--all] <file|dir>              Find code quality issues (primary)
slop scan   [options] <file|dir>            Score files (slop 0-10)
slop report [options] <file|dir>            Full quality report
slop dupes  [options] <dir>                 Find duplicate functions (NCD)
```

Aliases: `smell` = `check`.

### slop check

Finds code quality patterns that standard linters miss. Works on single
files or entire directories. Three severity levels:

**Strong tells** — near-zero false positive rate, not caught by existing linters:

- **Narration comments** — "First we...", "Now we...", "Step 1:..." at the
  start of comment text. Comments that narrate the obvious rather than
  explaining intent.
- **Comment density gradient** — comment density drops 3x+ between the first
  and second half of the code body. Indicates uneven attention.

**Score-related** — common quality issues that also affect the slop score:

- **Redundant re-implementation** — two functions in the same file with
  NCD < 0.30 (near-duplicate bodies, ≥ 200 bytes each).
- **Defensive over-wrapping** — try/catch nested 3+ deep, or duplicate
  null checks on the same variable within 10 lines.
- **Naming convention break** — mixed camelCase and snake_case within a
  single function (>25% share each).
- **`as any` casts** (TypeScript) — type safety bypasses.
- **`@ts-ignore` / `@ts-nocheck` / `@ts-expect-error`** (TypeScript) — type checking suppression.
- **`# type: ignore` / `# noqa`** (Python) — type/lint suppression directives.
- **`_ = err` suppression** (Go) — discarded error return values, including
  multi-return `result, _ := func()` patterns.

**General** (behind `--all` flag) — real issues that other linters may also catch:

- **Zombie parameters** — function parameters never referenced in the body.
- **Unused imports** — imported names not used elsewhere in the file.
- **Magic string repetition** — same string literal (≥ 8 chars) appearing 3+ times.
- **Dead code** — functions defined but never referenced within the file,
  with line counts. Language-aware: respects Go exports, TS/JS `export`,
  Python public functions, C/C++ non-`static` linkage, and common entry
  points (`main`, `init`, `render`, `setup`, `teardown`, Go `Test*`/`Benchmark*`/`Example*`).

| Flag | Description |
|------|-------------|
| `--all` | Include general issues (zombie params, unused imports, dead code) |

```
$ slop check src/api/handlers.ts

  src/api/handlers.ts — 15 findings

  strong tell
  :1   narration        "// First, we import the necessary modules"
  :6   narration        "// Now we define the interface for our handler"
  :13  narration        "// Step 1: Create the validation function"
  —    comment-decay    top half 20% comments, bottom half 5% (gradient 4.0x)

  score-related
  :56  over-wrap        try/catch nested 3 deep in createUser()

$ slop check --all src/
  12 files scanned, 4 with findings (23 total)
```

### slop scan

Scores files using statistical checks across up to thirteen signal groups
(9 Gaussian, 4 binary). Output is a **slop score from 0 to 10**: higher = sloppier.

| Signal | Type | What it measures |
|--------|------|-----------------|
| Regularity | Gaussian | Compression ratio, line-length entropy, line-length CV |
| Comment ratio | Gaussian | Comment lines / code lines |
| Narration | Binary | Count of narration-style comments (≥3 = present) |
| Conditional density | Gaussian | Conditional keyword density |
| Quality decay | Binary | Defect/comment trend over file position (Spearman ρ) |
| Over-wrapping | Binary | Defensive over-wrapping (try/catch depth >2 or redundant null checks) |
| Naming break | Binary | Mixed naming conventions within a function |
| Identifier specificity | Gaussian | Ratio of generic names to total identifiers |
| Function length CV | Gaussian | Coefficient of variation of function body lengths |
| Token diversity (TTR) | Gaussian | Unique identifiers / total identifiers |
| Indentation regularity | Gaussian | CV of leading whitespace depths |
| Dupes | Gaussian | Duplicate function ratio across project (directory mode only) |
| Git | Gaussian | Commit message patterns (opt-in with `--git`) |

**Directory mode** performs a two-pass scan: pass 1 collects all functions
and computes the project-wide duplicate ratio via NCD, pass 2 scores each
file using the real `dup_ratio`.

| Flag | Description |
|------|-------------|
| `--verbose` | Per-file signal breakdown with explanations |
| `--json` | JSON output (for CI pipelines and tooling) |
| `--git` | Include git commit patterns |
| `--stdin --lang=LANG` | Read from stdin (`js`, `ts`, `go`, `python`, `shell`) |

```
$ slop scan --verbose src/api/handlers.ts

  src/api/handlers.ts — slop 7.9/10  [moderate]

  Check (category / name)                  Value              weight
  ------------------------------------------------------------------
  regularity / composite                   0.22               -1.49
  comments / comment-to-code               0.19               +0.56
  comments / narration                     yes (13 hits)      +1.50
  structure / conditional dens.            0.09               -0.43
  position / quality decay                 detected           +1.50
  patterns / over-wrapping                 1 hit              +1.50
  patterns / naming breaks                 none               -0.14
  naming / ident. specificity              0.18 (26/146)      +1.50
  structure / func length CV               0.75 (5 funcs)     -0.85
  naming / token diversity                 0.33 (48/146)      +0.34
  whitespace / indent regularity           0.84 (70 lines)    -1.29
  ------------------------------------------------------------------
                             raw total +2.71
                           scaled (/T) +1.35

$ slop scan ./src/

  scanned 48 files
  dup_ratio = 0.05 (7/142 functions)

  slop    raw     dead  file
  ------------------------------------------------------------
  9.5     +5.83   13    src/api/handlers.ts
  8.5     +3.46   -     src/utils/format.ts
  ...
  0.6     -5.44   14    src/helpers/input.helper.ts
  ------------------------------------------------------------

  3 sloppy / 5 moderate / 40 clean
  267 dead lines detected across project
```

### slop report

Full quality report combining all analyses with methodology explanation.
Includes per-file signal breakdown, findings, dead code, dupes, and the
scoring formula.

| Flag | Description |
|------|-------------|
| `--git` | Include git commit patterns |

### slop dupes

Finds near-duplicate functions across an entire project using NCD.

| Flag | Description |
|------|-------------|
| `--threshold=N` | NCD threshold (default 0.30, lower = stricter) |
| `--cross-lang` | Compare across language families |

```
$ slop dupes ./src/

  collected 142 functions (>= 200 bytes) from ./src/

  2 clusters (NCD < 0.30, min body 200 bytes)

  cluster 1 — NCD 0.08 (nearly identical)
    src/utils/dates.ts:12    formatDate()       [18 lines]
    src/api/serialize.ts:45  formatTimestamp()   [16 lines]

  cluster 2 — NCD 0.24
    src/api/auth.ts:30       validateToken()    [12 lines]
    src/middleware/auth.ts:15 checkAuth()        [14 lines]
```

---

## Detection Algorithms

### Narration Comments

LLMs narrate code like tutorials: "First we...", "Now we...", "Finally..."
Human developers explain *why*, not *what*. This is the single most reliable
AI code fingerprint — near-zero false positive rate (~1-2%).

**Marker list** (matched case-insensitive at the START of comment text,
after stripping `//`, `#`, `*` prefix and leading whitespace):

```
"First we", "First,",
"Now we",   "Now,",    "Now let",
"Next we",  "Next,",
"Then we",  "Then,",
"Finally we", "Finally,",
"Here we",  "Here,",
"Let's",    "Let me",
"We need to", "We can ",
"We'll",    "We will",
"Step 1", "Step 2", "Step 3"
```

The markers `"Let's"`, `"We need to"`, `"We can "`, `"We'll"`, and `"We will"`
were added because these are overwhelmingly AI narration patterns when found
at the start of code comments. Humans rarely start inline comments with
first-person plural pronouns.

**Why START-of-comment only:** "Handle first-time setup" must NOT match.
"// First, validate input" DOES match.

**Why it works:** LLMs are trained on tutorials and documentation. Their
default mode is pedagogical — they explain what code does step by step.
Experienced developers never start a comment with "First we validate" in
production code because the code itself communicates the "what."

### Comment Density Gradient

Comment density drops sharply in the second half of AI-generated files. The
model "forgets" to comment as context grows — a direct, measurable consequence
of attention decay during generation.

**"Code body"** is defined as everything after the first function definition
(or after the first non-import, non-comment code line if no functions are
detected). This skips the preamble — module docstrings, license headers,
and import-section comments naturally cluster at the top of human files too.

Within the code body:

```
density_top    = comment_lines_in_first_half / lines_in_first_half
density_bottom = comment_lines_in_second_half / lines_in_second_half
gradient       = density_top / max(density_bottom, 0.001)
```

Flag if ALL conditions hold:
- `gradient > 3.0` (top half has 3x+ more comments)
- Code body has ≥ 50 lines (enough data for meaningful halves)
- Total comments in body ≥ 6 (file is commented, not just sparse)

**Why the preamble skip matters:** Without it, any file with a standard
license header would false-positive as "comment decay."

**Why no linter catches this:** Linters check comment presence/absence,
not positional distribution. This is a statistical property of the file.

### Defensive Over-Wrapping

AI doesn't know what callers guarantee, so it wraps everything defensively.

Two sub-detectors:
1. **Try/catch nesting:** Track try/catch/except depth per function. Flag depth > 2.
2. **Redundant null checks:** Track null/undefined/None checks. Flag checking
   the same variable twice within 10 lines. Uses a ring buffer of recent
   check targets (capacity 32).

### Naming Convention Break

Within a single function: mixed camelCase and snake_case identifiers. Happens
at "generation boundaries" where one AI generation pass ends and another begins.

**Classification:**
- camelCase: matches `[a-z][a-zA-Z]*` with at least one uppercase letter
- snake_case: matches `[a-z]+(_[a-z]+)+`

Flag if both styles have >25% share within the same function. Within a single
function, a human almost always uses one consistent style.

### Redundant Re-Implementation

Within a single file: two functions that do essentially the same thing. This
is the within-file version of the cross-file duplicate problem detected by
`slop dupes`.

For all pairs of functions within the same file, compute NCD. Flag pairs with
NCD < 0.30 and body size ≥ 200 bytes.

Why CORRELATED not DIAGNOSTIC: AI can usually "see" recent context within the
same file. This mainly occurs in very long files (500+ lines) where earlier
functions fall out of the model's effective attention window.

---

## Scoring Algorithm

### Bayesian Framework

Given observed measurements **x** = (x₁...xₙ):

```
              P(slop)            P(xᵢ | slop)
S = log ──────────── +  Σᵢ log ──────────────
              P(clean)           P(xᵢ | clean)
         └─────┬─────┘      └──────┬────────┘
           prior s₀          signal LLR sᵢ
```

Default prior: P(slop) = 0.5 (uninformative). s₀ = log(0.5/0.5) = 0, so the
prior term vanishes. Override with `--prior=0.7` to add s₀ = +0.85 to every
file's raw score.

**Continuous signal (Gaussian model):**

```
              ⎡ (x − μH)²    (x − μAI)² ⎤         σH
sᵢ(x) =  ½ · ⎢ ────────── − ────────── ⎥ + log ─────
              ⎣    σH²          σAI²     ⎦         σAI
```

**Binary signal (present/absent):**

```
                   p_slop                      1 − p_slop
sᵢ(1) = log ────────────       sᵢ(0) = log ──────────────
                   p_clean                     1 − p_clean
```

**LLR clamping** prevents any single signal from dominating:

| Mode | Clamp range | When active |
|------|-------------|-------------|
| Tuned | ±3.0 (`LLR_CLAMP`) | `.slop-calibration.json` loaded and valid |
| Default | ±1.5 (`LLR_CLAMP_UNCAL`) | No calibration file |
| No compression | ±1.5 (`LLR_CLAMP_NOCOMPR`) | File < 500 bytes (regularity signal unreliable) |
| Small file | ±1.0 (`LLR_CLAMP_SMALL`) | Files < 50 lines |

The default clamp (±1.5) is deliberately conservative: since all μ/σ/p
values are educated guesses, limiting individual LLR prevents a single
mis-estimated signal from producing extreme scores. With a tuning file,
the wider ±3.0 clamp allows stronger signals to have more influence.

### Signal Groups (A–J)

**Group A — Regularity** (1 composite LLR, confidence: MODERATE)

Three measurements normalized to [0,1] and averaged:

```
Compression ratio:   R = C(code) / |code|         (skip if < 500 bytes)
Line-length entropy: H_norm = H / log₂(buckets)   (5-char buckets)
Line-length CV:      CV = stdev / mean

r_norm = 1 − clamp((R − 0.10) / 0.35, 0, 1)
h_norm = 1 − clamp((H_norm − 0.30) / 0.60, 0, 1)
c_norm = 1 − clamp((CV − 0.15) / 0.75, 0, 1)

regularity = (r_norm + h_norm + c_norm) / 3       ∈ [0,1], higher = more sloppy
```

Default parameters: Sloppy(μ=0.50, σ=0.18), Clean(μ=0.20, σ=0.13).

Weakest signal group — formatters (prettier, black, gofmt) homogenize code
and reduce the gap. Grouped into a single composite so it can't contribute
more than one clamped LLR.

**Group B1 — Comment-to-code ratio** (1 Gaussian LLR, confidence: HIGH)

```
CCR = comment_lines / max(code_lines, 1)
```

Where `code_lines = total_lines − blank_lines − comment_lines`. Lines with
trailing comments (`x = 5 // init`) count as code, not comment. Guard: if
`code_lines = 0`, skip all signals and report "insufficient code."

Default parameters: Sloppy(μ=0.25, σ=0.15), Clean(μ=0.04, σ=0.10).

**Group B2 — Narration** (1 binary LLR, confidence: HIGHEST)

```
has_narration = 1  if narration_match_count ≥ 3
```

Count-based, not ratio-based: 3+ distinct narration comments is strong
evidence regardless of total comment count.

Default parameters: p_slop = 0.40, p_clean = 0.01.
LLR when present: log(0.40/0.01) = +3.69 (clamped to LLR_CLAMP).
LLR when absent: log(0.60/0.99) = −0.50.

**Group C — Conditional density** (1 Gaussian LLR, confidence: MEDIUM-HIGH)

```
CD = count(if, else, elif, switch, case, match) / code_lines
```

Only keyword occurrences — NOT ternary `?` (ambiguous with optional
chaining `?.` in JS/TS).

Default parameters: Sloppy(μ=0.18, σ=0.10), Clean(μ=0.10, σ=0.10).

**Minimum keyword threshold:** If a file contains fewer than 3 conditional
keywords (`if`, `else`, `elif`, `switch`, `case`, `match`), the conditional
density signal is skipped entirely (LLR=0, status "n/a"). This prevents
small utility files and DB CRUD code with few conditionals from receiving a
strong negative LLR that incorrectly drags the score down.

Wide σ (0.10 for both classes) reflects that conditional density is language-
dependent — C code with heavy control flow naturally has higher density than
TypeScript, making this signal inherently noisy across languages.

**Group D — Positional quality** (1 binary LLR, confidence: MEDIUM-HIGH)

Only for files ≥ 60 lines. Divide file into K=4 quartiles. Per quartile,
count quality defects (TODO, FIXME, empty catch, bare except). Compute
defect rate D(block) = defects / lines_in_block.

Spearman rank correlation between position (1,2,3,4) and D:

```
            6 · Σ dᵢ²
ρ = 1 − ─────────────       (K=4, so denominator = 60)
            K(K² − 1)
```

quality_decay = 1 if EITHER condition fires:

- **Condition A (defect gradient):** ρ > +0.5 AND total defects ≥ 4
- **Condition B (comment gradient):** gradient > 3.0 AND body ≥ 50 lines AND body comments ≥ 6

Condition B ensures the DIAGNOSTIC smell (comment decay) also contributes
to the probability score.

Default parameters: p_slop = 0.25, p_clean = 0.05.

**Group D2 — Over-wrapping score** (1 binary LLR, confidence: MEDIUM-HIGH)

Fires when `slop check` detects defensive over-wrapping patterns:
try/catch nesting >2 levels deep, or redundant null checks on the same
variable within 10 lines.

```
has_overwrap = 1  if overwrap_count ≥ 1
```

Default parameters: p_slop = 0.18, p_clean = 0.01.
LLR when present: log(0.18/0.01) = +2.89 (clamped to LLR_CLAMP).
LLR when absent: log(0.82/0.99) = −0.19.

This is a **pattern-based signal** — it doesn't depend on Gaussian μ/σ
estimates, making it less sensitive to parameter tuning.

**Group D3 — Naming break score** (1 binary LLR, confidence: MEDIUM)

Fires when `slop check` detects mixed naming conventions (camelCase +
snake_case, each >25% share) within a single function.

```
has_namebrk = 1  if naming_break_count ≥ 1
```

Default parameters: p_slop = 0.15, p_clean = 0.02.
LLR when present: log(0.15/0.02) = +2.01.
LLR when absent: log(0.85/0.98) = −0.14.

Also pattern-based — less sensitive to parameter tuning.

**Group G — Identifier specificity** (1 Gaussian LLR, confidence: MEDIUM-HIGH)

Sloppy code over-uses generic variable names: `data`, `result`, `response`,
`handler`, `value`, `config`, `options`, `params`, `callback`, `helper`,
`manager`, `service`, `controller`, `processor`, etc. (38 entries in the
dictionary).

```
ident_spec = generic_identifiers / total_identifiers
```

Skipped if the file has fewer than 20 identifiers (`MIN_IDENTIFIERS`).
Keywords and ALL_CAPS constants are excluded from the count.

Default parameters: Sloppy(μ=0.18, σ=0.08), Clean(μ=0.07, σ=0.04).

**Group H — Function length CV** (1 Gaussian LLR, confidence: MEDIUM)

Sloppy code tends to have functions of similar length (low variance). Clean
code has more natural variation — some short helpers, some long complex functions.

```
CV = stdev(function_line_counts) / mean(function_line_counts)
```

Skipped if fewer than 5 functions detected (`MIN_FUNCS_CV`).

Default parameters: Sloppy(μ=0.40, σ=0.20), Clean(μ=0.90, σ=0.35).

**Group I — Token diversity (TTR)** (1 Gaussian LLR, confidence: MEDIUM-HIGH)

Type-Token Ratio measures naming vocabulary richness. Sloppy code reuses the
same identifiers more often, producing lower TTR. Clean code tends to use
more diverse, context-specific names.

```
TTR = unique_identifiers / total_identifiers
```

Unique identifiers are tracked via an FNV-1a hash set (2048 buckets).
Skipped if fewer than 50 identifier tokens (`MIN_TTR_TOKENS`).

Default parameters: Sloppy(μ=0.35, σ=0.15), Clean(μ=0.40, σ=0.20).

Wide σ and close means reflect that TTR is language-dependent — C/Go code
naturally reuses identifiers heavily (producing TTR 0.11–0.39 in human code),
while JS/Python code tends toward higher diversity. The signal provides mild
directional evidence rather than strong discrimination.

Based on type-token ratio research from computational linguistics, adapted
for source code identifier analysis.

**Group J — Indentation regularity** (1 Gaussian LLR, confidence: HIGH)

Sloppy code has very uniform indentation — consistent depth, regular
blank-line spacing. Clean code has more organic variation. This is the
strongest whitespace-based signal, validated by Nirob et al. 2025
("Whitespaces Don't Lie") as the #1 discriminative feature for
distinguishing clean from sloppy code.

```
indent_cv = stdev(leading_whitespace_per_code_line) / mean(leading_whitespace_per_code_line)
```

Leading whitespace is counted in spaces (tabs = 4 spaces). Only non-blank
code lines are included. Skipped if fewer than 15 code lines
(`MIN_INDENT_LINES`).

Default parameters: Sloppy(μ=0.45, σ=0.22), Clean(μ=0.70, σ=0.25).
Low CV = uniform indentation = more sloppy. High CV = varied = more clean.

**Group E — Duplicate ratio** (1 Gaussian LLR, confidence: HIGHEST)

```
dup_ratio = |{functions with NCD < 0.3 match elsewhere}| / |{all functions}|
```

Default parameters: Sloppy(μ=0.22, σ=0.12), Clean(μ=0.10, σ=0.15).

σ_H = 0.15 reflects that human codebases DO have some duplication — legacy
code, test boilerplate, copy-paste during refactoring. The wide σ prevents
normal duplication from being penalized too heavily.

- `slop scan FILE` (single file): Group E is SKIPPED (no project context).
- `slop scan DIR` (directory): Group E is INCLUDED, computed in pass 1.

**Group F — Git patterns** (1 composite LLR, opt-in `--git`, confidence: HIGH when applicable)

**Disabled by default.** Git commit messages describe the committer's habits,
not the code's origin. When humans commit AI-generated code (the common case),
the Git signal provides strong FALSE NEGATIVE evidence (up to −2.24), actively
suppressing the score. Only useful when the committer is the AI agent itself.

From `git log -20 --format="%B%x00" -- <file>`:

```
Multiline ratio:     m = count(commits with '\n') / total
Conventional ratio:  c = count(matching ^(feat|fix|chore|docs|...)(\(.*\))?:/) / total
Avg message length:  l = Σ len(message) / total

m_norm = clamp(m, 0, 1)
c_norm = clamp(c, 0, 1)
l_norm = clamp((l − 10) / 190, 0, 1)

git_score = (m_norm + c_norm + l_norm) / 3
```

Default parameters: Sloppy(μ=0.55, σ=0.20), Clean(μ=0.25, σ=0.22).

### Temperature Scaling

Following Guo et al. 2017, the raw score is divided by temperature T
before applying sigmoid:

```
score = σ(S / T) = 1 / (1 + exp(−S/T))
```

- T = 1.0: standard naive Bayes (overconfident with correlated signals)
- T = 2.0: recommended default (halves confidence)
- T can be overridden via `.slop-calibration.json`

**Why temperature scaling:** Naive Bayes assumes signal independence. Our
signals ARE grouped to reduce correlation, but residual correlation remains
(e.g., high comment ratio weakly correlates with narration presence). A
single temperature parameter corrects the overall confidence level.

### Worked Example

File: 180 lines, directory mode. Measurements: regularity=0.45, CCR=0.20,
4 narration hits, CD=0.11, quality decay present, overwrap detected,
no naming breaks, ident_spec=0.15, func_cv=0.50, TTR=0.30,
indent_cv=0.50, dup_ratio=0.08. **Default mode** (LLR clamp = ±1.5).

**Group A (regularity=0.45):**

```
s_A = 0.5×[(0.45−0.20)²/0.13² − (0.45−0.50)²/0.18²] + log(0.13/0.18)
    = 0.5×[3.698 − 0.077] + (−0.326) = +1.49
```

**Group B1 (CCR=0.20):**

```
s_B1 = 0.5×[(0.20−0.04)²/0.10² − (0.20−0.25)²/0.15²] + log(0.10/0.15)
     = 0.5×[2.56 − 0.111] + (−0.405) = +0.82
```

**Group B2 (narration present):**

```
s_B2 = log(0.40/0.01) = +3.69 → clamped to +1.50
```

**Group C (CD=0.11, ≥3 keywords so signal fires):**

```
s_C = 0.5×[(0.11−0.10)²/0.10² − (0.11−0.18)²/0.10²] + log(0.10/0.10)
    = 0.5×[0.01 − 0.49] + 0 = −0.24
```

**Group D1 (decay present):** s_D1 = log(0.25/0.05) = +1.61 → clamped to +1.50
**Group D2 (overwrap present):** s_D2 = log(0.18/0.01) = +2.89 → clamped to +1.50
**Group D3 (no naming break):** s_D3 = log(0.85/0.98) = −0.14

**Group G (ident_spec=0.15):**

```
s_G = 0.5×[(0.15−0.07)²/0.04² − (0.15−0.18)²/0.08²] + log(0.04/0.08)
    = 0.5×[4.00 − 0.141] + (−0.693) = +1.24
```

**Group H (func_cv=0.50):**

```
s_H = 0.5×[(0.50−0.90)²/0.35² − (0.50−0.40)²/0.20²] + log(0.35/0.20)
    = 0.5×[1.306 − 0.25] + 0.559 = +1.09
```

**Group I (TTR=0.30):**

```
s_I = 0.5×[(0.30−0.40)²/0.20² − (0.30−0.35)²/0.15²] + log(0.20/0.15)
    = 0.5×[0.25 − 0.111] + 0.288 = +0.36
```

**Group J (indent_cv=0.50):**

```
s_J = 0.5×[(0.50−0.70)²/0.25² − (0.50−0.45)²/0.22²] + log(0.25/0.22)
    = 0.5×[0.64 − 0.052] + 0.128 = +0.42
```

**Group E (dup_ratio=0.08):**

```
s_E = 0.5×[(0.08−0.10)²/0.15² − (0.08−0.22)²/0.12²] + log(0.15/0.12)
    = 0.5×[0.018 − 1.361] + 0.223 = −0.45
```

```
S_raw = 1.49 + 0.82 + 1.50 + (−0.24) + 1.50 + 1.50 + (−0.14)
      + 1.24 + 1.09 + 0.36 + 0.42 + (−0.45) = +9.08
score = σ(9.08 / 2.0) = σ(4.54) = 0.989
```

slop 9.9/10 — narration, decay, and overwrap each hit the +1.50 clamp.
Identifier specificity (+1.24) and function length CV (+1.09) add strong
evidence. TTR (+0.36) and indent (+0.42) contribute moderate evidence —
these signals are deliberately tuned with wide σ to avoid false positives
on C/Go code while still providing directional evidence.

**Borderline case** — same regularity + comments, but NO narration, NO decay,
NO overwrap, no naming breaks, moderate ident/TTR/indent, single file:

```
s_B2 = log(0.60/0.99) = −0.50   (no narration)
s_D1 = log(0.75/0.95) = −0.24   (no decay)
s_D2 = log(0.82/0.99) = −0.19   (no overwrap)
s_D3 = log(0.85/0.98) = −0.14   (no naming break)
s_G  = −0.91                     (ident_spec=0.10, toward human)
s_H  = −0.40                     (func_cv=0.70, toward human)
s_I  = +0.18                     (TTR=0.42, near-neutral)
s_J  = −0.27                     (indent_cv=0.65, toward human)
s_E  = 0                         (single file, skipped)

S_raw = 1.49 + 0.82 + (−0.50) + (−0.24) + (−0.24) + (−0.19) + (−0.14)
      + (−0.91) + (−0.40) + 0.18 + (−0.27) = −0.40
score = σ(−0.40 / 2.0) = σ(−0.20) = 0.450
```

slop 4.5/10 — "mild." Only structural signals, nothing diagnostic. Without
binary pattern signals, the Gaussian signals alone can't produce a confident
result.

**Pure human file** — low regularity, sparse comments, no pattern signals,
diverse naming, varied indentation:

```
regularity=0.15 → s_A  = −1.50  (clamped)
CCR=0.05        → s_B1 = −1.29
no narration    → s_B2 = −0.50
CD=0.06         → s_C  = −0.64
no decay        → s_D1 = −0.24
no overwrap     → s_D2 = −0.19
no naming break → s_D3 = −0.14
ident_spec=0.04 → s_G  = −1.50  (clamped)
func_cv=1.10    → s_H  = −1.50  (clamped)
TTR=0.55        → s_I  = −0.32
indent_cv=0.80  → s_J  = −1.06

S_raw = −8.87
score = σ(−8.87 / 2.0) = σ(−4.44) = 0.012
```

slop 0.1/10 — "clean." All eleven signals point toward clean code. The
strongest contributors are ident specificity, func_cv, and regularity
(all hitting the −1.50 clamp).

**Temperature matters.** Without it (T=1.0), the borderline case gives
σ(−0.40) = 0.402 (slop 4.0) — technically "clean" but barely. With
T=2.0 it gives 0.450 (slop 4.5), appropriately "mild."

**Clamping matters.** With tuned clamp (±3.0), the narration signal in
the first example would contribute +3.69 instead of +1.50, producing a much
higher raw score. The conservative ±1.5 default clamp keeps scores
realistic.

---

## Duplicate Detection Algorithm

### NCD Math

Normalized Compression Distance (Cilibrasi & Vitanyi 2005):

```
                 C(x‖y) − min(C(x), C(y))
NCD(x, y) =  ─────────────────────────────
                    max(C(x), C(y))
```

Where C(x) = deflate-compressed size (level 6), x‖y = concatenation.

- NCD ≈ 0: nearly identical (compressor finds x inside y)
- NCD ≈ 1: completely unrelated

Threshold: NCD < 0.30 → "near-duplicate."

**Why NCD works for code:**
- Language-agnostic — raw bytes, no parser beyond function boundaries
- Tolerates moderate variable renaming — identifiers are 10-25% of function
  bytes; structure (keywords, operators, control flow) dominates. Two
  structurally identical functions with different names score NCD 0.10-0.20.
- Tolerates whitespace/comment differences — these compress well
- Proven in production (NCDSearch, Ishio et al. ICSME 2018)

### NCD Limitations

**Minimum body size: 200 bytes.** Deflate has ~15 bytes of header overhead
per compression call. Below 200 bytes, this overhead dominates: C(x‖y) is
artificially close to min(C(x), C(y)) because of shared dictionary
initialization, NOT because the strings are similar. At 200 bytes, overhead
is ~12% — acceptable.

**Language keyword bias.** Two unrelated functions in the same language share
keywords (`function`, `return`, `if`, `const`), creating a baseline NCD of
~0.4-0.6. The 0.30 threshold is below this baseline, so it's safe.

### Dupe Pipeline

```
1. Walk directory tree, collect source files (respecting ignore rules)
2. For each file: detect language, extract function bodies
3. Filter: body size ≥ 200 bytes
4. Group functions by language family
5. Pre-filter pairs: skip if max(|a|,|b|) / min(|a|,|b|) > 2.0
6. Compute NCD for remaining pairs
7. Cluster with union-find on pairs where NCD < threshold
8. Report clusters sorted by cluster size (largest first)
```

**Performance:**

| Functions | Pairs (naive) | After pre-filter | Time (~5μs/pair) |
|-----------|--------------|-----------------|------------------|
| 100 | 4,950 | ~990 | 5ms |
| 500 | 124,750 | ~25,000 | 125ms |
| 2,000 | 1,999,000 | ~400,000 | 2s |

---

## Scanner Internals

The scanner (`scan.c`) is a single-pass state machine with 4 states:

| State | ID | Description |
|-------|-----|-------------|
| Code | 0 | Normal source code |
| Line comment | 1 | `//` or `#` to end of line |
| String | 2 | Inside `"..."`, `'...'`, or `` `...` `` |
| Block comment | 3 | `/* ... */` or `""" ... """` |

Transitions interact subtly: `//` inside a string is NOT a comment, `"` inside
a comment is NOT a string start, escaped characters inside strings are tracked.
The scanner handles three language families (C-like, Python, Shell) with
different comment and string syntax.

**Function boundary detection (C-like):** A function starts when `{` appears
at brace-depth 0, AND the preceding non-empty line contains `(` (signature
marker), AND the line does NOT begin with a control-flow keyword (`if`,
`else`, `for`, `while`, `switch`, `do`). Works for standalone functions,
methods inside a single-level class/struct, and Go `func` declarations.
Will miss some constructs (deeply nested classes, object literal methods).

**Function boundary detection (Python):** `def` at indent level 0, body
determined by subsequent indented block.

**Function boundary detection (Shell):** `function name` or `name() {`.

The scanner produces per-line classification (code/comment/blank), function
boundaries, narration hits, overwrap depth, naming style, line lengths,
and defect counts per quartile — all in a single pass.

---

## Language Support

Primary targets: **TypeScript**, **JavaScript**, **Go**

Also supports: Python, C, C++, Java, Rust, Swift, Shell, and other C-like
languages. Detection is by file extension. Unknown extensions → C-like.

| Family | Languages | Comment syntax | Function detection |
|--------|-----------|---------------|--------------------|
| C-like | C, C++, Java, JS, TS, Go, Rust, C#, Swift | `//` and `/* */` | Top-level `{`...`}` after signature with `(` |
| Python | Python | `#` and `"""..."""` | `def` at indent 0 + indent block |
| Shell | Bash, Zsh | `#` | `function name` or `name() {` |

**Minified and generated files** are automatically skipped:
- Average line length > 200 characters
- Extensions: `.min.js`, `.pb.go`, `.generated.cs`
- Binary content (null bytes in first 8KB)

**Dead code detection** is language-aware:
- **Go** — uppercase functions are exported, `Test*`/`Benchmark*`/`Example*` are entry points
- **TypeScript/JavaScript** — `export`ed functions are excluded
- **Python** — public functions (no leading `_`) are excluded
- **C/C++** — non-`static` functions are excluded (external linkage)
- Common entry points are always excluded: `main`, `init`, `constructor`, `render`, `setup`, `teardown`

## File Filtering

- Respects `.gitignore` automatically when inside a git repository (uses `git ls-files`)
- Supports `.slopignore` for additional exclusions (one directory/file name per line, no glob syntax, path-component match)
- Skips common non-source directories: `node_modules`, `vendor`, `.git`, `build`, `dist`, `target`, `__pycache__`, `.venv`, etc.
- Skips binary files (null-byte probe on first 8KB)

## Exit Codes

```
0  clean     — no strong tells, no dupes, slop < 6
1  warnings  — moderate findings, or slop 6–8.5
2  sloppy    — strong tells, dupes found, or slop > 8.5
```

## CI Integration

```bash
# Fail CI if any strong quality issues are found
slop check ./src/ || exit 1

# Fail CI if any file scores above 8.5/10
slop scan ./src/ && echo "clean" || echo "issues found"

# Scan only files changed in last commit
git diff --name-only HEAD~1 | xargs slop scan

# Read from stdin in a pipeline
cat handler.ts | slop scan --stdin --lang=ts
```

## Library API

`slop` builds as both a CLI tool (`zig-out/bin/slop`) and a static library
(`zig-out/lib/libslop.a`) with a public header (`zig-out/include/slop.h`).

### Functions

```c
#include "slop.h"

void             slop_options_default(SlopOptions *opts);
SlopFileResult    slop_analyze_file(const char *path, const SlopOptions *opts);
SlopProjectResult slop_analyze_dir(const char *dirpath, const SlopOptions *opts);
void             slop_file_result_free(SlopFileResult *r);
void             slop_project_result_free(SlopProjectResult *r);
```

### Usage

```c
#include "slop.h"

SlopOptions opts;
slop_options_default(&opts);
opts.include_general_smells = true;
opts.prior = 0.5;

SlopFileResult fr = slop_analyze_file("handler.ts", &opts);
printf("slop = %.1f / 10, findings = %d, dead lines = %d\n",
       fr.probability * 10.0, fr.smells.count, fr.dead_lines);
slop_file_result_free(&fr);

SlopProjectResult pr = slop_analyze_dir("./src/", &opts);
printf("Files: %d, sloppy: %d, dup_ratio: %.2f\n",
       pr.file_count, pr.sloppy, pr.dup_ratio);
for (int i = 0; i < pr.file_count; i++) {
    SlopFileResult *f = &pr.files[i];
    if (!f->skipped)
        printf("  %s — slop %.1f/10\n", f->filepath, f->probability * 10.0);
}
slop_project_result_free(&pr);
```

### SlopOptions

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `include_general_smells` | `bool` | `false` | Include general-severity findings |
| `use_git` | `bool` | `false` | Include git commit pattern signal |
| `prior` | `double` | `0.5` | Prior probability (0.5 = uninformative) |
| `calibration_dir` | `const char *` | `nullptr` | Path to dir containing `.slop-calibration.json` |

### SlopFileResult

| Field | Type | Description |
|-------|------|-------------|
| `filepath` | `char[4096]` | File path |
| `probability` | `double` | Slop probability (0.0–1.0; multiply by 10 for slop score) |
| `raw_score` | `double` | Raw Bayesian log-odds |
| `dead_lines` | `int` | Lines of dead/unused code |
| `smells` | `SmellReport` | Individual findings |
| `score` | `ScoreResult` | Per-signal breakdown |
| `total_lines` | `int` | Total line count |
| `code_lines` | `int` | Code lines (non-blank, non-comment) |
| `comment_lines` | `int` | Comment line count |
| `blank_lines` | `int` | Blank line count |
| `function_count` | `int` | Detected function count |
| `lang` | `LangFamily` | Detected language family |
| `skipped` | `bool` | True if binary/minified/generated |

### SlopProjectResult

| Field | Type | Description |
|-------|------|-------------|
| `files` | `SlopFileResult *` | Array of per-file results |
| `file_count` | `int` | Number of files analyzed |
| `dup_ratio` | `double` | Project-wide duplicate function ratio |
| `funcs_in_dup` | `int` | Functions involved in duplicates |
| `total_funcs` | `int` | Total functions extracted |
| `dupes` | `DupeResult` | Full duplicate cluster data |
| `sloppy` | `int` | Files with slop >= 8.5 |
| `moderate` | `int` | Files with slop 6.0–8.5 |
| `clean` | `int` | Files with slop < 6.0 |
| `skipped` | `int` | Files skipped |

### Building with libslop

```bash
zig build -Doptimize=ReleaseFast
cc myapp.c -Izig-out/include -Lzig-out/lib -lslop -lz -o myapp
```

See `examples/lib_example.c` for a complete working example.

## Cross-Compilation

Zig handles cross-compilation natively. The build system conditionally links
zlib: from source (via `allyourcodebase/zlib` package) for cross targets,
and from the system library for native builds.

```bash
# Linux (static musl binary)
zig build -Doptimize=ReleaseFast -Dtarget=x86_64-linux-musl
zig build -Doptimize=ReleaseFast -Dtarget=aarch64-linux-musl

# macOS
zig build -Doptimize=ReleaseFast -Dtarget=x86_64-macos
zig build -Doptimize=ReleaseFast -Dtarget=aarch64-macos
```

All targets produce a single static binary with no runtime dependencies.

> **Note:** Windows is not supported — the tool relies on POSIX APIs (`popen`, `strdup`) for git integration and file walking.

## Tuning

All signal parameters (μ, σ, p) ship as educated defaults. T=2.0 and
LLR_CLAMP=±1.5 prevent overconfidence. **Treat the slop score as a ranking**,
not a calibrated probability.

| Component | Accuracy |
|-----------|----------|
| `slop check` | Full — pattern matching, fixed thresholds |
| `slop dupes` | Full — NCD is pure math |
| `slop scan` ranking | Good — signal directions are correct |
| `slop scan` score | Directional; 5.8 vs 6.2 is meaningless |

To tune signal weights for your codebase, create a `.slop-calibration.json`
file in your project root (or pass `--calibration-dir`). When loaded,
LLR clamp widens to ±3.0 (trusting your parameters more).

```json
{
  "version": 1,
  "temperature": 1.45,
  "signals": {
    "regularity": { "mu_ai": 0.50, "sigma_ai": 0.18, "mu_h": 0.20, "sigma_h": 0.13 },
    "ccr":        { "mu_ai": 0.25, "sigma_ai": 0.15, "mu_h": 0.04, "sigma_h": 0.10 },
    "cond":       { "mu_ai": 0.18, "sigma_ai": 0.10, "mu_h": 0.10, "sigma_h": 0.10 },
    "dup":        { "mu_ai": 0.22, "sigma_ai": 0.12, "mu_h": 0.10, "sigma_h": 0.15 },
    "git":        { "mu_ai": 0.55, "sigma_ai": 0.20, "mu_h": 0.25, "sigma_h": 0.22 },
    "ident":      { "mu_ai": 0.18, "sigma_ai": 0.08, "mu_h": 0.07, "sigma_h": 0.04 },
    "func_cv":    { "mu_ai": 0.40, "sigma_ai": 0.20, "mu_h": 0.90, "sigma_h": 0.35 },
    "ttr":        { "mu_ai": 0.35, "sigma_ai": 0.15, "mu_h": 0.40, "sigma_h": 0.20 },
    "indent":     { "mu_ai": 0.45, "sigma_ai": 0.22, "mu_h": 0.70, "sigma_h": 0.25 }
  },
  "binary": {
    "narration": { "p_ai": 0.40, "p_h": 0.01 },
    "decay":     { "p_ai": 0.25, "p_h": 0.05 }
  },
  "patterns": {
    "overwrap":  { "p_ai": 0.18, "p_h": 0.01 },
    "namebreak": { "p_ai": 0.15, "p_h": 0.02 }
  }
}
```

**Validation on load:** Malformed JSON silently falls back to defaults.
Sigma values are clamped to ≥ 0.01, binary probabilities to ≥ 0.01.
Temperature < 0.1 falls back to 2.0. At least 3 Gaussian signals must
have non-trivial sigma (> 0.01) for the file to be accepted.

### Default Parameter Reference

All parameters below ship as defaults and can be overridden via
`.slop-calibration.json`. They are defined as `#define` constants in
`slop.h` and runtime values in `score.c:calibration_default()`.

---

#### Temperature

| JSON key | Default | Range | Description |
|----------|---------|-------|-------------|
| `temperature` | 2.0 | ≥0.1 | Divides raw LLR sum before sigmoid. Higher = less confident output. |

**What it controls:** The mapping from raw score to slop score.
At T=1.0, the model is a standard naive Bayes classifier (overconfident
when signals are correlated). At T=2.0, every raw score is halved before
applying sigmoid, making the output more conservative.

**When to hand-tune:** If scores feel too confident (many files at 9.5+),
increase T in `.slop-calibration.json`. If scores cluster near 5.0 and
you want more decisive output, decrease T.

---

#### Gaussian Signal Parameters

Each Gaussian signal has four parameters that define two normal distributions —
one for "sloppy" code, one for "clean" code:

| Parameter | Meaning |
|-----------|---------|
| `mu_ai` | Expected (mean) value of the measurement in sloppy code |
| `sigma_ai` | Standard deviation of the measurement in sloppy code |
| `mu_h` | Expected (mean) value of the measurement in clean code |
| `sigma_h` | Standard deviation of the measurement in clean code |

The LLR formula computes how much more likely the observed measurement is
under the sloppy hypothesis vs. the clean hypothesis:

```
LLR = 0.5 × [(x − μ_H)²/σ_H² − (x − μ_AI)²/σ_AI²] + ln(σ_H/σ_AI)
```

Measurements close to μ_AI produce positive LLR (sloppy evidence). Measurements
close to μ_H produce negative LLR (clean evidence). The σ values control
sensitivity — smaller σ means the signal is more decisive near its mean
but penalizes outliers more harshly.

**Signal: `regularity`** — code uniformity

| Key | Default | Description |
|-----|---------|-------------|
| `mu_ai` | 0.50 | Sloppy code is more uniform (consistent formatting, repetitive structure) |
| `sigma_ai` | 0.18 | Wide — regularity varies with formatter usage |
| `mu_h` | 0.20 | Clean code is less uniform (organic style, varying line lengths) |
| `sigma_h` | 0.13 | Narrower — clean code consistently has lower regularity |

Composite of three sub-measurements, each normalized to [0,1] and averaged:

```
Compression ratio:   R = compressed_size / raw_size    (skip if < 500 bytes)
Line-length entropy: H_norm = H / log₂(buckets)       (5-char buckets, up to 100)
Line-length CV:      CV = stdev(line_lengths) / mean(line_lengths)

r_norm = 1 − clamp((R − 0.10) / 0.35, 0, 1)
h_norm = 1 − clamp((H_norm − 0.30) / 0.60, 0, 1)
c_norm = 1 − clamp((CV − 0.15) / 0.75, 0, 1)

regularity = (r_norm + h_norm + c_norm) / 3
```

Higher regularity = more sloppy. Weakest signal group — formatters
(prettier, black, gofmt) homogenize code and inflate human regularity.

**Signal: `ccr`** — comment-to-code ratio

| Key | Default | Description |
|-----|---------|-------------|
| `mu_ai` | 0.25 | Sloppy code has ~25% comment lines relative to code |
| `sigma_ai` | 0.15 | Wide — varies by generation style |
| `mu_h` | 0.04 | Clean code averages ~4% comment density |
| `sigma_h` | 0.10 | Some clean code is heavily commented (libraries, APIs) |

Measured as: `comment_lines / max(code_lines, 1)`. Lines with trailing
comments count as code. Guard: if `code_lines = 0`, all signals are skipped.

**Signal: `cond`** — conditional keyword density

| Key | Default | Description |
|-----|---------|-------------|
| `mu_ai` | 0.18 | Sloppy code uses more branching (defensive coding, verbose logic) |
| `sigma_ai` | 0.10 | Wide — language-dependent, C code naturally has higher density |
| `mu_h` | 0.10 | Clean code uses fewer conditionals on average |
| `sigma_h` | 0.10 | Wide — C parsers/state machines can be much higher |

Counts keywords: `if`, `else`, `elif`, `else if`, `switch`, `case`, `match`.
**Skipped** if file has fewer than 3 conditional keywords (`MIN_COND_KEYWORDS`).
This is a weak Gaussian signal — the wide σ (0.10 for both classes) reflects
that conditional density varies more by language than by authorship. C code
with heavy control flow (parsers, state machines) has higher density than
AI-generated TypeScript, so the signal provides only mild directional evidence.

**Signal: `dup`** — duplicate function ratio

| Key | Default | Description |
|-----|---------|-------------|
| `mu_ai` | 0.22 | Sloppy code frequently contains similar functions |
| `sigma_ai` | 0.12 | Wide — depends on file count and project structure |
| `mu_h` | 0.10 | Clean code has some duplication (legacy, test boilerplate) |
| `sigma_h` | 0.15 | Wide — Go test boilerplate and legacy code inflate dup ratio |

Only active in directory mode. Measured as:
`|functions with NCD < 0.3 match| / |all functions ≥ 200 bytes|`.
Skipped for single-file scans (no project context).

**Signal: `git`** — git commit pattern score

| Key | Default | Description |
|-----|---------|-------------|
| `mu_ai` | 0.55 | Sloppy commits tend to be formulaic |
| `sigma_ai` | 0.20 | Varies by tooling configuration |
| `mu_h` | 0.25 | Clean commits are more varied |
| `sigma_h` | 0.22 | Wide — some developers use conventional commits rigorously |

**Opt-in only** (`--git` flag). Disabled by default because git commits
describe the *committer*, not the *code author*. When humans commit AI code
(the common case), this signal produces strong false negatives.

**Signal: `ident`** — identifier specificity

| Key | Default | Description |
|-----|---------|-------------|
| `mu_ai` | 0.18 | Sloppy code uses more generic names (data, result, handler, etc.) |
| `sigma_ai` | 0.08 | Moderate — consistent pattern |
| `mu_h` | 0.07 | Clean code uses fewer generic names proportionally |
| `sigma_h` | 0.04 | Narrow — clean code consistently uses specific names |

Measured as: `generic_identifiers / total_identifiers`. Dictionary of 38
generic names. Skipped if fewer than 20 identifiers (`MIN_IDENTIFIERS`).

**Signal: `func_cv`** — function length coefficient of variation

| Key | Default | Description |
|-----|---------|-------------|
| `mu_ai` | 0.40 | Sloppy code has functions of similar length (low variance) |
| `sigma_ai` | 0.20 | Moderate spread |
| `mu_h` | 0.90 | Clean code has diverse function lengths (high variance) |
| `sigma_h` | 0.35 | Wide — project-dependent |

Measured as: `stdev(function_line_counts) / mean(function_line_counts)`.
Skipped if fewer than 5 functions detected (`MIN_FUNCS_CV`).

**Signal: `ttr`** — token diversity (type-token ratio)

| Key | Default | Description |
|-----|---------|-------------|
| `mu_ai` | 0.35 | Sloppy code reuses identifiers more (lower vocabulary richness) |
| `sigma_ai` | 0.15 | Wide — varies by language and project |
| `mu_h` | 0.40 | Modest separation — C/Go code has low TTR naturally |
| `sigma_h` | 0.20 | Wide — C code TTR (0.11–0.28) is naturally lower than JS/Python |

Measured as: `unique_identifiers / total_identifiers`. Unique tracking via
FNV-1a hash set. Skipped if fewer than 50 tokens (`MIN_TTR_TOKENS`).

The close means (0.35 vs 0.40) and wide σ make this a deliberately weak
signal — TTR overlaps substantially between AI and human C/Go code, so
tight parameters would cause false positives on human systems code.

**Signal: `indent`** — indentation regularity

| Key | Default | Description |
|-----|---------|-------------|
| `mu_ai` | 0.45 | Sloppy code has very uniform indentation (low CV) |
| `sigma_ai` | 0.22 | Wide — deeply nested JS/TS code has higher indent CV |
| `mu_h` | 0.70 | Clean code has more varied indentation (higher CV) |
| `sigma_h` | 0.25 | Wide — depends on project structure and style |

Measured as: `stdev(leading_spaces_per_line) / mean(leading_spaces_per_line)`.
Tabs count as 4 spaces. Only non-blank code lines are included.
Skipped if fewer than 15 code lines (`MIN_INDENT_LINES`).

Empirically validated as the strongest whitespace-based discriminator
(Nirob et al. 2025). μ_H of 0.70 matches observed human C/Go code
(typically 0.58–0.76). σ_AI of 0.22 accounts for language-dependent
nesting depth — AI-generated TypeScript with nested callbacks can have
high indent CV that overlaps with the human distribution.

---

#### Binary Signal Parameters

Each binary signal has two parameters — the probability of the pattern
appearing in sloppy code vs. clean code:

| Parameter | Meaning |
|-----------|---------|
| `p_ai` | P(pattern present \| sloppy code) |
| `p_h` | P(pattern present \| clean code) |

LLR when present: `ln(p_ai / p_h)`. LLR when absent: `ln((1−p_ai) / (1−p_h))`.
The ratio `p_ai / p_h` is the discrimination factor.

**Signal: `narr`** — narration comments

| Key | Default | LLR present | LLR absent |
|-----|---------|-------------|------------|
| `narr_p_ai` | 0.40 | +3.69 (clamped) | — |
| `narr_p_h` | 0.01 | — | −0.50 |

Fires when ≥3 narration-style comments are detected (`NARRATION_TIER1`).
Discrimination: 40x. The **strongest single signal**. Near-zero false
positive rate on production code.

**How to tune:** If your codebase has legitimate pedagogical comments
(textbook ports, tutorial-style libraries), raise `narr_p_h` to 0.05–0.10
to reduce the penalty when narration is detected.

**Signal: `decay`** — quality/comment decay

| Key | Default | LLR present | LLR absent |
|-----|---------|-------------|------------|
| `decay_p_ai` | 0.25 | +1.61 | — |
| `decay_p_h` | 0.05 | — | −0.24 |

Fires when EITHER condition is met:
- Defect gradient: Spearman ρ > 0.5 between file position and defect rate,
  with ≥4 defects, in files ≥60 lines
- Comment gradient: top-half comment density ≥3x bottom-half, in code bodies
  ≥50 lines with ≥6 comments

Discrimination: 5x. Measures attention decay — a direct consequence of
LLM context length limitations.

**How to tune:** If your project has files that naturally front-load
comments (API headers, module documentation), raise `decay_p_h` to 0.10.

**Signal: `overwrap`** — defensive over-wrapping

| Key | Default | LLR present | LLR absent |
|-----|---------|-------------|------------|
| `overwrap_p_ai` | 0.18 | +2.89 (clamped) | — |
| `overwrap_p_h` | 0.01 | — | −0.19 |

Fires when any of these are detected:
- try/catch/except nesting depth >2 within a function
- Same variable null-checked twice within 10 lines

Discrimination: 18x. AI over-defends because it doesn't know what callers
guarantee.

**Signal: `namebrk`** — naming convention break

| Key | Default | LLR present | LLR absent |
|-----|---------|-------------|------------|
| `namebrk_p_ai` | 0.15 | +2.01 | — |
| `namebrk_p_h` | 0.02 | — | −0.14 |

Fires when a function contains mixed camelCase and snake_case identifiers
(each >25% share). Occurs at AI "generation boundaries."

Discrimination: 7.5x. The weakest binary signal — some codebases that
interface with libraries using different conventions (e.g., C code calling
Python-style APIs) may trigger this legitimately.

**How to tune:** If your codebase bridges naming conventions by design,
raise `namebrk_p_h` to 0.05–0.10.

---

#### Fixed Thresholds

These thresholds are hardcoded in `slop.h` and not overridden by the
tuning file. They control when detectors fire and how signals are clamped:

**LLR clamping:**

| Constant | Value | When active |
|----------|-------|-------------|
| `LLR_CLAMP` | ±3.0 | Tuned mode (calibration file loaded) |
| `LLR_CLAMP_UNCAL` | ±1.5 | Default mode |
| `LLR_CLAMP_NOCOMPR` | ±1.5 | File < 500 bytes (compression unreliable) |
| `LLR_CLAMP_SMALL` | ±1.0 | File < 50 lines |

**Classification thresholds:**

| Constant | Value | Label |
|----------|-------|-------|
| `PROB_FLAGGED` | 0.85 | slop >= 8.5 → "sloppy" (exit code 2) |
| `PROB_SUSPICIOUS` | 0.60 | slop >= 6.0 → "moderate" (exit code 1) |
| `PROB_INCONCLUSIVE` | 0.40 | slop >= 4.0 → "mild" |

**Finding detector thresholds:**

| Constant | Value | What it controls |
|----------|-------|-----------------|
| `NARRATION_TIER1` | 3 | Minimum narration hits for binary signal to fire |
| `NARRATION_TIER2` | 8 | (reserved for future graduated scoring) |
| `NCD_THRESHOLD` | 0.30 | NCD below this → "near-duplicate" functions |
| `NCD_MIN_BODY_BYTES` | 200 | Minimum function body size for NCD comparison |
| `NCD_SIZE_RATIO_MAX` | 2.0 | Skip NCD if larger/smaller body ratio exceeds this |
| `MIN_COMPRESS_BYTES` | 500 | Skip compression ratio if file smaller than this |
| `GRADIENT_THRESHOLD` | 3.0 | Comment gradient ratio to trigger decay detection |
| `MIN_LINES_GRADIENT` | 50 | Minimum code body lines for gradient check |
| `MIN_GRADIENT_CMTS` | 6 | Minimum comments in body for gradient check |
| `MIN_LINES_DECAY` | 60 | Minimum file lines for Spearman decay check |
| `MIN_DEFECTS_DECAY` | 4 | Minimum defect markers for Spearman decay check |
| `SPEARMAN_THRESHOLD` | 0.5 | Spearman ρ above this → quality decay detected |
| `MIN_COND_KEYWORDS` | 3 | Conditional density skipped if fewer keywords |
| `MIN_IDENTIFIERS` | 20 | Identifier specificity skipped if fewer identifiers |
| `MIN_FUNCS_CV` | 5 | Function length CV skipped if fewer functions (CV unreliable with <5 data points) |
| `MIN_TTR_TOKENS` | 50 | Token diversity skipped if fewer identifier tokens |
| `MIN_INDENT_LINES` | 15 | Indentation regularity skipped if fewer code lines |
| `NULL_CHECK_WINDOW` | 10 | Lines within which duplicate null checks are flagged |
| `MAX_AVG_LINE_LEN` | 200 | Average line length above this → file is minified |

**Calibration file loading:**

| Constant | Value | What it controls |
|----------|-------|-----------------|
| `MIN_GAUSS_SIGMA` | 0.01 | Floor for loaded sigma values (prevents div-by-zero) |
| `MIN_BINARY_PROB` | 0.01 | Floor for loaded probability values |

---


## Honest Assessment

**What this tool is good at:**
- Narration comments — no other linter catches "First, we validate the input"
- NCD for duplicates — proven technology (Cilibrasi & Vitanyi 2005), genuinely useful
- Comment density gradient — measures real uneven effort across a file
- Redundant null checks, over-wrapping — real code hygiene issues
- Speed — sub-second on real projects, no cloud, no config

**What this tool is NOT:**
- Not an AI authorship detector. The slop score measures code quality patterns,
  not who wrote the code. High-quality code from any source (human or AI)
  will score low; sloppy code from any source will score high.
- Not a replacement for language-specific linters (eslint, clippy, ruff).
  slop catches patterns they miss; they catch patterns slop misses. Use both.

**Known limitations:**
- Short files (< 50 lines) lack data for most signals
- Test files may score higher (repetitive by nature, legitimate step narration)
- Formatters (prettier, black, gofmt) reduce regularity and indentation signals
- Token diversity (TTR) is a weak signal — C/Go code naturally has low TTR
- Default Gaussian parameters are educated guesses; override via
  `.slop-calibration.json` with empirical values for your codebase
- The Gaussian model is technically wrong for bounded signals (CCR, regularity),
  but ranking is preserved and LLR clamping limits the impact

## Project Structure

```
build.zig          Zig build system (sole build backend)
build.zig.zon      Zig package manifest (zlib dependency for cross-compilation)
Makefile           Thin wrapper — all targets delegate to zig build
src/
  slop.h            Public header: types, constants, thresholds, API (473 lines)
  main.c           CLI entry point, arg parsing, output formatting (1435 lines)
  scan.c           Single-pass scanner, TTR hash set, indent tracking (933 lines)
  smell.c          Smell detectors: diagnostic + correlated + general (1033 lines)
  score.c          Bayesian LLR scoring, temperature scaling, calibration loading (527 lines)
  compress.c       Deflate wrapper, compression ratio, NCD (48 lines)
  dupes.c          Cross-file duplicate detection, union-find clustering (151 lines)
  git.c            Git commit feature extraction via popen (93 lines)
  lang.c           Language detection by file extension (106 lines)
  util.c           File I/O, binary/minified detection (43 lines)
  walk.c           Directory walking, .gitignore/.slopignore, FileList (150 lines)
  api.c            High-level library API (139 lines)

scripts/
  build.sh         Cross-compilation + .deb packaging (6 targets)
  install.sh       One-liner installer (curl | bash)
  update-homebrew.sh  Updates Homebrew formula SHA256 after release

packaging/
  homebrew/slop.rb   Homebrew formula template

.github/workflows/
  release.yml      CI: test → semantic-release → build → upload → update tap
  ci.yml           PR checks: build + test

tests/
  run.sh           Integration test harness (86 tests, 332 lines)
  fixtures/        Test input files (AI example, human example, dupes, Go errors)

examples/
  lib_example.c    Working example of using libslop programmatically
```

### Compiler flags

```
-std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
-Wshadow -Wdouble-promotion -Wformat=2 -Wnull-dereference
-Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes
```

Zero warnings across all source files.

## References

- Ghaleb 2026, "Fingerprinting AI Coding Agents on GitHub" (MSR '26) — 33,580 PRs, 41 features, 97.2% F1
- Guo et al. 2017, "On Calibration of Modern Neural Networks" (ICML) — temperature scaling
- Cilibrasi & Vitanyi 2005, "Clustering by Compression" — NCD theory
- Ishio et al. 2018, "NCDSearch" (ICSME) — NCD for code clones in production
- Shannon 1948, "A Mathematical Theory of Communication"
- Li & Vitanyi 1997, "Kolmogorov Complexity and Its Applications"
- Liu et al. 2023, "Lost in the Middle" — U-shaped attention in LLMs
- Chroma 2025, "Context Rot" — all frontier models degrade with context length
- Nirob et al. 2025, "Whitespaces Don't Lie" — indentation regularity as #1 discriminative feature for AI vs human code
- Tian et al. 2023, GPTSniffer — perplexity/burstiness for AI text detection
