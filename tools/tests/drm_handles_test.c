#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* Only the device/driver boundary is simulated. Descriptors and their lifetimes are real.
 * F_DUPFD_QUERY is modelled so this test also runs on older CI kernels; the device probe
 * separately exercises the actual query on Android. */
#define TEST_FD_LIMIT 65536
static _Atomic unsigned identities[TEST_FD_LIMIT];
static unsigned identity_serial = 1;
static int identity_denied, kcmp_works, kgsl_present;
static int kernel_error = EINVAL;
static int native_imports, native_exports, native_closes;
static const char *driver_name = "pvr";
static int fixture_stat(const char *path, struct stat *st);
static long fixture_syscall(long number, ...);
static int fixture_fcntl(int fd, int op, int arg);
static void *fixture_dlsym(void *object, const char *name);
static int fixture_close(int fd);

#define stat(path, st) fixture_stat(path, st)
#define syscall fixture_syscall
#define fcntl fixture_fcntl
#define dlsym fixture_dlsym
#define close fixture_close
#include "../linuxfs/preload/drm.c"
#undef stat
#undef syscall
#undef fcntl
#undef dlsym
#undef close

static int test_open(const char *path) {
  int fd = open(path, O_RDWR | O_CLOEXEC);
  assert(fd >= 0 && fd < TEST_FD_LIMIT);
  identities[fd] = identity_serial++;
  return fd;
}

static int fixture_close(int fd) {
  if (fd >= 0 && fd < TEST_FD_LIMIT) identities[fd] = 0;
  return close(fd);
}

static int fixture_stat(const char *path, struct stat *st) {
  if (!strcmp(path, "/dev/kgsl-3d0")) {
    if (kgsl_present) return stat("/dev/zero", st);
    errno = ENOENT;
    return -1;
  }
  if (!strcmp(path, "/dev/dri/renderD128")) return stat("/dev/null", st);
  return stat(path, st);
}

/* The intercepted DRM calls all take an integer argument. Keep that contract
 * explicit; native_import's argument-free F_GETFD uses libc after #undef above. */
static int fixture_fcntl(int fd, int op, int arg) {
  if (op == F_DUPFD_QUERY) {
    if (identity_denied) { errno = EINVAL; return -1; }
    if (fd < 0 || arg < 0 || fd >= TEST_FD_LIMIT || arg >= TEST_FD_LIMIT ||
        !identities[fd] || !identities[arg]) { errno = EBADF; return -1; }
    return identities[fd] == identities[arg];
  }
  int ret = fcntl(fd, op, arg);
  if (ret >= 0 && (op == F_DUPFD || op == F_DUPFD_CLOEXEC)) {
    assert(ret < TEST_FD_LIMIT);
    identities[ret] = identities[fd];
  }
  return ret;
}

static long fixture_syscall(long number, ...) {
  va_list ap;
  va_start(ap, number);
  if (number == SYS_ioctl) {
    (void)va_arg(ap, int);
    unsigned long request = va_arg(ap, unsigned long);
    struct bl_drm_version *v = va_arg(ap, void *);
    assert(request == BL_DRM_VERSION);
    snprintf(v->name, v->name_len, "%s", driver_name);
    v->name_len = strlen(driver_name);
    va_end(ap);
    return 0;
  }
  assert(number == SYS_kcmp);
  if (!kcmp_works) { va_end(ap); errno = EPERM; return -1; }
  (void)va_arg(ap, int); (void)va_arg(ap, int); (void)va_arg(ap, int);
  int a = va_arg(ap, int), b = va_arg(ap, int);
  va_end(ap);
  if (a < 0 || b < 0 || a >= TEST_FD_LIMIT || b >= TEST_FD_LIMIT ||
      !identities[a] || !identities[b]) { errno = EBADF; return -1; }
  return identities[a] == identities[b] ? 0 : identities[a] < identities[b] ? 1 : 2;
}

