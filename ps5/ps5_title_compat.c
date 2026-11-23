/* Title build only: POSIX functions the game and its libraries import that no module a
 * title loads exports. Linked against the SDK's stubs they would be null at run time and
 * the first call would jump to address 0 (PS5_Vulkan's tools/radv-link.sh has the story).
 * A payload gets them from the payload SDK's static libc instead.
 *
 * Local at link: a name the SDK's stub libraries also define must not be exported from the
 * title, and the title converter refuses exports. */
#include <sys/types.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <netdb.h>

#define LOCAL  /* kept out of the title's exports by the link's version script (build_headless.sh) */

/* The game's file shim matches Windows wildcards (FindFirstFile): '*' and '?', with
 * FNM_CASEFOLD or not. Brackets are ordinary characters, as they are on Windows. */
static int match(const char *p, const char *s, int fold) {
    for (;; ++p, ++s) {
        if (*p == '*') {
            while (*p == '*') ++p;
            if (!*p) return 1;
            for (; *s; ++s)
                if (match(p, s, fold)) return 1;
            return 0;
        }
        if (!*s) return !*p;
        if (*p == '?') continue;
        if (fold ? tolower((unsigned char)*p) != tolower((unsigned char)*s) : *p != *s) return 0;
    }
}

LOCAL int fnmatch(const char *pattern, const char *string, int flags) {
    return match(pattern, string, (flags & FNM_CASEFOLD) != 0) ? 0 : FNM_NOMATCH;
}

/* FFmpeg's and libc++'s logging ask before colouring output: never a terminal. */
LOCAL int isatty(int fd) {
    (void)fd;
    errno = ENOTTY;
    return 0;
}

/* FFmpeg's and RADV's temporary files: the six X's replaced until a new file opens. */
LOCAL int mkstemp(char *template) {
    static unsigned counter;
    const size_t n = strlen(template);
    if (n < 6 || strcmp(template + n - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    for (int attempt = 0; attempt < 1000; ++attempt) {
        unsigned v = (unsigned)getpid() * 2654435761u + ++counter * 40503u;
        for (int i = 0; i < 6; ++i, v /= 36) template[n - 6 + i] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36];
        const int fd = open(template, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST;
    return -1;
}

/* Referenced by library code this game does not reach (std::filesystem's link calls, SDL's
 * process launcher, the driver's and SDL's path lookups): each fails as POSIX lets it. */
LOCAL ssize_t readlink(const char *path, char *buffer, size_t size) {
    (void)path, (void)buffer, (void)size;
    errno = EINVAL;  /* not a symbolic link */
    return -1;
}
LOCAL int link(const char *from, const char *to) {
    (void)from, (void)to;
    errno = ENOSYS;
    return -1;
}
LOCAL int symlink(const char *target, const char *path) {
    (void)target, (void)path;
    errno = ENOSYS;
    return -1;
}
LOCAL long pathconf(const char *path, int name) {
    (void)path, (void)name;
    errno = EINVAL;  /* no limit known for the name */
    return -1;
}
LOCAL pid_t setsid(void) {
    errno = ENOSYS;
    return -1;
}
LOCAL pid_t vfork(void) {
    errno = ENOSYS;
    return -1;
}

/* The console's libc exports these, but the memory they return is from libc's own heap, and
 * the title's free is the platform's heap in direct memory (the link's --wrap=free): freeing
 * one there crashed inside mspace_free. Here they allocate with the title's malloc. */
LOCAL char *strdup(const char *s) {
    const size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    return copy ? memcpy(copy, s, n) : NULL;
}
LOCAL char *strndup(const char *s, size_t limit) {
    size_t n = 0;
    while (n < limit && s[n]) ++n;
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, s, n);
    copy[n] = 0;
    return copy;
}
LOCAL int vasprintf(char **out, const char *format, va_list args) {
    va_list again;
    va_copy(again, args);
    const int n = vsnprintf(NULL, 0, format, again);
    va_end(again);
    *out = n < 0 ? NULL : malloc((size_t)n + 1);
    if (!*out) return -1;
    return vsnprintf(*out, (size_t)n + 1, format, args);
}
LOCAL int asprintf(char **out, const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int n = vasprintf(out, format, args);
    va_end(args);
    return n;
}

/* A contended pthread mutex costs the console about 3.5 microseconds a lock (four threads on
 * one mutex: ps5::benchmark): the waiter sleeps in the kernel and is woken again. The game's
 * critical sections and the heap's locks are held for far less than that, so try for a short
 * while before sleeping (linked with --wrap=pthread_mutex_lock). */
extern int __real_pthread_mutex_lock(pthread_mutex_t *m);
LOCAL int __wrap_pthread_mutex_lock(pthread_mutex_t *m) {
    for (int i = 0; i < 300; ++i) {
        if (pthread_mutex_trylock(m) == 0) return 0;
        __builtin_ia32_pause();
    }
    return __real_pthread_mutex_lock(m);
}

/* SDL's open-URL helper (and the engine's launch-a-browser calls) would fork and exec; the console cannot. */
LOCAL int execl(const char *path, const char *arg, ...) {
    (void)path, (void)arg;
    errno = ENOSYS;
    return -1;
}
LOCAL int execlp(const char *file, const char *arg, ...) {
    (void)file, (void)arg;
    errno = ENOSYS;
    return -1;
}

LOCAL pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}

/* The engine's IpDrv resolves names with gethostbyname; the console's libc does not export it. Name lookup is not
 * supported yet (LAN play by address only), so it reports "not found". */
LOCAL struct hostent *gethostbyname(const char *name) {
    (void)name;
    return NULL;
}

LOCAL char *strcasestr(const char *haystack, const char *needle) {
    const size_t n = strlen(needle);
    if (!n) return (char *)haystack;
    for (; *haystack; ++haystack) {
        size_t i = 0;
        while (i < n && haystack[i] && tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i])) ++i;
        if (i == n) return (char *)haystack;
    }
    return NULL;
}

