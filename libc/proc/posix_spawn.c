/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│ vi: set et ft=c ts=2 sts=2 sw=2 fenc=utf-8                               :vi │
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2021 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include "libc/proc/posix_spawn.h"
#include "libc/assert.h"
#include "libc/atomic.h"
#include "libc/calls/calls.h"
#include "libc/calls/internal.h"
#include "libc/calls/state.internal.h"
#include "libc/calls/struct/rlimit.h"
#include "libc/calls/struct/rlimit.internal.h"
#include "libc/calls/struct/rusage.internal.h"
#include "libc/calls/struct/sigaction.h"
#include "libc/calls/struct/sigset.h"
#include "libc/calls/struct/sigset.internal.h"
#include "libc/calls/syscall-sysv.internal.h"
#include "libc/calls/syscall_support-nt.internal.h"
#include "libc/dce.h"
#include "libc/errno.h"
#include "libc/fmt/itoa.h"
#include "libc/fmt/magnumstrs.internal.h"
#include "libc/intrin/atomic.h"
#include "libc/intrin/bsf.h"
#include "libc/intrin/describeflags.h"
#include "libc/intrin/dll.h"
#include "libc/intrin/fds.h"
#include "libc/intrin/strace.h"
#include "libc/intrin/weaken.h"
#include "libc/mem/alloca.h"
#include "libc/mem/mem.h"
#include "libc/nt/createfile.h"
#include "libc/nt/enum/accessmask.h"
#include "libc/nt/enum/creationdisposition.h"
#include "libc/nt/enum/fileflagandattributes.h"
#include "libc/nt/enum/filesharemode.h"
#include "libc/nt/enum/processcreationflags.h"
#include "libc/nt/enum/startf.h"
#include "libc/nt/files.h"
#include "libc/nt/process.h"
#include "libc/nt/runtime.h"
#include "libc/nt/struct/processinformation.h"
#include "libc/nt/struct/startupinfo.h"
#include "libc/proc/describefds.internal.h"
#include "libc/proc/ntspawn.h"
#include "libc/proc/posix_spawn.h"
#include "libc/proc/posix_spawn.internal.h"
#include "libc/proc/proc.h"
#include "libc/runtime/internal.h"
#include "libc/runtime/runtime.h"
#include "libc/sock/sock.h"
#include "libc/stdio/stdio.h"
#include "libc/stdio/sysparam.h"
#include "libc/str/str.h"
#include "libc/sysv/consts/at.h"
#include "libc/sysv/consts/f.h"
#include "libc/sysv/consts/fd.h"
#include "libc/sysv/consts/limits.h"
#include "libc/sysv/consts/o.h"
#include "libc/sysv/consts/ok.h"
#include "libc/sysv/consts/sig.h"
#include "libc/sysv/errfuns.h"
#include "libc/thread/thread.h"
#include "libc/thread/tls.h"

#ifndef SYSDEBUG
#define read        sys_read
#define write       sys_write
#define close       sys_close
#define pipe2       sys_pipe2
#define getgid      sys_getgid
#define setgid      sys_setgid
#define getuid      sys_getuid
#define setuid      sys_setuid
#define setsid      sys_setsid
#define setpgid     sys_setpgid
#define fcntl       __sys_fcntl
#define wait4       __sys_wait4
#define openat      __sys_openat
#define setrlimit   sys_setrlimit
#define sigprocmask sys_sigprocmask
#endif

#define CLOSER_CONTAINER(e) DLL_CONTAINER(struct Closer, elem, e)

static atomic_bool has_vfork;  // i.e. not qemu/wsl/xnu/openbsd

#ifdef __x86_64__

struct Closer {
  int64_t handle;
  struct Dll elem;
};

struct SpawnFds {
  int n;
  struct Fd *p;
  struct Dll *closers;
};

static textwindows int64_t spawnfds_handle(struct SpawnFds *fds, int fd) {
  if (__is_cloexec(fds->p + fd))
    return -1;
  return fds->p[fd].handle;
}

