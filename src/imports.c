#include "smell_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── TS/JS import parser ─────────────────────────────────── */

int collect_imports_ts(const ScanResult *s, ImportEntry *imp) {
  int count = 0;
  const char *p = s->content;
  size_t len = s->content_len;
  int line = 1;

  for (size_t i = 0; i < len && count < MAX_IMPORTS; i++) {
    if (p[i] == '\n') {
      line++;
      continue;
    }
    if (s->byte_kind && s->byte_kind[i] != 0)
      continue;

    if (i + 7 >= len || memcmp(p + i, "import ", 7) != 0)
      continue;
    if (i > 0 && isalnum((unsigned char)p[i - 1]))
      continue;

    size_t j = i + 7;
    while (j < len && isspace((unsigned char)p[j]))
      j++;

    if (j + 5 < len && memcmp(p + j, "type ", 5) == 0 &&
        (j + 5 >= len || p[j + 5] == '{' || isalpha((unsigned char)p[j + 5])))
      j += 5;

    while (j < len && isspace((unsigned char)p[j]))
      j++;

    if (j < len && p[j] == '{') {
      j++;
      while (j < len && p[j] != '}' && count < MAX_IMPORTS) {
        while (j < len && (isspace((unsigned char)p[j]) || p[j] == ','))
          j++;
        if (j >= len || p[j] == '}')
          break;
        if (j + 5 < len && memcmp(p + j, "type ", 5) == 0)
          j += 5;
        while (j < len && isspace((unsigned char)p[j]))
          j++;
        int k = 0;
        size_t j0 = j;
        while (j < len && k < 63 &&
               (isalnum((unsigned char)p[j]) || p[j] == '_'))
          imp[count].name[k++] = p[j++];
        imp[count].name[k] = '\0';
        if (j == j0) {
          j++;
          continue;
        }
        while (j < len && isspace((unsigned char)p[j]))
          j++;
        if (j + 3 < len && memcmp(p + j, "as ", 3) == 0) {
          j += 3;
          while (j < len && isspace((unsigned char)p[j]))
            j++;
          k = 0;
          while (j < len && k < 63 &&
                 (isalnum((unsigned char)p[j]) || p[j] == '_'))
            imp[count].name[k++] = p[j++];
          imp[count].name[k] = '\0';
        }
        if (k > 0) {
          imp[count].line = line;
          count++;
        }
      }
    } else if (j < len && p[j] == '*') {
      j++;
      while (j < len && isspace((unsigned char)p[j]))
        j++;
      if (j + 3 < len && memcmp(p + j, "as ", 3) == 0) {
        j += 3;
        while (j < len && isspace((unsigned char)p[j]))
          j++;
        int k = 0;
        while (j < len && k < 63 &&
               (isalnum((unsigned char)p[j]) || p[j] == '_'))
          imp[count].name[k++] = p[j++];
        imp[count].name[k] = '\0';
        if (k > 0) {
          imp[count].line = line;
          count++;
        }
      }
    } else if (j < len && isalpha((unsigned char)p[j])) {
      int k = 0;
      while (j < len && k < 63 && (isalnum((unsigned char)p[j]) || p[j] == '_'))
        imp[count].name[k++] = p[j++];
      imp[count].name[k] = '\0';
      if (k > 0 && strcmp(imp[count].name, "type") != 0) {
        imp[count].line = line;
        count++;
      }
    }

    while (i < len && p[i] != '\n')
      i++;
    if (i < len)
      line++;
  }
  return count;
}

/* ── Go import parser ────────────────────────────────────── */

int collect_imports_go(const ScanResult *s, ImportEntry *imp) {
  int count = 0;
  const char *p = s->content;
  size_t len = s->content_len;
  int line = 1;

  for (size_t i = 0; i < len && count < MAX_IMPORTS; i++) {
    if (p[i] == '\n') {
      line++;
      continue;
    }
    if (s->byte_kind && s->byte_kind[i] != 0)
      continue;
    if (i + 7 >= len || memcmp(p + i, "import ", 7) != 0)
      continue;
    if (i > 0 && isalnum((unsigned char)p[i - 1]))
      continue;

    size_t j = i + 7;
    while (j < len && isspace((unsigned char)p[j])) {
      if (p[j] == '\n')
        line++;
      j++;
    }

    bool grouped = (j < len && p[j] == '(');
    if (grouped)
      j++;

    do {
      while (j < len && isspace((unsigned char)p[j])) {
        if (p[j] == '\n')
          line++;
        j++;
      }
      if (grouped && j < len && p[j] == ')')
        break;
      if (j >= len)
        break;

      char alias[64] = {};
      if (j < len && p[j] != '"' && p[j] != ')' && p[j] != '\n') {
        int k = 0;
        while (j < len && k < 63 && !isspace((unsigned char)p[j]) &&
               p[j] != '"')
          alias[k++] = p[j++];
        alias[k] = '\0';
        while (j < len && isspace((unsigned char)p[j]) && p[j] != '\n')
          j++;
      }

      if (j < len && p[j] == '"') {
        j++;
        size_t last_slash = j;
        while (j < len && p[j] != '"') {
          if (p[j] == '/')
            last_slash = j + 1;
          j++;
        }
        size_t pkg_end = j;
        if (j < len)
          j++;

        if (alias[0] != '.' && alias[0] != '_') {
          int k = 0;
          if (alias[0] && alias[0] != '.') {
            snprintf(imp[count].name, sizeof(imp[count].name), "%s", alias);
          } else {
            for (size_t x = last_slash; x < pkg_end && k < 63; x++)
              imp[count].name[k++] = p[x];
            imp[count].name[k] = '\0';
          }
          if (imp[count].name[0] && count < MAX_IMPORTS) {
            imp[count].line = line;
            count++;
          }
        }
      }
      while (j < len && p[j] != '\n' && p[j] != ')')
        j++;
      if (j < len && p[j] == '\n')
        line++;
    } while (grouped && j < len);

    i = j;
  }
  return count;
}

