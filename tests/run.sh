#!/bin/sh
set -e

SLOP="${SLOP_BIN:-zig-out/bin/slop}"
FIXTURES="tests/fixtures"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m %s - %s\n" "$1" "$2"; }

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
check_contains "help output" "code quality scanner"

# ── Check: AI file ────────────────────────────────────────
run "$SLOP" check "$FIXTURES/ai_example.ts"
check_exit "check ai_example.ts" 2
check_contains "check finds narration" "narration"
check_contains "check finds comment-decay" "comment-decay"
check_contains "check finds over-wrap" "over-wrap"
check_contains "check 15 findings" "15 finding"

# ── Check: human file (clean) ────────────────────────────
run "$SLOP" check "$FIXTURES/human_example.go"
check_exit "check human_example.go" 0
check_contains "check human clean" "no issues found"

# ── Check: Go error suppression ──────────────────────────
run "$SLOP" check "$FIXTURES/go_err.go"
check_exit "check go_err.go" 1
check_contains "check finds err-suppress" "err-suppress"

# ── Check: TS directives and as-any ──────────────────────
run "$SLOP" check "$FIXTURES/dupes_a.ts"
check_exit "check dupes_a.ts" 1
check_contains "check finds as-any" "as-any"
check_contains "check finds ts-directive" "ts-directive"

# ── Smell alias ──────────────────────────────────────────
run "$SLOP" smell "$FIXTURES/ai_example.ts"
check_exit "smell alias works" 2

# ── Scan: AI file scored moderate+ ────────────────────────
run "$SLOP" scan "$FIXTURES/ai_example.ts"
check_exit "scan ai_example.ts" 1

run "$SLOP" scan --json "$FIXTURES/ai_example.ts"
check_contains "scan ai json has slop_score" "slop_score"
check_contains "scan ai moderate+" "moderate"

# ── Scan: human file scored clean ────────────────────────
run "$SLOP" scan "$FIXTURES/human_example.go"
check_exit "scan human_example.go" 0

run "$SLOP" scan --json "$FIXTURES/human_example.go"
check_contains "scan human json label" "clean"

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
check_contains "scan stdin" "slop_score"

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
check_contains "report has FINDINGS" "FINDINGS"
check_contains "report has METHODOLOGY" "METHODOLOGY"
check_contains "report shows formula" "sigmoid"
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
check_contains "real scan all clean" "0 sloppy"
check_contains "real scan no moderate" "0 moderate"
check_contains "real scan file count" "scanned 48 file"
check_contains "real scan dead lines" "dead lines detected"
check_contains "real scan includes dist" "scanned 48"

run "$SLOP" scan "$REAL/telebuilder/src/utils.ts"
check_contains "real scan clean" "clean"

run "$SLOP" check "$REAL"
check_exit "real check finds issues" 2
check_contains "real check comment-decay" "comment-decay"
check_contains "real check naming-break" "naming-break"

run "$SLOP" check --all "$REAL"
check_contains "real check-all dead code" "dead-code"
check_contains "real check-all unused import" "unused-import"
check_excludes "real no constructor FP" "constructor() defined but never"

run "$SLOP" dupes "$REAL"
check_exit "real dupes finds cluster" 2
check_contains "real dupes has cluster" "cluster"

run "$SLOP" report "$REAL"
check_exit "real report" 0
check_contains "real report has methodology" "METHODOLOGY"
check_contains "real report has summary" "SUMMARY"
check_contains "real report has overview" "PROJECT OVERVIEW"
check_contains "real report has signals" "regularity"

run "$SLOP" scan --json "$REAL/telebuilder/src/utils.ts"
check_contains "real scan json" "slop_score"

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
run "$SLOP" check /tmp/slop_ec_strurl.ts
check_excludes "url string not narration" "narration"

cat > /tmp/slop_ec_narrmid.ts << 'FIXTURE'
// Firstly, handle things
// Finalize configuration
// Handle first-time setup
// Next-gen features
function setup() { return true; }
FIXTURE
run "$SLOP" check /tmp/slop_ec_narrmid.ts
check_exit "narration mid-word no match" 0