static textwindows errno_t spawnfds_ensure(struct SpawnFds *fds, int fd) {
  int n2;
  struct Fd *p2;
  if (fd < 0)
    return EBADF;
  if (fd < fds->n)
    return 0;
  n2 = fd + 1;
  if (!(p2 = realloc(fds->p, n2 * sizeof(*fds->p))))
    return ENOMEM;
  bzero(p2 + fds->n, (n2 - fds->n) * sizeof(*fds->p));
  fds->p = p2;
  fds->n = n2;
  return 0;
}

static textwindows void spawnfds_destroy(struct SpawnFds *fds) {
  struct Dll *e;
  while ((e = dll_first(fds->closers))) {
    struct Closer *closer = CLOSER_CONTAINER(e);
    dll_remove(&fds->closers, e);
    CloseHandle(closer->handle);
    free(closer);
  }
  free(fds->p);
}

static textwindows int spawnfds_closelater(struct SpawnFds *fds,
                                           int64_t handle) {
  struct Closer *closer;
  if (!(closer = malloc(sizeof(struct Closer))))
    return ENOMEM;
  closer->handle = handle;
  dll_init(&closer->elem);
  dll_make_last(&fds->closers, &closer->elem);
  return 0;
}

static textwindows bool spawnfds_exists(struct SpawnFds *fds, int fildes) {
  return fildes + 0u < fds->n && fds->p[fildes].kind;
}

static textwindows errno_t spawnfds_close(struct SpawnFds *fds, int fildes) {
  if (spawnfds_exists(fds, fildes)) {
    fds->p[fildes] = (struct Fd){0};
  }
  return 0;
}

static textwindows errno_t spawnfds_dup2(struct SpawnFds *fds, int fildes,
                                         int newfildes) {
  errno_t err;
  struct Fd *old;
  if (spawnfds_exists(fds, fildes)) {
    old = fds->p + fildes;
  } else if (__isfdopen(fildes)) {
    old = g_fds.p + fildes;
  } else {
    return EBADF;
  }
  if ((err = spawnfds_ensure(fds, newfildes)))
    return err;
  struct Fd *neu = fds->p + newfildes;
  memcpy(neu, old, sizeof(struct Fd));
  neu->flags &= ~O_CLOEXEC;
  if (!DuplicateHandle(GetCurrentProcess(), neu->handle, GetCurrentProcess(),
                       &neu->handle, 0, true, kNtDuplicateSameAccess)) {
    return EMFILE;
  }
  spawnfds_closelater(fds, neu->handle);
  return 0;
}

static textwindows errno_t spawnfds_open(struct SpawnFds *fds, int64_t dirhand,
                                         const char *path, int oflag, int mode,
                                         int fildes) {
  int64_t h;
  errno_t err;
  char16_t path16[PATH_MAX];
  uint32_t perm, share, disp, attr;
  if (!strcmp(path, "/dev/null")) {
    strcpy16(path16, u"NUL");
  } else if (!strcmp(path, "/dev/stdin")) {
    return spawnfds_dup2(fds, 0, fildes);
  } else if (!strcmp(path, "/dev/stdout")) {
    return spawnfds_dup2(fds, 1, fildes);
  } else if (!strcmp(path, "/dev/stderr")) {
    return spawnfds_dup2(fds, 2, fildes);
  } else {
    if (__mkntpathath(dirhand, path, 0, path16) == -1)
      return errno;
  }
  if ((err = spawnfds_ensure(fds, fildes)))
    return err;
  if (GetNtOpenFlags(oflag, mode, &perm, &share, &disp, &attr) != -1 &&
      (h = CreateFile(path16, perm, share, &kNtIsInheritable, disp, attr, 0))) {
    spawnfds_closelater(fds, h);
    fds->p[fildes].kind = kFdFile;
    fds->p[fildes].flags = oflag;
    fds->p[fildes].mode = mode;
    fds->p[fildes].handle = h;
    return 0;
  } else {
    return errno;
  }
}