/* ── Python import parser ────────────────────────────────── */

int collect_imports_py(const ScanResult *s, ImportEntry *imp) {
  int count = 0;
  const char *p = s->content;
  size_t len = s->content_len;
  int line = 1;

  for (size_t i = 0; i < len && count < MAX_IMPORTS; i++) {
    if (p[i] == '\n') {
      line++;
      continue;
    }
    if (s->byte_kind && s->byte_kind[i] != 0)
      continue;

    bool is_from = (i + 5 < len && memcmp(p + i, "from ", 5) == 0);
    bool is_import = (i + 7 < len && memcmp(p + i, "import ", 7) == 0);
    if (!is_from && !is_import)
      continue;
    if (i > 0 && isalnum((unsigned char)p[i - 1]))
      continue;

    if (is_from) {
      size_t j = i + 5;
      while (j < len && !isspace((unsigned char)p[j]))
        j++;
      while (j < len && isspace((unsigned char)p[j]))
        j++;
      if (j + 7 >= len || memcmp(p + j, "import ", 7) != 0) {
        while (i < len && p[i] != '\n')
          i++;
        if (i < len)
          line++;
        continue;
      }
      j += 7;
      while (j < len && p[j] != '\n' && p[j] != '#' && count < MAX_IMPORTS) {
        while (j < len && (isspace((unsigned char)p[j]) || p[j] == ',' ||
                           p[j] == '(' || p[j] == ')'))
          j++;
        if (j >= len || p[j] == '\n' || p[j] == '#')
          break;
        if (p[j] == '*') {
          j++;
          continue;
        }
        int k = 0;
        size_t j0 = j;
        while (j < len && k < 63 &&
               (isalnum((unsigned char)p[j]) || p[j] == '_'))
          imp[count].name[k++] = p[j++];
        imp[count].name[k] = '\0';
        if (j == j0) {
          j++;
          continue;
        }
        while (j < len && isspace((unsigned char)p[j]))
          j++;
        if (j + 3 < len && memcmp(p + j, "as ", 3) == 0) {
          j += 3;
          while (j < len && isspace((unsigned char)p[j]))
            j++;
          k = 0;
          while (j < len && k < 63 &&
                 (isalnum((unsigned char)p[j]) || p[j] == '_'))
            imp[count].name[k++] = p[j++];
          imp[count].name[k] = '\0';
        }
        if (k > 0) {
          imp[count].line = line;
          count++;
        }
      }
    } else {
      size_t j = i + 7;
      while (j < len && isspace((unsigned char)p[j]))
        j++;
      int k = 0;
      while (j < len && k < 63 && (isalnum((unsigned char)p[j]) || p[j] == '_'))
        imp[count].name[k++] = p[j++];
      imp[count].name[k] = '\0';
      while (j < len && isspace((unsigned char)p[j]))
        j++;
      if (j + 3 < len && memcmp(p + j, "as ", 3) == 0) {
        j += 3;
        while (j < len && isspace((unsigned char)p[j]))
          j++;
        k = 0;
        while (j < len && k < 63 &&
               (isalnum((unsigned char)p[j]) || p[j] == '_'))
          imp[count].name[k++] = p[j++];
        imp[count].name[k] = '\0';
      }
      if (k > 0) {
        imp[count].line = line;
        count++;
      }
    }
    while (i < len && p[i] != '\n')
      i++;
    if (i < len)
      line++;
  }
  return count;
}

/* ── Name search (used by unused imports and dead code) ──── */

bool name_found_in_range(const ScanResult *s, const char *name, int skip_from,
                         int skip_to) {
  int nlen = (int)strlen(name);
  if (nlen == 0)
    return true;
  const char *p = s->content;
  size_t len = s->content_len;
  int line = 1;

  for (size_t i = 0; i + (size_t)nlen <= len; i++) {
    if (p[i] == '\n') {
      line++;
      continue;
    }
    if (line >= skip_from && line <= skip_to)
      continue;
    if (s->byte_kind && s->byte_kind[i] != 0)
      continue;
    if (memcmp(p + i, name, (size_t)nlen) != 0)
      continue;
    bool wb =
        (i == 0 || (!isalnum((unsigned char)p[i - 1]) && p[i - 1] != '_'));
    size_t end = i + (size_t)nlen;
    bool wa =
        (end >= len || (!isalnum((unsigned char)p[end]) && p[end] != '_'));
    if (wb && wa)
      return true;
  }
  return false;
}
