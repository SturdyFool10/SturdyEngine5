// glibc version pinning for self-contained Linux executables (cmake/SturdySelfContained.cmake).
//
// A binary linked on a current glibc records, for every libc/libm function, the *newest* symbol version that library
// exports. glibc 2.43, for example, re-versioned sqrtf/acosf/... (new errno/exception semantics), so an executable built
// on it refuses to start on any system older than 2.43 even though it uses nothing new. The old versions are still
// exported, so this file defines the affected functions in the executable itself, each forwarding to the original
// version (`.symver`). Every reference in the link -- engine code, static third-party libraries, libstdc++.a -- then
// binds to these definitions instead of the newest libm/libc one.
//
// Functions glibc added *without* an older version (the C23 `__isoc23_*` redirections, arc4random) are implemented on
// top of their pre-C23 / getrandom equivalents.
//
// Compiled as C (not C++) on purpose: in C17 mode glibc's headers do not redirect strtol and friends to `__isoc23_*`.
// Add a function here when `tools`-style inspection (llvm-readelf -V / the post-link check) reports a symbol version
// above the floor named in STURDY_GLIBC_FLOOR. x86-64 only: the pinned version names are architecture-specific.

#include <features.h>

#if defined(__linux__) && defined(__GLIBC__) && defined(__x86_64__)

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define STURDY_HIDDEN __attribute__((visibility("hidden")))

// libm: forward to the x86-64 baseline version.
#define STURDY_PIN_LIBM(ret, name, params, args)                                                                          \
    extern ret sturdy_glibc_old_##name params;                                                                            \
    __asm__(".symver sturdy_glibc_old_" #name "," #name "@GLIBC_2.2.5");                                                  \
    STURDY_HIDDEN ret name params {                                                                                       \
        return sturdy_glibc_old_##name args;                                                                              \
    }

STURDY_PIN_LIBM(float, sqrtf, (float x), (x))
STURDY_PIN_LIBM(float, acosf, (float x), (x))
STURDY_PIN_LIBM(float, asinf, (float x), (x))
STURDY_PIN_LIBM(float, atan2f, (float y, float x), (y, x))
STURDY_PIN_LIBM(float, log10f, (float x), (x))
STURDY_PIN_LIBM(float, remainderf, (float x, float y), (x, y))
STURDY_PIN_LIBM(float, fmodf, (float x, float y), (x, y))
STURDY_PIN_LIBM(double, fmod, (double x, double y), (x, y))
STURDY_PIN_LIBM(float, hypotf, (float x, float y), (x, y))
STURDY_PIN_LIBM(double, hypot, (double x, double y), (x, y))

// C23 string-to-integer entry points (glibc 2.38). The only behavioural difference from the originals is the C23
// "0b" binary prefix for base 0/2, which nothing in the engine or its dependencies parses.
extern long strtol(const char *, char **, int);
extern long long strtoll(const char *, char **, int);
extern unsigned long strtoul(const char *, char **, int);
extern unsigned long long strtoull(const char *, char **, int);
extern int vsscanf(const char *, const char *, va_list);

STURDY_HIDDEN long __isoc23_strtol(const char *s, char **end, int base) { return strtol(s, end, base); }
STURDY_HIDDEN long long __isoc23_strtoll(const char *s, char **end, int base) { return strtoll(s, end, base); }
STURDY_HIDDEN unsigned long __isoc23_strtoul(const char *s, char **end, int base) { return strtoul(s, end, base); }
STURDY_HIDDEN unsigned long long __isoc23_strtoull(const char *s, char **end, int base) { return strtoull(s, end, base); }
STURDY_HIDDEN int __isoc23_vsscanf(const char *s, const char *fmt, va_list ap) { return vsscanf(s, fmt, ap); }
STURDY_HIDDEN int __isoc23_sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int result = vsscanf(s, fmt, ap);
    va_end(ap);
    return result;
}

extern long wcstol(const wchar_t *, wchar_t **, int);
extern long long wcstoll(const wchar_t *, wchar_t **, int);
extern unsigned long wcstoul(const wchar_t *, wchar_t **, int);
extern unsigned long long wcstoull(const wchar_t *, wchar_t **, int);
extern int vfscanf(FILE *, const char *, va_list);
extern int vscanf(const char *, va_list);