static textwindows errno_t spawnfds_chdir(struct SpawnFds *fds, int64_t dirhand,
                                          const char *path,
                                          int64_t *out_dirhand) {
  int64_t h;
  char16_t path16[PATH_MAX];
  if (__mkntpathath(dirhand, path, 0, path16) != -1 &&
      (h = CreateFile(path16, kNtFileGenericRead,
                      kNtFileShareRead | kNtFileShareWrite | kNtFileShareDelete,
                      0, kNtOpenExisting,
                      kNtFileAttributeNormal | kNtFileFlagBackupSemantics,
                      0))) {
    spawnfds_closelater(fds, h);
    *out_dirhand = h;
    return 0;
  } else {
    return errno;
  }
}

static textwindows errno_t spawnfds_fchdir(struct SpawnFds *fds, int fildes,
                                           int64_t *out_dirhand) {
  int64_t h;
  if (spawnfds_exists(fds, fildes)) {
    h = fds->p[fildes].handle;
  } else if (__isfdopen(fildes)) {
    h = g_fds.p[fildes].handle;
  } else {
    return EBADF;
  }
  *out_dirhand = h;
  return 0;
}

static textwindows errno_t posix_spawn_nt_impl(
    int *pid, const char *path, const posix_spawn_file_actions_t *file_actions,
    const posix_spawnattr_t *attrp, char *const argv[], char *const envp[]) {

  // signals, locks, and resources
  char *fdspec = 0;
  errno_t e = errno;
  struct Proc *proc = 0;
  struct SpawnFds fds = {0};
  int64_t dirhand = AT_FDCWD;
  int64_t *lpExplicitHandles = 0;
  sigset_t sigmask = __sig_block();
  uint32_t dwExplicitHandleCount = 0;
  int64_t hCreatorProcess = GetCurrentProcess();

  // reserve process tracking object
  __proc_lock();
  proc = __proc_new();
  __proc_unlock();

  // setup return path
  errno_t err;
  if (!proc) {
    err = ENOMEM;
  ReturnErr:
    __undescribe_fds(hCreatorProcess, lpExplicitHandles, dwExplicitHandleCount);
    free(fdspec);
    if (proc) {
      __proc_lock();
      dll_make_first(&__proc.free, &proc->elem);
      __proc_unlock();
    }
    spawnfds_destroy(&fds);
    __sig_unblock(sigmask);
    errno = e;
    return err;
  }

  // fork file descriptor table
  for (int fd = g_fds.n; fd--;) {
    if (__is_cloexec(g_fds.p + fd))
      continue;
    if ((err = spawnfds_ensure(&fds, fd)))
      goto ReturnErr;
    fds.p[fd] = g_fds.p[fd];
  }

  // apply user file actions
  if (file_actions) {
    for (struct _posix_faction *a = *file_actions; a && !err; a = a->next) {
      char errno_buf[30];
      char oflags_buf[128];
      char openmode_buf[15];
      switch (a->action) {
        case _POSIX_SPAWN_CLOSE:
          err = spawnfds_close(&fds, a->fildes);
          STRACE("spawnfds_close(%d) → %s", a->fildes,
                 _DescribeErrno(errno_buf, err));
          break;
        case _POSIX_SPAWN_DUP2:
          err = spawnfds_dup2(&fds, a->fildes, a->newfildes);
          STRACE("spawnfds_dup2(%d, %d) → %s", a->fildes, a->newfildes,
                 _DescribeErrno(errno_buf, err));
          break;
        case _POSIX_SPAWN_OPEN:
          err = spawnfds_open(&fds, dirhand, a->path, a->oflag, a->mode,
                              a->fildes);
          STRACE("spawnfds_open(%#s, %s, %s, %d) → %s", a->path,
                 _DescribeOpenFlags(oflags_buf, a->oflag),
                 _DescribeOpenMode(openmode_buf, a->oflag, a->mode), a->fildes,
                 _DescribeErrno(errno_buf, err));
          break;
        case _POSIX_SPAWN_CHDIR:
          err = spawnfds_chdir(&fds, dirhand, a->path, &dirhand);
          STRACE("spawnfds_chdir(%#s) → %s", a->path,
                 _DescribeErrno(errno_buf, err));
          break;
        case _POSIX_SPAWN_FCHDIR:
          err = spawnfds_fchdir(&fds, a->fildes, &dirhand);
          STRACE("spawnfds_fchdir(%d) → %s", a->fildes,
                 _DescribeErrno(errno_buf, err));
          break;
        default:
          __builtin_unreachable();
      }
      if (err) {
        goto ReturnErr;
      }
    }
  }

  // figure out flags
  uint32_t dwCreationFlags = 0;
  short flags = attrp && *attrp ? (*attrp)->flags : 0;
  if (flags & (POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSID))
    dwCreationFlags |= kNtCreateNewProcessGroup;

  // create process startinfo
  struct NtStartupInfo startinfo = {
      .cb = sizeof(struct NtStartupInfo),
      .dwFlags = kNtStartfUsestdhandles,
      .hStdInput = spawnfds_handle(&fds, 0),
      .hStdOutput = spawnfds_handle(&fds, 1),
      .hStdError = spawnfds_handle(&fds, 2),
  };

  // determine spawn directory
  char16_t *lpCurrentDirectory = 0;
  if (dirhand != AT_FDCWD) {
    lpCurrentDirectory = alloca(PATH_MAX * sizeof(char16_t));
    if (!GetFinalPathNameByHandle(dirhand, lpCurrentDirectory, PATH_MAX,
                                  kNtFileNameNormalized | kNtVolumeNameDos)) {
      err = GetLastError();
      goto ReturnErr;
    }
  }

  // UNC paths break some things when they are not needed.
  if (lpCurrentDirectory) {
    size_t n = strlen16(lpCurrentDirectory);
    if (n > 4 && n < 260 &&               //
        lpCurrentDirectory[0] == '\\' &&  //
        lpCurrentDirectory[1] == '\\' &&  //
        lpCurrentDirectory[2] == '?' &&   //
        lpCurrentDirectory[3] == '\\') {
      memmove(lpCurrentDirectory, lpCurrentDirectory + 4,
              (n - 4 + 1) * sizeof(char16_t));
    }
  }

  // inherit signal mask
  sigset_t childmask;
  char maskvar[6 + 21];
  if (flags & POSIX_SPAWN_SETSIGMASK) {
    childmask = (*attrp)->sigmask;
  } else {
    childmask = sigmask;
  }
  FormatUint64(stpcpy(maskvar, "_MASK="), childmask);

  // inherit parent process id
  char ppidvar[12 + 21 + 1 + 21 + 1], *p = ppidvar;
  p = stpcpy(p, "_COSMO_PPID=");
  p = FormatUint64(p, GetCurrentProcessId());
  *p++ = ':';
  p = FormatUint64(p, __pid);
  setenv("_COSMO_PPID", ppidvar, true);

  // launch process
  int rc = -1;
  struct NtProcessInformation procinfo;
  if (!envp)
    envp = environ;
  if ((fdspec = __describe_fds(fds.p, fds.n, &startinfo, hCreatorProcess,
                               &lpExplicitHandles, &dwExplicitHandleCount))) {
    rc = ntspawn(&(struct NtSpawnArgs){
        dirhand, path, argv, envp, (char *[]){fdspec, maskvar, 0},
        dwCreationFlags, lpCurrentDirectory, 0, lpExplicitHandles,
        dwExplicitHandleCount, &startinfo, &procinfo});
  }
  if (rc == -1) {
    err = errno;
    goto ReturnErr;
  }

  // return result
  CloseHandle(procinfo.hThread);
  proc->pid = procinfo.dwProcessId;
  proc->handle = procinfo.hProcess;
  if (pid)
    *pid = proc->pid;
  __proc_lock();
  __proc_add(proc);
  __proc_unlock();
  proc = 0;
  err = 0;
  goto ReturnErr;
}

