// AVFoundation/VideoToolbox recording encoder — the macOS twin of
// MediaFoundationEncoderAdapter (increment 1: program A+V only; ISO writers,
// the session subfolder scheme and manifest.json ride increment 2 — see
// docs/mac-port-phase3-avf-encoder.md).
//
// Structure mirrors the MF adapter deliberately: an Mp4Writer-shaped class
// (AvfMp4Writer) plus the sink. AVAssetWriter cannot add inputs after
// startWriting and cannot restart after finishWriting, so open() REBUILDS the
// writer object outright — the structural analogue of the MF writer's
// "open() resets ALL per-session state" (#286): a reused writer across the
// live flow's double start() keeps its audio track because the second open()
// starts from nothing.
//
// Program pixels: reads ProgramFrame::programFullBgra when present (the Metal
// compositor fills it during recording via wantsFullProgramReadbackForRecording)
// and falls back to the 320x180 preview — never the MF path's silent
// thumbnail-into-1080p upscale.

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AVF_ENCODER

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "modules/Interfaces.h"
#include "modules/RecordingArtifactNaming.h"
#include "modules/RecordingPtsClock.h"

namespace corevideo::modules {
namespace {

using recording::i420ToNv12;
using recording::jsonEscape;
using recording::sanitizeForFilename;
using recording::sessionTimestampFolder;
using recording::toStereo;

constexpr int64_t kHundredNsPerSecond = 10'000'000;

CMTime ptsToTime(int64_t pts100ns) {
  return CMTimeMake(pts100ns, static_cast<int32_t>(kHundredNsPerSecond));
}

enum class VideoInput { Bgra, Nv12 };

class AvfMp4Writer {
 public:
  ~AvfMp4Writer() { finalize(); }

  bool open(const std::string& path, int width, int height, int fps, double bitrateMbps,
            const std::string& codec, std::string& errorOut,
            VideoInput videoInput = VideoInput::Bgra) {
    (void)codec;  // H.264-only, matching the product default
    finalize();
    inputKind_ = videoInput;
    if (inputKind_ == VideoInput::Nv12) {
      width &= ~1;  // NV12 needs even luma dims (mirrors the MF writer)
      height &= ~1;
    }
    width_ = std::max(16, width);
    height_ = std::max(16, height);
    fps_ = std::max(1, fps);
    path_ = path;
    // AVAssetWriter refuses to overwrite ("Cannot Save"), unlike the MF sink
    // writer — a restarted session (the live flow's double start()) reopens
    // the same artifact path, so replace any prior file explicitly.
    std::error_code removeEc;
    std::filesystem::remove(path_, removeEc);
    NSError* error = nil;
    writer_ = [[AVAssetWriter alloc]
        initWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]]
           fileType:AVFileTypeMPEG4
              error:&error];
    if (!writer_) {
      errorOut = std::string("AVAssetWriter init failed: ") +
                 (error ? error.localizedDescription.UTF8String : "no diagnostics");
      return false;
    }
    NSDictionary* videoSettings = @{
      AVVideoCodecKey : AVVideoCodecTypeH264,
      AVVideoWidthKey : @(width_),
      AVVideoHeightKey : @(height_),
      AVVideoCompressionPropertiesKey :
          @{AVVideoAverageBitRateKey : @(static_cast<int64_t>(std::max(1.0, bitrateMbps) * 1e6))},
    };
    videoInput_ = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                     outputSettings:videoSettings];
    videoInput_.expectsMediaDataInRealTime = YES;
    if (![writer_ canAddInput:videoInput_]) {
      errorOut = "AVAssetWriter rejected the video input";
      return false;
    }
    [writer_ addInput:videoInput_];
    const OSType pixelFormat = inputKind_ == VideoInput::Nv12
                                   ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                   : kCVPixelFormatType_32BGRA;
    NSDictionary* adaptorAttrs = @{
      (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey : @(pixelFormat),
      (__bridge NSString*)kCVPixelBufferWidthKey : @(width_),
      (__bridge NSString*)kCVPixelBufferHeightKey : @(height_),
    };
    adaptor_ = [AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:videoInput_
                                   sourcePixelBufferAttributes:adaptorAttrs];
    return true;
  }