cat > /tmp/slop_ec_narrhit.ts << 'FIXTURE'
// First, validate input
// Now we check auth
// Finally, send response
function handler() { return true; }
FIXTURE
run "$SLOP" check /tmp/slop_ec_narrhit.ts
check_exit "narration match exit=2" 2
check_contains "narration finds 3" "3 findings"

cat > /tmp/slop_ec_nullts.ts << 'FIXTURE'
function check(obj: any) {
    if (obj.data !== null) { console.log("a"); }
    if (obj.data !== null) { console.log("b"); }
}
FIXTURE
run "$SLOP" check /tmp/slop_ec_nullts.ts
check_contains "redundant null TS" "redundant null check"

cat > /tmp/slop_ec_nilgo.go << 'FIXTURE'
package main
func run(err error) {
    if err != nil { return }
    if err != nil { return }
}
FIXTURE
run "$SLOP" check /tmp/slop_ec_nilgo.go
check_contains "redundant nil Go" "redundant null check"

cat > /tmp/slop_ec_nonepy.py << 'FIXTURE'
def process(data):
    if data is None:
        return
    x = 1
    if data is None:
        return
FIXTURE
run "$SLOP" check /tmp/slop_ec_nonepy.py
check_contains "redundant None Python" "redundant null check on 'data'"

cat > /tmp/slop_ec_maingo.go << 'FIXTURE'
package main
import "fmt"
func main() { fmt.Println("hello") }
func init() { fmt.Println("init") }
FIXTURE
run "$SLOP" check --all /tmp/slop_ec_maingo.go
check_exit "main/init not dead" 0

cat > /tmp/slop_ec_export.ts << 'FIXTURE'
export function helper(): string { return "exported"; }
export default function main(): void { console.log("main"); }
FIXTURE
run "$SLOP" check --all /tmp/slop_ec_export.ts
check_exit "export not dead" 0

cat > /tmp/slop_ec_ctor.ts << 'FIXTURE'
class Foo {
    constructor(x: number) { this.x = x; }
}
FIXTURE
run "$SLOP" check --all /tmp/slop_ec_ctor.ts
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
run "$SLOP" check --all /tmp/slop_ec_static.c
check_contains "C static dead code" "unused_internal"
check_excludes "C non-static not dead" "public_api() defined"

mkdir -p /tmp/slop_ec_emptydir
run "$SLOP" scan /tmp/slop_ec_emptydir
check_exit "empty dir scan" 0
run "$SLOP" dupes /tmp/slop_ec_emptydir
check_exit "empty dir dupes" 0
run "$SLOP" check /tmp/slop_ec_emptydir
check_exit "empty dir check" 0

run "$SLOP" scan /tmp/sad_does_not_exist_ever.ts
check_exit "nonexistent file" 1

mkdir -p "/tmp/slop_edge_case_dir"
printf 'const y = 1;\n' > "/tmp/slop_edge_case_dir/test file.ts"
run "$SLOP" scan "/tmp/slop_edge_case_dir/test file.ts"
check_exit "path with spaces" 0

