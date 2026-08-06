#include "modules/Interfaces.h"
#include "modules/RecordingPtsClock.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace corevideo::modules {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kDefaultWidth = 1920;
constexpr int kDefaultHeight = 1080;
constexpr int kDefaultFps = 30;

std::string hresultString(HRESULT result) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
  return stream.str();
}

// Map a recording-command video codec name to an MF output subtype. Anything we
// do not explicitly recognize falls back to H.264, which is the guaranteed-
// compatible MP4 baseline.
GUID videoSubtypeForCodec(const std::string& codec) {
  if (codec == "hevc" || codec == "h265" || codec == "hvc1") {
    return MFVideoFormat_HEVC;
  }
  return MFVideoFormat_H264;
}

std::string canonicalVideoCodecName(const std::string& codec) {
  if (codec == "hevc" || codec == "h265" || codec == "hvc1") {
    return "hevc";
  }
  return "h264";
}

// The input pixel format a writer accepts. Program keeps BGRA (RGB32); ISO Zoom
// sources feed NV12 so raw I420 needs no CPU color-convert (spec §2b) — a cheap
// plane interleave (below) runs on the async encoder thread, never a lock.
enum class VideoInput { Bgra, Nv12 };

// Cheap I420 (planar Y/U/V) -> NV12 (Y plane + interleaved UV) conversion. No
// color-space math, just a chroma-plane interleave; runs on the async encoder
// writer thread (off coreMutex / the audio worker). `w`/`h` must be even (Zoom
// I420 always is — hasI420() guarantees it). `out` is resized to w*h*3/2.
void i420ToNv12(const uint8_t* i420, int w, int h, std::vector<uint8_t>& out) {
  const size_t ySize = static_cast<size_t>(w) * static_cast<size_t>(h);
  const size_t chromaW = static_cast<size_t>(w) / 2;
  const size_t chromaH = static_cast<size_t>(h) / 2;
  const size_t uSize = chromaW * chromaH;
  out.resize(ySize + uSize * 2);
  std::memcpy(out.data(), i420, ySize);
  const uint8_t* u = i420 + ySize;
  const uint8_t* v = i420 + ySize + uSize;
  uint8_t* uv = out.data() + ySize;
  for (size_t i = 0; i < uSize; ++i) {
    uv[2 * i] = u[i];
    uv[2 * i + 1] = v[i];
  }
}

// ISO-2: normalize a raw-stem PCM buffer to interleaved STEREO (the ISO writers'
// uniform AAC layout). Mono → duplicated L/R; stereo → passed through; >2ch →
// first two channels. `out` is resized to frameCount*2. Runs on the async
// encoder writer thread (off every lock).
void toStereo(const std::vector<float>& in, int channels, int frameCount, std::vector<float>& out) {
  out.resize(static_cast<size_t>(frameCount) * 2);
  const int ch = channels > 0 ? channels : 1;
  const size_t have = in.size();
  for (int i = 0; i < frameCount; ++i) {
    const size_t base = static_cast<size_t>(i) * ch;
    float l = 0.0f;
    float r = 0.0f;
    if (base < have) {
      l = in[base];
      r = ch >= 2 && base + 1 < have ? in[base + 1] : l;  // mono → both channels
    }
    out[static_cast<size_t>(i) * 2] = l;
    out[static_cast<size_t>(i) * 2 + 1] = r;
  }
}

// Sanitize a roster/display name into a safe MP4 file-name fragment (spec §5):
// keep alphanumerics, space→'-', dash/underscore; drop everything else; collapse
// runs; trim; cap length; fall back to a stable token so a file is always named.
std::string sanitizeForFilename(const std::string& name, const std::string& fallback) {
  std::string out;
  out.reserve(name.size());
  bool lastDash = false;
  for (char c : name) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (alnum || c == '_') {
      out.push_back(c);
      lastDash = false;
    } else if (c == ' ' || c == '-' || c == '.') {
      if (!out.empty() && !lastDash) {
        out.push_back('-');
        lastDash = true;
      }
    }
    // all other characters (incl. path separators, control chars) are dropped
    if (out.size() >= 48) break;
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  if (out.empty()) {
    // Derive a token from the fallback id (strip scheme + unsafe chars).
    for (char c : fallback) {
      const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
      if (alnum) out.push_back(c);
      if (out.size() >= 32) break;
    }
  }
  if (out.empty()) out = "source";
  return out;
}

// Per-session subfolder timestamp `yyyymmdd-hhmmss` (local time).
std::string sessionTimestampFolder() {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tmBuf{};
#if defined(_WIN32)
  localtime_s(&tmBuf, &now);
#else
  localtime_r(&now, &tmBuf);
#endif
  char out[32] = {0};
  std::strftime(out, sizeof(out), "%Y%m%d-%H%M%S", &tmBuf);
  return std::string(out);
}

std::string jsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

