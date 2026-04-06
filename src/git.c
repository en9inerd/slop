#include "slop.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_conventional(const char *msg, int len) {
  static const char *prefixes[] = {"feat",  "fix",      "chore", "docs",
                                   "style", "refactor", "test",  "ci",
                                   "perf",  "build",    nullptr};
  for (int i = 0; prefixes[i]; i++) {
    int plen = (int)strlen(prefixes[i]);
    if (len < plen)
      continue;
    if (memcmp(msg, prefixes[i], (size_t)plen) != 0)
      continue;
    int j = plen;
    if (j < len && msg[j] == '(') {
      while (j < len && msg[j] != ')')
        j++;
      if (j < len)
        j++;
    }
    if (j < len && msg[j] == ':')
      return true;
    if (j + 1 < len && msg[j] == '!' && msg[j + 1] == ':')
      return true;
  }
  return false;
}

void git_features(GitFeatures *out, const char *filepath) {
  *out = (GitFeatures){};

  char cmd[8192];
  snprintf(cmd, sizeof(cmd),
           "git log -20 --format=\"%%B%%x00\" -- \"%s\" 2>/dev/null", filepath);

  FILE *proc = popen(cmd, "r");
  if (!proc)
    return;

  size_t buf_cap = 4096, buf_len = 0;
  char *buf = malloc(buf_cap);
  if (!buf) {
    pclose(proc);
    return;
  }

  char tmp[4096];
  size_t n;
  while ((n = fread(tmp, 1, sizeof(tmp), proc)) > 0) {
    if (buf_len + n >= buf_cap) {
      buf_cap = (buf_len + n) * 2;
      buf = realloc(buf, buf_cap);
      if (!buf) {
        pclose(proc);
        return;
      }
    }
    memcpy(buf + buf_len, tmp, n);
    buf_len += n;
  }
  int status = pclose(proc);
  if (status != 0 || buf_len == 0) {
    free(buf);
    return;
  }
  buf[buf_len] = '\0';

  int total = 0, multiline = 0, conventional = 0;
  double total_len = 0;

  char *p = buf;
  while ((size_t)(p - buf) < buf_len) {
    char *end = memchr(p, '\0', buf_len - (size_t)(p - buf));
    if (!end)
      break;

    int mlen = (int)(end - p);
    while (mlen > 0 && isspace((unsigned char)p[mlen - 1]))
      mlen--;

    if (mlen > 0) {
      total++;
      total_len += mlen;
      for (int i = 0; i < mlen; i++) {
        if (p[i] == '\n') {
          multiline++;
          break;
        }
      }
      if (is_conventional(p, mlen))
        conventional++;
    }
    p = end + 1;
  }

  free(buf);
  if (total == 0)
    return;

  out->available = true;
  out->multiline_ratio = (double)multiline / total;
  out->conventional_ratio = (double)conventional / total;
  out->avg_msg_length = total_len / total;

  double m_norm = out->multiline_ratio;
  double c_norm = out->conventional_ratio;
  double l_norm = (out->avg_msg_length - GIT_MSG_LEN_MIN) / GIT_MSG_LEN_SPAN;
  if (l_norm < 0)
    l_norm = 0;
  if (l_norm > 1)
    l_norm = 1;

  out->composite = (m_norm + c_norm + l_norm) / 3.0;
}
