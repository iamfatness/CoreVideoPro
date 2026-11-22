#pragma once

// POSIX child lifetime (mac streaming Phase 2, P1): what a Windows job object
// gives the core's helper children, on macOS/Linux.
//
// 1. No orphans. A child is started through corevideo-child-guard
//    (native/child-guard/main.cpp) holding the read end of ONE process-wide
//    "lifeline" pipe whose write end only this process holds (close-on-exec,
//    never closed). When this process dies for any reason, SIGKILL included,
//    the kernel closes the write end, the guard reads EOF and ends its child.
// 2. No inherited descriptors. On Apple every spawn uses
//    POSIX_SPAWN_CLOEXEC_DEFAULT: a child gets stdin/stdout/stderr and the
//    descriptors the request names, nothing else. Before this, every ffmpeg
//    inherited every earlier sibling's pipe write ends, so closing the core's
//    end of one child's stdin never delivered EOF while a sibling lived.
//    Linux has no such flag: with glibc >= 2.34 spawn instead stages each
//    mapped descriptor above every target (F_DUPFD_CLOEXEC), dup2s it into
//    place, then closes every other descriptor in the child (addclose for the
//    gaps below the highest target, addclosefrom_np above it).
// 3. Bounded stop by process group. Each child leads its own group
//    (pgid == pid), so signalGroup/stopGroup reach the guard AND its target;
//    SIGKILLing only the guard would orphan the target.
// 4. Exit status stays wire vocabulary: the guard mirrors its target's exit
//    code or terminating signal.
//
// Header-only. spawn allocates and stopGroup sleeps: call them from writer or
// supervisor threads only, never the render tick, and never while holding a
// mutex a writer thread needs (stopGroup can wait `grace` plus a blocking reap).

#if !defined(_WIN32)

#include "modules/PosixExecutableLocator.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <spawn.h>
#include <string>
#include <thread>
#include <vector>

#include <algorithm>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace corevideo::modules::posix_child {

// Where the guard finds the lifeline (passed on its command line too).
inline constexpr int kLifelineChildFd = 9;

struct FdMapping {
  int parentFd;
  int childFd;
};

struct SpawnRequest {
  std::string executable;              // absolute path of the target
  std::vector<std::string> arguments;  // argv[1..]
  std::vector<FdMapping> fds;          // every descriptor besides 1/2; fd 0 is /dev/null unless mapped
  std::string stdoutPath = "/dev/null";
  std::string stderrPath = "/dev/null";
  // "" = resolveChildGuard(); still "" = run unguarded (reported). An explicit
  // path that is not executable is an error, never a silent unguarded child.
  std::string guardExecutable;
};

struct SpawnResult {
  pid_t pid = 0;         // the guard's pid when guarded (also the process group id)
  bool guarded = false;
  int error = 0;         // errno-style, when pid == 0
};

inline constexpr const char* kChildGuardName = "corevideo-child-guard";

// $COREVIDEO_CHILD_GUARD_PATH when set (an unusable value resolves to ""),
// else corevideo-child-guard beside this executable (the app bundle's
// Contents/Resources, or the build directory).
inline std::string resolveChildGuard() {
  if (const char* env = std::getenv("COREVIDEO_CHILD_GUARD_PATH"); env && *env) {
    return posix_exec::isExecutableFile(env) ? std::string(env) : std::string();
  }
  const auto directory = posix_exec::currentExecutableDirectory();
  if (directory.empty()) return {};
  const auto candidate = directory / kChildGuardName;
  return posix_exec::isExecutableFile(candidate) ? candidate.string() : std::string();
}

inline int lifelineReadFd() {
  static int readFd = -1;
  static std::once_flag once;
  std::call_once(once, [] {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) return;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    // fds[1] is deliberately never closed: its lifetime IS this process's.
    readFd = fds[0];
  });
  return readFd;
}

