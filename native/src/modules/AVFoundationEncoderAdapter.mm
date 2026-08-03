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
#include <memory>
#include <string>
#include <vector>

#include "modules/Interfaces.h"
#include "modules/RecordingPtsClock.h"

namespace corevideo::modules {
namespace {

constexpr int64_t kHundredNsPerSecond = 10'000'000;

CMTime ptsToTime(int64_t pts100ns) {
  return CMTimeMake(pts100ns, static_cast<int32_t>(kHundredNsPerSecond));
}

class AvfMp4Writer {
 public:
  ~AvfMp4Writer() { finalize(); }

  bool open(const std::string& path, int width, int height, int fps, double bitrateMbps,
            const std::string& codec, std::string& errorOut) {
    (void)codec;  // increment 1 is H.264-only, matching the product default
    finalize();
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
    NSDictionary* adaptorAttrs = @{
      (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
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
    if (recordingArmed_) {
      openRecordingWriter();
    }
    return session_;
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
    const fs::path artifact = dir / (prefix + "-program-0.mp4");
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

  RecordingSessionRequest request_;
  OutputSession session_;
  AvfMp4Writer writer_;
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
