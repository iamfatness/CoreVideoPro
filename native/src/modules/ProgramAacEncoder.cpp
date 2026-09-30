#include "modules/ProgramAacEncoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/BoundedAsyncLog.h"

#if defined(_WIN32)
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#endif

namespace corevideo::modules {

struct ProgramAacEncoder::Impl {
  bool started = false;
  int64_t nextSample = 0;
  std::vector<int16_t> pending;
#if defined(_WIN32)
  bool mfStarted = false;
  Microsoft::WRL::ComPtr<IMFTransform> transform;
  DWORD outputBufferBytes = 4096;

  bool drain(std::vector<ProgramAacPacket>& packets) {
    while (true) {
      Microsoft::WRL::ComPtr<IMFSample> output;
      Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(MFCreateSample(&output)) ||
          FAILED(MFCreateMemoryBuffer(outputBufferBytes, &buffer)) ||
          FAILED(output->AddBuffer(buffer.Get()))) return false;
      MFT_OUTPUT_DATA_BUFFER data{};
      data.dwStreamID = 0;
      data.pSample = output.Get();
      DWORD status = 0;
      const HRESULT hr = transform->ProcessOutput(0, 1, &data, &status);
      if (data.pEvents) data.pEvents->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
      if (FAILED(hr)) return false;
      Microsoft::WRL::ComPtr<IMFSample> produced;
      if (data.pSample == output.Get()) produced = output;
      else produced.Attach(data.pSample);
      if (!produced) return false;
      Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguous;
      if (FAILED(produced->ConvertToContiguousBuffer(&contiguous))) return false;
      BYTE* bytes = nullptr;
      DWORD length = 0;
      if (FAILED(contiguous->Lock(&bytes, nullptr, &length))) return false;
      ProgramAacPacket packet;
      packet.adts.assign(bytes, bytes + length);
      contiguous->Unlock();
      LONGLONG pts = 0;
      if (FAILED(produced->GetSampleTime(&pts))) return false;
      packet.pts100ns = pts;
      packet.sampleIndex = (pts * 48000 + 5000000) / 10000000;
      packets.push_back(std::move(packet));
    }
  }
#endif
};

ProgramAacEncoder::ProgramAacEncoder() : impl_(std::make_unique<Impl>()) {}
ProgramAacEncoder::~ProgramAacEncoder() { stop(); }

bool ProgramAacEncoder::start() {
  stop();
#if defined(_WIN32)
  auto& state = *impl_;
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
  state.mfStarted = true;
  if (FAILED(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&state.transform)))) { stop(); return false; }
  Microsoft::WRL::ComPtr<IMFMediaType> input;
  Microsoft::WRL::ComPtr<IMFMediaType> output;
  if (FAILED(MFCreateMediaType(&input)) || FAILED(MFCreateMediaType(&output))) { stop(); return false; }
  const auto setCommon = [](IMFMediaType* type, const GUID& subtype) {
    return SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) &&
           SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, subtype)) &&
           SUCCEEDED(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000)) &&
           SUCCEEDED(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2)) &&
           SUCCEEDED(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
  };
  if (!setCommon(input.Get(), MFAudioFormat_PCM) ||
      FAILED(input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4)) ||
      FAILED(input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000)) ||
      !setCommon(output.Get(), MFAudioFormat_AAC) ||
      FAILED(output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 20000)) ||
      FAILED(output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1)) ||
      FAILED(state.transform->SetInputType(0, input.Get(), 0)) ||
      FAILED(state.transform->SetOutputType(0, output.Get(), 0))) { stop(); return false; }
  MFT_OUTPUT_STREAM_INFO info{};
  if (FAILED(state.transform->GetOutputStreamInfo(0, &info))) { stop(); return false; }
  state.outputBufferBytes = (std::max)(DWORD{4096}, info.cbSize);
  if (FAILED(state.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
      FAILED(state.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) {
    stop(); return false;
  }
  state.started = true;
  ::corevideo::core::nativeLogf("[program-aac] started sampleRate=48000 channels=2 bitrateKbps=160\n");
  return true;
#else
  return false;
#endif
}

void ProgramAacEncoder::stop() {
  auto& state = *impl_;
#if defined(_WIN32)
  if (state.transform) {
    state.transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    state.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    state.transform.Reset();
  }
  if (state.mfStarted) { MFShutdown(); state.mfStarted = false; }
#endif
  state.started = false;
  state.nextSample = 0;
  state.pending.clear();
}

bool ProgramAacEncoder::running() const { return impl_->started; }

int64_t ProgramAacEncoder::acceptedSamples() const {
  return impl_->nextSample + static_cast<int64_t>(impl_->pending.size() / 2);
}

bool ProgramAacEncoder::encode(const std::vector<float>& pcm, int channels, int sampleRate,
                               std::vector<ProgramAacPacket>& packets) {
  packets.clear();
#if defined(_WIN32)
  auto& state = *impl_;
  if (!state.started || channels != 2 || sampleRate != 48000 || pcm.size() % 2) return false;
  state.pending.reserve(state.pending.size() + pcm.size());
  for (const float sample : pcm) {
    const float finite = std::isfinite(sample) ? sample : 0.0f;
    state.pending.push_back(static_cast<int16_t>(std::lrintf(
        (std::max)(-1.0f, (std::min)(finite, 1.0f)) * 32767.0f)));
  }
  constexpr size_t kSamplesPerPacket = 1024 * 2;
  size_t consumed = 0;
  while (state.pending.size() - consumed >= kSamplesPerPacket) {
    Microsoft::WRL::ComPtr<IMFSample> input;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateSample(&input)) ||
        FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(kSamplesPerPacket * sizeof(int16_t)), &buffer))) return false;
    BYTE* bytes = nullptr;
    if (FAILED(buffer->Lock(&bytes, nullptr, nullptr))) return false;
    std::memcpy(bytes, state.pending.data() + consumed, kSamplesPerPacket * sizeof(int16_t));
    buffer->Unlock();
    if (FAILED(buffer->SetCurrentLength(static_cast<DWORD>(kSamplesPerPacket * sizeof(int16_t)))) ||
        FAILED(input->AddBuffer(buffer.Get()))) return false;
    const int64_t pts = state.nextSample * 10000000 / 48000;
    if (FAILED(input->SetSampleTime(pts)) ||
        FAILED(input->SetSampleDuration(1024LL * 10000000 / 48000))) return false;
    HRESULT hr = state.transform->ProcessInput(0, input.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      if (!state.drain(packets)) return false;
      hr = state.transform->ProcessInput(0, input.Get(), 0);
    }
    if (FAILED(hr) || !state.drain(packets)) return false;
    state.nextSample += 1024;
    consumed += kSamplesPerPacket;
  }
  state.pending.erase(state.pending.begin(), state.pending.begin() + consumed);
  return true;
#else
  (void)pcm; (void)channels; (void)sampleRate;
  return false;
#endif
}

}  // namespace corevideo::modules
