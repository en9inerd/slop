#!/bin/sh
set -e

SLOP="${SLOP_BIN:-zig-out/bin/slop}"
FIXTURES="tests/fixtures"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m %s — %s\n" "$1" "$2"; }

OUT=/tmp/slop_test_out
LAST_EXIT=0

run() {
    LAST_EXIT=0
    "$@" > "$OUT" 2>&1 || LAST_EXIT=$?
}

check_exit() {
    if [ "$LAST_EXIT" -eq "$2" ]; then
        pass "$1 (exit=$2)"
    else
        fail "$1" "expected exit $2, got $LAST_EXIT"
        cat "$OUT"
    fi
}

check_contains() {
    if grep -q "$2" "$OUT"; then
        pass "$1 (contains '$2')"
    else
        fail "$1" "output missing '$2'"
        head -20 "$OUT"
    fi
}

check_excludes() {
    if grep -q "$2" "$OUT"; then
        fail "$1" "output should not contain '$2'"
        head -20 "$OUT"
    else
        pass "$1 (excludes '$2')"
    fi
}

printf "\n  slop integration tests\n  ──────────────────────\n\n"

# ── Help ──────────────────────────────────────────────────
run "$SLOP" --help
check_exit "help flag" 0
check_contains "help output" "AI slop detector"

# ── Smell: AI file ────────────────────────────────────────
run "$SLOP" smell "$FIXTURES/ai_example.ts"
check_exit "smell ai_example.ts" 2
check_contains "smell finds narration" "narration"
check_contains "smell finds comment-decay" "comment-decay"
check_contains "smell finds over-wrap" "over-wrap"
check_contains "smell 15 findings" "15 finding"

# ── Smell: human file (clean) ────────────────────────────
run "$SLOP" smell "$FIXTURES/human_example.go"
check_exit "smell human_example.go" 0
check_contains "smell human clean" "no smells found"

# ── Smell: Go error suppression ──────────────────────────
run "$SLOP" smell "$FIXTURES/go_err.go"
check_exit "smell go_err.go" 1
check_contains "smell finds err-suppress" "err-suppress"

# ── Smell: TS directives and as-any ──────────────────────
run "$SLOP" smell "$FIXTURES/dupes_a.ts"
check_exit "smell dupes_a.ts" 1
check_contains "smell finds as-any" "as-any"
check_contains "smell finds ts-directive" "ts-directive"

# ── Scan: AI file scored suspicious+ ─────────────────────
run "$SLOP" scan "$FIXTURES/ai_example.ts"
check_exit "scan ai_example.ts" 1

run "$SLOP" scan --json "$FIXTURES/ai_example.ts"
check_contains "scan ai json has probability" "probability"
check_contains "scan ai suspicious+" "suspicious"

# ── Scan: human file scored likely human ─────────────────
run "$SLOP" scan "$FIXTURES/human_example.go"
check_exit "scan human_example.go" 0

run "$SLOP" scan --json "$FIXTURES/human_example.go"
check_contains "scan human json label" "likely human"

# ── Scan: directory mode ─────────────────────────────────
run "$SLOP" scan "$FIXTURES"
check_contains "scan dir shows summary" "scanned"

# ── Scan: --verbose shows signal table ───────────────────
run "$SLOP" scan --verbose "$FIXTURES/ai_example.ts"
check_contains "scan verbose has signal groups" "regularity"
check_contains "scan verbose has narration" "narration"

# ── Scan: stdin mode ─────────────────────────────────────
LAST_EXIT=0
echo 'function foo() { return 1; }' | "$SLOP" scan --stdin --json --lang=js > "$OUT" 2>&1 || LAST_EXIT=$?
check_contains "scan stdin" "probability"

# ── Dupes: finds cross-file duplicates ───────────────────
run "$SLOP" dupes "$FIXTURES"
check_exit "dupes fixtures" 2
check_contains "dupes finds cluster" "cluster"
check_contains "dupes finds fetchUserProfile" "fetchUserProfile"
check_contains "dupes finds getUserDetails" "getUserDetails"

