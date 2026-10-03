// Verifies the core<->DLL shared-memory contract in-process: write an NV12 frame
// into the slot exactly as the publisher does (seqlock), then read it back with
// the DLL's SharedFrameReader. This covers the SHM layout + seqlock discipline
// that is otherwise only exercised when the Frame Server loads the DLL for real.
#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>

#include <cstdlib>  // std::getenv, _putenv_s
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#if COREVIDEO_WITH_VIRTUALCAM
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>
#endif

#include "SharedFrameReader.h"  // native/virtualcam-dll (on the test include path)
#include "modules/VirtualCameraPublisher.h"
#include "modules/VirtualCameraShm.h"

using corevideo::modules::VirtualCameraShmHeader;
using corevideo::modules::kVirtualCameraMagic;
using corevideo::modules::mapVirtualCameraShmView;
using corevideo::modules::openVirtualCameraShmFile;
using corevideo::modules::virtualCameraShmFilePath;
using corevideo::modules::virtualCameraShmSize;
using corevideo::virtualcam::SharedFrameReader;

namespace {

// THIS SUITE NEVER TOUCHES THE PRODUCTION SLOT. Every test below unlinks the
// slot file for isolation, and until 2026-09-20 that was the PRODUCTION path:
// running the suite while CoreVideo Pro was live unlinked the very file the
// core's publisher was writing into (FILE_SHARE_DELETE lets a delete succeed
// under an open writer). The core kept publishing into the orphaned file object
// and reported healthy, while the Frame Server reader got ERROR_FILE_NOT_FOUND
// on the path and served the standby slate - the operator's virtual camera
// showed a grey bar in Zoom until they toggled it off and on. So the whole
// process is redirected ONCE, at static-initialization time (before main, so
// before any test in this binary can run — the in-house gtest shim has no test
// environments), to a private temp directory via COREVIDEO_VCAM_SHM_DIR, which
// virtualCameraShmDir() honors. The DLL's SharedFrameReader and the real
// publisher both resolve the path through that one helper, so the redirect
// covers every writer and reader in this binary.
struct VcamShmTestIsolation {
  std::string dir;
  bool armed = false;

  VcamShmTestIsolation() {
    char tmp[MAX_PATH] = {};
    const DWORD n = ::GetTempPathA(MAX_PATH, tmp);
    std::string root = (n > 0 && n < MAX_PATH) ? std::string(tmp, n) : std::string("C:\\Temp\\");
    if (!root.empty() && root.back() != '\\') root.push_back('\\');
    dir = root + "cvp-vcam-shm-test-" + std::to_string(::GetCurrentProcessId());
    ::CreateDirectoryA(dir.c_str(), nullptr);
    armed = _putenv_s("COREVIDEO_VCAM_SHM_DIR", dir.c_str()) == 0;
  }
  ~VcamShmTestIsolation() {
    ::DeleteFileA((dir + "\\vcam-frame.shm").c_str());
    ::DeleteFileA((dir + "\\vcam-serve.log").c_str());
    ::RemoveDirectoryA(dir.c_str());
    _putenv_s("COREVIDEO_VCAM_SHM_DIR", "");
  }
};

const VcamShmTestIsolation kVcamShmTestIsolation;

std::string productionVirtualCameraShmFilePath() {
  const char* pd = std::getenv("ProgramData");
  std::string root = (pd != nullptr && *pd != '\0') ? std::string(pd) : std::string("C:\\ProgramData");
  return root + "\\CoreVideoPro\\vcam-frame.shm";
}

// Minimal writer mirroring WindowsVirtualCameraPublisher's slot format
// (file-backed on %ProgramData%, exactly as the real publisher does it).
struct ShmWriter {
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mapping = nullptr;
  void* view = nullptr;
  VirtualCameraShmHeader* header = nullptr;
  bool deleteOnClose = true;

  bool open() {
    file = openVirtualCameraShmFile(/*writer=*/true);
    if (file == INVALID_HANDLE_VALUE) return false;
    view = mapVirtualCameraShmView(file, /*writer=*/true, &mapping);
    if (view == nullptr) return false;
    header = static_cast<VirtualCameraShmHeader*>(view);
    // Mirror WindowsVirtualCameraPublisher::start(): the file is REUSED across
    // runs, so re-init under the seqlock, continuing FORWARD from any prior seq
    // (never back to 0 - a reader from the prior run must see a CHANGED seq).
    header->seq = header->seq | 1u;  // odd: writing
    header->byteLen = 0;
    header->frameNumber = 0;
    header->magic = kVirtualCameraMagic;
    header->seq = (header->seq | 1u) + 1u;  // even = complete
    return true;
  }