// A single MP4 sink writer (program or one ISO participant). Owns its own video
// + audio stream indices and per-stream timing, so program and ISO writers stay
// independent and individually finalizable.
class Mp4Writer {
 public:
  bool open(const std::filesystem::path& path, int width, int height, int fps, int bitrateMbps,
            const std::string& codec, std::string& errorOut, VideoInput videoInput = VideoInput::Bgra) {
    // RESET ALL per-session state before configuring the new sink writer. This
    // writer object is REUSED across recording sessions (every encoder start()
    // reopens it), and stream indices / audioConfigured_ describe the PREVIOUS
    // IMFSinkWriter. A stale audioConfigured_==true made ensureAudioStream skip
    // AddStream on the new writer, so every audio WriteSample hit the missing
    // stream index and failed with MF_E_INVALIDSTREAMNUMBER (0xC00D36B3) — a
    // video-only MP4 while the master bus carried live program audio (the
    // 2026-07-13 alpha-blocking zero-audio-recording bug: start-program-output
    // opened generation 1, start-recording-session reopened generation 2).
    sinkWriter_.Reset();
    open_ = false;
    writing_ = false;
    audioConfigured_ = false;
    videoStreamIndex_ = 0;
    audioStreamIndex_ = 0;
    videoFrameCount_ = 0;
    audioPacketCount_ = 0;
    audioSampleCount_ = 0;
    audioWriteFailureCount_ = 0;
    bytesWritten_ = 0;
    path_ = path;
    videoInput_ = videoInput;
    // NV12 requires even luma dimensions; floor to even (Zoom I420 is already even).
    width_ = videoInput == VideoInput::Nv12 ? std::max(2, width & ~1) : std::max(2, width);
    height_ = videoInput == VideoInput::Nv12 ? std::max(2, height & ~1) : std::max(2, height);
    fps_ = std::max(1, fps);
    frameDuration100ns_ = 10'000'000LL / fps_;
    videoCodec_ = canonicalVideoCodecName(codec);

    ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(&attributes, 1);
    if (FAILED(result)) {
      errorOut = "create Sink Writer attributes: " + hresultString(result);
      return false;
    }
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    result = MFCreateSinkWriterFromURL(path_.wstring().c_str(), nullptr, attributes.Get(), &sinkWriter_);
    if (FAILED(result)) {
      errorOut = "create MP4 Sink Writer: " + hresultString(result);
      return false;
    }

    // ---- Video stream ----
    ComPtr<IMFMediaType> outputType;
    result = MFCreateMediaType(&outputType);
    if (FAILED(result)) {
      errorOut = "create video output type: " + hresultString(result);
      return false;
    }
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, videoSubtypeForCodec(codec));
    outputType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(std::max(1, bitrateMbps) * 1'000'000));
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width_), static_cast<UINT32>(height_));
    MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps_), 1);
    MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    result = sinkWriter_->AddStream(outputType.Get(), &videoStreamIndex_);
    if (FAILED(result)) {
      errorOut = "add video stream: " + hresultString(result);
      return false;
    }

    ComPtr<IMFMediaType> inputType;
    result = MFCreateMediaType(&inputType);
    if (FAILED(result)) {
      errorOut = "create video input type: " + hresultString(result);
      return false;
    }
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (videoInput_ == VideoInput::Nv12) {
      // NV12: top-down (positive stride = luma width), Y plane then interleaved
      // UV. Zoom ISO frames arrive I420 and are plane-interleaved to NV12 on the
      // async encoder thread — no CPU color-convert (spec §2b).
      inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
      inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width_));
    } else {
      inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
      // Top-down BGRA: negative stride so row 0 is the top scanline, matching the
      // ProgramFrame preview/shared-texture layout.
      inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(-(width_ * 4)));
    }
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width_), static_cast<UINT32>(height_));
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps_), 1);
    MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    result = sinkWriter_->SetInputMediaType(videoStreamIndex_, inputType.Get(), nullptr);
    if (FAILED(result)) {
      errorOut = "set video input type: " + hresultString(result);
      return false;
    }

    open_ = true;
    return true;
  }

  // Lazily configures the AAC audio stream the first time real audio arrives.
  // BeginWriting is deferred until both streams are configured, so the audio
  // stream can still be added after the first video frames.
  bool ensureAudioStream(int channels, int sampleRate, int audioBitrateKbps, std::string& errorOut) {
    if (!open_ || audioConfigured_) {
      return audioConfigured_;
    }
    audioChannels_ = std::clamp(channels, 1, 2);
    audioSampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    audioBitrateKbps_ = std::clamp(audioBitrateKbps, 32, 512);

    ComPtr<IMFMediaType> outputType;
    HRESULT result = MFCreateMediaType(&outputType);
    if (FAILED(result)) {
      errorOut = "create AAC output type: " + hresultString(result);
      return false;
    }
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(audioSampleRate_));
    outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(audioChannels_));
    outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>((audioBitrateKbps_ * 1000) / 8));
    outputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

    result = sinkWriter_->AddStream(outputType.Get(), &audioStreamIndex_);
    if (FAILED(result)) {
      errorOut = "add AAC stream: " + hresultString(result);
      return false;
    }

    ComPtr<IMFMediaType> inputType;
    result = MFCreateMediaType(&inputType);
    if (FAILED(result)) {
      errorOut = "create PCM input type: " + hresultString(result);
      return false;
    }
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(audioSampleRate_));
    inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(audioChannels_));
    inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(audioChannels_ * 2));
    inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                         static_cast<UINT32>(audioSampleRate_ * audioChannels_ * 2));

    result = sinkWriter_->SetInputMediaType(audioStreamIndex_, inputType.Get(), nullptr);
    if (FAILED(result)) {
      errorOut = "set PCM input type: " + hresultString(result);
      return false;
    }

    audioConfigured_ = true;
    return true;
  }

  bool beginWriting(std::string& errorOut) {
    if (!open_ || writing_) {
      return open_;
    }
    const HRESULT result = sinkWriter_->BeginWriting();
    if (FAILED(result)) {
      errorOut = "begin MP4 writing: " + hresultString(result);
      return false;
    }
    writing_ = true;
    return true;
  }

  // Writes one BGRA frame. `bgra` is tightly packed top-down at `srcStride`
  // bytes per row; pixels outside the configured size are letterbox/cropped to
  // fit so a resolution mismatch never corrupts the stream.
  bool writeVideo(const uint8_t* bgra, int srcWidth, int srcHeight, int srcStride, LONGLONG pts100ns,
                  std::string& errorOut) {
    if (!writing_ || bgra == nullptr) {
      return false;
    }
    const int dstStride = width_ * 4;
    const DWORD byteCount = static_cast<DWORD>(dstStride) * static_cast<DWORD>(height_);

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = MFCreateMemoryBuffer(byteCount, &buffer);
    if (FAILED(result)) {
      errorOut = "allocate video buffer: " + hresultString(result);
      return false;
    }
    BYTE* locked = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    result = buffer->Lock(&locked, &maxLength, &currentLength);
    if (FAILED(result)) {
      errorOut = "lock video buffer: " + hresultString(result);
      return false;
    }
    const int copyWidth = std::min(srcWidth, width_) * 4;
    const int copyRows = std::min(srcHeight, height_);
    for (int y = 0; y < height_; ++y) {
      BYTE* dstRow = locked + static_cast<size_t>(y) * dstStride;
      if (y < copyRows) {
        const uint8_t* srcRow = bgra + static_cast<size_t>(y) * srcStride;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(copyWidth));
        if (copyWidth < dstStride) {
          std::memset(dstRow + copyWidth, 0, static_cast<size_t>(dstStride - copyWidth));
        }
      } else {
        std::memset(dstRow, 0, static_cast<size_t>(dstStride));
      }
    }
    buffer->Unlock();
    buffer->SetCurrentLength(byteCount);

    ComPtr<IMFSample> sample;
    result = MFCreateSample(&sample);
    if (FAILED(result)) {
      errorOut = "create video sample: " + hresultString(result);
      return false;
    }
    sample->AddBuffer(buffer.Get());
    // Shared-epoch wall-clock PTS from RecordingPtsClock (spec 4.3): frames
    // carry the time they were actually composed, so the video timeline stays
    // aligned with the sample-counted audio track instead of drifting at
    // (muxed fps / nominal fps) of real time. Nominal duration is fine — the
    // container's frame timing comes from the PTS deltas (VFR MP4).
    sample->SetSampleTime(pts100ns);
    sample->SetSampleDuration(frameDuration100ns_);

    result = sinkWriter_->WriteSample(videoStreamIndex_, sample.Get());
    if (FAILED(result)) {
      errorOut = "write video sample: " + hresultString(result);
      return false;
    }
    ++videoFrameCount_;
    bytesWritten_ += byteCount;
    return true;
  }

  // Writes one NV12 frame (Y plane then interleaved UV). `nv12` is tightly packed
  // top-down at the SOURCE size; pixels outside the writer's configured size are
  // cropped and any shortfall is filled black (Y=0) / neutral chroma (UV=128) so
  // a resolution change never corrupts the stream. Used by the Zoom ISO writers.
  bool writeVideoNv12(const uint8_t* nv12, int srcWidth, int srcHeight, LONGLONG pts100ns,
                      std::string& errorOut) {
    if (!writing_ || nv12 == nullptr) {
      return false;
    }
    const int dstY = width_ * height_;
    const int dstUv = width_ * (height_ / 2);
    const DWORD byteCount = static_cast<DWORD>(dstY + dstUv);

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = MFCreateMemoryBuffer(byteCount, &buffer);
    if (FAILED(result)) {
      errorOut = "allocate NV12 buffer: " + hresultString(result);
      return false;
    }
    BYTE* locked = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    result = buffer->Lock(&locked, &maxLength, &currentLength);
    if (FAILED(result)) {
      errorOut = "lock NV12 buffer: " + hresultString(result);
      return false;
    }
    const int srcStrideY = srcWidth;
    const int srcStrideUv = srcWidth;  // one interleaved UV row = srcWidth bytes
    const int copyWidth = std::min(srcWidth, width_);
    const int copyRows = std::min(srcHeight, height_);
    // Luma plane.
    for (int y = 0; y < height_; ++y) {
      BYTE* dstRow = locked + static_cast<size_t>(y) * width_;
      if (y < copyRows) {
        std::memcpy(dstRow, nv12 + static_cast<size_t>(y) * srcStrideY, static_cast<size_t>(copyWidth));
        if (copyWidth < width_) std::memset(dstRow + copyWidth, 0, static_cast<size_t>(width_ - copyWidth));
      } else {
        std::memset(dstRow, 0, static_cast<size_t>(width_));
      }
    }
    // Interleaved UV plane (half-height). Round copyWidth up to an even byte count
    // so a UV pair is never split.
    const uint8_t* srcUv = nv12 + static_cast<size_t>(srcStrideY) * srcHeight;
    const int copyUvWidth = std::min(srcWidth, width_) & ~1;
    const int copyUvRows = std::min(srcHeight, height_) / 2;
    for (int y = 0; y < height_ / 2; ++y) {
      BYTE* dstRow = locked + dstY + static_cast<size_t>(y) * width_;
      if (y < copyUvRows) {
        std::memcpy(dstRow, srcUv + static_cast<size_t>(y) * srcStrideUv, static_cast<size_t>(copyUvWidth));
        if (copyUvWidth < width_) std::memset(dstRow + copyUvWidth, 128, static_cast<size_t>(width_ - copyUvWidth));
      } else {
        std::memset(dstRow, 128, static_cast<size_t>(width_));
      }
    }
    buffer->Unlock();
    buffer->SetCurrentLength(byteCount);

    ComPtr<IMFSample> sample;
    result = MFCreateSample(&sample);
    if (FAILED(result)) {
      errorOut = "create NV12 sample: " + hresultString(result);
      return false;
    }
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(pts100ns);
    sample->SetSampleDuration(frameDuration100ns_);

    result = sinkWriter_->WriteSample(videoStreamIndex_, sample.Get());
    if (FAILED(result)) {
      errorOut = "write NV12 sample: " + hresultString(result);
      return false;
    }
    ++videoFrameCount_;
    bytesWritten_ += byteCount;
    return true;
  }

  // Converts interleaved float PCM [-1,1] to 16-bit and writes one audio sample.
  bool writeAudio(const float* interleaved, int frameCount, LONGLONG pts100ns, std::string& errorOut) {
    if (!writing_ || !audioConfigured_ || interleaved == nullptr || frameCount <= 0) {
      return false;
    }
    pcm16_.resize(static_cast<size_t>(frameCount) * audioChannels_);
    for (size_t i = 0; i < pcm16_.size(); ++i) {
      const float clamped = std::clamp(interleaved[i], -1.0f, 1.0f);
      pcm16_[i] = static_cast<int16_t>(std::lround(clamped * 32767.0f));
    }
    const DWORD byteCount = static_cast<DWORD>(pcm16_.size() * sizeof(int16_t));

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = MFCreateMemoryBuffer(byteCount, &buffer);
    if (FAILED(result)) {
      errorOut = "allocate audio buffer: " + hresultString(result);
      return false;
    }
    BYTE* locked = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    result = buffer->Lock(&locked, &maxLength, &currentLength);
    if (FAILED(result)) {
      errorOut = "lock audio buffer: " + hresultString(result);
      return false;
    }
    std::memcpy(locked, pcm16_.data(), byteCount);
    buffer->Unlock();
    buffer->SetCurrentLength(byteCount);

    const LONGLONG duration100ns =
        static_cast<LONGLONG>(frameCount) * 10'000'000LL / std::max(1, audioSampleRate_);
    ComPtr<IMFSample> sample;
    result = MFCreateSample(&sample);
    if (FAILED(result)) {
      errorOut = "create audio sample: " + hresultString(result);
      return false;
    }
    sample->AddBuffer(buffer.Get());
    // Sample-counted PTS with a shared-epoch base offset (RecordingPtsClock):
    // gapless within the track, aligned to the video timeline at its start.
    sample->SetSampleTime(pts100ns);
    sample->SetSampleDuration(duration100ns);

    result = sinkWriter_->WriteSample(audioStreamIndex_, sample.Get());
    if (FAILED(result)) {
      // LOUD failure (rate-limited): a recording quietly muxing zero audio is
      // an alpha-blocking failure mode — log the first drop and every 250th
      // (~every 5s at the 50Hz worker cadence) so it can never hide again.
      ++audioWriteFailureCount_;
      if (audioWriteFailureCount_ == 1 || audioWriteFailureCount_ % 250 == 0) {
        std::fprintf(stderr,
                     "[recording] program audio WriteSample FAILED hr=%s stream=%lu (failure %lld) file=%s\n",
                     hresultString(result).c_str(), static_cast<unsigned long>(audioStreamIndex_),
                     static_cast<long long>(audioWriteFailureCount_), path_.string().c_str());
      }
      errorOut = "write audio sample: " + hresultString(result);
      return false;
    }
    ++audioPacketCount_;
    audioSampleCount_ += frameCount;
    bytesWritten_ += byteCount;
    return true;
  }

  // ISO-2: write `frames` sample-frames of digital silence starting at
  // `ptsStart100ns`, advancing the AAC sample clock so a gated ISO stem stays
  // time-aligned to program (spec §2c). Chunked so a long leading gap (a guest
  // who talks minutes in) never allocates or emits one giant sample. Returns
  // false on the first WriteSample failure (surfaced as an ISO warning by the
  // caller — a stem that silently loses its track is the #286 class).
  bool writeAudioSilence(int64_t frames, LONGLONG ptsStart100ns, std::string& errorOut) {
    if (!writing_ || !audioConfigured_ || frames <= 0) {
      return true;  // nothing to do (not a failure)
    }
    constexpr int64_t kChunkFrames = 4800;  // 0.1s at 48kHz — bounded sample size
    const int64_t rate = std::max(1, audioSampleRate_);
    std::vector<float> zeros(static_cast<size_t>(std::min<int64_t>(frames, kChunkFrames)) *
                                 static_cast<size_t>(audioChannels_),
                             0.0f);
    int64_t remaining = frames;
    int64_t emitted = 0;
    while (remaining > 0) {
      const int chunk = static_cast<int>(std::min<int64_t>(remaining, kChunkFrames));
      const LONGLONG pts = ptsStart100ns + emitted * 10'000'000LL / rate;
      if (!writeAudio(zeros.data(), chunk, pts, errorOut)) {
        return false;
      }
      remaining -= chunk;
      emitted += chunk;
    }
    return true;
  }

  void finalize() {
    if (writing_ && sinkWriter_) {
      sinkWriter_->Finalize();
    }
    writing_ = false;
    open_ = false;
    sinkWriter_.Reset();
    // Prefer the on-disk size once the container is finalized.
    if (!path_.empty() && std::filesystem::exists(path_)) {
      std::error_code error;
      const auto size = std::filesystem::file_size(path_, error);
      if (!error) {
        bytesWritten_ = static_cast<int64_t>(size);
      }
    }
  }

  bool writing() const { return writing_; }
  bool audioConfigured() const { return audioConfigured_; }
  int width() const { return width_; }
  int height() const { return height_; }
  int fps() const { return fps_; }
  const std::string& videoCodec() const { return videoCodec_; }
  int64_t videoFrameCount() const { return videoFrameCount_; }
  int64_t audioPacketCount() const { return audioPacketCount_; }
  int64_t audioSampleCount() const { return audioSampleCount_; }
  int audioChannels() const { return audioChannels_; }
  int audioSampleRate() const { return audioSampleRate_; }
  int audioBitrateKbps() const { return audioBitrateKbps_; }
  int64_t bytesWritten() const { return bytesWritten_; }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
  ComPtr<IMFSinkWriter> sinkWriter_;
  DWORD videoStreamIndex_ = 0;
  DWORD audioStreamIndex_ = 0;
  int width_ = kDefaultWidth;
  int height_ = kDefaultHeight;
  int fps_ = kDefaultFps;
  LONGLONG frameDuration100ns_ = 10'000'000LL / kDefaultFps;
  int audioChannels_ = 2;
  int audioSampleRate_ = 48000;
  int audioBitrateKbps_ = 192;
  std::string videoCodec_ = "h264";
  int64_t videoFrameCount_ = 0;
  int64_t audioPacketCount_ = 0;
  int64_t audioSampleCount_ = 0;
  int64_t audioWriteFailureCount_ = 0;
  int64_t bytesWritten_ = 0;
  bool open_ = false;
  bool writing_ = false;
  bool audioConfigured_ = false;
  VideoInput videoInput_ = VideoInput::Bgra;
  std::vector<int16_t> pcm16_;
};