static const char *DescribePid(char buf[12], int err, int *pid) {
  if (err)
    return "n/a";
  if (!pid)
    return "NULL";
  FormatInt32(buf, *pid);
  return buf;
}

static textwindows dontinline errno_t posix_spawn_nt(
    int *pid, const char *path, const posix_spawn_file_actions_t *file_actions,
    const posix_spawnattr_t *attrp, char *const argv[], char *const envp[]) {
  int err;
  if (!path || !argv) {
    err = EFAULT;
  } else {
    err = posix_spawn_nt_impl(pid, path, file_actions, attrp, argv, envp);
  }
  STRACE("posix_spawn([%s], %#s, %s, %s) → %s",
         DescribePid(alloca(12), err, pid), path, DescribeStringList(argv),
         DescribeStringList(envp), !err ? "0" : _strerrno(err));
  return err;
}

#endif  // __x86_64__

/**
 * Spawns process, the POSIX way, e.g.
 *
 *     int pid, status;
 *     posix_spawnattr_t sa;
 *     posix_spawnattr_init(&sa);
 *     posix_spawnattr_setflags(&sa, POSIX_SPAWN_SETPGROUP);
 *     posix_spawn_file_actions_t fa;
 *     posix_spawn_file_actions_init(&fa);
 *     posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDWR, 0644);
 *     posix_spawn_file_actions_adddup2(&fa, 0, 1);
 *     posix_spawnp(&pid, "lol", &fa, &sa, (char *[]){"lol", 0}, 0);
 *     posix_spawnp(&pid, "cat", &fa, &sa, (char *[]){"cat", 0}, 0);
 *     posix_spawn_file_actions_destroy(&fa);
 *     posix_spawnattr_destroy(&sa);
 *     while (wait(&status) != -1);
 *
 * The posix_spawn() function may be used to launch subprocesses. The
 * primary advantage of using posix_spawn() instead of the traditional
 * fork() / execve() combination for launching processes is efficiency
 * and cross-platform compatibility.
 *
 * 1. On Linux, FreeBSD, and NetBSD:
 *
 *    Cosmopolitan Libc's posix_spawn() uses vfork() under the hood on
 *    these platforms automatically, since it's faster than fork(). It's
 *    because vfork() creates a child process without needing to copy
 *    the parent's page tables, making it more efficient, especially for
 *    large processes. Furthermore, vfork() avoids the need to acquire
 *    every single mutex (see pthread_atfork() for more details) which
 *    makes it scalable in multi-threaded apps, since the other threads
 *    in your app can keep going while the spawning thread waits for the
 *    subprocess to call execve(). Normally vfork() is error-prone since
 *    there exists few functions that are @vforksafe. the posix_spawn()
 *    API is designed to offer maximum assurance that you can't shoot
 *    yourself in the foot. If you do, then file a bug with Cosmo.
 *
 * 2. On Windows:
 *
 *    posix_spawn() avoids fork() entirely. Windows doesn't natively
 *    support fork(), and emulating it can be slow and memory-intensive.
 *    By using posix_spawn(), we get a much faster process creation on
 *    Windows systems, because it only needs to call CreateProcess().
 *    Your file actions are replayed beforehand in a simulated way. Only
 *    Cosmopolitan Libc offers this level of quality. With Cygwin you'd
 *    have to use its proprietary APIs to achieve the same performance.
 *
 * 3. Simplified error handling:
 *
 *    posix_spawn() combines process creation and program execution in a
 *    single call, reducing the points of failure and simplifying error
 *    handling. One important thing that happens with Cosmopolitan's
 *    posix_spawn() implementation is that the error code of execve()
 *    inside your subprocess, should it fail, will be propagated to your
 *    parent process. This will happen efficiently via vfork() shared
 *    memory in the event your Linux environment supports this. If it
 *    doesn't, then Cosmopolitan will fall back to a throwaway pipe().
 *    The pipe is needed on platforms like XNU and OpenBSD which do not
 *    support vfork(). It's also needed under QEMU User.
 *
 * 4. Signal safety:
 *
 *    posix_spawn() guarantees your signal handler callback functions
 *    won't be executed in the child process. By default, it'll remove
 *    sigaction() callbacks atomically. This ensures that if something
 *    like a SIGTERM or SIGHUP is sent to the child process before it's
 *    had a chance to call execve(), then the child process will simply
 *    be terminated (like the spawned process would) instead of running
 *    whatever signal handlers the spawning process has installed. If
 *    you've set some signals to SIG_IGN, then that'll be preserved for
 *    the child process by posix_spawn(), unless you explicitly call
 *    posix_spawnattr_setsigdefault() to reset them.
 *
 * 5. Portability:
 *
 *    posix_spawn() is part of the POSIX standard, making it more
 *    portable across different UNIX-like systems and Windows (with
 *    appropriate libraries). Even the non-POSIX APIs we use here are
 *    portable; e.g. posix_spawn_file_actions_addchdir_np() is supported
 *    by glibc, musl libc, and apple libc too.
 *
 * When using posix_spawn() you have the option of passing an attributes
 * object that specifies how the child process should be created. These
 * functions are provided by Cosmopolitan Libc for setting attributes:
 *
 * - posix_spawnattr_init()
 * - posix_spawnattr_destroy()
 * - posix_spawnattr_setflags()
 * - posix_spawnattr_getflags()
 * - posix_spawnattr_setsigmask()
 * - posix_spawnattr_getsigmask()
 * - posix_spawnattr_setpgroup()
 * - posix_spawnattr_getpgroup()
 * - posix_spawnattr_setrlimit_np()
 * - posix_spawnattr_getrlimit_np()
 * - posix_spawnattr_setschedparam()
 * - posix_spawnattr_getschedparam()
 * - posix_spawnattr_setschedpolicy()
 * - posix_spawnattr_getschedpolicy()
 * - posix_spawnattr_setsigdefault()
 * - posix_spawnattr_getsigdefault()
 *
 * You can also pass an ordered list of file actions to perform. The
 * following APIs are provided by Cosmopolitan Libc for doing that:
 *
 * - posix_spawn_file_actions_init()
 * - posix_spawn_file_actions_destroy()
 * - posix_spawn_file_actions_adddup2()
 * - posix_spawn_file_actions_addopen()
 * - posix_spawn_file_actions_addclose()
 * - posix_spawn_file_actions_addchdir_np()
 * - posix_spawn_file_actions_addfchdir_np()
 *
 * @param pid if non-null shall be set to child pid on success
 * @param path is resolved path of program which is not `$PATH` searched
 * @param file_actions specifies close(), dup2(), and open() operations
 * @param attrp specifies signal masks, user ids, scheduling, etc.
 * @param envp is environment variables, or `environ` if null
 * @return 0 on success or error number on failure
 * @raise ETXTBSY if another process has `path` open in write mode
 * @raise ENOEXEC if file is executable but not a valid format
 * @raise ENOMEM if remaining stack memory is insufficient
 * @raise EACCES if execute permission was denied
 * @see posix_spawnp() for `$PATH` searching
 * @returnserrno
 * @tlsrequired
 */