  void write(const std::vector<std::uint8_t>& nv12, int w, int h) {
    auto* payload = static_cast<std::uint8_t*>(view) + sizeof(VirtualCameraShmHeader);
    header->seq = header->seq + 1;  // odd
    std::memcpy(payload, nv12.data(), nv12.size());
    header->width = w;
    header->height = h;
    header->byteLen = static_cast<std::uint32_t>(nv12.size());
    header->frameNumber += 1;
    header->seq = header->seq + 1;  // even
  }

  void closeHandles() {
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    view = nullptr;
    mapping = nullptr;
    file = INVALID_HANDLE_VALUE;
    header = nullptr;
  }

  ~ShmWriter() {
    closeHandles();
    if (deleteOnClose) {
      ::DeleteFileA(virtualCameraShmFilePath().c_str());  // no stale slot for the next test
    }
  }
};

}  // namespace

// REGRESSION (2026-09-20 incident, see VcamShmTestIsolation above): with the
// isolation environment armed, the slot path every test in this binary resolves
// - and therefore every DeleteFileA below - must live under the private test
// directory, never at the production %ProgramData% path the live app publishes
// to. Fails when virtualCameraShmDir() ignores COREVIDEO_VCAM_SHM_DIR.
TEST(VirtualCameraShmRoundtrip, TheSuiteNeverResolvesTheProductionSlotPath) {
  ASSERT_TRUE(kVcamShmTestIsolation.armed);
  const std::string resolved = virtualCameraShmFilePath();
  EXPECT_NE(resolved, productionVirtualCameraShmFilePath())
      << "the test suite resolved the PRODUCTION vcam slot; running it would "
         "unlink the file a live CoreVideo Pro is publishing into";
  EXPECT_EQ(resolved, kVcamShmTestIsolation.dir + "\\vcam-frame.shm")
      << "COREVIDEO_VCAM_SHM_DIR is not honored by virtualCameraShmDir()";
}

TEST(VirtualCameraShmRoundtrip, ReadsBackTheFrameTheWriterPublished) {
  ShmWriter writer;
  ASSERT_TRUE(writer.open());

  const int w = 64, h = 36;
  std::vector<std::uint8_t> frame(static_cast<size_t>(w) * h * 3 / 2);
  for (size_t i = 0; i < frame.size(); ++i) {
    frame[i] = static_cast<std::uint8_t>(i * 7 + 3);  // deterministic pattern
  }
  writer.write(frame, w, h);

  SharedFrameReader reader;
  std::vector<std::uint8_t> out;
  int rw = 0, rh = 0;
  ASSERT_TRUE(reader.readLatest(out, rw, rh));
  EXPECT_EQ(reader.evidence().lastPublication, 1u);
  EXPECT_EQ(reader.evidence().lastSequence, writer.header->seq);
  EXPECT_EQ(rw, w);
  EXPECT_EQ(rh, h);
  EXPECT_EQ(out, frame);
}

TEST(VirtualCameraShmRoundtrip, EvidenceSeparatesUnchangedContentFromAnInProgressWrite) {
  using Result = corevideo::modules::VirtualCameraReadResult;
  ShmWriter writer;
  ASSERT_TRUE(writer.open());
  std::vector<std::uint8_t> frame(64 * 36 * 3 / 2, 42), out;
  writer.write(frame, 64, 36);
  SharedFrameReader reader;
  int w = 0, h = 0;
  ASSERT_TRUE(reader.readLatest(out, w, h));
  EXPECT_FALSE(reader.readLatest(out, w, h));
  EXPECT_EQ(reader.evidence().lastResult, Result::Unchanged);
  ++writer.header->seq; // odd: a write is in progress; do not invent an identity
  writer.header->frameNumber = 99;
  EXPECT_FALSE(reader.readLatest(out, w, h));
  EXPECT_EQ(reader.evidence().lastResult, Result::Contended);
  EXPECT_EQ(reader.evidence().lastPublication, 1u);
  ++writer.header->seq; // complete: the same payload now has publication 99
  ASSERT_TRUE(reader.readLatest(out, w, h));
  EXPECT_EQ(reader.evidence().lastPublication, 99u);
  EXPECT_EQ(reader.evidence().count(Result::Fresh), 2u);
  EXPECT_EQ(reader.evidence().count(Result::Unchanged), 1u);
  EXPECT_EQ(reader.evidence().count(Result::Contended), 1u);
}

