# Scoring & detection internals

Detailed reference for how `slop` scores files and detects issues.
For usage, see [README.md](README.md).

## Bayesian scoring framework

Given measurements **x** = (x₁...xₙ):

```
              P(slop)            P(xᵢ | slop)
S = log ──────────── +  Σᵢ log ──────────────
              P(clean)           P(xᵢ | clean)
         └─────┬─────┘      └──────┬────────┘
           prior s₀          signal LLR sᵢ
```

Default prior: P(slop) = 0.5 → s₀ = 0. Override with `--prior=0.7`.

Continuous signal (Gaussian):

```
              ⎡ (x − μH)²    (x − μAI)² ⎤         σH
sᵢ(x) =  ½ · ⎢ ────────── − ────────── ⎥ + log ─────
              ⎣    σH²          σAI²     ⎦         σAI
```

Binary signal (present/absent):

```
                   p_slop                      1 − p_slop
sᵢ(1) = log ────────────       sᵢ(0) = log ──────────────
                   p_clean                     1 − p_clean
```

LLR clamping prevents any single signal from dominating:

| Mode | Clamp | Condition |
|------|-------|-----------|
| Tuned | ±3.0 | `.slop-calibration.json` loaded |
| Default | ±1.5 | No calibration file |
| No compression | ±1.5 | File < 500 bytes |
| Small file | ±1.0 | File < 50 lines |

The default ±1.5 is conservative — all μ/σ/p values are educated guesses,
so clamping prevents a mis-estimated signal from producing extreme scores.

### Temperature scaling

Following Guo et al. 2017:

```
score = σ(S / T) = 1 / (1 + exp(−S/T))
```

T = 2.0 by default (halves confidence). Naive Bayes assumes independence
but our signals are correlated, so T compensates.

## Signal groups

13 signals across 10 groups (9 Gaussian, 4 binary):

| Signal | Type | Measures |
|--------|------|----------|
| Regularity | Gaussian | Compression ratio + line-length entropy + line-length CV |
| Comment ratio | Gaussian | Comment lines / code lines |
| Narration | Binary | ≥3 narration-style comments |
| Conditional density | Gaussian | `if`/`else`/`switch`/`case` per code line |
| Quality decay | Binary | Defect/comment trend over file position |
| Over-wrapping | Binary | try/catch depth >2 or redundant null checks |
| Naming break | Binary | Mixed camelCase + snake_case in one function |
| Identifier specificity | Gaussian | Generic names / total identifiers |
| Function length CV | Gaussian | CV of function body lengths |
| Token diversity | Gaussian | Unique identifiers / total identifiers |
| Indentation regularity | Gaussian | CV of leading whitespace depths |
| Dupes | Gaussian | Duplicate function ratio (directory mode only) |
| Git | Gaussian | Commit message patterns (opt-in `--git`) |

### Group A — Regularity

Three measurements normalized to [0,1] and averaged:

```
r_norm = 1 − clamp((compress_ratio − 0.10) / 0.35, 0, 1)
h_norm = 1 − clamp((entropy_norm − 0.30) / 0.60, 0, 1)
c_norm = 1 − clamp((line_cv − 0.15) / 0.75, 0, 1)
regularity = (r_norm + h_norm + c_norm) / 3
```

Weakest group — formatters homogenize code and narrow the gap.

### Group B1 — Comment ratio

`CCR = comment_lines / max(code_lines, 1)`

Trailing comments (`x = 5 // init`) count as code. If `code_lines = 0`,
all signals are skipped ("insufficient code").

### Group B2 — Narration

`has_narration = 1 if count ≥ 3`

Count-based, not ratio-based. Strongest single signal (discrimination 40x).

### Group C — Conditional density

`CD = count(if, else, elif, switch, case, match) / code_lines`

Skipped if < 3 keywords. Wide σ reflects language dependence.

### Group D — Positional quality

For files ≥ 60 lines. Spearman ρ on defect rate across 4 quartiles,
OR comment gradient > 3x between halves.

