// P1 (mac streaming Phase 2): no helper child outlives the core, not even
// when the core is SIGKILLed. These tests drive the REAL corevideo-child-guard
// and a probe child (tests/PosixChildProbe.cpp); every expectation is a
// constant the fixture chose (exit 42, SIGUSR2, exit 7), never a value read
// back from the code under test.

#include "modules/PosixChildProcess.h"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

namespace fs = std::filesystem;
namespace pc = corevideo::modules::posix_child;
using namespace std::chrono_literals;

constexpr const char* kProbe = COREVIDEO_POSIX_CHILD_PROBE_PATH;
constexpr const char* kGuard = COREVIDEO_CHILD_GUARD_PATH;

fs::path tempPath(const std::string& name) {
  return fs::temp_directory_path() /
         ("cvp-child-" + std::to_string(::getpid()) + "-" + name);
}

// The pid a probe wrote, once the file holds one (0 on timeout).
pid_t waitForPidFile(const fs::path& path, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::ifstream in(path);
    long pid = 0;
    if (in >> pid && pid > 0) return static_cast<pid_t>(pid);
    std::this_thread::sleep_for(10ms);
  }
  return 0;
}

bool alive(pid_t pid) { return pid > 0 && ::kill(pid, 0) == 0; }

bool goneWithin(pid_t pid, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!alive(pid)) return true;
    std::this_thread::sleep_for(20ms);
  }
  return !alive(pid);
}

// Reap `pid` (our own child) within the timeout; false when it is still running.
bool reapWithin(pid_t pid, std::chrono::milliseconds timeout, int* status) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t r = ::waitpid(pid, status, WNOHANG);
    if (r == pid) return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

pc::SpawnRequest probe(std::vector<std::string> args) {
  pc::SpawnRequest request;
  request.executable = kProbe;
  request.arguments = std::move(args);
  request.guardExecutable = kGuard;
  return request;
}

}  // namespace

TEST(PosixChildProcess, ParentSigkillLeavesNoChild) {
  const fs::path pidfile = tempPath("sigkill.pid");
  fs::remove(pidfile);
  // The stand-in core is started with a plain posix_spawn: it, not this test
  // process, owns the lifeline its stubborn child's guard watches.
  pid_t core = 0;
  const char* argv[] = {kProbe, "fake-core", kProbe, kGuard, pidfile.c_str(), nullptr};
  ASSERT_EQ(::posix_spawn(&core, kProbe, nullptr, nullptr, const_cast<char**>(argv), environ), 0);
  const pid_t child = waitForPidFile(pidfile, 5000ms);
  ASSERT_GT(child, 0) << "the stand-in core never started its stubborn child";
  // A plain-spawned sibling (how the core's fork/exec helpers start) must not
  // inherit the lifeline's write end, or it would keep every guard alive.
  const fs::path siblingPidfile = fs::path(pidfile.string() + ".sibling");
  const pid_t sibling = waitForPidFile(siblingPidfile, 5000ms);
  ASSERT_GT(sibling, 0) << "the stand-in core never started its plain sibling";
  // Precondition, or the assertion below is vacuous: the child is running now.
  ASSERT_TRUE(alive(child));
  ASSERT_EQ(::kill(core, SIGKILL), 0);
  int status = 0;
  ASSERT_EQ(::waitpid(core, &status, 0), core);
  // The guard's grace is 2 s (SIGTERM is ignored by this child); 5 s of slack.
  EXPECT_TRUE(goneWithin(child, 5000ms))
      << "pid " << child << " outlived a SIGKILLed parent: an orphaned helper";
  if (alive(child)) ::kill(child, SIGKILL);
  if (alive(sibling)) ::kill(sibling, SIGKILL);
  fs::remove(pidfile);
  fs::remove(siblingPidfile);
}

