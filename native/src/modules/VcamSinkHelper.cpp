#include "modules/VcamSinkHelper.h"

#include "core/BoundedAsyncLog.h"
#include "modules/SinkVirtualCameraPublisher.h"
#include "modules/VirtualCameraFrame.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#include <pthread/qos.h>
#endif

extern char** environ;

namespace corevideo::modules {
namespace {

void mirrorCounters(VcamHelperShared* shared, const SinkVirtualCameraPublisher& camera) {
  shared->framesPublished.store(camera.status().framesPublished, std::memory_order_relaxed);
  shared->sequenceGaps.store(camera.sequenceGaps(), std::memory_order_relaxed);
  shared->queueFullDrops.store(camera.queueFullDrops(), std::memory_order_relaxed);
}

std::string resolved(const char* path) {
  char buffer[PATH_MAX] = {};
  return ::realpath(path, buffer) != nullptr ? std::string(buffer) : std::string();
}

}  // namespace

int runVcamSinkHelper(VcamHelperShared* shared, std::size_t mappedBytes, int wakeFd,
                      std::unique_ptr<IVirtualCameraSink> sink, std::function<std::int64_t()> hostClockNs) {
  if (!vcamHelperRegionValid(shared, mappedBytes) || !sink) return kVcamHelperExitBadInvocation;
#if defined(__APPLE__)
  // Per-frame work with a 16.7 ms period: same class as the core's video workers.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  const int width = shared->width, height = shared->height;
  const std::size_t frameBytes = nv12FrameSize(width, height);
  SinkVirtualCameraPublisher camera(std::move(sink), hostClockNs, /*logConnectFailures=*/false);

  // ONE attempt. This process's CoreMediaIO view was taken just now, at birth.
  if (!camera.start(width, height, shared->fps)) {
    const std::string warning = camera.status().warning;
    shared->deviceSeen.store(static_cast<std::uint32_t>(vcamConnectResultSawDevice(warning) ? VcamDeviceSeen::Yes
                                                                                             : VcamDeviceSeen::No),
                             std::memory_order_relaxed);
    vcamHelperPublishState(shared, VcamHelperState::Failed, warning);
    return kVcamHelperExitUnreachable;
  }
  shared->deviceSeen.store(static_cast<std::uint32_t>(VcamDeviceSeen::Yes), std::memory_order_relaxed);
  vcamHelperPublishState(shared, VcamHelperState::Live, "");

  char wakeBytes[256];
  for (;;) {
    const ssize_t got = ::read(wakeFd, wakeBytes, sizeof(wakeBytes));
    if (got < 0 && errno == EINTR) continue;
    VcamRingFrame frame;
    while (vcamRingPeek(shared, frame)) {
      const std::int64_t takenNs = hostClockNs ? hostClockNs() : 0;
      const std::uint64_t waited =
          takenNs > frame.meta.writtenAtNs ? static_cast<std::uint64_t>(takenNs - frame.meta.writtenAtNs) : 0;
      camera.publishNv12Borrowed(frame.nv12, frameBytes, width, height, frame.meta.programSequence);
      const std::int64_t doneNs = hostClockNs ? hostClockNs() : 0;
      const std::uint64_t worked = doneNs > takenNs ? static_cast<std::uint64_t>(doneNs - takenNs) : 0;
      vcamRingPop(shared);
      shared->transportLastNs.store(waited, std::memory_order_relaxed);
      shared->transportSumNs.fetch_add(waited, std::memory_order_relaxed);
      vcamHelperRecordMax(shared->transportMaxNs, waited);
      shared->publishLastNs.store(worked, std::memory_order_relaxed);
      shared->publishSumNs.fetch_add(worked, std::memory_order_relaxed);
      vcamHelperRecordMax(shared->publishMaxNs, worked);
      mirrorCounters(shared, camera);
      shared->framesReceived.fetch_add(1, std::memory_order_relaxed);
      const auto status = camera.status();
      if (status.state == "failed") {
        // The camera went away (extension restarted, updated, stopped draining).
        // Say why and leave: the core's next attempt is a new process.
        vcamHelperPublishState(shared, VcamHelperState::Failed, status.warning);
        return kVcamHelperExitLost;
      }
    }
    if (got == 0 || (got < 0 && errno != EINTR)) break;  // the core closed the pipe, or is gone
  }
  camera.stop();  // CMIODeviceStopStream: the camera shows its slate at once
  mirrorCounters(shared, camera);
  vcamHelperPublishState(shared, VcamHelperState::Stopped, "");
  return kVcamHelperExitStopped;
}

int runVcamSinkHelperFromDescriptors(int regionFd, int wakeFd, std::unique_ptr<IVirtualCameraSink> sink,
                                     std::function<std::int64_t()> hostClockNs) {
  struct stat info {};
  if (::fstat(regionFd, &info) != 0 || info.st_size < static_cast<off_t>(kVcamHelperHeaderBytes)) {
    return kVcamHelperExitBadInvocation;
  }
  const std::size_t bytes = static_cast<std::size_t>(info.st_size);
  void* mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, regionFd, 0);
  if (mapped == MAP_FAILED) return kVcamHelperExitBadInvocation;
  const int result = runVcamSinkHelper(static_cast<VcamHelperShared*>(mapped), bytes, wakeFd, std::move(sink),
                                       std::move(hostClockNs));
  ::munmap(mapped, bytes);
  return result;
}