  bool ensureAudioStream(int channels, int sampleRate, int audioBitrateKbps,
                         std::string& errorOut) {
    if (audioConfigured_) {
      return true;
    }
    if (!writer_ || writing_) {
      errorOut = "audio stream must be added before beginWriting";
      return false;
    }
    audioChannels_ = std::clamp(channels, 1, 2);
    audioSampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    NSDictionary* audioSettings = @{
      AVFormatIDKey : @(kAudioFormatMPEG4AAC),
      AVNumberOfChannelsKey : @(audioChannels_),
      AVSampleRateKey : @(audioSampleRate_),
      AVEncoderBitRateKey : @(std::max(32, audioBitrateKbps) * 1000),
    };
    audioInput_ = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                     outputSettings:audioSettings];
    audioInput_.expectsMediaDataInRealTime = YES;
    if (![writer_ canAddInput:audioInput_]) {
      errorOut = "AVAssetWriter rejected the audio input";
      audioInput_ = nil;
      return false;
    }
    [writer_ addInput:audioInput_];
    audioConfigured_ = true;
    return true;
  }

  bool beginWriting(std::string& errorOut) {
    if (!writer_) {
      errorOut = "writer not open";
      return false;
    }
    if (![writer_ startWriting]) {
      errorOut = std::string("startWriting failed: ") +
                 (writer_.error ? writer_.error.localizedDescription.UTF8String : "unknown");
      return false;
    }
    [writer_ startSessionAtSourceTime:kCMTimeZero];
    writing_ = true;
    return true;
  }

  bool writeVideo(const uint8_t* bgra, int srcWidth, int srcHeight, int srcStride,
                  int64_t pts100ns, std::string& errorOut) {
    if (!writing_ || !adaptor_) {
      errorOut = "writer not writing";
      return false;
    }
    if (!videoInput_.readyForMoreMediaData) {
      return true;  // realtime input backpressure: drop this frame, not the session
    }
    CVPixelBufferRef buffer = nullptr;
    CVPixelBufferPoolRef pool = adaptor_.pixelBufferPool;
    if (!pool ||
        CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &buffer) != kCVReturnSuccess || !buffer) {
      errorOut = "pixel buffer pool exhausted";
      return false;
    }
    CVPixelBufferLockBaseAddress(buffer, 0);
    auto* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    const size_t dstStride = CVPixelBufferGetBytesPerRow(buffer);
    const int copyWidth = std::min(srcWidth, width_);
    const int copyHeight = std::min(srcHeight, height_);
    for (int y = 0; y < height_; ++y) {
      uint8_t* row = dst + static_cast<size_t>(y) * dstStride;
      if (y < copyHeight) {
        std::memcpy(row, bgra + static_cast<size_t>(y) * srcStride,
                    static_cast<size_t>(copyWidth) * 4u);
        if (copyWidth < width_) {
          std::memset(row + static_cast<size_t>(copyWidth) * 4u, 0,
                      static_cast<size_t>(width_ - copyWidth) * 4u);
        }
      } else {
        std::memset(row, 0, static_cast<size_t>(width_) * 4u);
      }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    const BOOL appended = [adaptor_ appendPixelBuffer:buffer withPresentationTime:ptsToTime(pts100ns)];
    CVPixelBufferRelease(buffer);
    if (!appended) {
      errorOut = std::string("appendPixelBuffer failed: ") +
                 (writer_.error ? writer_.error.localizedDescription.UTF8String : "unknown");
      return false;
    }
    ++videoFrameCount_;
    return true;
  }

  // NV12 frame into the biplanar pixel buffer (ISO Zoom sources). Crops /
  // fills to the configured dims like the MF writer: luma fill 0, chroma 128.
  bool writeVideoNv12(const uint8_t* nv12, int srcWidth, int srcHeight, int64_t pts100ns,
                      std::string& errorOut) {
    if (!writing_ || !adaptor_ || inputKind_ != VideoInput::Nv12) {
      errorOut = "writer not accepting NV12";
      return false;
    }
    if (!videoInput_.readyForMoreMediaData) {
      return true;
    }
    CVPixelBufferRef buffer = nullptr;
    CVPixelBufferPoolRef pool = adaptor_.pixelBufferPool;
    if (!pool ||
        CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &buffer) != kCVReturnSuccess || !buffer) {
      errorOut = "pixel buffer pool exhausted";
      return false;
    }
    CVPixelBufferLockBaseAddress(buffer, 0);
    const int copyWidth = std::min(srcWidth & ~1, width_);
    const int copyHeight = std::min(srcHeight & ~1, height_);
    auto* yDst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
    const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
    for (int y = 0; y < height_; ++y) {
      uint8_t* row = yDst + static_cast<size_t>(y) * yStride;
      if (y < copyHeight) {
        std::memcpy(row, nv12 + static_cast<size_t>(y) * srcWidth, static_cast<size_t>(copyWidth));
        if (copyWidth < width_) {
          std::memset(row + copyWidth, 0, static_cast<size_t>(width_ - copyWidth));
        }
      } else {
        std::memset(row, 0, static_cast<size_t>(width_));
      }
    }
    auto* uvDst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
    const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
    const uint8_t* uvSrc = nv12 + static_cast<size_t>(srcWidth) * srcHeight;
    for (int y = 0; y < height_ / 2; ++y) {
      uint8_t* row = uvDst + static_cast<size_t>(y) * uvStride;
      if (y < copyHeight / 2) {
        std::memcpy(row, uvSrc + static_cast<size_t>(y) * srcWidth, static_cast<size_t>(copyWidth));
        if (copyWidth < width_) {
          std::memset(row + copyWidth, 128, static_cast<size_t>(width_ - copyWidth));
        }
      } else {
        std::memset(row, 128, static_cast<size_t>(width_));
      }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    const BOOL appended = [adaptor_ appendPixelBuffer:buffer withPresentationTime:ptsToTime(pts100ns)];
    CVPixelBufferRelease(buffer);
    if (!appended) {
      errorOut = std::string("appendPixelBuffer (NV12) failed: ") +
                 (writer_.error ? writer_.error.localizedDescription.UTF8String : "unknown");
      return false;
    }
    ++videoFrameCount_;
    return true;
  }

  // Silence-fill for gapped ISO stems (0.1s chunks like the MF writer).
  bool writeAudioSilence(int64_t frames, int64_t ptsStart100ns, std::string& errorOut) {
    constexpr int kChunkFrames = 4800;
    std::vector<float> zeros;
    int64_t written = 0;
    while (written < frames) {
      const int chunk = static_cast<int>(std::min<int64_t>(kChunkFrames, frames - written));
      zeros.assign(static_cast<size_t>(chunk) * audioChannels_, 0.f);
      const int64_t pts = ptsStart100ns + written * kHundredNsPerSecond / audioSampleRate_;
      if (!writeAudio(zeros.data(), chunk, pts, errorOut)) {
        return false;
      }
      written += chunk;
    }
    return true;
  }

  bool writeAudio(const float* interleaved, int frameCount, int64_t pts100ns,
                  std::string& errorOut) {
    if (!writing_ || !audioConfigured_ || !audioInput_) {
      errorOut = "audio stream not configured";
      return false;
    }
    if (!audioInput_.readyForMoreMediaData) {
      return true;  // drop under backpressure; the async sink already budgets
    }
    const size_t sampleCount = static_cast<size_t>(frameCount) * audioChannels_;
    std::vector<int16_t> pcm(sampleCount);
    for (size_t i = 0; i < sampleCount; ++i) {
      const float clamped = std::clamp(interleaved[i], -1.f, 1.f);
      pcm[i] = static_cast<int16_t>(std::lround(clamped * 32767.f));
    }
    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate = audioSampleRate_;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket = static_cast<UInt32>(2 * audioChannels_);
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = static_cast<UInt32>(2 * audioChannels_);
    asbd.mChannelsPerFrame = static_cast<UInt32>(audioChannels_);
    asbd.mBitsPerChannel = 16;
    CMAudioFormatDescriptionRef format = nullptr;
    if (CMAudioFormatDescriptionCreate(nullptr, &asbd, 0, nullptr, 0, nullptr, nullptr, &format) !=
        noErr) {
      errorOut = "CMAudioFormatDescriptionCreate failed";
      return false;
    }
    CMBlockBufferRef block = nullptr;
    const size_t byteLen = pcm.size() * sizeof(int16_t);
    if (CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, byteLen, nullptr, nullptr, 0, byteLen,
                                           kCMBlockBufferAssureMemoryNowFlag, &block) != noErr) {
      CFRelease(format);
      errorOut = "CMBlockBufferCreate failed";
      return false;
    }
    CMBlockBufferReplaceDataBytes(pcm.data(), block, 0, byteLen);
    CMSampleBufferRef sample = nullptr;
    const CMSampleTimingInfo timing{CMTimeMake(1, audioSampleRate_), ptsToTime(pts100ns),
                                    kCMTimeInvalid};
    const OSStatus status = CMSampleBufferCreate(nullptr, block, true, nullptr, nullptr, format,
                                                 frameCount, 1, &timing, 0, nullptr, &sample);
    CFRelease(block);
    CFRelease(format);
    if (status != noErr || !sample) {
      errorOut = "CMSampleBufferCreate failed";
      return false;
    }
    const BOOL appended = [audioInput_ appendSampleBuffer:sample];
    CFRelease(sample);
    if (!appended) {
      errorOut = std::string("appendSampleBuffer failed: ") +
                 (writer_.error ? writer_.error.localizedDescription.UTF8String : "unknown");
      return false;
    }
    ++audioPacketCount_;
    audioSampleCount_ += frameCount;
    return true;
  }

  void finalize() {
    if (writer_ && writing_) {
      [videoInput_ markAsFinished];
      if (audioInput_) {
        [audioInput_ markAsFinished];
      }
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      [writer_ finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(done);
      }];
      dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
      std::error_code ec;
      const auto size = std::filesystem::file_size(path_, ec);
      bytesWritten_ = ec ? 0 : static_cast<int64_t>(size);
    }
    writer_ = nil;
    videoInput_ = nil;
    audioInput_ = nil;
    adaptor_ = nil;
    writing_ = false;
    audioConfigured_ = false;
  }

  bool audioConfigured() const { return audioConfigured_; }
  int64_t videoFrameCount() const { return videoFrameCount_; }
  int64_t audioPacketCount() const { return audioPacketCount_; }
  int64_t audioSampleCount() const { return audioSampleCount_; }
  int64_t bytesWritten() const { return bytesWritten_; }
  int width() const { return width_; }
  int height() const { return height_; }

 private:
  AVAssetWriter* writer_ = nil;
  AVAssetWriterInput* videoInput_ = nil;
  AVAssetWriterInput* audioInput_ = nil;
  AVAssetWriterInputPixelBufferAdaptor* adaptor_ = nil;
  std::string path_;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 30;
  int audioChannels_ = 2;
  int audioSampleRate_ = 48000;
  VideoInput inputKind_ = VideoInput::Bgra;
  bool writing_ = false;
  bool audioConfigured_ = false;
  int64_t videoFrameCount_ = 0;
  int64_t audioPacketCount_ = 0;
  int64_t audioSampleCount_ = 0;
  int64_t bytesWritten_ = 0;
};

