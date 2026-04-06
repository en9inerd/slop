#include "slop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *util_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return nullptr; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return nullptr; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

bool util_is_binary(const char *content, size_t len) {
    size_t check = len > BINARY_PROBE_BYTES ? BINARY_PROBE_BYTES : len;
    for (size_t i = 0; i < check; i++)
        if (content[i] == '\0') return true;
    return false;
}

bool util_is_minified(const char *content, size_t len) {
    if (len == 0) return false;
    int lines = 0;
    size_t total_len = 0;
    for (size_t i = 0; i < len; i++) {
        if (content[i] == '\n') lines++;
        else total_len++;
    }
    if (lines < 1) lines = 1;
    return (total_len / (size_t)lines) > MAX_AVG_LINE_LEN;
}

bool util_should_skip(const char *content, size_t len) {
    return util_is_binary(content, len) || util_is_minified(content, len);
}