std::string currentExecutablePath() {
#if defined(__APPLE__)
  char buffer[PATH_MAX] = {};
  std::uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) != 0) return {};
  return resolved(buffer);
#else
  return resolved("/proc/self/exe");
#endif
}

std::string executablePathOfProcess(std::int64_t pid) {
#if defined(__APPLE__)
  char buffer[PROC_PIDPATHINFO_MAXSIZE] = {};
  if (proc_pidpath(static_cast<int>(pid), buffer, sizeof(buffer)) <= 0) return {};
  return resolved(buffer);
#else
  return resolved(("/proc/" + std::to_string(pid) + "/exe").c_str());
#endif
}

namespace {

class PosixVcamHelperChannel final : public IVcamHelperChannel {
 public:
  PosixVcamHelperChannel(pid_t pid, int wakeWriteFd, VcamHelperShared* shared, std::size_t bytes)
      : pid_(pid), wakeWriteFd_(wakeWriteFd), shared_(shared), bytes_(bytes) {}

  ~PosixVcamHelperChannel() override {
    requestStop();
    int ignored = 0;
    if (!exited(&ignored)) kill();
    if (shared_ != nullptr) ::munmap(shared_, bytes_);
  }

  VcamHelperShared* shared() override { return shared_; }

  void wake() override {
    if (wakeWriteFd_ < 0) return;
    const char byte = 'f';
    // Non-blocking: a full pipe already holds more wake-ups than the helper needs.
    (void)::write(wakeWriteFd_, &byte, 1);
  }

  bool exited(int* exitCode) override {
    if (!reaped_) {
      int status = 0;
      pid_t waited;
      do { waited = ::waitpid(pid_, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
      if (waited == pid_) {
        reaped_ = true;
        exitCode_ = WIFSIGNALED(status) ? -WTERMSIG(status) : WEXITSTATUS(status);
      } else if (waited < 0) {
        reaped_ = true;  // not our child any more: nothing left to wait for
        exitCode_ = -1;
      }
    }
    if (reaped_ && exitCode != nullptr) *exitCode = exitCode_;
    return reaped_;
  }

  void requestStop() override {
    if (wakeWriteFd_ >= 0) {
      ::close(wakeWriteFd_);
      wakeWriteFd_ = -1;
    }
  }

  void kill() override {
    if (reaped_) return;
    ::kill(pid_, SIGKILL);
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(pid_, &status, 0); } while (waited < 0 && errno == EINTR);
    reaped_ = true;
    exitCode_ = -SIGKILL;
  }

  std::int64_t pid() const override { return pid_; }

 private:
  pid_t pid_;
  int wakeWriteFd_;
  VcamHelperShared* shared_;
  std::size_t bytes_;
  bool reaped_ = false;
  int exitCode_ = 0;
};

class PosixVcamHelperLauncher final : public IVcamHelperLauncher {
 public:
  PosixVcamHelperLauncher(std::string executable, std::vector<std::string> arguments)
      : executable_(std::move(executable)), arguments_(std::move(arguments)) {}

