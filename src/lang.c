#include "slop.h"
#include <string.h>

typedef struct {
  const char *ext;
  LangFamily family;
  SpecificLang specific;
} ExtMap;

static const ExtMap ext_table[] = {{".ts", LANG_C_LIKE, SPECIFIC_TS},
                                   {".tsx", LANG_C_LIKE, SPECIFIC_TS},
                                   {".js", LANG_C_LIKE, SPECIFIC_JS},
                                   {".jsx", LANG_C_LIKE, SPECIFIC_JS},
                                   {".go", LANG_C_LIKE, SPECIFIC_GO},

                                   {".c", LANG_C_LIKE, SPECIFIC_C},
                                   {".h", LANG_C_LIKE, SPECIFIC_C},
                                   {".cc", LANG_C_LIKE, SPECIFIC_CPP},
                                   {".cpp", LANG_C_LIKE, SPECIFIC_CPP},
                                   {".cxx", LANG_C_LIKE, SPECIFIC_CPP},
                                   {".hpp", LANG_C_LIKE, SPECIFIC_CPP},
                                   {".cs", LANG_C_LIKE, SPECIFIC_OTHER},
                                   {".java", LANG_C_LIKE, SPECIFIC_JAVA},
                                   {".rs", LANG_C_LIKE, SPECIFIC_RUST},
                                   {".swift", LANG_C_LIKE, SPECIFIC_SWIFT},
                                   {".kt", LANG_C_LIKE, SPECIFIC_OTHER},
                                   {".scala", LANG_C_LIKE, SPECIFIC_OTHER},
                                   {".m", LANG_C_LIKE, SPECIFIC_OTHER},
                                   {".mm", LANG_C_LIKE, SPECIFIC_OTHER},
                                   {".dart", LANG_C_LIKE, SPECIFIC_OTHER},
                                   {".php", LANG_C_LIKE, SPECIFIC_OTHER},
                                   {".zig", LANG_C_LIKE, SPECIFIC_OTHER},

                                   {".py", LANG_PYTHON, SPECIFIC_PYTHON},
                                   {".pyi", LANG_PYTHON, SPECIFIC_PYTHON},
                                   {".pyw", LANG_PYTHON, SPECIFIC_PYTHON},

                                   {".sh", LANG_SHELL, SPECIFIC_SHELL},
                                   {".bash", LANG_SHELL, SPECIFIC_SHELL},
                                   {".zsh", LANG_SHELL, SPECIFIC_SHELL},
                                   {".fish", LANG_SHELL, SPECIFIC_SHELL},
                                   {".ksh", LANG_SHELL, SPECIFIC_SHELL},

                                   {nullptr, 0, 0}};

static const char *find_ext(const char *filename) {
  const char *dot = nullptr;
  for (const char *p = filename; *p; p++)
    if (*p == '.')
      dot = p;
  return dot;
}

static const ExtMap *find_entry(const char *filename) {
  const char *dot = find_ext(filename);
  if (!dot)
    return nullptr;
  for (const ExtMap *m = ext_table; m->ext; m++)
    if (strcmp(dot, m->ext) == 0)
      return m;
  return nullptr;
}

LangFamily lang_detect(const char *filename) {
  const ExtMap *m = find_entry(filename);
  return m ? m->family : LANG_C_LIKE;
}

SpecificLang lang_detect_specific(const char *filename) {
  const ExtMap *m = find_entry(filename);
  return m ? m->specific : SPECIFIC_UNKNOWN;
}

const char *lang_family_name(LangFamily f) {
  switch (f) {
  case LANG_C_LIKE:
    return "c-like";
  case LANG_PYTHON:
    return "python";
  case LANG_SHELL:
    return "shell";
  }
  return "unknown";
}

static const char *skip_exts[] = {".min.js",       ".min.css", ".pb.go",
                                  ".generated.cs", ".pb.h",    ".pb.cc",
                                  ".lock",         nullptr};

static bool has_skip_suffix(const char *name) {
  size_t nlen = strlen(name);
  for (int i = 0; skip_exts[i]; i++) {
    size_t slen = strlen(skip_exts[i]);
    if (nlen >= slen && strcmp(name + nlen - slen, skip_exts[i]) == 0)
      return true;
  }
  return false;
}

bool lang_is_source_ext(const char *filename) {
  if (has_skip_suffix(filename))
    return false;
  const char *dot = find_ext(filename);
  if (!dot)
    return false;
  for (const ExtMap *m = ext_table; m->ext; m++)
    if (strcmp(dot, m->ext) == 0)
      return true;
  /* extra extensions not in ext_table but still source */
  static const char *extra[] = {".rb", ".lua", ".pl", ".pm",
                                ".r",  ".R",   ".jl", nullptr};
  for (int i = 0; extra[i]; i++)
    if (strcmp(dot, extra[i]) == 0)
      return true;
  return false;
}