TEST(VirtualCameraShmRoundtrip, EvidenceSeparatesInvalidHeaderFromUninitializedMapping) {
  using Result = corevideo::modules::VirtualCameraReadResult;
  ShmWriter writer;
  ASSERT_TRUE(writer.open());
  SharedFrameReader reader;
  std::vector<std::uint8_t> out;
  int w = 0, h = 0;
  EXPECT_FALSE(reader.readLatest(out, w, h)); // writer has not provided pixels
  EXPECT_EQ(reader.evidence().lastResult, Result::InvalidHeader);
  writer.header->magic = 0;
  EXPECT_FALSE(reader.readLatest(out, w, h));
  EXPECT_EQ(reader.evidence().lastResult, Result::Uninitialized);
  EXPECT_FALSE(reader.evidence().identityObserved);
}

#if COREVIDEO_WITH_VIRTUALCAM
// Load this build's DLL directly. No camera registration or Frame Server restart;
// both pixels and diagnostics remain in the isolated per-test-process directory.
TEST(VirtualCameraShmRoundtrip, RealDllCountsEmittedFreshHeldAndSlateSamples) {
  using Microsoft::WRL::ComPtr;
  struct Session {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT mf = MFStartup(MF_VERSION);
    HMODULE dll = nullptr;
    ComPtr<IMFMediaSource> source;
    ~Session() {
      if (source) { source->Shutdown(); source.Reset(); }
      if (dll) FreeLibrary(dll);
      if (SUCCEEDED(mf)) MFShutdown();
      if (SUCCEEDED(com)) CoUninitialize();
    }
  } session;
  ASSERT_TRUE(SUCCEEDED(session.mf));
  ShmWriter writer;
  ASSERT_TRUE(writer.open());
  std::vector<std::uint8_t> pixels(1920 * 1080 * 3 / 2, 42);
  writer.write(pixels, 1920, 1080);
  const auto logPath = corevideo::modules::virtualCameraShmDir() + "\\vcam-serve.log";
  const auto logOffset = [&] {
    std::ifstream before(logPath, std::ios::binary | std::ios::ate);
    return before ? static_cast<std::streamoff>(before.tellg()) : std::streamoff{0};
  }();
  session.dll = LoadLibraryA(COREVIDEO_VCAM_DLL_PATH);
  ASSERT_NE(session.dll, nullptr);
  using GetFactory = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
  const auto getFactory = reinterpret_cast<GetFactory>(GetProcAddress(session.dll, "DllGetClassObject"));
  ASSERT_NE(getFactory, nullptr);
  CLSID clsid{};
  ASSERT_TRUE(SUCCEEDED(CLSIDFromString(L"{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}", &clsid)));
  ComPtr<IClassFactory> factory;
  ASSERT_TRUE(SUCCEEDED(getFactory(clsid, IID_PPV_ARGS(&factory))));
  ComPtr<IMFActivate> activate;
  ASSERT_TRUE(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate))));
  ASSERT_TRUE(SUCCEEDED(activate->ActivateObject(IID_PPV_ARGS(&session.source))));
  ComPtr<IMFPresentationDescriptor> descriptor;
  ASSERT_TRUE(SUCCEEDED(session.source->CreatePresentationDescriptor(&descriptor)));
  PROPVARIANT start{};
  ASSERT_TRUE(SUCCEEDED(session.source->Start(descriptor.Get(), nullptr, &start)));
  ComPtr<IMFMediaEvent> announcement;
  ASSERT_TRUE(SUCCEEDED(session.source->GetEvent(MF_EVENT_FLAG_NO_WAIT, &announcement)));
  PROPVARIANT value{};
  ASSERT_TRUE(SUCCEEDED(announcement->GetValue(&value)));
  ComPtr<IMFMediaStream> stream;
  const auto streamHr = value.punkVal->QueryInterface(IID_PPV_ARGS(&stream));
  PropVariantClear(&value);
  ASSERT_TRUE(SUCCEEDED(streamHr));
  // A receiver of the actual MF samples, including their pixels and PTS. A
  // queued event or a publication counter alone is not receiver evidence.
  LONGLONG lastPts = -1;
  auto receive = [&]() -> int {
    if (FAILED(stream->RequestSample(nullptr))) return -1;
    for (int eventIndex = 0; eventIndex < 3; ++eventIndex) {
      ComPtr<IMFMediaEvent> event;
      if (FAILED(stream->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event))) return -1;
      MediaEventType type{};
      if (FAILED(event->GetType(&type))) return -1;
      if (type != MEMediaSample) continue;
      PROPVARIANT payload{};
      if (FAILED(event->GetValue(&payload))) return -1;
      ComPtr<IMFSample> sample;
      const auto hr = payload.punkVal ? payload.punkVal->QueryInterface(IID_PPV_ARGS(&sample)) : E_FAIL;
      PropVariantClear(&payload);
      if (FAILED(hr)) return -1;
      LONGLONG pts = 0;
      if (FAILED(sample->GetSampleTime(&pts)) || pts <= lastPts) return -1;
      lastPts = pts;
      ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return -1;
      BYTE* data = nullptr;
      DWORD length = 0;
      if (FAILED(buffer->Lock(&data, nullptr, &length))) return -1;
      const int observed = length == pixels.size() ? data[0] : -1;
      buffer->Unlock();
      return observed;
    }
    return -1;
  };
  for (int i = 0; i < 61; ++i) {
    const int observed = receive();
    ASSERT_GE(observed, 0);
    if (i <= 30) EXPECT_EQ(observed, 42);
  }
  std::ifstream log(logPath, std::ios::binary);
  ASSERT_TRUE(log.good());
  log.seekg(logOffset);
  const std::string emittedLog((std::istreambuf_iterator<char>(log)), {});
  EXPECT_NE(emittedLog.find("emitted=61 fresh=1 held=30 slate=30 failed=0"), std::string::npos);
  EXPECT_NE(emittedLog.find("readFresh=1 unchanged=60 contended=0"), std::string::npos);
  EXPECT_NE(emittedLog.find("programIdentityVerified=0 receiverVerified=0"), std::string::npos);
  // Resume with a changing synthetic identity. This catches held/duplicated
  // sample contents despite successful requests and advancing event counters.
  for (int identity = 60; identity < 180; ++identity) {
    pixels[0] = static_cast<uint8_t>(identity);
    writer.write(pixels, 1920, 1080);
    EXPECT_EQ(receive(), identity);
  }
}
#endif