  std::unique_ptr<IVcamHelperChannel> launch(int width, int height, int fps, std::string* error) override {
    auto fail = [&](const std::string& what) -> std::unique_ptr<IVcamHelperChannel> {
      if (error != nullptr) *error = what + ": " + std::strerror(errno);
      return nullptr;
    };
    if (executable_.empty()) {
      if (error != nullptr) *error = "the core's own path is unknown";
      return nullptr;
    }
    const std::uint32_t slotBytes = static_cast<std::uint32_t>(nv12FrameSize(width, height));
    const std::size_t bytes = vcamHelperRegionBytes(kVcamHelperSlots, slotBytes);

    // An unnamed region: created, unlinked at once, handed over as a descriptor.
    static std::atomic<unsigned> counter{0};
    char name[32];  // macOS caps shm names at 31 characters
    std::snprintf(name, sizeof(name), "/cvp-vh-%d-%u", static_cast<int>(::getpid()) % 1000000,
                  counter.fetch_add(1) % 100000u);
    const int regionFd = ::shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (regionFd < 0) return fail("shm_open");
    ::shm_unlink(name);
    if (::ftruncate(regionFd, static_cast<off_t>(bytes)) != 0) {
      const int saved = errno; ::close(regionFd); errno = saved;
      return fail("ftruncate");
    }
    void* mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, regionFd, 0);
    if (mapped == MAP_FAILED) {
      const int saved = errno; ::close(regionFd); errno = saved;
      return fail("mmap");
    }
    auto* shared = static_cast<VcamHelperShared*>(mapped);
    formatVcamHelperRegion(shared, width, height, fps, slotBytes);

    int wake[2] = {-1, -1};
    if (::pipe(wake) != 0) {
      const int saved = errno; ::munmap(mapped, bytes); ::close(regionFd); errno = saved;
      return fail("pipe");
    }
    ::fcntl(wake[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(wake[1], F_SETFD, FD_CLOEXEC);
    ::fcntl(regionFd, F_SETFD, FD_CLOEXEC);
    ::fcntl(wake[1], F_SETFL, ::fcntl(wake[1], F_GETFL) | O_NONBLOCK);
#if defined(__APPLE__)
    ::fcntl(wake[1], F_SETNOSIGPIPE, 1);  // a dead helper must not signal the core
#endif

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, wake[0], kVcamHelperWakeFd);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
#if defined(__APPLE__)
    // Only the descriptors named here reach the helper.
    posix_spawn_file_actions_addinherit_np(&actions, 2);
    if (regionFd == kVcamHelperRegionFd) posix_spawn_file_actions_addinherit_np(&actions, regionFd);
    else posix_spawn_file_actions_adddup2(&actions, regionFd, kVcamHelperRegionFd);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
    posix_spawn_file_actions_adddup2(&actions, regionFd, kVcamHelperRegionFd);
#endif
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable_.c_str()));
    for (auto& argument : arguments_) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = ::posix_spawn(&pid, executable_.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    ::close(wake[0]);
    ::close(regionFd);
    if (spawned != 0) {
      ::close(wake[1]);
      ::munmap(mapped, bytes);
      errno = spawned;
      return fail("posix_spawn " + executable_);
    }
    return std::make_unique<PosixVcamHelperChannel>(pid, wake[1], shared, bytes);
  }

 private:
  std::string executable_;
  std::vector<std::string> arguments_;
};

}  // namespace

std::unique_ptr<IVcamHelperLauncher> createPosixVcamHelperLauncher(std::string executable,
                                                                   std::vector<std::string> arguments) {
  return std::make_unique<PosixVcamHelperLauncher>(std::move(executable), std::move(arguments));
}

}  // namespace corevideo::modules
