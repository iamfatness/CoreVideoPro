#include "modules/MediaFoundationGpuVideoEncoder.h"

#include <algorithm>

#include "modules/EncoderCapacityProbe.h"

#if defined(_WIN32)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <codecapi.h>
#include <icodecapi.h>  // ICodecAPI (codecapi.h supplies only the property GUIDs).
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include "core/BoundedAsyncLog.h"

#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

namespace corevideo::modules {
namespace {

using Microsoft::WRL::ComPtr;

// Parse the compositor's keyed-mutex shared HANDLE, serialised as hex (e.g.
// "0x400002C2") in ProgramFrameSharedTexture::sharedHandleHex.
HANDLE handleFromHex(const std::string& hex) {
  if (hex.empty()) return nullptr;
  return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(hex, nullptr, 0)));
}

GUID subtypeForCodec(const std::string& codec) {
  if (codec == "hevc" || codec == "h265") return MFVideoFormat_HEVC;
  if (codec == "av1") return MFVideoFormat_AV1;
  return MFVideoFormat_H264;
}

UINT32 profileForCodec(const std::string& codec) {
  if (codec == "hevc" || codec == "h265") return eAVEncH265VProfile_Main_420_8;
  if (codec == "av1") return eAVEncAV1VProfile_Main_420_8;
  return eAVEncH264VProfile_High;
}

// COREVIDEO_GPU_ENCODE_CHUNK_TRACE=<n>: trace the first n output chunks (0/unset
// = off). Bounded by construction so a stray env var cannot flood a show's log.
int chunkTraceBudgetFromEnv() {
  size_t len = 0;
  char buf[16]{};
  if (getenv_s(&len, buf, sizeof(buf), "COREVIDEO_GPU_ENCODE_CHUNK_TRACE") != 0 || len == 0) return 0;
  const int n = std::atoi(buf);
  return n <= 0 ? 0 : (n > 200 ? 200 : n);
}

// #597 Task 8 Step 3b support. Walk an Annex-B elementary-stream chunk and name
// the NAL unit types it carries, so the acceptance gate's report can state
// whether a keyframe sample is a self-contained IDR (parameter sets IN BAND)
// or a bare slice whose headers arrived in some earlier sample. Named types
// only for the ones the question turns on; everything else prints its number.
//
// H.264 (Annex B): one header byte, type = byte & 0x1F.
// HEVC  (Annex B): two header bytes, type = (byte0 >> 1) & 0x3F.
// AV1 is NOT Annex-B (it is an OBU stream), so it is reported as such rather
// than mis-parsed - the gate refuses AV1 anyway (codec-not-deliverable).
void describeAnnexBNalTypes(const unsigned char* data, size_t size, const std::string& codec,
                            char* out, size_t outSize) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  if (!data || size == 0) { snprintf(out, outSize, "(empty)"); return; }
  const bool hevc = (codec == "hevc" || codec == "h265");
  if (codec == "av1") { snprintf(out, outSize, "(av1-obu, not annex-b)"); return; }
  size_t used = 0;
  int emitted = 0;
  for (size_t i = 0; i + 3 < size && emitted < 12; ++i) {
    // Start code: 00 00 01 or 00 00 00 01.
    if (data[i] != 0 || data[i + 1] != 0) continue;
    size_t payload = 0;
    if (data[i + 2] == 1) payload = i + 3;
    else if (data[i + 2] == 0 && data[i + 3] == 1) payload = i + 4;
    else continue;
    if (payload >= size) break;
    int type = hevc ? ((data[payload] >> 1) & 0x3F) : (data[payload] & 0x1F);
    const char* name = nullptr;
    if (hevc) {
      switch (type) {
        case 32: name = "VPS"; break;
        case 33: name = "SPS"; break;
        case 34: name = "PPS"; break;
        case 19: name = "IDR_W_RADL"; break;
        case 20: name = "IDR_N_LP"; break;
        case 21: name = "CRA"; break;
        case 1:  name = "TRAIL_R"; break;
        case 0:  name = "TRAIL_N"; break;
        case 39: name = "PREFIX_SEI"; break;
        case 40: name = "SUFFIX_SEI"; break;
        case 35: name = "AUD"; break;
        default: break;
      }
    } else {
      switch (type) {
        case 7: name = "SPS"; break;
        case 8: name = "PPS"; break;
        case 5: name = "IDR"; break;
        case 1: name = "non-IDR"; break;
        case 6: name = "SEI"; break;
        case 9: name = "AUD"; break;
        default: break;
      }
    }
    char piece[48];
    if (name) snprintf(piece, sizeof(piece), "%s%s(%d)", emitted ? "," : "", name, type);
    else snprintf(piece, sizeof(piece), "%s%d", emitted ? "," : "", type);
    const size_t len = strlen(piece);
    if (used + len + 1 >= outSize) break;
    memcpy(out + used, piece, len + 1);
    used += len;
    ++emitted;
    i = payload;  // continue the scan past this header byte
  }
  if (emitted == 0) snprintf(out, outSize, "(no annex-b start code)");
}

