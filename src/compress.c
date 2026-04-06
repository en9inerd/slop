#include "slop.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static size_t compressed_size(const char *data, size_t len) {
  if (len == 0)
    return 0;
  uLong bound = compressBound((uLong)len);
  Bytef *buf = malloc(bound);
  if (!buf)
    return 0;
  uLongf out_len = bound;
  int rc =
      compress2(buf, &out_len, (const Bytef *)data, (uLong)len, ZLIB_LEVEL);
  free(buf);
  return (rc == Z_OK) ? (size_t)out_len : 0;
}

double compress_ratio(const char *data, size_t len) {
  if (len < MIN_COMPRESS_BYTES)
    return -1.0;
  size_t csz = compressed_size(data, len);
  if (csz == 0)
    return -1.0;
  return (double)csz / (double)len;
}

double compress_ncd(const char *x, size_t xlen, const char *y, size_t ylen) {
  if (xlen == 0 || ylen == 0)
    return 1.0;

  size_t cx = compressed_size(x, xlen);
  size_t cy = compressed_size(y, ylen);

  char *xy = malloc(xlen + ylen);
  if (!xy)
    return 1.0;
  memcpy(xy, x, xlen);
  memcpy(xy + xlen, y, ylen);
  size_t cxy = compressed_size(xy, xlen + ylen);
  free(xy);

  if (cx == 0 || cy == 0 || cxy == 0)
    return 1.0;

  size_t min_c = cx < cy ? cx : cy;
  size_t max_c = cx > cy ? cx : cy;
  if (max_c == 0)
    return 1.0;

  double ncd;
  if (cxy <= min_c)
    ncd = 0;
  else
    ncd = (double)(cxy - min_c) / (double)max_c;
  if (ncd > 1.0)
    ncd = 1.0;
  return ncd;
}
