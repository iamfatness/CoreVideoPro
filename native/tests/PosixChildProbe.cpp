// corevideo-posix-child-probe: the child (and stand-in core) that the POSIX
// child-lifetime tests drive. Never shipped.
//
//   stubborn <pidfile>              ignores SIGTERM, never reads stdin, writes its pid, sleeps 120 s
//   eof-exit                        reads fd 0 until EOF, then exits 7
//   exit <n>                        exits n
//   sleep <seconds> <pidfile>       writes its pid, sleeps
//   fake-core <probe> <guard> <pidfile>
//                                   spawns "<probe> stubborn <pidfile>" through posix_child::spawn
//                                   with <guard>, then a sibling "<probe> sleep 30 <pidfile>.sibling"
//                                   with a PLAIN posix_spawn (no close-on-exec default, the way the
//                                   core's fork/exec children start), then sleeps: a core the test
//                                   can SIGKILL. The sibling must not hold the lifeline open.
//
// When COREVIDEO_TEST_PROBE_MODE is set the mode comes from it and argv is
// ignored, so a copy named "ffmpeg" can stand in for the sender's transport
// child (FfmpegChildRetirementTest): COREVIDEO_TEST_PROBE_MODE=stubborn with
// the pidfile in COREVIDEO_TEST_FFMPEG_PID_FILE.

#include "modules/PosixChildProcess.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <spawn.h>
#include <unistd.h>

extern char** environ;

namespace {

void writePid(const char* path) {
  std::ofstream out(path, std::ios::trunc);
  out << ::getpid();
}

int stubborn(const char* pidfile) {
  std::signal(SIGTERM, SIG_IGN);
  if (pidfile && *pidfile) writePid(pidfile);
  for (int i = 0; i < 1200; ++i) ::usleep(100000);
  return 0;
}

int eofExit() {
  char buffer[4096];
  for (;;) {
    const ssize_t got = ::read(0, buffer, sizeof(buffer));
    if (got == 0) return 7;
    if (got < 0 && errno != EINTR) return 9;
  }
}

int fakeCore(const char* probe, const char* guard, const char* pidfile) {
  corevideo::modules::posix_child::SpawnRequest request;
  request.executable = probe;
  request.arguments = {"stubborn", pidfile};
  request.guardExecutable = guard;
  const auto result = corevideo::modules::posix_child::spawn(request);
  if (result.pid <= 0) {
    std::fprintf(stderr, "fake-core: spawn failed (%s)\n", std::strerror(result.error));
    return 3;
  }
  const std::string siblingPidfile = std::string(pidfile) + ".sibling";
  const char* argv[] = {probe, "sleep", "30", siblingPidfile.c_str(), nullptr};
  pid_t sibling = 0;
  if (::posix_spawn(&sibling, probe, nullptr, nullptr, const_cast<char**>(argv), environ) != 0) return 4;
  std::printf("%d\n", static_cast<int>(result.pid));
  std::fflush(stdout);
  for (;;) ::pause();
}

}  // namespace

int main(int argc, char** argv) {
  if (const char* mode = std::getenv("COREVIDEO_TEST_PROBE_MODE"); mode && *mode) {
    if (std::string(mode) == "stubborn") return stubborn(std::getenv("COREVIDEO_TEST_FFMPEG_PID_FILE"));
    if (std::string(mode) == "eof-exit") return eofExit();
    return 2;
  }
  if (argc < 2) return 2;
  const std::string mode = argv[1];
  if (mode == "stubborn" && argc >= 3) return stubborn(argv[2]);
  if (mode == "eof-exit") return eofExit();
  if (mode == "sleep" && argc >= 4) {
    writePid(argv[3]);
    ::sleep(static_cast<unsigned>(std::atoi(argv[2])));
    return 0;
  }
  if (mode == "exit" && argc >= 3) return std::atoi(argv[2]);
  if (mode == "fake-core" && argc >= 5) return fakeCore(argv[2], argv[3], argv[4]);
  return 2;
}