static int native_import(int fd, int dma, uint32_t *handle) {
  (void)fd;
  native_imports++;
  if (fcntl(dma, F_GETFD) < 0) return -1;
  if (!handle) { errno = EFAULT; return -1; }
  if (kernel_error) { errno = kernel_error; return -1; }
  *handle = 42;
  return 0;
}
static int native_export(int fd, uint32_t handle, uint32_t flags, int *out) {
  (void)flags;
  native_exports++;
  if (handle != 42 || !out) { errno = EINVAL; return -1; }
  *out = fixture_fcntl(fd, F_DUPFD_CLOEXEC, 0);
  return *out < 0 ? -1 : 0;
}
static int native_close(int fd, uint32_t handle) {
  (void)fd;
  native_closes++;
  if (handle == 42) return 0;
  errno = EINVAL;
  return -1;
}
static void *fixture_dlsym(void *object, const char *name) {
  assert(object == RTLD_NEXT);
  if (!strcmp(name, "drmPrimeFDToHandle")) return native_import;
  if (!strcmp(name, "drmPrimeHandleToFD")) return native_export;
  if (!strcmp(name, "drmCloseBufferHandle")) return native_close;
  abort();
}

static int count_fds(void) {
  int count = 0;
  for (int fd = 0; fd < TEST_FD_LIMIT; ++fd) count += identities[fd] != 0;
  return count;
}
static void empty_table(void) {
  for (int i = 0; i < MAX_HANDLES; ++i) assert(!handle_entries[i].handle);
}

