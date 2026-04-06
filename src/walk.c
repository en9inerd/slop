#include "slop.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *skip_dirs[] = {"node_modules", "vendor",      ".git",  "build",    "dist",
                                  "target",       "__pycache__", ".venv", "venv",     ".tox",
                                  ".mypy_cache",  ".next",       ".nuxt", "coverage", nullptr};

static bool should_skip_dir(const char *name) {
    for (int i = 0; skip_dirs[i]; i++)
        if (strcmp(name, skip_dirs[i]) == 0) return true;
    return false;
}

/* ── .slopignore ─────────────────────────────────────────── */
/* Note: static storage is safe for sequential calls (each fl_collect
   resets the list). NOT safe for concurrent/threaded use. */

#define MAX_IGNORE_ENTRIES 256
#define MAX_IGNORE_LEN     256

static char s_ignore[MAX_IGNORE_ENTRIES][MAX_IGNORE_LEN];
static int s_ignore_n = 0;

static void load_slopignore(const char *dirpath) {
    s_ignore_n = 0;
    char path[4096];
    size_t dlen = strlen(dirpath);
    while (dlen > 1 && dirpath[dlen - 1] == '/') dlen--;
    snprintf(path, sizeof(path), "%.*s/.slopignore", (int)dlen, dirpath);

    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[MAX_IGNORE_LEN];
    while (fgets(line, sizeof(line), f) && s_ignore_n < MAX_IGNORE_ENTRIES) {
        int l = (int)strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (l > 0 && line[0] != '#') snprintf(s_ignore[s_ignore_n++], MAX_IGNORE_LEN, "%s", line);
    }
    fclose(f);
}

static bool matches_slopignore(const char *path) {
    for (int i = 0; i < s_ignore_n; i++) {
        const char *p = path;
        while (*p) {
            const char *end = p;
            while (*end && *end != '/') end++;
            int seglen = (int)(end - p);
            int elen = (int)strlen(s_ignore[i]);
            if (seglen == elen && memcmp(p, s_ignore[i], (size_t)seglen) == 0) return true;
            p = *end ? end + 1 : end;
        }
    }
    return false;
}

/* ── Path helpers ───────────────────────────────────────── */

static void join_path(char *buf, size_t bufsz, const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    while (dlen > 1 && dir[dlen - 1] == '/') dlen--;
    snprintf(buf, bufsz, "%.*s/%s", (int)dlen, dir, name);
}

/* ── FileList ───────────────────────────────────────────── */

void fl_init(FileList *fl) {
    fl->count = 0;
    fl->cap = 1024;
    fl->paths = malloc((size_t)fl->cap * sizeof(char *));
}

void fl_add(FileList *fl, const char *path) {
    if (fl->count >= fl->cap) {
        fl->cap *= 2;
        fl->paths = realloc(fl->paths, (size_t)fl->cap * sizeof(char *));
    }
    fl->paths[fl->count++] = strdup(path);
}

void fl_free(FileList *fl) {
    for (int i = 0; i < fl->count; i++) free(fl->paths[i]);
    free(fl->paths);
    fl->count = 0;
    fl->paths = nullptr;
}

/* ── Recursive walk ─────────────────────────────────────── */

static void walk_recurse(FileList *fl, const char *dirpath) {
    DIR *d = opendir(dirpath);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        char path[4096];
        join_path(path, sizeof(path), dirpath, ent->d_name);
        if (matches_slopignore(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!should_skip_dir(ent->d_name)) walk_recurse(fl, path);
        } else if (S_ISREG(st.st_mode) && lang_is_source_ext(ent->d_name)) {
            fl_add(fl, path);
        }
    }
    closedir(d);
}

/* ── Git-aware walk ─────────────────────────────────────── */

static bool is_git_repo(const char *dirpath) {
    char cmd[4200];
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-parse --git-dir 2>/dev/null", dirpath);
    FILE *proc = popen(cmd, "r");
    if (!proc) return false;
    char buf[256];
    bool ok = fread(buf, 1, sizeof(buf), proc) > 0;
    int rc = pclose(proc);
    return ok && rc == 0;
}

static void walk_git(FileList *fl, const char *dirpath) {
    char cmd[4200];
    snprintf(cmd, sizeof(cmd),
             "git -C \"%s\" ls-files --cached --others --exclude-standard 2>/dev/null", dirpath);
    FILE *proc = popen(cmd, "r");
    if (!proc) return;
    char line[4096];
    while (fgets(line, sizeof(line), proc)) {
        int l = (int)strlen(line);
        if (l > 0 && line[l - 1] == '\n') line[--l] = '\0';
        if (!lang_is_source_ext(line)) continue;
        if (matches_slopignore(line)) continue;
        char full[8192];
        join_path(full, sizeof(full), dirpath, line);
        fl_add(fl, full);
    }
    pclose(proc);
}

void fl_collect(FileList *fl, const char *dirpath) {
    load_slopignore(dirpath);
    if (is_git_repo(dirpath)) walk_git(fl, dirpath);
    else walk_recurse(fl, dirpath);
}
