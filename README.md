# slop

Code quality scanner. Catches stuff linters miss - narration comments,
dead code, naming drift, cross-file duplicates, comment density decay.

No ML, no API calls, no cloud. Single static binary. C23, built with Zig.

## Install

```bash
# homebrew
brew install en9inerd/tap/slop

# any platform
curl -sL https://raw.githubusercontent.com/en9inerd/slop/master/scripts/install.sh | bash

# from source (needs zig 0.14+)
make              # debug
make release      # optimized
make test         # 124 integration tests
make install      # /usr/local
```

## Usage

```bash
slop check ./src/               # find quality issues
slop check --all ./src/         # include dead code, zombie params, unused imports
slop scan ./src/                # score files 0-10
slop scan --verbose file.ts     # per-signal breakdown
slop dupes ./src/               # find duplicate functions (NCD)
slop report ./src/              # full report with methodology
```

## What it finds

**`slop check`** - pattern-based findings at three severity levels:

*Strong tells* (near-zero false positive rate):
- Narration comments - "First we...", "Now we...", "Step 1:..."
- Comment density gradient - 3x+ drop between first and second half

*Score-related:*
- Redundant re-implementation (NCD < 0.30 within same file)
- Defensive over-wrapping (try/catch 3+ deep, duplicate null checks)
- Naming convention break (mixed camelCase/snake_case in one function)
- `as any` casts, `@ts-ignore`/`@ts-nocheck` (TS)
- `# type: ignore` / `# noqa` (Python)
- `_ = err` suppression (Go)

*General* (behind `--all`):
- Zombie parameters, unused imports, magic string repetition
- Dead code - language-aware: respects Go uppercase exports, TS/JS
  `export`, C `static` linkage, Python `_` prefix, decorators, common
  entry points (`main`, `init`, `Test*`, `Benchmark*`, lifecycle hooks)

**`slop scan`** - Bayesian log-likelihood scoring across 13 signals
(regularity, comment ratio, narration, conditional density, quality decay,
over-wrapping, naming breaks, identifier specificity, function length CV,
token diversity, indentation regularity, dupes, git). See
[METRICS.md](METRICS.md) for the math.

**`slop dupes`** - NCD-based duplicate function detection across a
project. Threshold 0.30, min body 200 bytes, union-find clustering.

## Flags

| Command | Flag | Effect |
|---------|------|--------|
| check | `--all` | Include general-severity findings |
| scan | `--verbose` | Per-signal breakdown |
| scan | `--json` | Machine-readable output |
| scan | `--git` | Include git commit patterns (off by default) |
| scan | `--stdin --lang=EXT` | Read from stdin |
| scan | `--prior=N` | Prior probability (default 0.5) |
| dupes | `--threshold=N` | NCD threshold (default 0.30) |
| dupes | `--cross-lang` | Compare across language families |

## Exit codes

```
0  clean
1  moderate findings or slop 6-8.5
2  strong tells, dupes found, or slop > 8.5
```

## Language support

Detection by file extension. Three parser families:

| Family | Comment syntax | Function detection |
|--------|---------------|--------------------|
| C-like (C, JS, TS, Go, Java, Rust, Swift, C#) | `//` `/* */` | `{` after signature with `(` |
| Python | `#` `"""` | `def` at indent 0 |
| Shell | `#` | `function name` or `name() {` |

Skips binary files, minified files (avg line > 200 chars), and generated
files (`Code generated`, `DO NOT EDIT`, `@generated`). Respects
`.gitignore` and `.slopignore`.

## CI

```bash
slop check ./src/ || exit 1
git diff --name-only HEAD~1 | xargs slop scan
```

## Library API

Builds as CLI (`zig-out/bin/slop`) and static library
(`zig-out/lib/libslop.a` + `zig-out/include/slop.h`).

```c
SlopOptions opts;
slop_options_default(&opts);

SlopFileResult fr = slop_analyze_file("handler.ts", &opts);
printf("slop %.1f, findings %d\n", fr.probability * 10.0, fr.smells.count);
slop_file_result_free(&fr);

SlopProjectResult pr = slop_analyze_dir("./src/", &opts);
// pr.file_count, pr.dup_ratio, pr.sloppy, pr.moderate, pr.clean
slop_project_result_free(&pr);
```

Link: `cc myapp.c -Izig-out/include -Lzig-out/lib -lslop -lz -o myapp`

## Cross-compilation

```bash
zig build -Doptimize=ReleaseFast -Dtarget=x86_64-linux-musl
zig build -Doptimize=ReleaseFast -Dtarget=aarch64-linux-musl
zig build -Doptimize=ReleaseFast -Dtarget=aarch64-macos
```

Static binary, no runtime deps. No Windows support (POSIX APIs).

## Tuning

All signal parameters are educated defaults. Override via
`.slop-calibration.json` in project root. See [METRICS.md](METRICS.md)
for the full parameter reference and calibration file format.

Treat the slop score as a **ranking**, not a calibrated probability.
`slop check` and `slop dupes` are deterministic (pattern matching / NCD).
`slop scan` scoring is directional - 5.8 vs 6.2 is noise.

## Limitations

- Short files (< 50 lines) lack data for most signals
- Test files may score higher (repetitive structure, step-style comments)
- Formatters (prettier, gofmt) reduce regularity/indentation signals
- Default Gaussian parameters are guesses - tune for your codebase
- Not an authorship detector - measures code quality patterns, not origin

## Project layout

```
src/
  slop.h       types, constants, API
  main.c       CLI, arg parsing, output
  scan.c       single-pass scanner
  smell.c      smell detectors
  score.c      Bayesian scoring
  compress.c   deflate, NCD
  dupes.c      cross-file duplicate detection
  git.c        git commit features
  lang.c       language detection
  util.c       file I/O, skip logic
  walk.c       directory walking, ignore files
  api.c        library API
tests/
  run.sh       124 integration tests
  fixtures/    test input files
```

## References

- Cilibrasi & Vitanyi 2005, "Clustering by Compression"
- Guo et al. 2017, "On Calibration of Modern Neural Networks" (ICML)
- Nirob et al. 2025, "Whitespaces Don't Lie"
- Ghaleb 2026, "Fingerprinting AI Coding Agents" (MSR '26)
- Ishio et al. 2018, "NCDSearch" (ICSME)