# ── Report: single file ────────────────────────────────────
run "$SLOP" report "$FIXTURES/ai_example.ts"
check_exit "report ai_example.ts" 1
check_contains "report has SCORE section" "SCORE"
check_contains "report has SIGNAL BREAKDOWN" "SIGNAL BREAKDOWN"
check_contains "report has SMELL FINDINGS" "SMELL FINDINGS"
check_contains "report has METHODOLOGY" "METHODOLOGY"
check_contains "report shows LLR formula" "sigmoid"
check_contains "report shows dead code" "dead"

# ── Report: directory ──────────────────────────────────────
run "$SLOP" report "$FIXTURES"
check_exit "report directory" 1
check_contains "report dir has SUMMARY" "SUMMARY"
check_contains "report dir has PROJECT OVERVIEW" "PROJECT OVERVIEW"
check_contains "report dir has dup_ratio" "dup_ratio"
check_contains "report dir has DUPLICATE" "DUPLICATE"

# ── Real-world: telegram-workspace (human-written TS) ─────
REAL="../telegram-workspace"
if [ -d "$REAL" ]; then

run "$SLOP" scan "$REAL"
check_exit "real scan no false positives" 0
check_contains "real scan all human" "0 flagged"
check_contains "real scan no suspicious" "0 suspicious"
check_contains "real scan file count" "scanned 48 file"
check_contains "real scan dead lines" "dead lines detected"
check_contains "real scan includes dist" "scanned 48"

run "$SLOP" scan "$REAL/telebuilder/src/utils.ts"
check_contains "real scan likely human" "likely human"

run "$SLOP" smell "$REAL"
check_exit "real smell finds issues" 2
check_contains "real smell comment-decay" "comment-decay"
check_contains "real smell naming-break" "naming-break"

run "$SLOP" smell --all "$REAL"
check_contains "real smell-all dead code" "dead-code"
check_contains "real smell-all unused import" "unused-import"
check_excludes "real no constructor FP" "constructor() defined but never"

run "$SLOP" dupes "$REAL"
check_exit "real dupes clean" 0
check_contains "real dupes no dupes" "no duplicate functions"

run "$SLOP" report "$REAL"
check_exit "real report" 0
check_contains "real report has methodology" "METHODOLOGY"
check_contains "real report has summary" "SUMMARY"
check_contains "real report has overview" "PROJECT OVERVIEW"
check_contains "real report has signals" "regularity"

run "$SLOP" scan --json "$REAL/telebuilder/src/utils.ts"
check_contains "real scan json" "probability"

run "$SLOP" scan --verbose "$REAL/telebuilder/src/utils.ts"
check_contains "real scan verbose" "comment-to-code"

fi

# ── Edge cases ───────────────────────────────────────────
run "$SLOP" bogus
check_exit "unknown command" 1

LAST_EXIT=0
echo '' | "$SLOP" scan --stdin --lang=js > "$OUT" 2>&1 || LAST_EXIT=$?
check_exit "scan empty stdin" 0

touch /tmp/slop_ec_empty.ts
run "$SLOP" scan /tmp/slop_ec_empty.ts
check_exit "empty file" 0
check_contains "empty file insufficient" "insufficient code"

printf '// only comments\n// nothing else\n' > /tmp/slop_ec_comments.ts
run "$SLOP" scan /tmp/slop_ec_comments.ts
check_contains "all-comments insufficient" "insufficient code"

printf '\000\001\002' > /tmp/slop_ec_binary.ts
run "$SLOP" scan /tmp/slop_ec_binary.ts
check_contains "binary file skipped" "skipped"

printf 'const x = 42;' > /tmp/slop_ec_noeol.ts
run "$SLOP" scan /tmp/slop_ec_noeol.ts
check_exit "no trailing newline" 0

cat > /tmp/slop_ec_strurl.ts << 'FIXTURE'
function buildUrl(base: string): string {
    const url = "https://example.com/api/v1";
    return url;
}
FIXTURE
run "$SLOP" smell /tmp/slop_ec_strurl.ts
check_excludes "url string not narration" "narration"