class MediaFoundationEncoderSink final : public IEncoderSink {
 public:
  MediaFoundationEncoderSink() {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comResult)) {
      comInitialized_ = true;
    } else if (comResult != RPC_E_CHANGED_MODE) {
      session_.recordingWarning = "Media Foundation COM initialization failed with " + hresultString(comResult) + ".";
    }
    session_.encoderName = "media-foundation";
    session_.codec = "h264";
    session_.targetBitrateMbps = 18;
    session_.hardwareAccelerated = true;
  }

  ~MediaFoundationEncoderSink() override {
    closeWriters();
    MFShutdown();
    if (comInitialized_) {
      CoUninitialize();
    }
  }

  void configureRecording(const RecordingSessionRequest& request) override {
    request_ = request;
    session_.recordingSessionId = request.sessionId;
    session_.recordingTargetFolder = request.targetFolder;
    session_.recordingFilenamePrefix = request.filenamePrefix;
    session_.recordingFormat = request.format;
    session_.recordingQuality = request.quality;
    session_.recordingContainerFormat = request.format;
    session_.recordingVideoCodec = request.videoCodec;
    session_.recordingAudioCodec = request.audioCodec;
    session_.recordingAudioBitrateKbps = request.audioBitrateKbps;
    session_.recordingFps = request.fps;
    session_.targetBitrateMbps = request.targetBitrateMbps;
    if (!request.isoParticipantIds.empty()) {
      session_.isoParticipantIds = request.isoParticipantIds;
    }
  }

  OutputSession start(const std::vector<std::string>& destinations, const std::vector<std::string>& isoParticipantIds) override {
    closeWriters();
    session_.active = true;
    // Fresh session: re-decide full-res vs preview from this session's frames.
    fullResLocked_ = false;
    nv12MismatchWarned_ = false;
    session_.destinations = destinations;
    session_.isoParticipantIds = isoParticipantIds.empty() ? request_.isoParticipantIds : isoParticipantIds;
    session_.encodedFrameCount = 0;
    session_.recordingArtifactPath.clear();
    session_.recordingSessionDir.clear();
    session_.recordingManifestPath.clear();
    session_.isoStreams.clear();
    session_.recordingBytesWritten = 0;
    session_.recordingDurationMs = 0;
    session_.recordingVideoFrameCount = 0;
    session_.recordingLastFrameNumber = 0;
    session_.recordingWidth = 0;
    session_.recordingHeight = 0;
    session_.recordingFps = 0;
    session_.recordingContainerFormat.clear();
    session_.recordingVideoCodec.clear();
    session_.recordingAudioCodec.clear();
    session_.recordingAudioBitrateKbps = request_.audioBitrateKbps;
    session_.recordingAudioPacketCount = 0;
    session_.recordingAudioSampleCount = 0;
    session_.recordingAudioChannels = 0;
    session_.recordingAudioSampleRate = 0;
    session_.recordingMetadataValid = false;
    session_.recordingWarning.clear();
    session_.recordingError.clear();
    session_.recordingSessionId = request_.sessionId;
    session_.recordingTargetFolder = request_.targetFolder;
    session_.recordingFilenamePrefix = request_.filenamePrefix;
    session_.recordingFormat = request_.format;
    session_.recordingQuality = request_.quality;
    session_.recordingStatus = "encoding";
    if (std::find(destinations.begin(), destinations.end(), "recording") != destinations.end()) {
      session_.recordingStatus = "recording";
      openRecordingWriters();
    }
    return session_;
  }

  void submit(const ProgramFrame& frame) override {
    if (!session_.active) {
      return;
    }
    ++session_.encodedFrameCount;
    session_.recordingLastFrameNumber = frame.frameNumber;
    if (!program_.writing()) {
      return;
    }

    // Feed the REAL composed program pixels — the FULL-RESOLUTION readback, not
    // `preview`, which is a 320x180 UI thumbnail. Writing the thumbnail into a
    // writer opened at program dimensions is what put the whole show into a
    // small corner of an otherwise black frame (rig-verified against a July 13
    // recording: 8995 frames at a flat luma of 4). The virtual camera and RTMP
    // already prefer the full-res buffer; recording was the last consumer still
    // reading the thumbnail.
    //
    // The full-res tap is ASYNCHRONOUS — it publishes on the ticks where its
    // readback completed and is EMPTY on the rest — so once full-res has been
    // seen, NEVER downgrade: a mid-file geometry flip is the same defect again.
    // Skip the tick instead; the PTS clock timestamps each frame at the wall
    // time it was submitted, so a skipped tick costs one frame, not sync.
    //
    // WINDOWS takes the NV12 branch: `programFullBgra` is populated only by the
    // macOS Metal compositor, so on Windows the full-resolution program exists
    // solely as NV12 (produced by MediaCore via compositor->takeVcamNv12, the
    // same tap the virtual camera and RTMP consume). The writer was opened with
    // a matching NV12 media type when the request asked for it.
    if (programWritesNv12_) {
      if (frame.programNv12.empty() || frame.programNv12Width <= 0 || frame.programNv12Height <= 0) {
        return;  // async tap had nothing this tick — skip, never downgrade
      }
      const auto pts = recordingClock_.videoPts(now100ns(), frame.frameNumber);
      if (!pts) {
        return;  // already muxed
      }
      std::string nv12Error;
      if (!program_.writeVideoNv12(frame.programNv12.data(), frame.programNv12Width,
                                   frame.programNv12Height, *pts, nv12Error)) {
        setRecordingFailure("Media Foundation could not write program video", nv12Error);
        return;
      }
      ++session_.recordingVideoFrameCount;
      session_.recordingDurationMs = recordingClock_.lastVideoPts100ns() / 10'000;
      updateBytesWritten();
      return;
    }

    // LOUD, not silently wrong: the full-resolution NV12 tap exists on this frame,
    // yet we are about to mux BGRA. That happens when the recording size does not
    // match the tap (MediaCore only routes NV12 at an exact 1080p match, because
    // the tap is a pinned 1080p scale-blit and feeding a 4K writer would letterbox
    // the show). The result is the 320x180 preview in the corner of the frame —
    // the very defect #372/#373 fixed for 1080p — so say so instead of shipping a
    // silently broken file. Once per session.
    if (!programWritesNv12_ && !frame.programNv12.empty() && !nv12MismatchWarned_) {
      nv12MismatchWarned_ = true;
      session_.recordingWarning =
          "Program recording is muxing the low-resolution preview: the full-resolution program "
          "tap is " + std::to_string(frame.programNv12Width) + "x" +
          std::to_string(frame.programNv12Height) + " but this recording is " +
          std::to_string(request_.width) + "x" + std::to_string(request_.height) +
          ". Record at the tap resolution for a full-quality program.";
    }

    const bool hasFull = !frame.programFullBgra.bgra.empty() &&
                         frame.programFullBgra.width > 0 && frame.programFullBgra.height > 0;
    if (hasFull) {
      fullResLocked_ = true;
    } else if (fullResLocked_) {
      return;
    }
    const auto& preview = hasFull ? frame.programFullBgra : frame.preview;
    if (preview.bgra.empty() || preview.width <= 0 || preview.height <= 0) {
      return;
    }
    // Shared-epoch clock (spec 4.3): the audio worker re-submits the LATEST
    // program frame every ~20ms tick; the clock dedups by frameNumber so each
    // composed frame is muxed exactly once, timestamped at the wall time it
    // was submitted — keeping the video timeline aligned with the
    // sample-counted audio track instead of drifting monotonically apart.
    const auto pts = recordingClock_.videoPts(now100ns(), frame.frameNumber);
    if (!pts) {
      return;  // this program frame is already in the file
    }
    const int srcStride = preview.width * 4;
    std::string error;
    if (!program_.writeVideo(preview.bgra.data(), preview.width, preview.height, srcStride, *pts, error)) {
      setRecordingFailure("Media Foundation could not write program video", error);
      return;
    }
    // ISO-1: each selected source's OWN video is muxed by submitIsoVideo (mapped
    // sourceId → per-source writer), not the composed program frame. Program is
    // priority-1 and never regressed by ISO — the two paths are independent.

    ++session_.recordingVideoFrameCount;
    session_.recordingDurationMs = recordingClock_.lastVideoPts100ns() / 10'000;
    updateBytesWritten();
  }

  void setAudioContentLatencySamples(int latencySamples) override {
    audioContentLatencySamples_.store(latencySamples > 0 ? latencySamples : 0,
                                      std::memory_order_relaxed);
  }

  void submitAudio(const float* interleaved, int frameCount, int channels, int sampleRate) override {
    if (!session_.active || interleaved == nullptr || frameCount <= 0 || channels <= 0) {
      return;
    }
    if (!program_.writing()) {
      return;
    }
    std::string error;
    if (!program_.audioConfigured()) {
      // The audio stream is configured up front in openRecordingWriters (it
      // cannot be added after BeginWriting); if it is missing here the open
      // already failed and surfaced a recording failure.
      return;
    }
    if (channels != program_.audioChannels() || sampleRate != program_.audioSampleRate()) {
      // Defensive: the program tap is canonically 48kHz stereo. Drop mismatched
      // buffers (with a visible warning) rather than mux garbage or kill the
      // whole recording session.
      if (session_.recordingWarning.empty()) {
        session_.recordingWarning =
            "Program audio format " + std::to_string(sampleRate) + "Hz/" + std::to_string(channels) +
            "ch does not match the recording AAC stream (" + std::to_string(program_.audioSampleRate()) +
            "Hz/" + std::to_string(program_.audioChannels()) + "ch); dropping audio buffers.";
      }
      return;
    }

    // Down/area-mix to the program writer's channel count is handled by MF's PCM
    // input type matching what we declared; we pass channels through unchanged
    // since the program tap is already stereo. Convert/write float -> 16-bit PCM.
    // A3: the clock LATCHES the plugin content latency at the first buffer
    // (see RecordingPtsClock) so a delayed program mix keeps A/V truth.
    recordingClock_.setAudioContentLatency(audioContentLatencySamples_.load(std::memory_order_relaxed));
    const LONGLONG audioPts = recordingClock_.audioPts(now100ns(), frameCount, sampleRate);
    if (program_.writeAudio(interleaved, frameCount, audioPts, error)) {
      session_.recordingAudioPacketCount = program_.audioPacketCount();
      session_.recordingAudioSampleCount = program_.audioSampleCount();
      session_.recordingAudioChannels = program_.audioChannels();
      session_.recordingAudioSampleRate = program_.audioSampleRate();
      session_.recordingAudioBitrateKbps = program_.audioBitrateKbps();
    } else if (!error.empty() &&
               session_.recordingWarning.rfind("Media Foundation dropped program audio", 0) != 0) {
      // A failed audio WriteSample used to vanish silently (video kept muxing,
      // the finalized MP4 just had no AAC track and nothing was surfaced).
      // Keep the recording alive but make the drop visible — and let it
      // OVERWRITE a benign earlier note (e.g. the format-fallback warning),
      // which previously masked the drop entirely.
      session_.recordingWarning = "Media Foundation dropped program audio: " + error + ".";
    }
    // ISO-1 is VIDEO-ONLY: the ISO writers have no audio stream (per-source audio
    // stems are ISO-2). The old placeholder wrote the PROGRAM mix into every ISO
    // — removed; an ISO must carry its own source, not the program proxy.
    updateBytesWritten();
  }

  // ISO-1: mux each selected source's own video into its own MP4. Runs on the
  // async encoder writer thread (never coreMutex / the audio worker), so the
  // I420->NV12 interleave and disk WriteSample are off every hot path.
  void submitIsoVideo(const std::vector<IsoSourceVideoFrame>& sources) override {
    if (!session_.active || isoWriters_.empty() || sources.empty()) {
      return;
    }
    const int fps = request_.fps > 0 ? request_.fps : kDefaultFps;
    const int bitrate = request_.targetBitrateMbps > 0 ? request_.targetBitrateMbps : 18;
    const std::string codec = request_.videoCodec.empty() ? "h264" : request_.videoCodec;
    for (const auto& src : sources) {
      auto idxIt = isoIndexBySource_.find(src.sourceId);
      if (idxIt == isoIndexBySource_.end()) {
        continue;  // frame for a source that is not selected — ignore
      }
      auto& entry = isoWriters_[idxIt->second];
      if (entry.failed) {
        continue;
      }
      const auto& frame = src.frame;
      const bool haveI420 = frame.hasI420();
      const bool haveBgra = frame.hasPixels();
      if (!haveI420 && !haveBgra) {
        continue;  // metadata-only frame this tick (source has not decoded yet)
      }
      // Per-(sourceId,frameId) dedup on the SHARED program epoch (spec 2c): the
      // audio worker re-submits each source's latest frame every tick; each real
      // decoded frame muxes exactly once, on the program timeline.
      const auto pts = recordingClock_.videoPtsForSource(now100ns(), src.sourceId, frame.frameId);
      if (!pts) {
        continue;
      }
      std::string error;
      if (!entry.opened) {
        // Lazy open sized to this source's native frame (no scaling). Zoom I420
        // → NV12 input; BGRA (capture, ISO-3) → RGB32 input.
        const int w = haveI420 ? frame.i420Width : frame.pixelWidth;
        const int h = haveI420 ? frame.i420Height : frame.pixelHeight;
        const VideoInput input = haveI420 ? VideoInput::Nv12 : VideoInput::Bgra;
        // #286: the AAC audio stream MUST be added UP FRONT — before
        // BeginWriting — or AddStream fails 0xC00D36B2 and the ISO silently
        // becomes a track-less/audio-less file. ISO stems are uniformly 48kHz
        // stereo (matches program); mono source stems are up-mixed to stereo in
        // submitIsoAudio. Mp4Writer::open() reset audioConfigured_ (the #286
        // reset), so a REUSED ISO writer re-adds its stream cleanly.
        //
        // ISO-3 pairing: only add the audio stream when this source HAS audio —
        // a Zoom participant, or a capture device with a paired audio input. A
        // pure-video capture source (a camera with no paired audio) opens
        // VIDEO-ONLY, so its ISO is honestly video-only, NOT an all-silence AAC
        // track. submitIsoAudio then skips it (audioConfigured() stays false).
        if (!entry.writer.open(entry.path, w, h, fps, bitrate, codec, error, input) ||
            (entry.hasAudio &&
             !entry.writer.ensureAudioStream(2, 48000, request_.audioBitrateKbps, error)) ||
            !entry.writer.beginWriting(error)) {
          entry.failed = true;
          entry.warning = "ISO writer open failed for " + entry.displayName + " (" + src.sourceId +
                          "): " + error + ".";
          raiseIsoWarning(entry.warning);
          continue;
        }
        entry.opened = true;
      }
      bool ok = false;
      if (haveI420) {
        i420ToNv12(frame.i420->data(), frame.i420Width, frame.i420Height, entry.nv12Scratch);
        ok = entry.writer.writeVideoNv12(entry.nv12Scratch.data(), frame.i420Width, frame.i420Height,
                                         *pts, error);
      } else {
        ok = entry.writer.writeVideo(frame.pixels->data(), frame.pixelWidth, frame.pixelHeight,
                                     frame.pixelStride, *pts, error);
      }
      if (ok) {
        ++entry.videoFrameCount;
      } else if (entry.warning.empty()) {
        entry.warning = "ISO writer dropped video for " + entry.displayName + " (" + src.sourceId +
                        "): " + error + ".";
        raiseIsoWarning(entry.warning);
      }
    }
    refreshIsoStreams();
    updateBytesWritten();
  }

  // ISO-2: mux each selected source's OWN raw-stem audio into its own MP4. Runs
  // on the async encoder writer thread (never coreMutex / the audio worker). For
  // each source: silence-fill the stem to its epoch-anchored expected position,
  // then (if PCM present) write the real samples — so a gated guest's ISO carries
  // silence exactly where they were not talking and stays sample-aligned to
  // program (spec §2c). A stem whose writer never opened (no video yet) is
  // skipped: the wall-anchored clock silence-fills the whole gap when it opens.
  void submitIsoAudio(const std::vector<IsoSourceAudio>& sources) override {
    if (!session_.active || isoWriters_.empty() || sources.empty()) {
      return;
    }
    for (const auto& src : sources) {
      auto idxIt = isoIndexBySource_.find(src.sourceId);
      if (idxIt == isoIndexBySource_.end()) {
        continue;  // audio for a source that is not selected — ignore
      }
      auto& entry = isoWriters_[idxIt->second];
      if (entry.failed || !entry.opened || !entry.writer.audioConfigured()) {
        continue;  // writer not open yet (no video frame) or already failed
      }
      const int rate = entry.writer.audioSampleRate();
      const int frameCount = src.frameCount > 0 && !src.pcm.empty() ? src.frameCount : 0;
      const auto advance = recordingClock_.isoAudioAdvance(now100ns(), src.sourceId, frameCount, rate);
      std::string error;
      bool ok = true;
      if (advance.silenceFrames > 0) {
        ok = entry.writer.writeAudioSilence(advance.silenceFrames, advance.silencePts100ns, error);
      }
      if (ok && frameCount > 0) {
        // Up-mix the raw stem to the writer's stereo AAC layout (Zoom
        // isolate_audio is typically mono). Reused scratch — no per-tick alloc
        // churn beyond a grow.
        toStereo(src.pcm, src.channels > 0 ? src.channels : 1, frameCount, entry.audioScratch);
        ok = entry.writer.writeAudio(entry.audioScratch.data(), frameCount, advance.realPts100ns, error);
      }
      if (!ok && entry.warning.empty()) {
        // Loud, per #286: a video-only ISO where audio was expected must be as
        // loud as a video-only program was.
        entry.warning = "ISO writer dropped audio for " + entry.displayName + " (" + src.sourceId +
                        "): " + error + ".";
        raiseIsoWarning(entry.warning);
      }
    }
    refreshIsoStreams();
    updateBytesWritten();
  }

  void stopRecording() override {
    // Finalize the program + ISO writers so the MP4s gain their moov box and
    // are playable immediately. closeWriters() also refreshes
    // recordingBytesWritten from the finalized on-disk size. Subsequent
    // submit/submitAudio calls fall through safely (program_.writing() is
    // false), and the next start() reopens fresh writers.
    closeWriters();
    if (session_.recordingStatus == "recording" || session_.recordingStatus == "warning") {
      session_.recordingStatus = "stopped";
    }
  }

  OutputSession session() const override { return session_; }

 private:
  void openRecordingWriters() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const int width = request_.width > 0 ? request_.width : kDefaultWidth;
    const int height = request_.height > 0 ? request_.height : kDefaultHeight;
    const int fps = request_.fps > 0 ? request_.fps : kDefaultFps;
    const int bitrate = request_.targetBitrateMbps > 0 ? request_.targetBitrateMbps : 18;
    const std::string codec = request_.videoCodec.empty() ? "h264" : request_.videoCodec;
    const std::string requestedFormat = request_.format.empty() ? "mp4" : request_.format;
    if (requestedFormat != "mp4" && session_.recordingWarning.empty()) {
      session_.recordingWarning = "Media Foundation recorder currently writes MP4 artifacts; requested " +
                                  requestedFormat + " will be recorded as MP4.";
    }

    // Resolve the base folder; a bad target folder is LOUD (spec §4 — the silent
    // %TEMP% redirect is killed for ISO). `targetOk` is false when we fell back.
    bool targetOk = false;
    const auto baseDir = resolveTargetDir(targetOk);

    // Per-session subfolder so a show's artifacts stay together (spec §5):
    // <prefix>-<yyyymmdd-hhmmss>/Program.mp4 + ISO-NN-<SafeName>.mp4 + manifest.json.
    isoWriters_.clear();
    isoIndexBySource_.clear();
    sessionDir_.clear();
    sessionDirActive_ = false;
    std::filesystem::path programPath;
    {
      std::error_code ec;
      const auto candidate = baseDir / (request_.filenamePrefix + "-" + sessionTimestampFolder());
      std::filesystem::create_directories(candidate, ec);
      if (!ec && std::filesystem::is_directory(candidate)) {
        sessionDir_ = candidate;
        sessionDirActive_ = true;
        programPath = candidate / "Program.mp4";
      }
    }
    if (!sessionDirActive_) {
      // Fall back to a flat program file in the base dir (program is priority-1 —
      // it still records). Old-scheme name with a ms suffix to avoid collisions.
      programPath = baseDir / (request_.filenamePrefix + "-program-" + std::to_string(now) + ".mp4");
    }

    // Select the ISO sources (prefer the display-name-bearing list; fall back to
    // the legacy flat id list where the id doubles as the display name).
    std::vector<IsoSourceSelection> isoSel = request_.isoSources;
    if (isoSel.empty()) {
      const auto& ids =
          !request_.isoParticipantIds.empty() ? request_.isoParticipantIds : session_.isoParticipantIds;
      for (const auto& id : ids) isoSel.push_back({id, id});
    }

    // LOUD refusal (spec §7): never write ISO files into the temp fallback or a
    // non-creatable session folder. Program still records; ISO is disabled.
    if (!isoSel.empty() && (!targetOk || !sessionDirActive_)) {
      raiseIsoWarning(std::string("ISO recording disabled: ") +
                      (!targetOk ? "the recording folder '" + request_.targetFolder +
                                       "' is not writable (program is recording to a temporary folder)."
                                 : "could not create the per-session subfolder under '" +
                                       request_.targetFolder + "'."));
      isoSel.clear();
    }

    std::string error;
    // NV12 when the compositor supplies the full-resolution program tap (see
    // RecordingSessionRequest::programNv12). It is both correct — the alternative
    // is muxing the 320x180 preview — and cheaper, since MF's H.264 encoder takes
    // NV12 natively and would otherwise convert from BGRA internally.
    programWritesNv12_ = request_.programNv12;
    if (!program_.open(programPath, width, height, fps, bitrate, codec, error,
                       programWritesNv12_ ? VideoInput::Nv12 : VideoInput::Bgra)) {
      setRecordingFailure("Media Foundation could not open program MP4 writer", error);
      return;
    }

    // ISO writers: pre-assign ISO-NN-<SafeName>.mp4 in selection order (the writer
    // itself opens LAZILY at the source's first frame — no 0-byte tails, native
    // size, NV12 for Zoom I420). #286 per-writer reset lives in Mp4Writer::open().
    isoWriters_.reserve(isoSel.size());
    std::map<std::string, int> nameCollisions;
    for (size_t i = 0; i < isoSel.size(); ++i) {
      IsoWriterEntry entry;
      entry.sourceId = isoSel[i].sourceId;
      entry.displayName = isoSel[i].displayName.empty() ? isoSel[i].sourceId : isoSel[i].displayName;
      entry.hasAudio = isoSel[i].hasAudio;  // ISO-3: video-only for an unpaired capture source
      std::string safe = sanitizeForFilename(entry.displayName, entry.sourceId);
      // Disambiguate duplicate sanitized names within a session.
      if (int& seen = nameCollisions[safe]; ++seen > 1) {
        safe += "-" + std::to_string(seen);
      }
      char idxBuf[8];
      std::snprintf(idxBuf, sizeof(idxBuf), "%02zu", i + 1);
      entry.path = sessionDir_ / ("ISO-" + std::string(idxBuf) + "-" + safe + ".mp4");
      isoIndexBySource_[entry.sourceId] = isoWriters_.size();
      isoWriters_.push_back(std::move(entry));
    }
    session_.recordingSessionDir = sessionDirActive_ ? sessionDir_.string() : baseDir.string();

    // Configure the program AAC audio stream UP FRONT. The MF sink writer rejects
    // AddStream after BeginWriting with MF_E_INVALIDREQUEST (0xC00D36B2), so the
    // previous "add the audio stream lazily on first audio" flow killed every
    // recording ~1 second in as soon as real program audio arrived (found by the
    // 2026-07-02 alpha soak: 10-min recording produced a 1-frame MP4 with
    // "could not add program AAC stream: 0xC00D36B2"). The program audio tap is
    // canonically 48kHz stereo; submitAudio drops mismatched formats with a
    // warning instead of failing the session. (ISO-1 ISO writers are video-only;
    // per-source audio stems are ISO-2, and slot into the same up-front-stream
    // discipline via Mp4Writer::open() then.)
    if (!program_.ensureAudioStream(2, 48000, request_.audioBitrateKbps, error)) {
      setRecordingFailure("Media Foundation could not add program AAC stream", error);
      return;
    }

    // Begin writing only after the stream is configured.
    if (!program_.beginWriting(error)) {
      setRecordingFailure("Media Foundation could not begin program writing", error);
      return;
    }

    // Fresh shared-epoch clock per recording session: the epoch anchors to the
    // first media submitted after this point (spec 4.3). One clock serves program
    // + every ISO writer, so a clap on program lands at the same timeline position
    // on every ISO.
    recordingClock_.reset();

    writeManifest(programPath);
    refreshIsoStreams();

    session_.recordingArtifactPath = programPath.string();
    session_.recordingWidth = program_.width();
    session_.recordingHeight = program_.height();
    session_.recordingFps = program_.fps();
    session_.recordingContainerFormat = "mp4";
    session_.recordingVideoCodec = program_.videoCodec();
    session_.codec = program_.videoCodec();
    session_.recordingAudioCodec = "aac";
    session_.recordingMetadataValid = true;
  }

  // `targetOk` reports whether the REQUESTED folder was usable (true) or we fell
  // back to temp (false). Program still records on a fallback (priority-1), but
  // the caller makes the fallback LOUD and refuses ISO (spec §4/§7).
  std::filesystem::path resolveTargetDir(bool& targetOk) {
    targetOk = false;
    if (!request_.targetFolder.empty()) {
      std::error_code ec;
      std::filesystem::path target(request_.targetFolder);
      std::filesystem::create_directories(target, ec);
      if (!ec && std::filesystem::is_directory(target)) {
        targetOk = true;
        return target;
      }
    }
    return std::filesystem::temp_directory_path();
  }

  // Fold an ISO failure into the loud recording.warning channel (#286) without
  // clobbering an existing program-audio drop note.
  void raiseIsoWarning(const std::string& message) {
    if (session_.recordingWarning.empty() ||
        session_.recordingWarning.rfind("Media Foundation dropped program audio", 0) != 0) {
      session_.recordingWarning = message;
    }
  }

  // Rebuild the per-ISO-writer status vector the snapshot reads (spec §3/§6).
  void refreshIsoStreams() {
    session_.isoStreams.clear();
    session_.isoStreams.reserve(isoWriters_.size());
    for (const auto& iso : isoWriters_) {
      IsoStreamStatus status;
      status.sourceId = iso.sourceId;
      status.displayName = iso.displayName;
      status.path = iso.path.string();
      status.videoFrameCount = iso.videoFrameCount;
      status.audioSampleCount = iso.writer.audioSampleCount();  // ISO-2: silence + real
      status.bytesWritten = iso.writer.bytesWritten();
      status.trackOpen = iso.opened && !iso.failed;
      status.warning = iso.warning;
      session_.isoStreams.push_back(std::move(status));
    }
  }

  // Session manifest.json (spec §5): what the support bundle + any future
  // auto-import reads. Written up-front from the planned entries.
  void writeManifest(const std::filesystem::path& programPath) {
    if (!sessionDirActive_) {
      session_.recordingManifestPath.clear();
      return;
    }
    const auto manifestPath = sessionDir_ / "manifest.json";
    std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
    if (!out) {
      raiseIsoWarning("Could not write recording manifest to " + manifestPath.string() + ".");
      return;
    }
    const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    out << "{\n";
    out << "  \"sessionId\": \"" << jsonEscape(session_.recordingSessionId) << "\",\n";
    out << "  \"epochMs\": " << epochMs << ",\n";
    out << "  \"entries\": [\n";
    out << "    { \"sourceId\": \"program\", \"name\": \"Program\", \"path\": \""
        << jsonEscape(programPath.filename().string())
        << "\", \"kind\": \"program\", \"hasAudio\": true }";
    // ISO-2: a Zoom ISO is a self-contained A+V MP4 (its own video + raw-stem
    // audio). ISO-3: a pure-video capture source (a camera with no paired audio)
    // is VIDEO-ONLY, so hasAudio reflects the per-source pairing decision, not a
    // blanket true (spec §5).
    for (const auto& iso : isoWriters_) {
      out << ",\n    { \"sourceId\": \"" << jsonEscape(iso.sourceId) << "\", \"name\": \""
          << jsonEscape(iso.displayName) << "\", \"path\": \""
          << jsonEscape(iso.path.filename().string()) << "\", \"kind\": \"iso\", \"hasAudio\": "
          << (iso.hasAudio ? "true" : "false") << " }";
    }
    out << "\n  ]\n}\n";
    out.close();
    session_.recordingManifestPath = manifestPath.string();
  }

  void updateBytesWritten() {
    int64_t total = program_.bytesWritten();
    for (const auto& iso : isoWriters_) {
      total += iso.writer.bytesWritten();
    }
    session_.recordingBytesWritten = total;
  }

  // 100ns ticks on the steady clock since sink construction — the time base
  // RecordingPtsClock anchors its shared epoch to.
  [[nodiscard]] LONGLONG now100ns() const {
    return std::chrono::duration_cast<std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>>(
               std::chrono::steady_clock::now() - clockOrigin_)
        .count();
  }

  void closeWriters() {
    program_.finalize();
    // Each ISO writer finalizes INDEPENDENTLY (its own moov) so a source that
    // dropped mid-show still leaves a playable file, no 0-byte tails (spec §4).
    for (auto& iso : isoWriters_) {
      iso.writer.finalize();
    }
    // Refresh the on-disk sizes into the ISO status before clearing.
    refreshIsoStreams();
    if (session_.recordingWarning.empty()) {
      // Surface the finalized on-disk size of the program file.
      updateBytesWritten();
    }
    isoWriters_.clear();
    isoIndexBySource_.clear();
  }

  void setRecordingFailure(const std::string& message, const std::string& detail) {
    session_.recordingWarning = message + ": " + detail + ".";
    session_.recordingError = session_.recordingWarning;
    session_.recordingStatus = "warning";
    program_.finalize();
    for (auto& iso : isoWriters_) {
      iso.writer.finalize();
    }
    isoWriters_.clear();
    isoIndexBySource_.clear();
    session_.isoStreams.clear();
  }

  // One ISO source's writer + its pre-assigned on-disk path (assigned at
  // recording start; the writer opens LAZILY at the source's first frame, sized
  // to that frame — no scaling, no 0-byte files for sources that never deliver).
  struct IsoWriterEntry {
    std::string sourceId;
    std::string displayName;
    std::filesystem::path path;
    Mp4Writer writer;
    bool opened = false;
    bool failed = false;
    bool hasAudio = true;  // ISO-3: false → VIDEO-ONLY (no AAC stream added at open)
    int64_t videoFrameCount = 0;
    std::string warning;
    std::vector<uint8_t> nv12Scratch;   // reused I420->NV12 buffer (writer thread)
    std::vector<float> audioScratch;    // reused mono->stereo up-mix (ISO-2)
  };

  OutputSession session_;
  RecordingSessionRequest request_;
  Mp4Writer program_;
  std::vector<IsoWriterEntry> isoWriters_;
  std::map<std::string, size_t> isoIndexBySource_;
  std::filesystem::path sessionDir_;
  bool sessionDirActive_ = false;
  // Shared-epoch A/V PTS clock (spec 4.3) + the steady-clock origin its 100ns
  // tick count is measured from. One clock serves program + ISO writers (they
  // mux the same frames on the same timeline).
  RecordingPtsClock recordingClock_;
  std::chrono::steady_clock::time_point clockOrigin_ = std::chrono::steady_clock::now();
  bool comInitialized_ = false;
  // Has this session ever muxed a FULL-RESOLUTION program frame? Once it has,
  // never fall back to the 320x180 preview (see submit): a mid-file geometry
  // change is the corner-tile defect all over again. Mirrors the RTMP sender's
  // fullResLocked_, which exists for the same asynchronous-tap reason.
  bool fullResLocked_ = false;
  // Was the program writer opened with an NV12 media type this session? Decided
  // at open from RecordingSessionRequest::programNv12 and fixed for the session —
  // the writer's media type cannot change once BeginWriting has been called.
  bool programWritesNv12_ = false;
  // One-shot: the full-res tap was present but the recording size did not match it,
  // so this file is the low-resolution preview. Warned once per session.
  bool nv12MismatchWarned_ = false;
  // A3: plugin content latency (atomic — the audio worker sets it, the writer
  // thread reads it; the PTS clock latches per session).
  std::atomic<int> audioContentLatencySamples_{0};
};

}  // namespace

std::unique_ptr<IEncoderSink> createMediaFoundationEncoderSink() {
  // REQUIRES DEV MACHINE: Media Foundation is the Windows hardware encoder
  // gateway. Concrete NVENC/QuickSync/AMF selection belongs behind this facade.
  if (FAILED(MFStartup(MF_VERSION))) {
    return nullptr;
  }
  return std::make_unique<MediaFoundationEncoderSink>();
}

}  // namespace corevideo::modules

#else

namespace corevideo::modules {

std::unique_ptr<IEncoderSink> createMediaFoundationEncoderSink() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif
