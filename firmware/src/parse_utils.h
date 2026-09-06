#ifndef IO_DOCK_PARSE_UTILS_H
#define IO_DOCK_PARSE_UTILS_H
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
/* Complete-token conversion, checked before any narrowing or driver call. */
static inline int parse_integer(const char *s, int base, int64_t low, int64_t high, int64_t *out) {
    if (!s || !*s || (unsigned char)*s <= 32 || (low >= 0 && *s == '-')) return 1;
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, base);
    if (end == s || *end) return 1;
    if (errno == ERANGE || v < low || v > high) return 3;
    *out = v;
    return 0;
}
static inline int parse_channel(const char *s, const char *prefix, unsigned first, unsigned count) {
    size_t n = strlen(prefix);
    if (!s || strlen(s) != n + 1 || strncasecmp(s, prefix, n)) return -1;
    unsigned ch = (unsigned)(s[n] - '0');
    return ch >= first && ch - first < count ? (int)(ch - first) : -1;
}
#endif
