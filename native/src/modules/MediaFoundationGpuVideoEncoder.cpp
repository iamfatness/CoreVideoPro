#include "modules/MediaFoundationGpuVideoEncoder.h"

#include <algorithm>

#include "modules/EncoderCapacityProbe.h"

#if defined(_WIN32)

#include <atomic>
#include <cstdint>
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
  return MFVideoFormat_H264;
}

// Owns a dedicated D3D11 device + the MF hardware encoder MFT + a D3D11 video
// processor for BGRA->NV12, and drives the async MFT on its own thread. Every
// method is called by the sender; the encode loop is the only other thread.
class MediaFoundationGpuVideoEncoderImpl final : public GpuVideoEncoder {
 public:
  ~MediaFoundationGpuVideoEncoderImpl() override { stop(); }

  bool start(const GpuVideoEncoderConfig& config, GpuEncodedChunkSink sink) override {
    if (running_.load()) return true;
    config_ = config;
    sink_ = std::move(sink);
    if (config_.width <= 0 || config_.height <= 0 || config_.fps <= 0 || !sink_) {
      return fail("invalid-config");
    }
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return fail("mfstartup");
    mfStarted_ = true;
    if (!createDevice()) return false;
    if (!createEncoder()) return false;
    if (!createVideoProcessor()) return false;

    healthy_.store(true);
    running_.store(true);
    thread_ = std::thread([this] { encodeLoop(); });
    ::corevideo::core::nativeLogf("[gpu-encode] started %dx%d@%d %dkbps mft=hardware-h264\n",
                                 config_.width, config_.height, config_.fps, config_.bitrateKbps);
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
      haveHandle_ = true;
    }
    queueCv_.notify_one();
    return true;
  }

  void stop() override {
    running_.store(false);
    queueCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    videoProcessor_.Reset();
    videoProcessorEnum_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    nv12_.Reset();
    encoder_.Reset();
    eventGen_.Reset();
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
  }

  [[nodiscard]] bool healthy() const override { return healthy_.load(); }

 private:
  bool fail(const char* why) {
    ::corevideo::core::nativeLogf("[gpu-encode] init failed: %s\n", why);
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
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, subtypeForCodec("h264")};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           nullptr, &outInfo, &activates, &count);
    if (FAILED(hr) || count == 0) {
      if (activates) CoTaskMemFree(activates);
      return fail("no-hardware-h264-mft");
    }
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&encoder_));
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

    // Output type FIRST (encoders require it), then input.
    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, subtypeForCodec("h264"));
    outType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(config_.bitrateKbps) * 1000u);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
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
  bool convertToNv12(const std::string& hex) {
    if (!ensureOpened(hex) || !openedMutex_) return false;
    // Wait up to ~2 frame periods for the producer (the 60Hz render thread) to
    // release key 1. A 4ms wait missed the 16ms production cadence on almost
    // every frame, so most converts timed out, wasted the MFT's input slot and
    // starved the encoder to ~2fps. Bounded so stop() is never blocked for long.
    if (openedMutex_->AcquireSync(1, 34) != S_OK) return false;
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

  void drainOutput() {
    for (;;) {
      MFT_OUTPUT_STREAM_INFO info{};
      encoder_->GetOutputStreamInfo(0, &info);
      MFT_OUTPUT_DATA_BUFFER out{};
      ComPtr<IMFSample> sample;
      const bool mftAllocates =
          (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
      if (!mftAllocates) {
        if (FAILED(MFCreateSample(&sample))) return;
        ComPtr<IMFMediaBuffer> buf;
        MFCreateMemoryBuffer((std::max<DWORD>)(info.cbSize, 1u << 20), &buf);
        sample->AddBuffer(buf.Get());
        out.pSample = sample.Get();
      }
      DWORD status = 0;
      HRESULT hr = encoder_->ProcessOutput(0, 1, &out, &status);
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
      if (FAILED(hr)) return;
      ComPtr<IMFSample> produced;
      produced.Attach(out.pSample);
      if (out.pEvents) out.pEvents->Release();
      if (!produced) continue;
      emit(produced.Get());
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
    sample->GetSampleTime(&hns);
    GpuEncodedChunk chunk{};
    chunk.data = data;
    chunk.size = len;
    chunk.keyframe = clean != 0;
    chunk.frameNumber = hns * (std::max)(1, config_.fps) / 10000000LL;
    if (!firstEmitLogged_) {
      firstEmitLogged_ = true;
      ::corevideo::core::nativeLogf("[gpu-encode] first output chunk size=%zu keyframe=%d\n", chunk.size,
                                   chunk.keyframe ? 1 : 0);
    }
    if (sink_) sink_(chunk);
    buffer->Unlock();
  }

  void encodeLoop() {
    while (running_.load()) {
      ComPtr<IMFMediaEvent> event;
      HRESULT hr = eventGen_->GetEvent(0, &event);  // blocks until the MFT signals
      if (FAILED(hr) || !event) {
        if (!running_.load()) break;
        continue;
      }
      MediaEventType type = MEUnknown;
      event->GetType(&type);
      if (type == METransformNeedInput) {
        std::string handle;
        int64_t frameNumber = 0;
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
            have = true;
          }
        }
        if (!running_.load()) break;
        if (have) {
          if (convertToNv12(handle)) {
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
              break;
            }
          } else if (const HRESULT removed = deviceRemovedReason(); removed != S_OK) {
            // A failed BGRA->NV12 blit with a removed device is a device loss, not
            // a transient miss: retire so submit() fails and the supervisor restarts
            // the sender, which re-decides the encode path against the new device.
            ::corevideo::core::nativeLogf(
                "[gpu-encode] device lost (0x%08lx) during convert; encoder unhealthy -> supervisor\n",
                static_cast<unsigned long>(removed));
            healthy_.store(false);
            break;
          }
        }
      } else if (type == METransformHaveOutput) {
        drainOutput();
      }
    }
  }

  GpuVideoEncoderConfig config_{};
  GpuEncodedChunkSink sink_;
  std::atomic<bool> running_{false};
  std::atomic<bool> healthy_{false};
  bool mfStarted_ = false;
  bool firstEmitLogged_ = false;

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IMFDXGIDeviceManager> deviceManager_;
  ComPtr<IMFTransform> encoder_;
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