cat > /tmp/slop_ec_narrmid.ts << 'FIXTURE'
// Firstly, handle things
// Finalize configuration
// Handle first-time setup
// Next-gen features
function setup() { return true; }
FIXTURE
run "$SLOP" smell /tmp/slop_ec_narrmid.ts
check_exit "narration mid-word no match" 0

cat > /tmp/slop_ec_narrhit.ts << 'FIXTURE'
// First, validate input
// Now we check auth
// Finally, send response
function handler() { return true; }
FIXTURE
run "$SLOP" smell /tmp/slop_ec_narrhit.ts
check_exit "narration match exit=2" 2
check_contains "narration finds 3" "3 findings"

cat > /tmp/slop_ec_nullts.ts << 'FIXTURE'
function check(obj: any) {
    if (obj.data !== null) { console.log("a"); }
    if (obj.data !== null) { console.log("b"); }
}
FIXTURE
run "$SLOP" smell /tmp/slop_ec_nullts.ts
check_contains "redundant null TS" "redundant null check"

cat > /tmp/slop_ec_nilgo.go << 'FIXTURE'
package main
func run(err error) {
    if err != nil { return }
    if err != nil { return }
}
FIXTURE
run "$SLOP" smell /tmp/slop_ec_nilgo.go
check_contains "redundant nil Go" "redundant null check"

cat > /tmp/slop_ec_nonepy.py << 'FIXTURE'
def process(data):
    if data is None:
        return
    x = 1
    if data is None:
        return
FIXTURE
run "$SLOP" smell /tmp/slop_ec_nonepy.py
check_contains "redundant None Python" "redundant null check on 'data'"

cat > /tmp/slop_ec_maingo.go << 'FIXTURE'
package main
import "fmt"
func main() { fmt.Println("hello") }
func init() { fmt.Println("init") }
FIXTURE
run "$SLOP" smell --all /tmp/slop_ec_maingo.go
check_exit "main/init not dead" 0

cat > /tmp/slop_ec_export.ts << 'FIXTURE'
export function helper(): string { return "exported"; }
export default function main(): void { console.log("main"); }
FIXTURE
run "$SLOP" smell --all /tmp/slop_ec_export.ts
check_exit "export not dead" 0

cat > /tmp/slop_ec_ctor.ts << 'FIXTURE'
class Foo {
    constructor(x: number) { this.x = x; }
}
FIXTURE
run "$SLOP" smell --all /tmp/slop_ec_ctor.ts
check_excludes "constructor not dead" "constructor() defined"

cat > /tmp/slop_ec_static.c << 'FIXTURE'
#include <stdio.h>

static void unused_internal(void) {
    printf("I am static and never called\n");
    printf("Adding more lines\n");
    printf("So scanner detects the function\n");
}

void public_api(void) {
    printf("I am public\n");
    printf("Might be called from elsewhere\n");
    printf("So not dead code\n");
}

int main(void) {
    public_api();
    return 0;
}
FIXTURE
run "$SLOP" smell --all /tmp/slop_ec_static.c
check_contains "C static dead code" "unused_internal"
check_excludes "C non-static not dead" "public_api() defined"

mkdir -p /tmp/slop_ec_emptydir
run "$SLOP" scan /tmp/slop_ec_emptydir
check_exit "empty dir scan" 0
run "$SLOP" dupes /tmp/slop_ec_emptydir
check_exit "empty dir dupes" 0
run "$SLOP" smell /tmp/slop_ec_emptydir
check_exit "empty dir smell" 0

run "$SLOP" scan /tmp/sad_does_not_exist_ever.ts
check_exit "nonexistent file" 1

mkdir -p "/tmp/slop_edge_case_dir"
printf 'const y = 1;\n' > "/tmp/slop_edge_case_dir/test file.ts"
run "$SLOP" scan "/tmp/slop_edge_case_dir/test file.ts"
check_exit "path with spaces" 0

# ── Summary ──────────────────────────────────────────────
printf "\n  ─────────────────────\n"
TOTAL=$((PASS + FAIL))
if [ "$FAIL" -eq 0 ]; then
    printf "  \033[32m$PASS/$TOTAL passed\033[0m\n\n"
else
    printf "  \033[31m$FAIL/$TOTAL failed\033[0m\n\n"
    exit 1
fi