inline SpawnResult spawn(const SpawnRequest& request) {
  SpawnResult result;
  std::string guard = request.guardExecutable;
  if (!guard.empty()) {
    if (!posix_exec::isExecutableFile(guard)) {
      result.error = ENOENT;
      return result;
    }
  } else {
    guard = resolveChildGuard();
  }
  const bool guarded = !guard.empty();
  const int lifeline = guarded ? lifelineReadFd() : -1;
  if (guarded && lifeline < 0) {
    result.error = EMFILE;
    return result;
  }
  for (const auto& mapping : request.fds) {
    if (mapping.childFd == 1 || mapping.childFd == 2 || (guarded && mapping.childFd == kLifelineChildFd)) {
      result.error = EINVAL;
      return result;
    }
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  posix_spawnattr_setflags(&attributes, flags);
  posix_spawnattr_setpgroup(&attributes, 0);
  sigset_t mask;
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attributes, &mask);
  // The core ignores SIGPIPE; a child starts with ordinary dispositions.
  sigset_t defaults;
  sigemptyset(&defaults);
  for (int signal : {SIGPIPE, SIGTERM, SIGINT, SIGHUP}) sigaddset(&defaults, signal);
  posix_spawnattr_setsigdefault(&attributes, &defaults);

  std::vector<FdMapping> mappings = request.fds;
  if (guarded) mappings.push_back({lifeline, kLifelineChildFd});
  bool stdinMapped = false;
  for (const auto& mapping : mappings) stdinMapped = stdinMapped || mapping.childFd == 0;

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
#define COREVIDEO_SPAWN_CLOSEFROM 1
  // No CLOEXEC_DEFAULT here. Stage every source above every target so no
  // dup2 can clobber a source still to be mapped, then close the rest.
  int highestTarget = 2;
  for (const auto& mapping : mappings) highestTarget = std::max(highestTarget, mapping.childFd);
  std::vector<int> staged;
  staged.reserve(mappings.size());
  const auto releaseStaged = [&] {
    for (int fd : staged) ::close(fd);
  };
  for (auto& mapping : mappings) {
    const int copy = ::fcntl(mapping.parentFd, F_DUPFD_CLOEXEC, highestTarget + 1);
    if (copy < 0) {
      result.error = errno;
      releaseStaged();
      posix_spawn_file_actions_destroy(&actions);
      posix_spawnattr_destroy(&attributes);
      return result;
    }
    staged.push_back(copy);
    mapping.parentFd = copy;
  }
#endif
  for (const auto& mapping : mappings) {
#if defined(__APPLE__)
    if (mapping.parentFd == mapping.childFd) {
      posix_spawn_file_actions_addinherit_np(&actions, mapping.childFd);
      continue;
    }
#endif
    posix_spawn_file_actions_adddup2(&actions, mapping.parentFd, mapping.childFd);
  }
  if (!stdinMapped) posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, request.stdoutPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_addopen(&actions, 2, request.stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
#if defined(COREVIDEO_SPAWN_CLOSEFROM)
  // glibc ignores a close action on an in-range descriptor that is not open.
  for (int fd = 3; fd <= highestTarget; ++fd) {
    const bool isTarget = std::any_of(mappings.begin(), mappings.end(),
                                      [fd](const FdMapping& mapping) { return mapping.childFd == fd; });
    if (!isTarget) posix_spawn_file_actions_addclose(&actions, fd);
  }
  posix_spawn_file_actions_addclosefrom_np(&actions, highestTarget + 1);
#endif

  const std::string lifelineArg = std::to_string(kLifelineChildFd);
  std::vector<char*> argv;
  argv.reserve(request.arguments.size() + 6);
  if (guarded) {
    argv.push_back(const_cast<char*>(guard.c_str()));
    argv.push_back(const_cast<char*>("--lifeline-fd"));
    argv.push_back(const_cast<char*>(lifelineArg.c_str()));
    argv.push_back(const_cast<char*>("--"));
  }
  argv.push_back(const_cast<char*>(request.executable.c_str()));
  for (const auto& argument : request.arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);

  pid_t pid = 0;
  const char* path = guarded ? guard.c_str() : request.executable.c_str();
  const int rc = ::posix_spawn(&pid, path, &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
#if defined(COREVIDEO_SPAWN_CLOSEFROM)
  releaseStaged();
#undef COREVIDEO_SPAWN_CLOSEFROM
#endif
  if (rc != 0) {
    result.error = rc;
    return result;
  }
  result.pid = pid;
  result.guarded = guarded;
  return result;
}

// The child's whole process group (guard + target); falls back to the pid
// alone if the group is already gone.
inline void signalGroup(pid_t pid, int signal) {
  if (pid <= 0) return;
  if (::kill(-pid, signal) != 0 && errno == ESRCH) ::kill(pid, signal);
}

// SIGTERM the group, wait up to `grace` for the child to exit, then SIGKILL
// the group and reap. Returns true once the child is reaped (or was not ours).
inline bool stopGroup(pid_t pid, std::chrono::milliseconds grace, int* status) {
  if (pid <= 0) return true;
  int local = 0;
  int* out = status ? status : &local;
  const auto reaped = [&] {
    const pid_t r = ::waitpid(pid, out, WNOHANG);
    return r == pid || (r < 0 && errno == ECHILD);
  };
  if (reaped()) return true;
  signalGroup(pid, SIGTERM);
  const auto deadline = std::chrono::steady_clock::now() + grace;
  while (std::chrono::steady_clock::now() < deadline) {
    if (reaped()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  signalGroup(pid, SIGKILL);
  while (::waitpid(pid, out, 0) < 0 && errno == EINTR) {}
  return true;
}

}  // namespace corevideo::modules::posix_child

#endif  // !_WIN32