TEST(PosixChildProcess, GuardMirrorsExitCodeAndSignal) {
  const auto exited = pc::spawn(probe({"exit", "42"}));
  ASSERT_GT(exited.pid, 0);
  EXPECT_TRUE(exited.guarded);
  int status = 0;
  ASSERT_TRUE(reapWithin(exited.pid, 5000ms, &status));
  ASSERT_TRUE(WIFEXITED(status)) << "status " << status;
  EXPECT_EQ(WEXITSTATUS(status), 42);

  const fs::path pidfile = tempPath("signal.pid");
  fs::remove(pidfile);
  const auto signalled = pc::spawn(probe({"stubborn", pidfile.string()}));
  ASSERT_GT(signalled.pid, 0);
  const pid_t child = waitForPidFile(pidfile, 5000ms);
  ASSERT_GT(child, 0);
  ASSERT_NE(child, signalled.pid) << "the target must run under the guard, not as the guard";
  ASSERT_EQ(::kill(child, SIGUSR2), 0);
  ASSERT_TRUE(reapWithin(signalled.pid, 5000ms, &status));
  ASSERT_TRUE(WIFSIGNALED(status)) << "status " << status;
  EXPECT_EQ(WTERMSIG(status), SIGUSR2);
  fs::remove(pidfile);
}

TEST(PosixChildProcess, ASiblingNeverHoldsAnotherChildsStdinOpen) {
  // Plain pipes, NOT close-on-exec: exactly what the sender used to hand out.
  int pipeA[2];
  int pipeB[2];
  ASSERT_EQ(::pipe(pipeA), 0);
  ASSERT_EQ(::pipe(pipeB), 0);
  auto a = probe({"eof-exit"});
  a.fds = {{pipeA[0], 0}};
  const auto childA = pc::spawn(a);
  ASSERT_GT(childA.pid, 0);
  ::close(pipeA[0]);
  auto b = probe({"eof-exit"});
  b.fds = {{pipeB[0], 0}};
  const auto childB = pc::spawn(b);
  ASSERT_GT(childB.pid, 0);
  ::close(pipeB[0]);

  ::close(pipeA[1]);  // EOF for A, unless B inherited A's write end
  int status = 0;
  const bool reapedA = reapWithin(childA.pid, 2000ms, &status);
  EXPECT_TRUE(reapedA) << "A never saw EOF: a sibling holds its stdin open";
  if (reapedA) {
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 7);
  }
  EXPECT_EQ(::waitpid(childB.pid, &status, WNOHANG), 0) << "B must still be running";
  ::close(pipeB[1]);
  EXPECT_TRUE(reapWithin(childB.pid, 2000ms, &status));
  if (!reapedA) {
    pc::stopGroup(childA.pid, 100ms, &status);
  }
}

TEST(PosixChildProcess, StopGroupEndsAStubbornGuardedChild) {
  const fs::path pidfile = tempPath("stop.pid");
  fs::remove(pidfile);
  const auto spawned = pc::spawn(probe({"stubborn", pidfile.string()}));
  ASSERT_GT(spawned.pid, 0);
  const pid_t child = waitForPidFile(pidfile, 5000ms);
  ASSERT_GT(child, 0);
  ASSERT_TRUE(alive(child));
  int status = 0;
  EXPECT_TRUE(pc::stopGroup(spawned.pid, 200ms, &status));
  EXPECT_TRUE(goneWithin(child, 2000ms))
      << "stopGroup reaped the guard but its stubborn child survived";
  if (alive(child)) ::kill(child, SIGKILL);
  fs::remove(pidfile);
}

TEST(PosixChildProcess, AMissingGuardIsAnErrorNotASilentUnguardedChild) {
  auto request = probe({"exit", "0"});
  request.guardExecutable = "/nonexistent/corevideo-child-guard";
  const auto refused = pc::spawn(request);
  EXPECT_EQ(refused.pid, 0);
  EXPECT_EQ(refused.error, ENOENT);

  // Nothing resolvable at all: the target runs unguarded, says so, and still
  // leads its own process group so a group signal reaches it.
  ::setenv("COREVIDEO_CHILD_GUARD_PATH", "/nonexistent/corevideo-child-guard", 1);
  int input[2];
  ASSERT_EQ(::pipe(input), 0);
  auto fallback = probe({"eof-exit"});
  fallback.guardExecutable.clear();
  fallback.fds = {{input[0], 0}};
  const auto unguarded = pc::spawn(fallback);
  ::unsetenv("COREVIDEO_CHILD_GUARD_PATH");
  ::close(input[0]);
  ASSERT_GT(unguarded.pid, 0);
  EXPECT_FALSE(unguarded.guarded);
  // Still running (it waits for EOF), so getpgid answers.
  EXPECT_EQ(::getpgid(unguarded.pid), unguarded.pid);
  ::close(input[1]);
  int status = 0;
  ASSERT_TRUE(reapWithin(unguarded.pid, 5000ms, &status));
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 7);
}
