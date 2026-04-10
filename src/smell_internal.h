#ifndef SMELL_INTERNAL_H
#define SMELL_INTERNAL_H

#include "slop.h"

/* ── Shared finding helper (defined in smell.c) ──────────── */

__attribute__((format(printf, 5, 6)))
void add_finding(SmellReport *r, SmellKind kind, SmellSeverity sev, int line,
                 const char *fmt, ...);

/* ── Import parsers (imports.c) ──────────────────────────── */

#define MAX_IMPORTS 128

typedef struct {
  char name[64];
  int line;
} ImportEntry;

int collect_imports_ts(const ScanResult *s, ImportEntry *imp);
int collect_imports_go(const ScanResult *s, ImportEntry *imp);
int collect_imports_py(const ScanResult *s, ImportEntry *imp);
bool name_found_in_range(const ScanResult *s, const char *name, int skip_from,
                         int skip_to);

/* ── General severity detectors (smell_general.c) ────────── */

void detect_zombie_params(SmellReport *r, const ScanResult *s);
void detect_unused_imports(SmellReport *r, const ScanResult *s);
void detect_magic_strings(SmellReport *r, const ScanResult *s);
void detect_dead_code(SmellReport *r, const ScanResult *s);

bool is_entry_point(const char *name, SpecificLang spec);
bool func_is_exported(const ScanResult *s, const FuncInfo *fi);
bool name_used_outside_func(const ScanResult *s, const FuncInfo *fi);

#endif /* SMELL_INTERNAL_H */