### Groups D2/D3 — Over-wrapping, Naming break

Pattern-based binary signals. Less sensitive to parameter tuning.

### Group G — Identifier specificity

`ident_spec = generic_identifiers / total_identifiers`

38-word dictionary (data, result, response, handler, value, etc.).
Skipped if < 20 identifiers.

### Group H — Function length CV

`CV = stdev(func_lengths) / mean(func_lengths)`

Sloppy code tends toward uniform function sizes. Skipped if < 5 functions.

### Group I — Token diversity (TTR)

`TTR = unique_identifiers / total_identifiers`

Tracked via FNV-1a hash set (2048 buckets). Deliberately weak signal —
C/Go code naturally has low TTR.

### Group J — Indentation regularity

`indent_cv = stdev(leading_spaces) / mean(leading_spaces)`

Validated by Nirob et al. 2025 as the #1 whitespace discriminator.
Tabs = 4 spaces. Skipped if < 15 code lines.

### Group E — Duplicate ratio (directory mode only)

`dup_ratio = |{funcs with NCD < 0.3 match}| / |{all funcs}|`

Skipped for single-file scans.

### Group F — Git patterns (opt-in)

Disabled by default. Git commits describe the *committer*, not the *code
author* — produces false negatives when humans commit AI code.

## NCD (duplicate detection)

Normalized Compression Distance (Cilibrasi & Vitanyi 2005):

```
                 C(x‖y) − min(C(x), C(y))
NCD(x, y) =  ─────────────────────────────
                    max(C(x), C(y))
```

C(x) = deflate-compressed size (level 6). NCD ≈ 0 means identical,
NCD ≈ 1 means unrelated. Threshold: 0.30.

Minimum body: 200 bytes (below that, deflate header overhead dominates).
Size pre-filter: skip pairs where max/min body ratio > 2.0.

Pipeline: walk dir → extract functions → filter by size → group by
language → pre-filter pairs → compute NCD → union-find clustering.

## Detection algorithms

### Narration comments

Matched case-insensitive at the START of comment text (after stripping
`//`, `#`, `*` prefix and whitespace):

```
"First we", "First,", "Now we", "Now,", "Now let",
"Next we", "Next,", "Then we", "Then,",
"Finally we", "Finally,", "Here we", "Here,",
"Let's", "Let me", "We need to", "We can ",
"We'll", "We will", "Step 1", "Step 2", "Step 3"
```

START-of-comment only so "Handle first-time setup" doesn't match.

### Comment density gradient

```
gradient = density_top / max(density_bottom, 0.001)
```

Fires if gradient > 3.0, code body ≥ 50 lines, body comments ≥ 6.
"Code body" starts after the first function definition, skipping the
preamble (license headers, imports).

### Defensive over-wrapping

1. Try/catch nesting > 2 deep per function
2. Same variable null-checked twice within 10 lines (ring buffer, cap 32)

### Naming convention break

Mixed camelCase + snake_case within one function (each > 25% share).

### Redundant re-implementation

Within-file: two functions with NCD < 0.30 and body ≥ 200 bytes.

## Default parameters

All overridable via `.slop-calibration.json`.

### Gaussian signals

| Signal | μ_AI | σ_AI | μ_H | σ_H |
|--------|------|------|-----|-----|
| regularity | 0.50 | 0.18 | 0.20 | 0.13 |
| ccr | 0.25 | 0.15 | 0.04 | 0.10 |
| cond | 0.18 | 0.10 | 0.10 | 0.10 |
| dup | 0.22 | 0.12 | 0.10 | 0.15 |
| git | 0.55 | 0.20 | 0.25 | 0.22 |
| ident | 0.18 | 0.08 | 0.07 | 0.04 |
| func_cv | 0.40 | 0.20 | 0.90 | 0.35 |
| ttr | 0.35 | 0.15 | 0.40 | 0.20 |
| indent | 0.45 | 0.22 | 0.70 | 0.25 |