class AVFoundationEncoderSink final : public IEncoderSink {
 public:
  AVFoundationEncoderSink() : origin_(std::chrono::steady_clock::now()) {
    session_.encoderName = "videotoolbox";
    session_.codec = "h264";
    session_.hardwareAccelerated = true;
  }

  ~AVFoundationEncoderSink() override { writer_.finalize(); }

  void configureRecording(const RecordingSessionRequest& request) override { request_ = request; }

  OutputSession start(const std::vector<std::string>& destinations,
                      const std::vector<std::string>& isoParticipantIds) override {
    writer_.finalize();
    session_ = OutputSession{};
    session_.encoderName = "videotoolbox";
    session_.codec = "h264";
    session_.hardwareAccelerated = true;
    session_.active = true;
    session_.destinations = destinations;
    session_.isoParticipantIds = isoParticipantIds;
    recordingArmed_ = std::find(destinations.begin(), destinations.end(), "recording") !=
                      destinations.end();
    clock_.reset();
    isoWriters_.clear();
    if (recordingArmed_) {
      openRecordingWriter();
    }
    return session_;
  }

  void submitIsoVideo(const std::vector<IsoSourceVideoFrame>& sources) override {
    if (!recordingArmed_ || session_.recordingStatus == "failed") {
      return;
    }
    for (const auto& source : sources) {
      auto it = isoWriters_.find(source.sourceId);
      if (it == isoWriters_.end() || it->second.failed) {
        continue;
      }
      auto& entry = it->second;
      const auto& frame = source.frame;
      const bool isI420 = frame.hasI420();
      if (!isI420 && !frame.hasPixels()) {
        continue;
      }
      const auto pts = clock_.videoPtsForSource(now100ns(), source.sourceId, frame.frameId);
      if (!pts) {
        continue;  // same frame resubmitted this tick
      }
      std::string error;
      if (!entry.opened) {
        // Lazy open at the source's FIRST frame, sized to its native dims.
        const int width = isI420 ? frame.i420Width : frame.pixelWidth;
        const int height = isI420 ? frame.i420Height : frame.pixelHeight;
        if (!entry.writer->open(entry.status.path, width, height, request_.fps,
                                request_.targetBitrateMbps, request_.videoCodec, error,
                                isI420 ? VideoInput::Nv12 : VideoInput::Bgra) ||
            (entry.hasAudio &&
             !entry.writer->ensureAudioStream(2, 48000, request_.audioBitrateKbps, error)) ||
            !entry.writer->beginWriting(error)) {
          entry.failed = true;
          entry.status.warning = "ISO writer open failed: " + error;
          raiseWarning("ISO " + entry.status.displayName + ": " + entry.status.warning);
          continue;
        }
        entry.opened = true;
        entry.status.trackOpen = true;
      }
      bool ok = false;
      if (isI420) {
        i420ToNv12(frame.i420->data(), frame.i420Width & ~1, frame.i420Height & ~1,
                   entry.nv12Scratch);
        ok = entry.writer->writeVideoNv12(entry.nv12Scratch.data(), frame.i420Width & ~1,
                                          frame.i420Height & ~1, *pts, error);
      } else {
        ok = entry.writer->writeVideo(frame.pixels->data(), frame.pixelWidth, frame.pixelHeight,
                                      frame.pixelStride, *pts, error);
      }
      if (!ok) {
        entry.status.warning = "ISO video write failed: " + error;
        raiseWarning("ISO " + entry.status.displayName + ": " + entry.status.warning);
      }
      entry.status.videoFrameCount = entry.writer->videoFrameCount();
      entry.status.bytesWritten = entry.writer->bytesWritten();
    }
    refreshIsoStreams();
  }

