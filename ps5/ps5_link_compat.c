/* Keeps the PS5 ELF to the relocation types every loader handles.
 *
 * libvorbisfile holds a table of pointers to libc's fread and ftell (ov_open's default
 * callbacks). A pointer to an imported function stored in data becomes an R_X86_64_64
 * relocation against the import, which the console's ELF loader rejects ("not a valid
 * ELF payload"); the runtime and lifted code only produce RELATIVE and GLOB_DAT.
 * Linked with --wrap=fread --wrap=ftell, every reference goes to these local functions
 * instead, so the table holds addresses inside the ELF (RELATIVE). */
#include <stdio.h>

size_t __real_fread(void *ptr, size_t size, size_t count, FILE *stream);
long __real_ftell(FILE *stream);

size_t __wrap_fread(void *ptr, size_t size, size_t count, FILE *stream) { return __real_fread(ptr, size, count, stream); }
long __wrap_ftell(FILE *stream) { return __real_ftell(stream); }

/* SDL's GTK helper asks whether the process runs set-uid before it would load GTK, which
 * the console never does. The console's libc modules do not export these two; a payload's
 * static libc does, and its copy is used there instead (these are weak). */
#include <sys/types.h>

__attribute__((weak)) int getresuid(uid_t *real, uid_t *effective, uid_t *saved) {
    if (real) *real = 0;
    if (effective) *effective = 0;
    if (saved) *saved = 0;
    return 0;
}

__attribute__((weak)) int getresgid(gid_t *real, gid_t *effective, gid_t *saved) {
    if (real) *real = 0;
    if (effective) *effective = 0;
    if (saved) *saved = 0;
    return 0;
}
