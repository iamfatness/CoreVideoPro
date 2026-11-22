// corevideo-child-guard: keeps a helper child (ffmpeg) from outliving the
// media core on POSIX, the job of the Windows job object.
//
//   corevideo-child-guard --lifeline-fd <N> -- <executable> <args...>
//
// <N> is the read end of a pipe whose write end only the core holds
// (modules/PosixChildProcess.h). The guard forks and execs the target, which
// keeps descriptors 0..N-1 exactly as handed over, then closes its own copies
// of everything but stderr and the lifeline, and waits on both:
//   - lifeline EOF (the core died, even by SIGKILL): SIGTERM the target, wait
//     up to 2 s, SIGKILL it, reap it, exit 125;
//   - SIGTERM/SIGINT/SIGHUP to the guard: forwarded to the target;
//   - target exits N: the guard exits N; target dies by signal S: the guard
//     raises S on itself, so the core's waitpid sees exactly what ffmpeg did.
// Exit 126 = bad usage, 127 = the target could not be executed.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

volatile sig_atomic_t g_forward = 0;

void onSignal(int signal) { g_forward = signal; }

long long nowMs() {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

[[noreturn]] void mirror(int status) {
  if (WIFEXITED(status)) ::_exit(WEXITSTATUS(status));
  if (WIFSIGNALED(status)) {
    const int signal = WTERMSIG(status);
    std::signal(signal, SIG_DFL);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, signal);
    ::sigprocmask(SIG_UNBLOCK, &set, nullptr);
    ::raise(signal);
    ::_exit(128 + signal);
  }
  ::_exit(125);
}

[[noreturn]] void endChild(pid_t child) {
  ::kill(child, SIGTERM);
  int status = 0;
  const long long deadline = nowMs() + 2000;
  while (nowMs() < deadline) {
    if (::waitpid(child, &status, WNOHANG) == child) ::_exit(125);
    ::usleep(20000);
  }
  ::kill(child, SIGKILL);
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  ::_exit(125);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5 || std::string(argv[1]) != "--lifeline-fd" || std::string(argv[3]) != "--") {
    std::fprintf(stderr, "usage: corevideo-child-guard --lifeline-fd <N> -- <executable> <args...>\n");
    return 126;
  }
  char* end = nullptr;
  const long parsed = std::strtol(argv[2], &end, 10);
  if (end == argv[2] || *end != '\0' || parsed < 3 || parsed > 255) {
    std::fprintf(stderr, "corevideo-child-guard: bad lifeline descriptor '%s'\n", argv[2]);
    return 126;
  }
  const int lifeline = static_cast<int>(parsed);
  if (::fcntl(lifeline, F_GETFD) < 0) {
    std::fprintf(stderr, "corevideo-child-guard: lifeline descriptor %d is not open\n", lifeline);
    return 126;
  }
  // The target must not hold the lifeline.
  ::fcntl(lifeline, F_SETFD, FD_CLOEXEC);

  struct sigaction action{};
  action.sa_handler = onSignal;
  sigemptyset(&action.sa_mask);
  for (int signal : {SIGTERM, SIGINT, SIGHUP}) ::sigaction(signal, &action, nullptr);

  const pid_t child = ::fork();
  if (child < 0) {
    std::fprintf(stderr, "corevideo-child-guard: fork failed: %s\n", std::strerror(errno));
    return 127;
  }
  if (child == 0) {
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    for (int signal : {SIGTERM, SIGINT, SIGHUP}) ::sigaction(signal, &dfl, nullptr);
    ::execv(argv[4], argv + 4);
    std::fprintf(stderr, "corevideo-child-guard: cannot execute %s: %s\n", argv[4], std::strerror(errno));
    ::_exit(127);
  }
  // Our copies of the target's pipes would keep them open after it dies.
  for (int fd = 0; fd < 256; ++fd) {
    if (fd != 2 && fd != lifeline) ::close(fd);
  }

  for (;;) {
    if (const int signal = g_forward; signal != 0) {
      g_forward = 0;
      ::kill(child, signal);
    }
    int status = 0;
    const pid_t reaped = ::waitpid(child, &status, WNOHANG);
    if (reaped == child) mirror(status);
    pollfd watch{lifeline, POLLIN, 0};
    const int ready = ::poll(&watch, 1, 50);
    if (ready > 0 && (watch.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
      char byte = 0;
      const ssize_t got = ::read(lifeline, &byte, 1);
      if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) endChild(child);
    }
  }
}