errno_t posix_spawn(int *pid, const char *path,
                    const posix_spawn_file_actions_t *file_actions,
                    const posix_spawnattr_t *attrp, char *const argv[],
                    char *const envp[]) {
#ifdef __x86_64__
  if (IsWindows())
    return posix_spawn_nt(pid, path, file_actions, attrp, argv, envp);
#endif
  int pfds[2];
  bool use_pipe;
  volatile int status = 0;
  sigset_t blockall, oldmask;
  int child, res, cs, e = errno;
  volatile bool can_clobber = false;
  short flags = attrp && *attrp ? (*attrp)->flags : 0;
  sigfillset(&blockall);
  sigprocmask(SIG_SETMASK, &blockall, &oldmask);
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);
  if ((use_pipe = (flags & POSIX_SPAWN_USEFORK) ||
                  !atomic_load_explicit(&has_vfork, memory_order_acquire))) {
    if (pipe2(pfds, O_CLOEXEC)) {
      res = errno;
      goto ParentFailed;
    }
  }
  if (!(child = (flags & POSIX_SPAWN_USEFORK) ? fork() : vfork())) {
    can_clobber = true;
    sigset_t childmask;
    bool lost_cloexec = 0;
    struct sigaction dfl = {0};
    if (use_pipe)
      close(pfds[0]);
    for (int sig = 1; sig <= NSIG; sig++)
      if (__sighandrvas[sig] != (long)SIG_DFL &&
          (__sighandrvas[sig] != (long)SIG_IGN ||
           ((flags & POSIX_SPAWN_SETSIGDEF) &&
            sigismember(&(*attrp)->sigdefault, sig) == 1)))
        sigaction(sig, &dfl, 0);
    if (flags & POSIX_SPAWN_SETSID)
      setsid();
    if ((flags & POSIX_SPAWN_SETPGROUP) && setpgid(0, (*attrp)->pgroup))
      goto ChildFailed;
    if ((flags & POSIX_SPAWN_RESETIDS) && setgid(getgid()))
      goto ChildFailed;
    if ((flags & POSIX_SPAWN_RESETIDS) && setuid(getuid()))
      goto ChildFailed;
    if (file_actions) {
      struct _posix_faction *a;
      for (a = *file_actions; a; a = a->next) {
        if (use_pipe && pfds[1] == a->fildes) {
          int p2;
          if ((p2 = dup(pfds[1])) == -1)
            goto ChildFailed;
          lost_cloexec = true;
          close(pfds[1]);
          pfds[1] = p2;
        }
        switch (a->action) {
          case _POSIX_SPAWN_CLOSE:
            if (close(a->fildes))
              goto ChildFailed;
            break;
          case _POSIX_SPAWN_DUP2:
            if (dup2(a->fildes, a->newfildes) == -1)
              goto ChildFailed;
            break;
          case _POSIX_SPAWN_OPEN: {
            int t;
            if ((t = openat(AT_FDCWD, a->path, a->oflag, a->mode)) == -1)
              goto ChildFailed;
            if (t != a->fildes) {
              if (dup2(t, a->fildes) == -1) {
                close(t);
                goto ChildFailed;
              }
              if (close(t))
                goto ChildFailed;
            }
            break;
          }
          case _POSIX_SPAWN_CHDIR:
            if (chdir(a->path) == -1)
              goto ChildFailed;
            break;
          case _POSIX_SPAWN_FCHDIR:
            if (fchdir(a->fildes) == -1)
              goto ChildFailed;
            break;
          default:
            __builtin_unreachable();
        }
      }
    }
    if (IsLinux() || IsFreebsd() || IsNetbsd()) {
      if (flags & POSIX_SPAWN_SETSCHEDULER)
        if (sched_setscheduler(0, (*attrp)->schedpolicy,
                               &(*attrp)->schedparam) == -1)
          goto ChildFailed;
      if (flags & POSIX_SPAWN_SETSCHEDPARAM)
        if (sched_setparam(0, &(*attrp)->schedparam))
          goto ChildFailed;
    }
    if (flags & POSIX_SPAWN_SETRLIMIT_NP) {
      int rlimset = (*attrp)->rlimset;
      while (rlimset) {
        int resource = bsf(rlimset);
        rlimset &= ~(1u << resource);
        if (setrlimit(resource, (*attrp)->rlim + resource)) {
          // MacOS ARM64 RLIMIT_STACK always returns EINVAL
          if (!IsXnuSilicon()) {
            goto ChildFailed;
          }
        }
      }
    }
    if (lost_cloexec)
      fcntl(pfds[1], F_SETFD, FD_CLOEXEC);
    if (flags & POSIX_SPAWN_SETSIGMASK) {
      childmask = (*attrp)->sigmask;
    } else {
      childmask = oldmask;
    }
    sigprocmask(SIG_SETMASK, &childmask, 0);
    if (!envp)
      envp = environ;
    execve(path, argv, envp);
  ChildFailed:
    res = errno;
    if (!use_pipe) {
      status = res;
    } else {
      write(pfds[1], &res, sizeof(res));
    }
    _Exit(127);
  }
  if (use_pipe)
    close(pfds[1]);
  if (child != -1) {
    if (!use_pipe) {
      res = status;
    } else {
      if (can_clobber)
        atomic_store_explicit(&has_vfork, true, memory_order_release);
      res = 0;
      read(pfds[0], &res, sizeof(res));
    }
    if (!res) {
      if (pid)
        *pid = child;
    } else {
      wait4(child, 0, 0, 0);
    }
  } else {
    res = errno;
  }
  if (use_pipe)
    close(pfds[0]);
ParentFailed:
  sigprocmask(SIG_SETMASK, &oldmask, 0);
  pthread_setcancelstate(cs, 0);
  errno = e;
  return res;
}