  void submitIsoAudio(const std::vector<IsoSourceAudio>& sources) override {
    if (!recordingArmed_ || session_.recordingStatus == "failed") {
      return;
    }
    for (const auto& source : sources) {
      auto it = isoWriters_.find(source.sourceId);
      if (it == isoWriters_.end() || it->second.failed || !it->second.opened) {
        continue;
      }
      auto& entry = it->second;
      if (!entry.writer->audioConfigured()) {
        continue;  // video-only ISO (hasAudio=false): no fabricated stem
      }
      const auto advance =
          clock_.isoAudioAdvance(now100ns(), source.sourceId, source.frameCount, source.sampleRate);
      std::string error;
      if (advance.silenceFrames > 0 &&
          !entry.writer->writeAudioSilence(advance.silenceFrames, advance.silencePts100ns, error)) {
        entry.status.warning = "ISO silence-fill failed: " + error;
        raiseWarning("ISO " + entry.status.displayName + ": " + entry.status.warning);
        continue;
      }
      if (source.frameCount > 0 && !source.pcm.empty()) {
        toStereo(source.pcm, source.channels, source.frameCount, entry.stereoScratch);
        if (!entry.writer->writeAudio(entry.stereoScratch.data(), source.frameCount,
                                      advance.realPts100ns, error)) {
          entry.status.warning = "ISO audio write failed: " + error;
          raiseWarning("ISO " + entry.status.displayName + ": " + entry.status.warning);
        }
      }
      entry.status.audioSampleCount = entry.writer->audioSampleCount();
    }
    refreshIsoStreams();
  }