# ── Syntax: catch/finally/try not treated as functions ────
cat > /tmp/slop_syn_catch.ts << 'FIXTURE'
export function handler(input: string): string {
    try {
        const parsed = JSON.parse(input);
        return parsed.value;
    } catch (err) {
        console.error(err);
        return "fallback";
    } finally {
        console.log("done");
    }
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_catch.ts
check_excludes "catch not a function" "err() defined but never"
check_excludes "catch not dead code" "dead-code"

# ── Syntax: try/catch at top level ────────────────────────
cat > /tmp/slop_syn_trycatch_top.js << 'FIXTURE'
function doWork(data) {
    const x = data.trim();
    return x.toUpperCase();
}
try {
    doWork("hello");
} catch (error) {
    console.log(error);
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_trycatch_top.js
check_excludes "top-level catch not func" "error() defined"

# ── Syntax: struct/enum not treated as functions (C) ──────
cat > /tmp/slop_syn_struct.c << 'FIXTURE'
#include <stdio.h>

struct Point {
    int x;
    int y;
};

enum Color {
    RED,
    GREEN,
    BLUE
};

union Data {
    int i;
    float f;
};

typedef struct {
    int width;
    int height;
} Size;

void draw(struct Point p, enum Color c) {
    printf("draw %d %d %d\n", p.x, p.y, c);
    printf("extra line\n");
    printf("more lines\n");
}

int main(void) {
    struct Point p = {1, 2};
    draw(p, RED);
    return 0;
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_struct.c
check_excludes "struct not a function" "Point() defined"
check_excludes "enum not a function" "Color() defined"
check_excludes "union not a function" "Data() defined"
check_excludes "typedef struct not func" "Size() defined"

# ── Syntax: struct after function (} stop in lookback) ────
cat > /tmp/slop_syn_struct_after.c << 'FIXTURE'
#include <stdio.h>

void helper(int x) {
    printf("helper %d\n", x);
    printf("line 2\n");
    printf("line 3\n");
}

struct Config {
    int timeout;
    int retries;
};

int main(void) {
    helper(1);
    return 0;
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_struct_after.c
check_excludes "struct after func not func" "Config() defined"

# ── Syntax: multi-line C signature detected ──────────────
cat > /tmp/slop_syn_multiline_c.c << 'FIXTURE'
#include <stdio.h>

static void single_line_unused(int a, int b) {
    printf("single %d %d\n", a, b);
    printf("extra line\n");
    printf("more lines\n");
}

static int
multiline_unused(int arg1,
                 int arg2,
                 int arg3) {
    printf("multi %d %d %d\n", arg1, arg2, arg3);
    printf("extra line\n");
    printf("more lines\n");
    return arg1 + arg2 + arg3;
}

int main(void) {
    return 0;
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_multiline_c.c
check_contains "single-line static dead" "single_line_unused"
check_contains "multi-line static dead" "multiline_unused"

# ── Syntax: __attribute__((constructor)) not dead ─────────
cat > /tmp/slop_syn_attr.c << 'FIXTURE'
#include <stdio.h>

__attribute__((constructor))
static void startup_init(void) {
    printf("initializing\n");
    printf("more init\n");
    printf("done init\n");
}

__attribute__((destructor))
static void shutdown_cleanup(void) {
    printf("cleaning up\n");
    printf("more cleanup\n");
    printf("done cleanup\n");
}

int main(void) {
    printf("main\n");
    return 0;
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_attr.c
check_excludes "constructor attr not dead" "startup_init() defined but never"
check_excludes "destructor attr not dead" "shutdown_cleanup() defined but never"

# ── Syntax: TS/JS decorator = exported ────────────────────
cat > /tmp/slop_syn_decorator.ts << 'FIXTURE'
function Component(opts: any) { return (t: any) => t; }

@Component({ selector: "app" })
class AppComponent {
    title = "hello";
    handleClick() {
        console.log(this.title);
        console.log("clicked");
        console.log("done");
    }
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_decorator.ts
check_excludes "decorated class not dead" "AppComponent() defined but never"

# ── Syntax: TS/JS class field arrow detected ──────────────
cat > /tmp/slop_syn_field_arrow.ts << 'FIXTURE'
export class Handler {
    onClick = () => {
        console.log("clicked");
        console.log("line 2");
        console.log("line 3");
    }
    onSubmit = (data: string) => {
        console.log(data);
        console.log("submitted");
        console.log("done");
    }
    render() {
        this.onClick();
        this.onSubmit("test");
    }
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_field_arrow.ts
check_excludes "field arrow onClick not dead" "onClick() defined but never"
check_excludes "field arrow onSubmit not dead" "onSubmit() defined but never"

# ── Syntax: TS/JS framework lifecycle not dead ────────────
cat > /tmp/slop_syn_lifecycle.ts << 'FIXTURE'
export class MyComponent {
    componentDidMount() {
        console.log("mounted");
        console.log("line 2");
        console.log("line 3");
    }
    ngOnInit() {
        console.log("angular init");
        console.log("line 2");
        console.log("line 3");
    }
    getServerSideProps() {
        console.log("next.js SSR");
        console.log("line 2");
        console.log("line 3");
    }
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_lifecycle.ts
check_excludes "componentDidMount not dead" "componentDidMount() defined but never"
check_excludes "ngOnInit not dead" "ngOnInit() defined but never"

# ── Syntax: TS/JS export { name } not dead ────────────────
cat > /tmp/slop_syn_reexport.ts << 'FIXTURE'
function internalHelper(): string {
    console.log("helping");
    console.log("more help");
    return "result";
}

function anotherHelper(): number {
    console.log("computing");
    console.log("more compute");
    return 42;
}

export { internalHelper, anotherHelper };
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_reexport.ts
check_excludes "re-exported func not dead" "internalHelper() defined but never"
check_excludes "re-exported func2 not dead" "anotherHelper() defined but never"

# ── Syntax: module.exports not dead ───────────────────────
cat > /tmp/slop_syn_cjs.js << 'FIXTURE'
function processData(input) {
    const trimmed = input.trim();
    const upper = trimmed.toUpperCase();
    return upper;
}

function formatOutput(data) {
    const result = "[" + data + "]";
    console.log(result);
    return result;
}

module.exports = { processData, formatOutput };
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_cjs.js
check_excludes "cjs export not dead" "processData() defined but never"
check_excludes "cjs export2 not dead" "formatOutput() defined but never"

# ── Syntax: Go multi-line signature ──────────────────────
cat > /tmp/slop_syn_go_multi.go << 'FIXTURE'
package main

import "fmt"

func simpleUnused(x int) {
    fmt.Println(x)
    fmt.Println("line 2")
    fmt.Println("line 3")
}

func multiLineUnused(
    arg1 string,
    arg2 int,
    arg3 bool,
) string {
    fmt.Println(arg1, arg2, arg3)
    fmt.Println("line 2")
    fmt.Println("line 3")
    return arg1
}

func main() {
    fmt.Println("main")
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_go_multi.go
check_contains "Go simple unused detected" "simpleUnused"

# ── Syntax: Go method receiver ────────────────────────────
cat > /tmp/slop_syn_go_method.go << 'FIXTURE'
package main

import "fmt"

type Server struct {
    Name string
}

func (s *Server) Handle(req string) string {
    fmt.Println(s.Name, req)
    fmt.Println("handling")
    fmt.Println("more handling")
    return "ok"
}

func main() {
    s := &Server{Name: "test"}
    s.Handle("ping")
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_go_method.go
check_excludes "Go method receiver not dead" "Handle() defined but never"

# ── Syntax: Go entry points ──────────────────────────────
cat > /tmp/slop_syn_go_entry.go << 'FIXTURE'
package main

import (
    "fmt"
    "testing"
)

func TestSomething(t *testing.T) {
    fmt.Println("test")
    fmt.Println("line 2")
    fmt.Println("line 3")
}

func BenchmarkOp(b *testing.B) {
    fmt.Println("bench")
    fmt.Println("line 2")
    fmt.Println("line 3")
}

func ExampleUsage() {
    fmt.Println("example")
    fmt.Println("line 2")
    fmt.Println("line 3")
}

func FuzzParse(f *testing.F) {
    fmt.Println("fuzz")
    fmt.Println("line 2")
    fmt.Println("line 3")
}

func init() {
    fmt.Println("init")
}

func main() {
    fmt.Println("main")
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_go_entry.go
check_excludes "TestXxx not dead" "TestSomething() defined but never"
check_excludes "BenchmarkXxx not dead" "BenchmarkOp() defined but never"
check_excludes "ExampleXxx not dead" "ExampleUsage() defined but never"
check_excludes "FuzzXxx not dead" "FuzzParse() defined but never"
check_excludes "init not dead" "init() defined but never"

# ── Syntax: Go uppercase = exported ──────────────────────
cat > /tmp/slop_syn_go_export.go << 'FIXTURE'
package mylib

import "fmt"

func PublicAPI(x int) int {
    fmt.Println("public", x)
    fmt.Println("more lines")
    fmt.Println("even more")
    return x * 2
}

func privateHelper(x int) int {
    fmt.Println("private", x)
    fmt.Println("more lines")
    fmt.Println("even more")
    return x + 1
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_go_export.go
check_excludes "Go uppercase not dead" "PublicAPI() defined but never"
check_contains "Go lowercase dead" "privateHelper"

# ── Syntax: Go blank import not flagged ──────────────────
cat > /tmp/slop_syn_go_blank.go << 'FIXTURE'
package main

import (
    "fmt"
    _ "embed"
    _ "net/http/pprof"
)

func main() {
    fmt.Println("using blank imports for side effects")
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_go_blank.go
check_excludes "Go blank import not unused" "unused-import"

# ── Syntax: Go err suppress vs non-err ───────────────────
cat > /tmp/slop_syn_go_suppress.go << 'FIXTURE'
package main

import "fmt"

func work() (int, error) { return 1, nil }

func run() {
    result, _ := work()
    fmt.Println(result)
}

func main() {
    run()
}
FIXTURE
run "$SLOP" check /tmp/slop_syn_go_suppress.go
check_contains "Go multi-return suppress" "err-suppress"

cat > /tmp/slop_syn_go_nosuppress.go << 'FIXTURE'
package main

import "fmt"

func compute() int { return 42 }

func run() {
    _ = fmt.Sprintf("hello %d", compute())
    fmt.Println("done")
}

func main() {
    run()
}
FIXTURE
run "$SLOP" check /tmp/slop_syn_go_nosuppress.go
check_excludes "Go non-err not suppress" "err-suppress"

# ── Syntax: C (void)param not zombie ─────────────────────
cat > /tmp/slop_syn_void_param.c << 'FIXTURE'
#include <stdio.h>

void callback(int event, void *data, int flags) {
    (void)data;
    (void)flags;
    printf("event: %d\n", event);
    printf("handling event\n");
    printf("done handling\n");
}

int main(void) {
    callback(1, 0, 0);
    return 0;
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_void_param.c
check_excludes "void-cast param not zombie" "zombie-param"

# ── Syntax: generated file skipped ────────────────────────
cat > /tmp/slop_syn_generated.go << 'FIXTURE'
// Code generated by protoc-gen-go. DO NOT EDIT.
package generated

func weirdFunction() {
    // First, we initialize
    // Now we validate
    // Finally, we return
    return
}
FIXTURE
run "$SLOP" scan /tmp/slop_syn_generated.go
check_contains "generated file skipped" "skipped"

# ── Syntax: @generated marker skipped ─────────────────────
cat > /tmp/slop_syn_at_generated.ts << 'FIXTURE'
/**
 * @generated
 * This file was auto-generated by the build tool.
 */
function weirdStuff(): void {
    // First, we do stuff
    // Now we check
    // Finally, we return
    return;
}
FIXTURE
run "$SLOP" scan /tmp/slop_syn_at_generated.ts
check_contains "@generated file skipped" "skipped"

# ── Syntax: Go interface body not a function ──────────────
cat > /tmp/slop_syn_go_interface.go << 'FIXTURE'
package main

import "fmt"

type Reader interface {
    Read(p []byte) (int, error)
}

type Handler interface {
    ServeHTTP(w int, r int)
}

func main() {
    fmt.Println("interfaces")
}
FIXTURE
run "$SLOP" check --all /tmp/slop_syn_go_interface.go
check_excludes "Go interface not func" "Read() defined but never"
check_excludes "Go interface2 not func" "ServeHTTP() defined but never"

# ── Summary ──────────────────────────────────────────────
printf "\n  ─────────────────────\n"
TOTAL=$((PASS + FAIL))
if [ "$FAIL" -eq 0 ]; then
    printf "  \033[32m$PASS/$TOTAL passed\033[0m\n\n"
else
    printf "  \033[31m$FAIL/$TOTAL failed\033[0m\n\n"
    exit 1
fi
