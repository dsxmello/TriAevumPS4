// TriAevum PS4 product - relative paths.
//
// The PS4 kernel's path calls (open, stat, mkdir, ...) answer EINVAL for any
// path that does not start with '/', even after a successful chdir, and the
// SDK libc's getcwd is a stub that always fails. The game uses relative paths
// (create_directories("logs"), configuration defaults, std::filesystem's
// absolute()), so every path-taking kernel call is wrapped at link time
// (-Wl,--wrap=..., CMakeLists.txt) and resolves relative paths against the
// working directory OrbisProcessInit enters, the one getcwd reports. fopen,
// opendir, realpath and std::filesystem all reach the kernel through these.

#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {

constexpr char kWorkingDirectory[] = "/data/triaevum";

// Path for the kernel: absolute paths as they are, relative ones under the
// working directory. Null when the result does not fit (errno set).
class KernelPath {
  public:
    explicit KernelPath(const char* path) {
        if (path == nullptr || path[0] == '/' || path[0] == '\0') {
            mPath = path;
            return;
        }
        while (path[0] == '.' && path[1] == '/') path += 2;
        if (path[0] == '.' && path[1] == '\0') {
            mPath = kWorkingDirectory;
            return;
        }
        const size_t prefix = sizeof(kWorkingDirectory) - 1;
        const size_t length = std::strlen(path);
        if (prefix + 1 + length + 1 > sizeof(mBuffer)) {
            errno = ENAMETOOLONG;
            return;
        }
        std::memcpy(mBuffer, kWorkingDirectory, prefix);
        mBuffer[prefix] = '/';
        std::memcpy(mBuffer + prefix + 1, path, length + 1);
        mPath = mBuffer;
    }
    bool Valid(const char* original) const { return mPath != nullptr || original == nullptr; }
    const char* Get() const { return mPath; }

  private:
    const char* mPath = nullptr;
    char mBuffer[1024];
};

} // namespace

#define TRIAEVUM_RESOLVE(name, path)                                                     \
    const KernelPath name(path);                                                         \
    if (!name.Valid(path)) return -1

extern "C" {

int __real_open(const char*, int, ...);
int __real_stat(const char*, struct stat*);
int __real_lstat(const char*, struct stat*);
int __real_mkdir(const char*, mode_t);
int __real_rmdir(const char*);
int __real_unlink(const char*);
int __real_rename(const char*, const char*);
int __real_access(const char*, int);
int __real_chmod(const char*, mode_t);
int __real_truncate(const char*, off_t);
ssize_t __real_readlink(const char*, char*, size_t);
int __real_link(const char*, const char*);
int __real_statvfs(const char*, struct statvfs*);
long __real_pathconf(const char*, int);
int __real_utimensat(int, const char*, const struct timespec[2], int);
int __real_fchmodat(int, const char*, mode_t, int);

char* getcwd(char* buffer, size_t size) {
    constexpr size_t length = sizeof(kWorkingDirectory);
    if (buffer == nullptr) {
        if (size != 0 && size < length) {
            errno = ERANGE;
            return nullptr;
        }
        buffer = static_cast<char*>(std::malloc(length));
        if (buffer == nullptr) {
            errno = ENOMEM;
            return nullptr;
        }
    } else if (size < length) {
        errno = size == 0 ? EINVAL : ERANGE;
        return nullptr;
    }
    std::memcpy(buffer, kWorkingDirectory, length);
    return buffer;
}

int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        mode = static_cast<mode_t>(va_arg(arguments, int));
        va_end(arguments);
    }
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_open(resolved.Get(), flags, mode);
}

int __wrap_stat(const char* path, struct stat* status) {
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_stat(resolved.Get(), status);
}

int __wrap_lstat(const char* path, struct stat* status) {
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_lstat(resolved.Get(), status);
}

int __wrap_mkdir(const char* path, mode_t mode) {
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_mkdir(resolved.Get(), mode);
}

int __wrap_rmdir(const char* path) {
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_rmdir(resolved.Get());
}

int __wrap_unlink(const char* path) {
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_unlink(resolved.Get());
}

int __wrap_rename(const char* from, const char* to) {
    TRIAEVUM_RESOLVE(resolvedFrom, from);
    TRIAEVUM_RESOLVE(resolvedTo, to);
    return __real_rename(resolvedFrom.Get(), resolvedTo.Get());
}

int __wrap_access(const char* path, int mode) {
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_access(resolved.Get(), mode);
}

int __wrap_chmod(const char* path, mode_t mode) {
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_chmod(resolved.Get(), mode);
}

int __wrap_truncate(const char* path, off_t length) {
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_truncate(resolved.Get(), length);
}

ssize_t __wrap_readlink(const char* path, char* buffer, size_t size) {
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_readlink(resolved.Get(), buffer, size);
}

int __wrap_link(const char* existing, const char* created) {
    TRIAEVUM_RESOLVE(resolvedExisting, existing);
    TRIAEVUM_RESOLVE(resolvedCreated, created);
    return __real_link(resolvedExisting.Get(), resolvedCreated.Get());
}

int __wrap_statvfs(const char* path, struct statvfs* status) {
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_statvfs(resolved.Get(), status);
}

long __wrap_pathconf(const char* path, int name) {
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_pathconf(resolved.Get(), name);
}

int __wrap_utimensat(int directory, const char* path, const struct timespec times[2], int flags) {
    if (directory != AT_FDCWD) return __real_utimensat(directory, path, times, flags);
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_utimensat(directory, resolved.Get(), times, flags);
}

int __wrap_fchmodat(int directory, const char* path, mode_t mode, int flags) {
    if (directory != AT_FDCWD) return __real_fchmodat(directory, path, mode, flags);
    TRIAEVUM_RESOLVE(resolved, path);
    return __real_fchmodat(directory, resolved.Get(), mode, flags);
}

} // extern "C"