  void submit(const ProgramFrame& frame) override {
    if (!recordingArmed_ || session_.recordingStatus == "failed") {
      return;
    }
    // Full-resolution program when the compositor provided it (the Metal path
    // during recording); the 320x180 preview only as a last resort.
    const ProgramFramePreviewPixels& src =
        !frame.programFullBgra.bgra.empty() ? frame.programFullBgra : frame.preview;
    if (src.bgra.empty() || src.width <= 0 || src.height <= 0) {
      return;
    }
    const auto pts = clock_.videoPts(now100ns(), frame.frameNumber);
    if (!pts) {
      return;  // duplicate frame
    }
    std::string error;
    if (!writer_.writeVideo(src.bgra.data(), src.width, src.height, src.width * 4, *pts, error)) {
      raiseWarning("program video write failed: " + error);
    }
    session_.recordingVideoFrameCount = writer_.videoFrameCount();
    session_.recordingLastFrameNumber = frame.frameNumber;
    session_.recordingBytesWritten = writer_.bytesWritten();
    session_.encodedFrameCount = writer_.videoFrameCount();
    const auto last = clock_.lastVideoPts100ns();
    session_.recordingDurationMs = last > 0 ? last / 10'000 : 0;
  }

  void submitAudio(const float* interleaved, int frameCount, int channels,
                   int sampleRate) override {
    if (!recordingArmed_ || session_.recordingStatus == "failed" || frameCount <= 0 ||
        channels <= 0) {
      return;
    }
    if (!writer_.audioConfigured()) {
      return;
    }
    clock_.setAudioContentLatency(audioContentLatencySamples_.load(std::memory_order_relaxed));
    const auto pts = clock_.audioPts(now100ns(), frameCount, sampleRate);
    std::string error;
    if (!writer_.writeAudio(interleaved, frameCount, pts, error)) {
      raiseWarning("program audio write failed: " + error);
      return;
    }
    session_.recordingAudioPacketCount = writer_.audioPacketCount();
    session_.recordingAudioSampleCount = writer_.audioSampleCount();
    session_.recordingAudioChannels = channels;
    session_.recordingAudioSampleRate = sampleRate;
  }