TEST(VirtualCameraShmRoundtrip, NoRegionMeansNoFrame) {
  // With no writer mapping alive, the reader reports "no frame" (not a crash).
  ::DeleteFileA(virtualCameraShmFilePath().c_str());  // ensure no stale slot file
  SharedFrameReader reader;
  std::vector<std::uint8_t> out;
  int rw = 0, rh = 0;
  EXPECT_FALSE(reader.readLatest(out, rw, rh));
  EXPECT_EQ(reader.evidence().lastResult, corevideo::modules::VirtualCameraReadResult::Unavailable);
  EXPECT_FALSE(reader.evidence().identityObserved);
}

TEST(VirtualCameraShmRoundtrip, LatestWriteWins) {
  ShmWriter writer;
  ASSERT_TRUE(writer.open());
  const int w = 8, h = 8;
  std::vector<std::uint8_t> a(static_cast<size_t>(w) * h * 3 / 2, 0x11);
  std::vector<std::uint8_t> b(static_cast<size_t>(w) * h * 3 / 2, 0x22);
  writer.write(a, w, h);
  writer.write(b, w, h);  // newest

  SharedFrameReader reader;
  std::vector<std::uint8_t> out;
  int rw = 0, rh = 0;
  ASSERT_TRUE(reader.readLatest(out, rw, rh));
  EXPECT_EQ(out, b);
}

