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
STURDY_PIN_LIBM(float, acoshf, (float x), (x))
STURDY_PIN_LIBM(float, atanhf, (float x), (x))
STURDY_PIN_LIBM(float, coshf, (float x), (x))
STURDY_PIN_LIBM(float, sinhf, (float x), (x))
STURDY_PIN_LIBM(double, cosh, (double x), (x))
STURDY_PIN_LIBM(double, sinh, (double x), (x))

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


// _dl_find_object (glibc 2.35): the fast "which loaded object holds this code address" lookup that libgcc's unwinder (libgcc_eh.a, when
// it is linked into the program statically) calls on every frame it unwinds. Older glibc has only dl_iterate_phdr, which is what libgcc
// itself uses when it was built without _dl_find_object, so that is what this is built on. The result carries the object's mapping and
// its PT_GNU_EH_FRAME; the link map is an object of ours holding the load address (libgcc reads no other field of it on x86-64).
#include <elf.h>
#include <stdint.h>

// <link.h> declares these only under _GNU_SOURCE, which would also turn on glibc's C23 strtol redirection that this file exists to
// undo, so the one structure and function needed are declared here (the layout is glibc's, stable since 2.4).
struct sturdy_phdr_info {
    Elf64_Addr dlpi_addr;
    const char *dlpi_name;
    const Elf64_Phdr *dlpi_phdr;
    Elf64_Half dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};
extern int dl_iterate_phdr(int (*callback)(struct sturdy_phdr_info *info, size_t size, void *data), void *data);

struct sturdy_dl_find_object {
    unsigned long long dlfo_flags;
    void *dlfo_map_start;
    void *dlfo_map_end;
    void *dlfo_link_map;
    void *dlfo_eh_frame;
};

struct sturdy_fake_link_map {
    Elf64_Addr l_addr;
};

struct sturdy_find_state {
    uintptr_t address;
    struct sturdy_dl_find_object *result;
    int found;
};

// Called by dl_iterate_phdr with the loader lock held, so the pool needs no lock of its own.
static int sturdy_find_object_callback(struct sturdy_phdr_info *info, size_t size, void *data) {
    (void)size;
    struct sturdy_find_state *state = (struct sturdy_find_state *)data;
    uintptr_t low = UINTPTR_MAX;
    uintptr_t high = 0;
    const Elf64_Phdr *eh_frame = NULL;
    for (Elf64_Half i = 0; i < info->dlpi_phnum; ++i) {
        const Elf64_Phdr *header = &info->dlpi_phdr[i];
        if (header->p_type == PT_LOAD) {
            const uintptr_t start = (uintptr_t)(info->dlpi_addr + header->p_vaddr);
            const uintptr_t end = start + header->p_memsz;
            if (start < low) {
                low = start;
            }
            if (end > high) {
                high = end;
            }
        } else if (header->p_type == PT_GNU_EH_FRAME) {
            eh_frame = header;
        }
    }
    if (eh_frame == NULL || state->address < low || state->address >= high) {
        return 0;
    }
    static struct sturdy_fake_link_map pool[128];
    static unsigned used;
    struct sturdy_fake_link_map *map = NULL;
    for (unsigned i = 0; i < used; ++i) {
        if (pool[i].l_addr == info->dlpi_addr) {
            map = &pool[i];
            break;
        }
    }
    if (map == NULL) {
        map = &pool[used < sizeof pool / sizeof pool[0] ? used++ : 0];
        map->l_addr = info->dlpi_addr;
    }
    state->result->dlfo_flags = 0;
    state->result->dlfo_map_start = (void *)low;
    state->result->dlfo_map_end = (void *)high;
    state->result->dlfo_link_map = map;
    state->result->dlfo_eh_frame = (void *)(info->dlpi_addr + eh_frame->p_vaddr);
    state->found = 1;
    return 1;
}

STURDY_HIDDEN int _dl_find_object(void *address, struct sturdy_dl_find_object *result) {
    struct sturdy_find_state state = {(uintptr_t)address, result, 0};
    dl_iterate_phdr(sturdy_find_object_callback, &state);
    return state.found ? 0 : -1;
}

#endif