  void setAudioContentLatencySamples(int latencySamples) override {
    audioContentLatencySamples_.store(latencySamples, std::memory_order_relaxed);
  }

  void stopRecording() override {
    if (!recordingArmed_) {
      return;
    }
    writer_.finalize();
    // Each ISO finalizes independently — its own moov, no 0-byte tails.
    for (auto& [sourceId, entry] : isoWriters_) {
      if (entry.opened) {
        entry.writer->finalize();
        entry.status.bytesWritten = entry.writer->bytesWritten();
      }
    }
    refreshIsoStreams();
    session_.recordingBytesWritten = writer_.bytesWritten();
    if (session_.recordingStatus != "failed") {
      session_.recordingStatus = "stopped";
    }
    recordingArmed_ = false;
  }

  OutputSession session() const override { return session_; }

 private:
  void openRecordingWriter() {
    namespace fs = std::filesystem;
    fs::path dir = request_.targetFolder.empty() ? fs::temp_directory_path()
                                                 : fs::path(request_.targetFolder);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::exists(dir)) {
      failRecording("recording folder could not be created: " + dir.string());
      return;
    }
    const std::string prefix = request_.filenamePrefix.empty() ? "show" : request_.filenamePrefix;
    // ISO sessions get the per-session subfolder + manifest (spec §5, shared
    // scheme with the MF sink); program-only sessions keep the flat artifact.
    fs::path artifact;
    if (!request_.isoSources.empty()) {
      dir = dir / (prefix + "-" + sessionTimestampFolder());
      fs::create_directories(dir, ec);
      if (!fs::exists(dir)) {
        failRecording("recording session folder could not be created: " + dir.string());
        return;
      }
      artifact = dir / "Program.mp4";
      int index = 0;
      for (const auto& selection : request_.isoSources) {
        IsoWriterEntry entry;
        entry.writer = std::make_unique<AvfMp4Writer>();
        entry.hasAudio = selection.hasAudio;
        entry.status.sourceId = selection.sourceId;
        entry.status.displayName = selection.displayName;
        char number[8];
        std::snprintf(number, sizeof(number), "%02d", ++index);
        std::string name = sanitizeForFilename(selection.displayName, selection.sourceId);
        fs::path isoPath = dir / ("ISO-" + std::string(number) + "-" + name + ".mp4");
        entry.status.path = isoPath.string();
        isoWriters_.emplace(selection.sourceId, std::move(entry));
      }
      writeManifest(dir);
      session_.recordingSessionDir = dir.string();
    } else {
      artifact = dir / (prefix + "-program-0.mp4");
    }
    std::string error;
    const int width = request_.width > 0 ? request_.width : 1920;
    const int height = request_.height > 0 ? request_.height : 1080;
    if (!writer_.open(artifact.string(), width, height, request_.fps,
                      request_.targetBitrateMbps, request_.videoCodec, error) ||
        !writer_.ensureAudioStream(2, 48000, request_.audioBitrateKbps, error) ||
        !writer_.beginWriting(error)) {
      failRecording("recording writer open failed: " + error);
      return;
    }
    session_.recordingStatus = "recording";
    session_.recordingSessionId = request_.sessionId;
    session_.recordingTargetFolder = dir.string();
    session_.recordingFilenamePrefix = prefix;
    session_.recordingFormat = "mp4";
    session_.recordingArtifactPath = artifact.string();
    session_.recordingWidth = writer_.width();
    session_.recordingHeight = writer_.height();
    session_.recordingFps = request_.fps;
    session_.recordingContainerFormat = "mp4";
    session_.recordingVideoCodec = "h264";
    session_.recordingAudioCodec = "aac";
    session_.recordingAudioBitrateKbps = request_.audioBitrateKbps;
    session_.recordingMetadataValid = true;
  }

  void refreshIsoStreams() {
    session_.isoStreams.clear();
    session_.isoStreams.reserve(isoWriters_.size());
    for (const auto& [sourceId, entry] : isoWriters_) {
      session_.isoStreams.push_back(entry.status);
    }
  }

  void writeManifest(const std::filesystem::path& dir) {
    const auto path = dir / "manifest.json";
    std::string body = "{\"sessionId\":\"" + jsonEscape(request_.sessionId) +
                       "\",\"epochMs\":" +
                       std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count()) +
                       ",\"entries\":[";
    bool first = true;
    for (const auto& [sourceId, entry] : isoWriters_) {
      if (!first) {
        body += ",";
      }
      first = false;
      const auto filename = std::filesystem::path(entry.status.path).filename().string();
      body += "{\"sourceId\":\"" + jsonEscape(sourceId) + "\",\"name\":\"" +
              jsonEscape(entry.status.displayName) + "\",\"path\":\"" + jsonEscape(filename) +
              "\",\"kind\":\"iso\",\"hasAudio\":" + (entry.hasAudio ? "true" : "false") + "}";
    }
    body += "]}";
    if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
      std::fwrite(body.data(), 1, body.size(), f);
      std::fclose(f);
      session_.recordingManifestPath = path.string();
    } else {
      raiseWarning("manifest write failed: " + path.string());
    }
  }

  void failRecording(const std::string& why) {
    session_.recordingStatus = "failed";
    session_.recordingError = why;
    raiseWarning(why);
  }

  void raiseWarning(const std::string& warning) {
    session_.recordingWarning = warning;
    static std::atomic<int> warnBudget{8};
    if (warnBudget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      std::fprintf(stderr, "[avf-encoder] %s\n", warning.c_str());
    }
  }

  int64_t now100ns() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - origin_)
               .count() /
           100;
  }

  struct IsoWriterEntry {
    std::unique_ptr<AvfMp4Writer> writer;
    IsoStreamStatus status;
    bool hasAudio = false;
    bool opened = false;
    bool failed = false;
    std::vector<uint8_t> nv12Scratch;
    std::vector<float> stereoScratch;
  };

  RecordingSessionRequest request_;
  OutputSession session_;
  AvfMp4Writer writer_;
  std::map<std::string, IsoWriterEntry> isoWriters_;
  RecordingPtsClock clock_;
  std::chrono::steady_clock::time_point origin_;
  std::atomic<int> audioContentLatencySamples_{0};
  bool recordingArmed_ = false;
};

}  // namespace

std::unique_ptr<IEncoderSink> createAVFoundationEncoderSink() {
  return std::make_unique<AVFoundationEncoderSink>();
}

}  // namespace corevideo::modules

#else  // gate

#include <memory>

#include "modules/Interfaces.h"

namespace corevideo::modules {

std::unique_ptr<IEncoderSink> createAVFoundationEncoderSink() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AVF_ENCODER