// Owns a dedicated D3D11 device + the MF hardware encoder MFT + a D3D11 video
// processor for BGRA->NV12, and drives the async MFT on its own thread. Every
// method is called by the sender; the encode loop is the only other thread.
class MediaFoundationGpuVideoEncoderImpl final : public GpuVideoEncoder {
 public:
  ~MediaFoundationGpuVideoEncoderImpl() override { stop(); }

  bool start(const GpuVideoEncoderConfig& config, GpuEncodedChunkSink sink) override {
    if (running_.load()) return true;
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      haveHandle_ = false;
      latestHandle_.clear();
      latestFrameNumber_ = 0;
      latestPublishedFrameNumber_.reset();
    }
    config_ = config;
    sink_ = std::move(sink);
    if (config_.width <= 0 || config_.height <= 0 || config_.fps <= 0 || !sink_) {
      return fail("invalid-config");
    }
    EncoderCapacityCache::instance().beginLiveEncoding();
    capacityLeaseActive_ = true;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return fail("mfstartup");
    mfStarted_ = true;
    if (!createDevice()) return false;
    if (!createEncoder()) return false;
    if (!createVideoProcessor()) return false;

    healthy_.store(true);
    running_.store(true);
    thread_ = std::thread([this] { encodeLoop(); });
    ::corevideo::core::nativeLogf("[gpu-encode] started %dx%d@%d %dkbps mft=hardware-%s\n",
                                 config_.width, config_.height, config_.fps, config_.bitrateKbps,
                                 config_.codec.c_str());
    return true;
  }

  bool submit(const GpuVideoEncoderFrame& frame) override {
    if (!running_.load() || !healthy_.load()) return false;
    if (frame.sharedHandleHex.empty()) return true;  // nothing to encode this tick
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      // Publish the latest handle; the encode loop reads it on the MFT's next
      // NeedInput. The keyed mutex — not a per-submit wait — paces the encoder to
      // the producer's frame rate: convertToNv12's AcquireSync(1) only succeeds
      // once the compositor has released a new frame.
      latestHandle_ = frame.sharedHandleHex;
      latestFrameNumber_ = frame.frameNumber;
      latestPublishedFrameNumber_ = frame.publishedFrameNumber;
      haveHandle_ = true;
    }
    queueCv_.notify_one();
    return true;
  }

  void stop() override {
    const bool wasRunning = running_.exchange(false);
    queueCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    // Joining our event reader does not retire the driver's queued work. Let
    // the asynchronous transform finish its accepted samples before destroying
    // its callback state (METransformDrainComplete is the completion fence).
    if (wasRunning && encoder_ && eventGen_) {
      encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      const HRESULT drain = encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
      bool completed = false;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
      while (SUCCEEDED(drain) && std::chrono::steady_clock::now() < deadline) {
        ComPtr<IMFMediaEvent> event;
        if (SUCCEEDED(eventGen_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event)) && event) {
          MediaEventType type = MEUnknown;
          event->GetType(&type);
          if (type == METransformDrainComplete) { completed = true; break; }
          if (type == METransformHaveOutput) noteDrain(drainOutput());
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      if (!completed) encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
      encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
      ::corevideo::core::nativeLogf("[gpu-encode] shutdown drain_complete=%d hr=0x%08lX\n",
          completed ? 1 : 0, static_cast<unsigned long>(drain));
    }
    // Async hardware MFTs retain queued driver callbacks. Release alone is not
    // shutdown: retire those callbacks while the D3D resources are still alive.
    // The activation owns a cached reference and must also be shut down.
    if (activation_) {
      const HRESULT hr = activation_->ShutdownObject();
      if (FAILED(hr)) {
        ::corevideo::core::nativeLogf("[gpu-encode] ShutdownObject failed hr=0x%08lX\n",
                                    static_cast<unsigned long>(hr));
      }
    }
    eventGen_.Reset();
    encoder_.Reset();
    activation_.Reset();
    videoProcessor_.Reset();
    videoProcessorEnum_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    nv12_.Reset();
    openedTexture_.Reset();
    openedMutex_.Reset();
    openedHandleHex_.clear();
    deviceManager_.Reset();
    context_.Reset();
    device_.Reset();
    if (mfStarted_) {
      MFShutdown();
      mfStarted_ = false;
    }
    if (capacityLeaseActive_) {
      EncoderCapacityCache::instance().endLiveEncoding();
      capacityLeaseActive_ = false;
    }
  }

  [[nodiscard]] bool healthy() const override { return healthy_.load(); }

  // The last fail() detail, so the sender can quote it in the refusal sentence.
  // Written only on the caller's thread inside start() (every fail() site is on
  // the create path) and read by that same caller right after start() returns.
  [[nodiscard]] std::string lastFailure() const override { return lastFailure_; }

 private:
  bool fail(const char* why) {
    ::corevideo::core::nativeLogf("[gpu-encode] init failed: %s\n", why);
    lastFailure_ = why ? why : "";
    healthy_.store(false);
    stop();
    return false;
  }

  // S_OK when the encode device is alive; a DXGI removed/reset/hung HRESULT when
  // it has been lost (TDR, driver upgrade, hardware fault). The encode loop uses
  // this to distinguish a genuine device loss from a transient encode miss.
  HRESULT deviceRemovedReason() const { return device_ ? device_->GetDeviceRemovedReason() : S_OK; }

  bool createDevice() {
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, &device_, &level, &context_);
    if (FAILED(hr) || !device_) return fail("d3d11-create-device");
    // A device shared between MF and our threads must be multithread-protected.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device_.As(&mt)) && mt) mt->SetMultithreadProtected(TRUE);

    UINT token = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &deviceManager_)) || !deviceManager_) {
      return fail("create-dxgi-device-manager");
    }
    if (FAILED(deviceManager_->ResetDevice(device_.Get(), token))) return fail("reset-device-manager");
    return true;
  }

  bool createEncoder() {
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, subtypeForCodec(config_.codec)};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           nullptr, &outInfo, &activates, &count);
    if (FAILED(hr) || count == 0) {
      if (activates) CoTaskMemFree(activates);
      const std::string why = "no-hardware-mft-" + config_.codec;
      return fail(why.c_str());
    }
    activation_ = activates[0];
    hr = activation_->ActivateObject(IID_PPV_ARGS(&encoder_));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr) || !encoder_) return fail("activate-mft");

    // Hardware encoder MFTs are ASYNC and refuse SetInputType until the caller
    // declares it understands the async model (EncoderCapacityProbe comment).
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(encoder_->GetAttributes(&attrs)) && attrs) {
      attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }
    // Bind the encoder to our D3D11 device so it takes D3D surfaces on input.
    if (FAILED(encoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                        reinterpret_cast<ULONG_PTR>(deviceManager_.Get())))) {
      return fail("set-d3d-manager");
    }

    // EVERY CODEC BUT H.264: NO DEEP PIPELINE / NO REORDERED FRAMES.
    //
    // This path is a LIVE stream: FFmpeg is demoted to a muxer reading a raw
    // elementary stream off a pipe and stamping each arriving access unit with
    // the wallclock. Two things break that. (a) Reordered frames - the FLV muxer
    // refuses raw HEVC with B-frames outright ("Packet is missing PTS", measured
    // 2026-09-20). (b) A deep encoder pipeline - measured 2026-09-20, NVENC AV1
    // at its defaults (lookahead / alt-ref reordering) starved the muxer: the MFT
    // bound, emitted a normal first chunk, then the muxed stream flatlined at
    // ~18.4 kbit/s over 29 s against a configured 6 Mbps, three times, with the
    // GPU encoder otherwise idle. A 12-frame round-trip fits inside that pipeline
    // and cannot see it; only the sustained gate can.
    //
    // So both are asked for, in one ladder, for every non-H.264 codec:
    // CODECAPI_AVEncMPVDefaultBPictureCount = 0 first, and on rejection
    // CODECAPI_AVLowLatencyMode + MF_LOW_LATENCY, which on NVENC means no
    // B-frames AND no deep pipeline. Measured on this rig (RTX 4090, driver
    // 616.92) the NVIDIA HEVC and AV1 Encoder MFTs both reject the B-picture
    // count with E_INVALIDARG (0x80070057) - for HEVC, before AND after
    // SetOutputType - so low-latency mode is what actually takes here; the
    // per-rig proof is each codec's round-trip test (raw stream copy-muxed into
    // FLV) plus scripts/validate-gpu-encode.mjs for the sustained rate. start()
    // fails only when BOTH are refused: a stream the muxer starves on or rejects
    // twenty frames in is far worse than a start() that refuses loudly.
    //
    // H.264 IS DELIBERATELY EXCLUDED and its configuration stays byte-identical:
    // it is the shipped, gate-proven path, it holds 60.0 fps with sink speed
    // ~1.37x at the MFT defaults, and it has nothing to gain from this ladder.
    if (config_.codec != "h264") {
      ComPtr<ICodecAPI> codecApi;
      if (FAILED(encoder_.As(&codecApi)) || !codecApi) return fail("no-codec-api");

      VARIANT bframes;
      VariantInit(&bframes);
      bframes.vt = VT_UI4;
      bframes.ulVal = 0;
      const HRESULT bhr = codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &bframes);
      if (SUCCEEDED(bhr)) {
        ::corevideo::core::nativeLogf("[gpu-encode] %s b-frames off via bpicture-count\n", config_.codec.c_str());
      } else {
        // Name the rejecting HRESULT: the bare detail string Task 8 reports to
        // the operator cannot tell a controller WHICH mechanism was refused.
        ::corevideo::core::nativeLogf(
            "[gpu-encode] %s MFT refused CODECAPI_AVEncMPVDefaultBPictureCount hr=0x%08lX\n",
            config_.codec.c_str(), static_cast<unsigned long>(bhr));
        VARIANT lowLatency;
        VariantInit(&lowLatency);
        lowLatency.vt = VT_BOOL;
        lowLatency.boolVal = VARIANT_TRUE;
        const HRESULT lhr = codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &lowLatency);
        // The transform attribute is the other half of the same request: the
        // codec-API property configures the encoder, MF_LOW_LATENCY tells the
        // MFT pipeline not to buffer. `attrs` may be null on an MFT with no
        // attribute store, which is not itself a failure - the codec-API result
        // is what decides.
        if (attrs) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        if (FAILED(lhr)) {
          ::corevideo::core::nativeLogf(
              "[gpu-encode] %s MFT refused CODECAPI_AVLowLatencyMode hr=0x%08lX\n",
              config_.codec.c_str(), static_cast<unsigned long>(lhr));
          return fail("set-bframes-off");
        }
        ::corevideo::core::nativeLogf("[gpu-encode] %s b-frames off via low-latency-mode\n", config_.codec.c_str());
      }
    }

    // Output type FIRST (encoders require it), then input.
    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, subtypeForCodec(config_.codec));
    outType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(config_.bitrateKbps) * 1000u);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, profileForCodec(config_.codec));
    if (config_.codec == "hevc" || config_.codec == "h265") {
      setHevcColorType(outType.Get());
    }
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(config_.width),
                       static_cast<UINT32>(config_.height));
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(config_.fps), 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(encoder_->SetOutputType(0, outType.Get(), 0))) return fail("set-output-type");

    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (config_.codec == "hevc" || config_.codec == "h265") {
      setHevcColorType(inType.Get());
    }
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(config_.width),
                       static_cast<UINT32>(config_.height));
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(config_.fps), 1);
    MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(encoder_->SetInputType(0, inType.Get(), 0))) return fail("set-input-type");

    if (FAILED(encoder_.As(&eventGen_)) || !eventGen_) return fail("no-event-generator");
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
  }

  static void setHevcColorType(IMFMediaType* type) {
    // Match the explicit BGRA -> limited-range BT.709 NV12 conversion below.
    // Missing VUI leaves downstream ingest guessing the matrix and transfer.
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
  }

  bool createVideoProcessor() {
    if (FAILED(device_.As(&videoDevice_)) || !videoDevice_) return fail("no-video-device");
    if (FAILED(context_.As(&videoContext_)) || !videoContext_) return fail("no-video-context");
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = static_cast<UINT>(config_.width);
    desc.InputHeight = static_cast<UINT>(config_.height);
    desc.OutputWidth = static_cast<UINT>(config_.width);
    desc.OutputHeight = static_cast<UINT>(config_.height);
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&desc, &videoProcessorEnum_))) {
      return fail("video-processor-enum");
    }
    if (FAILED(videoDevice_->CreateVideoProcessor(videoProcessorEnum_.Get(), 0, &videoProcessor_))) {
      return fail("video-processor");
    }
    if (config_.codec == "hevc" || config_.codec == "h265") {
      D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{};
      inputColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
      videoContext_->VideoProcessorSetStreamColorSpace(videoProcessor_.Get(), 0, &inputColor);
      D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor{};
      outputColor.YCbCr_Matrix = 1;  // BT.709; the D3D11 default is BT.601.
      outputColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
      videoContext_->VideoProcessorSetOutputColorSpace(videoProcessor_.Get(), &outputColor);
    }
    // NV12 encode target, bindable as a video processor output.
    D3D11_TEXTURE2D_DESC nv{};
    nv.Width = static_cast<UINT>(config_.width);
    nv.Height = static_cast<UINT>(config_.height);
    nv.MipLevels = 1;
    nv.ArraySize = 1;
    nv.Format = DXGI_FORMAT_NV12;
    nv.SampleDesc.Count = 1;
    nv.Usage = D3D11_USAGE_DEFAULT;
    nv.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&nv, nullptr, &nv12_))) return fail("nv12-texture");
    return true;
  }

  // Open (and cache) the compositor's keyed-mutex BGRA shared texture by handle.
  bool ensureOpened(const std::string& hex) {
    if (openedTexture_ && openedHandleHex_ == hex) return true;
    openedTexture_.Reset();
    openedMutex_.Reset();
    HANDLE handle = handleFromHex(hex);
    if (!handle) return false;
    if (FAILED(device_->OpenSharedResource(handle, IID_PPV_ARGS(&openedTexture_))) || !openedTexture_) {
      return false;
    }
    openedTexture_.As(&openedMutex_);
    openedHandleHex_ = hex;
    return true;
  }

  // BGRA (shared) -> NV12 (encode target) on the GPU via the driver's video
  // processor. Acquires the keyed mutex (key 1) held by the producer, then releases
  // key 0 for it to continue rendering.
  bool convertToNv12(const std::string& hex, const std::shared_ptr<std::atomic<int64_t>>& published, int64_t& frameNumber) {
    if (!ensureOpened(hex) || !openedMutex_) return false;
    // Wait up to ~2 frame periods for the producer (the 60Hz render thread) to
    // release key 1. A 4ms wait missed the 16ms production cadence on almost
    // every frame, so most converts timed out, wasted the MFT's input slot and
    // starved the encoder to ~2fps. Bounded so stop() is never blocked for long.
    if (openedMutex_->AcquireSync(1, 34) != S_OK) return false;
    if (published) frameNumber = published->load(std::memory_order_acquire);
    bool ok = false;
    do {
      D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
      ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
      ivd.Texture2D.MipSlice = 0;
      ComPtr<ID3D11VideoProcessorInputView> inView;
      if (FAILED(videoDevice_->CreateVideoProcessorInputView(openedTexture_.Get(),
                                                             videoProcessorEnum_.Get(), &ivd, &inView))) {
        break;
      }
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
      ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      ovd.Texture2D.MipSlice = 0;
      ComPtr<ID3D11VideoProcessorOutputView> outView;
      if (FAILED(videoDevice_->CreateVideoProcessorOutputView(nv12_.Get(), videoProcessorEnum_.Get(),
                                                             &ovd, &outView))) {
        break;
      }
      D3D11_VIDEO_PROCESSOR_STREAM stream{};
      stream.Enable = TRUE;
      stream.pInputSurface = inView.Get();
      ok = SUCCEEDED(videoContext_->VideoProcessorBlt(videoProcessor_.Get(), outView.Get(), 0, 1, &stream));
    } while (false);
    openedMutex_->ReleaseSync(0);
    return ok;
  }

  // Feed the NV12 encode target to the MFT as a D3D-backed sample.
  bool processInput(int64_t frameNumber) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12_.Get(), 0, FALSE, &buffer))) {
      return false;
    }
    ComPtr<IMF2DBuffer2> b2d;
    if (SUCCEEDED(buffer.As(&b2d))) {
      DWORD len = 0;
      if (SUCCEEDED(b2d->GetContiguousLength(&len))) buffer->SetCurrentLength(len);
    }
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    sample->AddBuffer(buffer.Get());
    const LONGLONG hns = static_cast<LONGLONG>(frameNumber) * 10000000LL / (std::max)(1, config_.fps);
    sample->SetSampleTime(hns);
    sample->SetSampleDuration(10000000LL / (std::max)(1, config_.fps));
    return SUCCEEDED(encoder_->ProcessInput(0, sample.Get(), 0));
  }

  // DRAIN every ready output on each METransformHaveOutput, rather than taking
  // exactly one. A strict generalization: an MFT with only one output ready
  // exits on the first MF_E_TRANSFORM_NEED_MORE_INPUT and behaves exactly as
  // before, so this is correct for any MFT rather than for the ones we happen to
  // have measured. NeedInput credit retention is untouched - that fix is
  // separate and load-bearing.
  //
  // It was written to test the hypothesis that the NVIDIA AV1 Encoder MFT queues
  // several outputs per event and was therefore being starved by a one-per-event
  // reader. THAT HYPOTHESIS IS FALSE, measured 2026-09-20 on this rig (RTX 4090,
  // driver 616.92) by the counter below: over a 30 s 1080p60 AV1 stream the MFT
  // reported events=1260 samples=1260 mean=1.00 max-per-event=1. Exactly one
  // output per event, so the drain changes nothing for AV1 and the ~18.4 kbit/s
  // AV1 stall has some other cause. The drain stays because it is correct; do
  // not read it as a fix for that stall.
  // Returns how many samples this event actually yielded.
  int drainOutput() {
    int produced_count = 0;
    for (;;) {
      MFT_OUTPUT_STREAM_INFO info{};
      encoder_->GetOutputStreamInfo(0, &info);
      MFT_OUTPUT_DATA_BUFFER out{};
      ComPtr<IMFSample> sample;
      const bool mftAllocates =
          (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
      if (!mftAllocates) {
        if (FAILED(MFCreateSample(&sample))) return produced_count;
        ComPtr<IMFMediaBuffer> buf;
        MFCreateMemoryBuffer((std::max<DWORD>)(info.cbSize, 1u << 20), &buf);
        sample->AddBuffer(buf.Get());
        out.pSample = sample.Get();
      }
      DWORD status = 0;
      HRESULT hr = encoder_->ProcessOutput(0, 1, &out, &status);
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return produced_count;
      if (FAILED(hr)) return produced_count;
      ComPtr<IMFSample> produced;
      if (mftAllocates) produced.Attach(out.pSample);
      else produced = sample;
      if (out.pEvents) out.pEvents->Release();
      if (!produced) return produced_count;
      emit(produced.Get());
      ++produced_count;
    }
  }

  // One bounded, quotable line saying how many samples each HaveOutput event
  // actually yields, so "one output per event" is never assumed again for a new
  // MFT. Encode-thread only; no lock needed.
  void noteDrain(int samples) {
    ++drainEvents_;
    drainSamples_ += static_cast<uint64_t>(samples);
    if (samples > drainMaxPerEvent_) drainMaxPerEvent_ = samples;
    if (drainSamples_ >= drainLogNextAt_) {
      drainLogNextAt_ = drainSamples_ + 600;
      ::corevideo::core::nativeLogf(
          "[gpu-encode] %s output drain: events=%llu samples=%llu mean=%.2f max-per-event=%d\n",
          config_.codec.c_str(), static_cast<unsigned long long>(drainEvents_),
          static_cast<unsigned long long>(drainSamples_),
          drainEvents_ ? static_cast<double>(drainSamples_) / static_cast<double>(drainEvents_) : 0.0,
          drainMaxPerEvent_);
    }
  }

  void emit(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return;
    BYTE* data = nullptr;
    DWORD len = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &len))) return;
    UINT32 clean = 0;
    sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
    LONGLONG hns = 0;
    const bool hasTime = SUCCEEDED(sample->GetSampleTime(&hns));
    LONGLONG duration = 0, dts = hns;
    sample->GetSampleDuration(&duration);
    UINT64 decodeTime = 0;
    if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &decodeTime)))
      dts = static_cast<LONGLONG>(decodeTime);
    GpuEncodedChunk chunk{};
    chunk.data = data;
    chunk.size = len;
    chunk.keyframe = clean != 0;
    chunk.frameNumber = hns * (std::max)(1, config_.fps) / 10000000LL;
    chunk.pts100ns = hns;
    chunk.dts100ns = dts;
    chunk.duration100ns = duration;
    chunk.timingValid = hasTime;
    if (!firstEmitLogged_) {
      firstEmitLogged_ = true;
      ::corevideo::core::nativeLogf("[gpu-encode] first output chunk size=%zu keyframe=%d\n", chunk.size,
                                   chunk.keyframe ? 1 : 0);
    }
    // #597 Task 8, Step 3b - THE PARAMETER-SET EVIDENCE.
    //
    // Lever B (StreamBackpressurePolicy's GOP-tail discard) drops every chunk
    // ahead of the first keyframe in the outgoing queue. That is safe ONLY if
    // each CleanPoint sample is a SELF-CONTAINED IDR carrying its own
    // VPS/SPS/PPS in band. Nothing in this tree configures sequence-header
    // repetition, so "does this MFT ever emit the parameter sets as a SEPARATE
    // non-CleanPoint sample just before the IDR?" cannot be settled by reading
    // code - only by looking at real chunks off the real hardware encoder.
    // This trace is how the acceptance gate looks, for h264 and for hevc.
    //
    // Deliberately env-gated and BOUNDED: it is a diagnostic, not telemetry,
    // and a per-chunk NAL walk has no business on a live show's hot path.
    // COREVIDEO_GPU_ENCODE_CHUNK_TRACE=<n> traces the first n chunks.
    if (chunkTraceRemaining_ > 0) {
      --chunkTraceRemaining_;
      char nals[256];
      describeAnnexBNalTypes(chunk.data, chunk.size, config_.codec, nals, sizeof(nals));
      ::corevideo::core::nativeLogf(
          "[gpu-encode] chunk-trace #%d codec=%s size=%zu keyframe=%d nal=%s\n",
          chunkTraceIndex_++, config_.codec.c_str(), chunk.size, chunk.keyframe ? 1 : 0, nals);
    }
    if (sink_) sink_(chunk);
    buffer->Unlock();
  }

  void encodeLoop() {
    while (running_.load()) {
      ComPtr<IMFMediaEvent> event;
      // Do not block in the MFT event queue during stop. Join this client
      // worker before ShutdownObject, so it never calls an already shut MFT.
      HRESULT hr = eventGen_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
      if (FAILED(hr) || !event) {
        if (!running_.load()) break;
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCv_.wait_for(lock, std::chrono::milliseconds(1), [&] { return !running_.load(); });
        continue;
      }
      MediaEventType type = MEUnknown;
      event->GetType(&type);
      if (type == METransformNeedInput) {
        // A NeedInput event grants an input credit; a missing texture or mutex
        // timeout does not consume it. Retain it until ProcessInput succeeds.
        // Dropping credits eventually starves an async MFT permanently.
        while (running_.load()) {
          std::string handle;
          int64_t frameNumber = 0;
          std::shared_ptr<std::atomic<int64_t>> published;
          bool have = false;
          {
            std::unique_lock<std::mutex> lock(queueMutex_);
            // Only wait during startup, before the first frame ever arrives. Once we
            // have a handle we take the latest immediately: the per-NeedInput wait
            // used to serialize with the mutex wait (~20ms + ~16ms) and halved the
            // encode rate to ~30fps. The keyed mutex does the pacing.
            if (!haveHandle_) {
              queueCv_.wait_for(lock, std::chrono::milliseconds(20),
                                [this] { return haveHandle_ || !running_.load(); });
            }
            if (haveHandle_) {
              handle = latestHandle_;
              frameNumber = latestFrameNumber_;
              published = latestPublishedFrameNumber_;
              have = true;
            }
          }
          if (!running_.load()) break;
          if (have) {
            if (convertToNv12(handle, published, frameNumber)) {
              if (!processInput(frameNumber)) {
                const HRESULT removed = deviceRemovedReason();
                if (removed != S_OK) {
                  ::corevideo::core::nativeLogf(
                      "[gpu-encode] device lost (0x%08lx) during encode; encoder unhealthy -> supervisor\n",
                      static_cast<unsigned long>(removed));
                } else {
                  ::corevideo::core::nativeLogf("[gpu-encode] ProcessInput failed; encoder unhealthy\n");
                }
                healthy_.store(false);
                return;
              }
              break;  // this input credit was consumed by ProcessInput
            } else if (const HRESULT removed = deviceRemovedReason(); removed != S_OK) {
              // A failed BGRA->NV12 blit with a removed device is a device loss, not
              // a transient miss: retire so submit() fails and the supervisor restarts
              // the sender, which re-decides the encode path against the new device.
              ::corevideo::core::nativeLogf(
                  "[gpu-encode] device lost (0x%08lx) during convert; encoder unhealthy -> supervisor\n",
                  static_cast<unsigned long>(removed));
              healthy_.store(false);
              return;
            }
            // An invalid/stale handle can fail before AcquireSync has waited.
            // Bound retry CPU usage until the producer publishes a usable one.
            std::unique_lock<std::mutex> retryLock(queueMutex_);
            queueCv_.wait_for(retryLock, std::chrono::milliseconds(2),
                              [this] { return !running_.load(); });
          }
        }
      } else if (type == METransformHaveOutput) {
        noteDrain(drainOutput());
      }
    }
  }

  GpuVideoEncoderConfig config_{};
  GpuEncodedChunkSink sink_;
  std::shared_ptr<std::atomic<int64_t>> latestPublishedFrameNumber_;
  std::atomic<bool> running_{false};
  std::atomic<bool> healthy_{false};
  bool mfStarted_ = false;
  std::string lastFailure_;
  bool capacityLeaseActive_ = false;
  bool firstEmitLogged_ = false;
  // #597 Task 8 Step 3b: bounded, env-gated parameter-set trace (see emit()).
  int chunkTraceRemaining_ = chunkTraceBudgetFromEnv();
  int chunkTraceIndex_ = 0;
  // Output-drain accounting (encode thread only).
  uint64_t drainEvents_ = 0;
  uint64_t drainSamples_ = 0;
  uint64_t drainLogNextAt_ = 60;
  int drainMaxPerEvent_ = 0;

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IMFDXGIDeviceManager> deviceManager_;
  ComPtr<IMFTransform> encoder_;
  ComPtr<IMFActivate> activation_;
  ComPtr<IMFMediaEventGenerator> eventGen_;
  ComPtr<ID3D11VideoDevice> videoDevice_;
  ComPtr<ID3D11VideoContext> videoContext_;
  ComPtr<ID3D11VideoProcessorEnumerator> videoProcessorEnum_;
  ComPtr<ID3D11VideoProcessor> videoProcessor_;
  ComPtr<ID3D11Texture2D> nv12_;
  ComPtr<ID3D11Texture2D> openedTexture_;
  ComPtr<IDXGIKeyedMutex> openedMutex_;
  std::string openedHandleHex_;

  std::thread thread_;
  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::string latestHandle_;
  int64_t latestFrameNumber_ = 0;
  bool haveHandle_ = false;
};

}  // namespace

bool mediaFoundationHardwareEncoderAvailable(int width, int height, int fps) {
  if (width <= 0 || height <= 0 || fps <= 0) return false;
  const auto capacity =
      EncoderCapacityCache::instance().lookup(EncoderProbeKey{"h264", width, height, fps});
  return capacity.probed && capacity.hardwareAvailable && capacity.hardwareSessionCeiling > 0;
}

std::unique_ptr<GpuVideoEncoder> createMediaFoundationGpuVideoEncoder() {
  return std::make_unique<MediaFoundationGpuVideoEncoderImpl>();
}

}  // namespace corevideo::modules

#else  // !_WIN32

namespace corevideo::modules {
bool mediaFoundationHardwareEncoderAvailable(int, int, int) { return false; }
std::unique_ptr<GpuVideoEncoder> createMediaFoundationGpuVideoEncoder() { return nullptr; }
}  // namespace corevideo::modules

#endif