// REGRESSION (the "flashing camera"): the writer restarting must NOT orphan a
// reader that already holds the backing file. The old writer deleted+recreated
// the path on start; a Frame Server reader kept mapping the unlinked old file
// object (FILE_SHARE_DELETE), never saw another frame, and served the slate
// forever - interleaved with a fresh instance's program frames = strobing.
TEST(VirtualCameraShmRoundtrip, WriterRestartKeepsAnExistingReaderLive) {
  const int w = 8, h = 8;
  std::vector<std::uint8_t> f1(static_cast<size_t>(w) * h * 3 / 2, 0x11);
  std::vector<std::uint8_t> f2(static_cast<size_t>(w) * h * 3 / 2, 0x22);

  SharedFrameReader reader;
  std::vector<std::uint8_t> out;
  int rw = 0, rh = 0;
  {
    ShmWriter first;
    first.deleteOnClose = false;  // an app restart does not remove the slot file
    ASSERT_TRUE(first.open());
    first.write(f1, w, h);
    ASSERT_TRUE(reader.readLatest(out, rw, rh));
    EXPECT_EQ(out, f1);
  }  // "app exit": writer handles closed, file stays

  ShmWriter second;  // "app relaunch": open-or-create IN PLACE (same file object)
  ASSERT_TRUE(second.open());
  second.write(f2, w, h);

  // The ORIGINAL reader mapping (opened before the restart) must see the new
  // frame - with a delete-on-start writer this read returned stale/no frames.
  ASSERT_TRUE(reader.readLatest(out, rw, rh));
  EXPECT_EQ(out, f2);
}

// If the file object DOES get swapped underneath a reader (the exact failure
// mode the delete-on-start writer caused), the reader self-heals: after
// kReopenAfterUnchangedReads frozen requests it re-opens by path and serves the
// live file instead of degrading to the slate forever.
TEST(VirtualCameraShmRoundtrip, ReaderSelfHealsAfterTheBackingFileIsSwapped) {
  const int w = 8, h = 8;
  std::vector<std::uint8_t> f1(static_cast<size_t>(w) * h * 3 / 2, 0x11);
  std::vector<std::uint8_t> f2(static_cast<size_t>(w) * h * 3 / 2, 0x22);

  SharedFrameReader reader;
  std::vector<std::uint8_t> out;
  int rw = 0, rh = 0;
  {
    ShmWriter first;
    first.deleteOnClose = false;
    ASSERT_TRUE(first.open());
    first.write(f1, w, h);
    ASSERT_TRUE(reader.readLatest(out, rw, rh));
    EXPECT_EQ(out, f1);
    first.closeHandles();
    ::DeleteFileA(virtualCameraShmFilePath().c_str());  // swap: unlink the path
  }

  ShmWriter second;  // NEW file object at the same path
  ASSERT_TRUE(second.open());
  second.write(f2, w, h);
  second.write(f2, w, h);  // advance seq past any collision with the reader's last

  bool healed = false;
  for (std::uint32_t i = 0; i <= SharedFrameReader::kReopenAfterUnchangedReads + 5 && !healed;
       ++i) {
    healed = reader.readLatest(out, rw, rh);
  }
  ASSERT_TRUE(healed);
  EXPECT_EQ(out, f2);
}

#if defined(COREVIDEO_WITH_VIRTUALCAM) && COREVIDEO_WITH_VIRTUALCAM
// REGRESSION (G4 teardown finding): the REAL publisher's stop() must leave the
// slot file in place. Deleting it orphans Frame Server readers that hold the
// file object via FILE_SHARE_DELETE - they freeze on the unlinked file and
// interleave with the next session's fresh instance as program/slate strobing.
// start() may fail at MFCreateVirtualCamera on rigs without the registered DLL;
// the SHM slot is created before that step either way, so the assertion holds
// in both environments.
TEST(VirtualCameraPublisher, StopLeavesTheShmSlotFileInPlace) {
  auto publisher = corevideo::modules::createVirtualCameraPublisher();
  ASSERT_NE(publisher, nullptr);
  publisher->start(1920, 1080, 60);
  if (::GetFileAttributesA(virtualCameraShmFilePath().c_str()) ==
      INVALID_FILE_ATTRIBUTES) {
    // Slot could not be created (no ProgramData access on this rig) - there is
    // nothing to assert about stop() then. (This gtest has no GTEST_SKIP.)
    return;
  }
  publisher->stop();
  EXPECT_NE(::GetFileAttributesA(virtualCameraShmFilePath().c_str()),
            INVALID_FILE_ATTRIBUTES)
      << "stop() must NOT delete the vcam SHM file: readers hold it via "
         "FILE_SHARE_DELETE and delete orphans them (frozen frames / strobing)";
  ::DeleteFileA(virtualCameraShmFilePath().c_str());  // test isolation only
}
#endif  // COREVIDEO_WITH_VIRTUALCAM

#endif  // _WIN32