int main(void) {
  /* Descriptor 0 stays taken, so no test descriptor lands there by accident. */
  if (fcntl(0, F_GETFD) < 0) assert(open("/dev/null", O_RDONLY) == 0);
  int drm = test_open("/dev/null"), dma = test_open("/dev/zero");
  uint32_t handle = 0;
  unsetenv("BL_POWERVR_DRM_HANDLES");
  assert(drmPrimeFDToHandle(drm, dma, &handle) != 0);
  setenv("BL_POWERVR_DRM_HANDLES", "yes", 1);
  assert(drmPrimeFDToHandle(drm, dma, &handle) != 0);
  setenv("BL_POWERVR_DRM_HANDLES", "1", 1);
  driver_name = "other";
  assert(drmPrimeFDToHandle(drm, dma, &handle) != 0);
  driver_name = "pvr";
  assert(drmPrimeFDToHandle(dma, dma, &handle) != 0); /* wrong character device */
  kernel_error = EACCES;
  assert(drmPrimeFDToHandle(drm, dma, &handle) != 0 && errno == EACCES);
  kernel_error = EINVAL;
  assert(drmPrimeFDToHandle(drm, -1, &handle) != 0 && errno == EBADF);
  empty_table();

  /* Handle 0 is never a token: an empty slot must not answer for descriptor 0 and close it. */
  struct gem_close zero = {0};
  int zero_rc = 123;
  assert(bl_drm_gem_close(0, GEM_CLOSE, &zero, &zero_rc) == 0 && zero_rc == 123);
  assert(drmCloseBufferHandle(0, 0) != 0 && fcntl(0, F_GETFD) >= 0);

  /* A 6.6 kernel in the app sandbox (Pixel 10: no F_DUPFD_QUERY, kcmp refused) cannot compare
   * open files. Handles then match by descriptor number, as KGSL's do there, and the session log
   * says so once per process - and says nothing while the kernel can answer. */
  int baseline = count_fds(), out = -1;
  uint32_t tokens[3] = {0};
  int unproven = fixture_fcntl(drm, F_DUPFD_CLOEXEC, 0);
  fflush(stderr);
  FILE *log = tmpfile();
  int saved_stderr = dup(2);
  assert(log && saved_stderr >= 0 && dup2(fileno(log), 2) == 2);
  int failed = drmPrimeFDToHandle(drm, dma, &tokens[2]);
  off_t quiet = lseek(2, 0, SEEK_CUR);
  identity_denied = 1;
  failed = failed || drmPrimeFDToHandle(drm, dma, &tokens[0]) || drmPrimeFDToHandle(drm, dma, &tokens[1]);
  fflush(stderr);
  assert(dup2(saved_stderr, 2) == 2 && close(saved_stderr) == 0);
  assert(!failed && quiet == 0);
  char said[512] = {0};
  rewind(log);
  assert(fread(said, 1, sizeof(said) - 1, log) > 0 && fclose(log) == 0);
  const char *once = strstr(said, "droiddeck-drm:");
  assert(once && !strstr(once + 1, "droiddeck-drm:"));
  assert(count_fds() == baseline + 7);
  for (int n = 0; n < 3; ++n) {
    assert(tokens[n] & PVR_HANDLE_BIT);
    assert(drmPrimeHandleToFD(unproven, tokens[n], O_CLOEXEC, &out) != 0);
    assert(drmPrimeHandleToFD(drm, tokens[n], O_CLOEXEC, &out) == 0);
    fixture_close(out);
    assert(drmCloseBufferHandle(drm, tokens[n]) == 0);
    assert(drmPrimeHandleToFD(drm, tokens[n], O_CLOEXEC, &out) != 0);
  }
  fixture_close(unproven);
  assert(count_fds() == baseline);
  empty_table();

  /* The same kernel outside the sandbox (adb shell) answers through kcmp: an alias is proven, and
   * neither a closed nor a reused number is. */
  kcmp_works = 1;
  assert(drmPrimeFDToHandle(drm, dma, &handle) == 0);
  int proven = fixture_fcntl(drm, F_DUPFD_CLOEXEC, 0), other = test_open("/dev/null");
  assert(drmPrimeHandleToFD(other, handle, O_CLOEXEC, &out) != 0);
  int number = drm;
  fixture_close(drm);
  assert(drmPrimeHandleToFD(number, handle, O_CLOEXEC, &out) != 0);
  drm = test_open("/dev/null");
  assert(drm == number && drmPrimeHandleToFD(drm, handle, O_CLOEXEC, &out) != 0);
  assert(drmPrimeHandleToFD(proven, handle, O_CLOEXEC, &out) == 0);
  fixture_close(out);
  assert(drmCloseBufferHandle(proven, handle) == 0);
  fixture_close(proven);
  fixture_close(other);
  identity_denied = kcmp_works = 0;
  assert(count_fds() == baseline);
  empty_table();

  assert(drmPrimeFDToHandle(drm, dma, &handle) == 0 && (handle & PVR_HANDLE_BIT));
  assert(count_fds() == baseline + 2);
  int alias = fixture_fcntl(drm, F_DUPFD_CLOEXEC, 0);
  int separate = test_open("/dev/null");
  assert(drmPrimeHandleToFD(separate, handle, O_CLOEXEC | O_RDWR, &out) != 0);
  struct gem_close close_request = {.handle = handle};
  int rc = 123;
  assert(bl_drm_gem_close(separate, GEM_CLOSE, &close_request, &rc) == 0 && rc == 123);
  assert(drmPrimeHandleToFD(alias, handle, O_CLOEXEC | O_RDWR, &out) == 0);
  assert(fcntl(out, F_GETFD) & FD_CLOEXEC);
  struct stat src, dst;
  assert(fstat(dma, &src) == 0 && fstat(out, &dst) == 0);
  assert(src.st_dev == dst.st_dev && src.st_ino == dst.st_ino);
  fixture_close(out);
  assert(drmPrimeHandleToFD(alias, handle, 0, &out) == 0);
  assert(!(fcntl(out, F_GETFD) & FD_CLOEXEC));
  fixture_close(out);
  assert(drmPrimeHandleToFD(alias, handle, 0x40, &out) == -EINVAL);

  /* A closed-and-reused descriptor must not inherit the old open file's handles. */
  int old_number = drm;
  fixture_close(drm);
  assert(drmPrimeHandleToFD(old_number, handle, O_CLOEXEC, &out) != 0); /* closed is not unknown */
  drm = test_open("/dev/null");
  assert(drm == old_number);
  assert(drmPrimeHandleToFD(drm, handle, O_CLOEXEC, &out) != 0);
  assert(drmCloseBufferHandle(drm, handle) != 0);
  assert(drmPrimeHandleToFD(alias, handle, O_CLOEXEC, &out) == 0);
  fixture_close(out);
  assert(bl_drm_gem_close(alias, GEM_CLOSE, &close_request, &rc) == 1 && rc == 0);
  assert(drmPrimeHandleToFD(alias, handle, O_CLOEXEC, &out) != 0);
  fixture_close(alias);
  fixture_close(separate);
  assert(count_fds() == baseline);
  empty_table();

  /* Real kernel handles and their direct GEM_CLOSE ioctls are never swallowed. */
  kernel_error = 0;
  assert(drmPrimeFDToHandle(drm, dma, &handle) == 0 && handle == 42);
  empty_table();
  assert(drmPrimeHandleToFD(drm, handle, O_CLOEXEC, &out) == 0);
  fixture_close(out);
  close_request.handle = handle;
  assert(bl_drm_gem_close(drm, GEM_CLOSE, &close_request, &rc) == 0);
  assert(drmCloseBufferHandle(drm, handle) == 0);
  assert(native_imports && native_exports && native_closes);
  kernel_error = EINVAL;

  /* Slot reuse must not reuse public tokens or leak either owned descriptor. */
  uint32_t previous = 0;
  for (int n = 0; n < 1000; ++n) {
    assert(drmPrimeFDToHandle(drm, dma, &handle) == 0 && handle != previous);
    assert(drmCloseBufferHandle(drm, handle) == 0);
    if (previous) assert(drmPrimeHandleToFD(drm, previous, O_CLOEXEC, &out) != 0);
    previous = handle;
  }
  assert(count_fds() == baseline);
  empty_table();

  assert(drmPrimeFDToHandle(drm, dma, &handle) == 0);
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    assert(drmPrimeHandleToFD(drm, handle, O_CLOEXEC, &out) == 0);
    fixture_close(out);
    assert(drmCloseBufferHandle(drm, handle) == 0);
    _exit(0);
  }
  int status;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(drmPrimeHandleToFD(drm, handle, O_CLOEXEC, &out) == 0);
  fixture_close(out);
  assert(drmCloseBufferHandle(drm, handle) == 0);

  /* Retain the old KGSL path with the PowerVR opt-in unset. */
  unsetenv("BL_POWERVR_DRM_HANDLES");
  kgsl_present = 1;
  kgsl_state = 0;
  assert(drmPrimeFDToHandle(dma, drm, &handle) == 0 && !(handle & PVR_HANDLE_BIT));
  assert(drmPrimeHandleToFD(dma, handle, O_CLOEXEC | O_RDWR, &out) == 0);
  fixture_close(out);
  /* Another KGSL number reaches the handle only when kcmp proves it is the same open file. */
  int kgsl_alias = fixture_fcntl(dma, F_DUPFD_CLOEXEC, 0);
  assert(drmPrimeHandleToFD(kgsl_alias, handle, O_CLOEXEC, &out) != 0);
  kcmp_works = 1;
  assert(drmPrimeHandleToFD(kgsl_alias, handle, O_CLOEXEC, &out) == 0);
  fixture_close(out);
  kcmp_works = 0;
  fixture_close(kgsl_alias);
  close_request.handle = handle;
  assert(bl_drm_gem_close(dma, GEM_CLOSE, &close_request, &rc) == 1 && rc == 0);
  assert(drmCloseBufferHandle(dma, handle) == -EINVAL);
  empty_table();
  assert(count_fds() == baseline);
  fixture_close(drm);
  fixture_close(dma);
  puts("DRM adapter scoping, ownership, reuse, fork and kernel-passthrough tests passed");
}