STURDY_HIDDEN long __isoc23_wcstol(const wchar_t *s, wchar_t **end, int base) { return wcstol(s, end, base); }
STURDY_HIDDEN long long __isoc23_wcstoll(const wchar_t *s, wchar_t **end, int base) { return wcstoll(s, end, base); }
STURDY_HIDDEN unsigned long __isoc23_wcstoul(const wchar_t *s, wchar_t **end, int base) { return wcstoul(s, end, base); }
STURDY_HIDDEN unsigned long long __isoc23_wcstoull(const wchar_t *s, wchar_t **end, int base) { return wcstoull(s, end, base); }
STURDY_HIDDEN int __isoc23_vfscanf(FILE *f, const char *fmt, va_list ap) { return vfscanf(f, fmt, ap); }
STURDY_HIDDEN int __isoc23_vscanf(const char *fmt, va_list ap) { return vscanf(fmt, ap); }
STURDY_HIDDEN int __isoc23_fscanf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int result = vfscanf(f, fmt, ap);
    va_end(ap);
    return result;
}
STURDY_HIDDEN int __isoc23_scanf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int result = vscanf(fmt, ap);
    va_end(ap);
    return result;
}

// Bounded string copy/concatenate (glibc 2.38). Same contract as the BSD functions: the result is always terminated when the
// buffer has room for at least one character, and the return value is the length the full result would have had.
STURDY_HIDDEN size_t strlcpy(char *dst, const char *src, size_t size) {
    const size_t length = strlen(src);
    if (size != 0) {
        const size_t copied = length >= size ? size - 1 : length;
        memcpy(dst, src, copied);
        dst[copied] = '\0';
    }
    return length;
}

STURDY_HIDDEN size_t strlcat(char *dst, const char *src, size_t size) {
    const size_t used = strnlen(dst, size);
    if (used == size) {
        return size + strlen(src);
    }
    return used + strlcpy(dst + used, src, size - used);
}

STURDY_HIDDEN size_t wcslcpy(wchar_t *dst, const wchar_t *src, size_t size) {
    const size_t length = wcslen(src);
    if (size != 0) {
        const size_t copied = length >= size ? size - 1 : length;
        wmemcpy(dst, src, copied);
        dst[copied] = L'\0';
    }
    return length;
}

STURDY_HIDDEN size_t wcslcat(wchar_t *dst, const wchar_t *src, size_t size) {
    size_t used = 0;
    while (used < size && dst[used] != L'\0') {
        ++used;
    }
    if (used == size) {
        return size + wcslen(src);
    }
    return used + wcslcpy(dst + used, src, size - used);
}

// arc4random family (glibc 2.36), used by libstdc++'s std::random_device when it was built against a new glibc.
extern long syscall(long, ...);
#define STURDY_SYS_getrandom 318 /* x86-64 */

static void sturdy_fill_random(void *buffer, size_t size) {
    unsigned char *bytes = (unsigned char *)buffer;
    while (size > 0) {
        const long got = syscall(STURDY_SYS_getrandom, bytes, size, 0u);
        if (got <= 0) {
            continue; // EINTR; getrandom without GRND_NONBLOCK never reports "no entropy" once the pool is seeded
        }
        bytes += got;
        size -= (size_t)got;
    }
}

STURDY_HIDDEN void arc4random_buf(void *buffer, size_t size) { sturdy_fill_random(buffer, size); }

STURDY_HIDDEN uint32_t arc4random(void) {
    uint32_t value = 0;
    sturdy_fill_random(&value, sizeof value);
    return value;
}

STURDY_HIDDEN uint32_t arc4random_uniform(uint32_t upper_bound) {
    if (upper_bound < 2) {
        return 0;
    }
    // Rejection sampling avoids modulo bias: discard the first (2^32 mod bound) values.
    const uint32_t min = (uint32_t)(-upper_bound) % upper_bound;
    uint32_t value;
    do {
        value = arc4random();
    } while (value < min);
    return value % upper_bound;
}

#endif
