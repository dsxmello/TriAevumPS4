// TriAevum PS4 product - PGO instrumented builds only (PS4_PGO=generate).
//
// The host clang's profile runtime (libclang_rt.profile-x86_64.a) is built
// against glibc; its few glibc-only entry points map onto the SDK libc
// here. The runtime writes on exit, which a PS4 app never reaches: a thread
// rewrites /data/triaevum/pgo/product.profraw every 20 seconds (the
// counters are cumulative, so the newest file holds the whole session).

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int __llvm_profile_write_file(void);

// The runtime records build IDs from the program headers it finds through
// the linker's __ehdr_start. On the PS4 the fself does not map the ELF
// header there (1.19: SIGBUS in __llvm_write_binary_ids on the first
// write); this definition takes precedence over the linker's and declares
// no program headers, so no IDs are written.
struct TriaevumElfHeader {
    unsigned char Ident[16];
    unsigned short Type, Machine;
    unsigned int Version;
    unsigned long long Entry, ProgramHeaderOffset, SectionHeaderOffset;
    unsigned int Flags;
    unsigned short HeaderSize, ProgramHeaderSize, ProgramHeaderCount;
    unsigned short SectionHeaderSize, SectionHeaderCount, SectionNameIndex;
};
__attribute__((visibility("hidden"))) const struct TriaevumElfHeader __ehdr_start = {
    {0x7F, 'E', 'L', 'F', 2, 1, 1}, 2, 62, 1, 0, 0, 0, 0, 64, 56, 0, 64, 0, 0};
void __llvm_profile_set_filename(const char* name);

int __fprintf_chk(FILE* stream, int flag, const char* format, ...) {
    (void)flag;
    va_list arguments;
    va_start(arguments, format);
    const int written = vfprintf(stream, format, arguments);
    va_end(arguments);
    return written;
}

int __snprintf_chk(char* buffer, size_t size, int flag, size_t bufferSize, const char* format, ...) {
    (void)flag;
    (void)bufferSize;
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return written;
}

void* __memcpy_chk(void* destination, const void* source, size_t size, size_t destinationSize) {
    (void)destinationSize;
    return memcpy(destination, source, size);
}

void* __memset_chk(void* destination, int value, size_t size, size_t destinationSize) {
    (void)destinationSize;
    return memset(destination, value, size);
}

char* __strncpy_chk(char* destination, const char* source, size_t size, size_t destinationSize) {
    (void)destinationSize;
    return strncpy(destination, source, size);
}

long __isoc23_strtol(const char* text, char** end, int base) {
    return strtol(text, end, base);
}

int prctl(int option, ...) {
    (void)option;
    errno = EINVAL;
    return -1;
}

struct utsname;
int uname(struct utsname* name) {
    (void)name;
    errno = ENOSYS;
    return -1;
}

static void* WriteProfilePeriodically(void* unused) {
    (void)unused;
    const struct timespec period = {20, 0};
    for (;;) {
        nanosleep(&period, NULL);
        const int result = __llvm_profile_write_file();
        fprintf(stderr, "[triaevum] pgo: profile written (%d)\n", result);
    }
    return NULL;
}

void TriaevumPs4StartProfileWriter(void) {
    mkdir("/data/triaevum/pgo", 0777);
    __llvm_profile_set_filename("/data/triaevum/pgo/product.profraw");
    pthread_t thread;
    if (pthread_create(&thread, NULL, WriteProfilePeriodically, NULL) == 0) {
        fprintf(stderr, "[triaevum] pgo: instrumented build, writing every 20 s\n");
    }
}