### Binary signals

| Signal | p_AI | p_H | Discrimination |
|--------|------|-----|----------------|
| narration | 0.40 | 0.01 | 40x |
| decay | 0.25 | 0.05 | 5x |
| overwrap | 0.18 | 0.01 | 18x |
| namebreak | 0.15 | 0.02 | 7.5x |

### Fixed thresholds (slop.h)

| Constant | Value | Purpose |
|----------|-------|---------|
| `PROB_FLAGGED` | 0.85 | slop ≥ 8.5 → exit 2 |
| `PROB_SUSPICIOUS` | 0.60 | slop ≥ 6.0 → exit 1 |
| `NARRATION_TIER1` | 3 | min narration hits for signal |
| `NCD_THRESHOLD` | 0.30 | near-duplicate cutoff |
| `NCD_MIN_BODY_BYTES` | 200 | min function body for NCD |
| `GRADIENT_THRESHOLD` | 3.0 | comment gradient ratio |
| `MIN_LINES_DECAY` | 60 | min lines for decay check |
| `SPEARMAN_THRESHOLD` | 0.5 | defect trend cutoff |
| `MIN_COND_KEYWORDS` | 3 | skip conditional signal below this |
| `MIN_IDENTIFIERS` | 20 | skip ident specificity below this |
| `MIN_FUNCS_CV` | 5 | skip func CV below this |
| `MIN_TTR_TOKENS` | 50 | skip TTR below this |
| `MIN_INDENT_LINES` | 15 | skip indent regularity below this |

### Calibration file format

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

Malformed JSON silently falls back to defaults. Sigma ≥ 0.01,
probabilities ≥ 0.01, temperature ≥ 0.1. At least 3 Gaussian signals
must have sigma > 0.01 for the file to load.

## Worked example

180-line file, directory mode, default clamp ±1.5.

Measurements: regularity=0.45, CCR=0.20, 4 narration hits, CD=0.11,
decay present, overwrap detected, no naming breaks, ident_spec=0.15,
func_cv=0.50, TTR=0.30, indent_cv=0.50, dup_ratio=0.08.

| Signal | LLR | Note |
|--------|-----|------|
| regularity | +1.49 | |
| ccr | +0.82 | |
| narration | +1.50 | clamped from +3.69 |
| conditional | −0.24 | |
| decay | +1.50 | clamped from +1.61 |
| overwrap | +1.50 | clamped from +2.89 |
| naming break | −0.14 | absent |
| ident spec | +1.24 | |
| func cv | +1.09 | |
| ttr | +0.36 | |
| indent | +0.42 | |
| dupes | −0.45 | low dup_ratio = clean evidence |

```
S_raw = +9.08
score = σ(9.08 / 2.0) = 0.989 → slop 9.9/10
```

Same file but no narration, no decay, no overwrap, single file:

```
S_raw = −0.40
score = σ(−0.20) = 0.450 → slop 4.5/10
```

## Scanner internals

Single-pass state machine with 4 states: Code, Line comment, String,
Block comment. Handles C-like (`//`, `/* */`), Python (`#`, `"""`),
and Shell (`#`) syntax families.

Function detection:
- **C-like:** `{` at brace depth 0 (or 1 for class methods) where
  preceding line(s) contain `(`. Rejects control flow keywords,
  type declarations (`struct`, `enum`, `union`, `interface`), and
  closing-brace continuations (`} else {`).
- **Python:** `def` at indent 0, body = subsequent indented block.
- **Shell:** `function name` or `name() {`.

## References

- Ghaleb 2026, "Fingerprinting AI Coding Agents on GitHub" (MSR '26)
- Guo et al. 2017, "On Calibration of Modern Neural Networks" (ICML)
- Cilibrasi & Vitanyi 2005, "Clustering by Compression"
- Ishio et al. 2018, "NCDSearch" (ICSME)
- Nirob et al. 2025, "Whitespaces Don't Lie"
- Liu et al. 2023, "Lost in the Middle"
