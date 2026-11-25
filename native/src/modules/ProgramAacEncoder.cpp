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
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace corevideo::modules {

#if defined(__APPLE__)
namespace {
// AudioToolbox emits raw AAC access units; the transport wants ADTS (the MF
// encoder's MF_MT_AAC_PAYLOAD_TYPE=1). AAC-LC, 48 kHz (index 3), stereo.
void appendAdtsHeader(std::vector<uint8_t>& out, size_t payloadBytes) {
  const size_t length = payloadBytes + 7;
  out.push_back(0xFF);
  out.push_back(0xF1);                                   // MPEG-4, layer 0, no CRC
  out.push_back(static_cast<uint8_t>((1 << 6) | (3 << 2) | (2 >> 2)));  // LC, 48 kHz, ch hi bit
  out.push_back(static_cast<uint8_t>(((2 & 3) << 6) | ((length >> 11) & 3)));
  out.push_back(static_cast<uint8_t>((length >> 3) & 0xFF));
  out.push_back(static_cast<uint8_t>(((length & 7) << 5) | 0x1F));
  out.push_back(0xFC);                                   // fullness 0x7FF, one raw block
}
constexpr OSStatus kNoMoreInput = 'cvNo';
}  // namespace
#endif

struct ProgramAacEncoder::Impl {
  bool started = false;
  int64_t nextSample = 0;
  std::vector<int16_t> pending;
  std::vector<float> pendingFloat;  // Apple: interleaved, not yet a whole 1024-frame block
#if defined(__APPLE__)
  AudioConverterRef converter = nullptr;
  int64_t primingFrames = 0;      // kAudioConverterPrimeInfo.leadingFrames (2112 on Apple)
  int64_t packetsOut = 0;         // every packet the converter produced this session
  std::vector<float> feed;        // interleaved Float32 handed to the input proc
  size_t feedOffsetFrames = 0;
  std::vector<uint8_t> scratch;

  static OSStatus input(AudioConverterRef, UInt32* ioPackets, AudioBufferList* io,
                        AudioStreamPacketDescription**, void* user) {
    auto* self = static_cast<Impl*>(user);
    const size_t totalFrames = self->feed.size() / 2;
    const size_t available = totalFrames - self->feedOffsetFrames;
    if (available == 0) {
      *ioPackets = 0;
      io->mBuffers[0].mDataByteSize = 0;
      return kNoMoreInput;  // ends this FillComplexBuffer call; not an error
    }
    const size_t frames = std::min<size_t>(available, *ioPackets);
    io->mBuffers[0].mData = self->feed.data() + self->feedOffsetFrames * 2;
    io->mBuffers[0].mDataByteSize = static_cast<UInt32>(frames * 2 * sizeof(float));
    io->mBuffers[0].mNumberChannels = 2;
    *ioPackets = static_cast<UInt32>(frames);
    self->feedOffsetFrames += frames;
    return noErr;
  }
#endif
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
#elif defined(__APPLE__)
  // Phase 4 O5 (D4): AudioToolbox AAC-LC, 48 kHz stereo, 160 kbps (the MF
  // encoder's 20000 bytes/s), one 1024-frame packet at a time, ADTS-framed.
  auto& state = *impl_;
  AudioStreamBasicDescription in{};
  in.mSampleRate = 48000;
  in.mFormatID = kAudioFormatLinearPCM;
  in.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  in.mChannelsPerFrame = 2;
  in.mBitsPerChannel = 32;
  in.mFramesPerPacket = 1;
  in.mBytesPerFrame = 8;
  in.mBytesPerPacket = 8;
  AudioStreamBasicDescription out{};
  out.mSampleRate = 48000;
  out.mFormatID = kAudioFormatMPEG4AAC;
  out.mFormatFlags = kMPEG4Object_AAC_LC;
  out.mChannelsPerFrame = 2;
  out.mFramesPerPacket = 1024;
  if (AudioConverterNew(&in, &out, &state.converter) != noErr || !state.converter) { stop(); return false; }
  UInt32 bitrate = 160000;
  if (AudioConverterSetProperty(state.converter, kAudioConverterEncodeBitRate, sizeof(bitrate), &bitrate) != noErr) {
    stop();
    return false;
  }
  AudioConverterPrimeInfo prime{};
  UInt32 primeSize = sizeof(prime);
  state.primingFrames = AudioConverterGetProperty(state.converter, kAudioConverterPrimeInfo, &primeSize, &prime) == noErr
      ? static_cast<int64_t>(prime.leadingFrames) : 0;
  UInt32 maxPacket = 0;
  UInt32 maxSize = sizeof(maxPacket);
  if (AudioConverterGetProperty(state.converter, kAudioConverterPropertyMaximumOutputPacketSize, &maxSize, &maxPacket) != noErr ||
      maxPacket == 0) maxPacket = 1536;
  state.scratch.assign(maxPacket, 0);
  state.started = true;
  ::corevideo::core::nativeLogf("[program-aac] started sampleRate=48000 channels=2 bitrateKbps=160 priming=%lld (AudioToolbox)\n",
                                static_cast<long long>(state.primingFrames));
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
#if defined(__APPLE__)
  if (state.converter) { AudioConverterDispose(state.converter); state.converter = nullptr; }
  state.packetsOut = 0;
  state.primingFrames = 0;
  state.feed.clear();
  state.feedOffsetFrames = 0;
#endif
  state.started = false;
  state.nextSample = 0;
  state.pending.clear();
  state.pendingFloat.clear();
}

bool ProgramAacEncoder::running() const { return impl_->started; }

int64_t ProgramAacEncoder::acceptedSamples() const {
  return impl_->nextSample + static_cast<int64_t>(impl_->pending.size() / 2) +
         static_cast<int64_t>(impl_->pendingFloat.size() / 2);
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
#elif defined(__APPLE__)
  auto& state = *impl_;
  if (!state.started || channels != 2 || sampleRate != 48000 || pcm.size() % 2) return false;
  // Hand whole 1024-frame blocks to the converter; keep the remainder pending
  // so acceptedSamples() means the same as on Windows.
  state.feed.clear();
  state.feedOffsetFrames = 0;
  for (const float sample : pcm) {
    const float finite = std::isfinite(sample) ? sample : 0.0f;
    state.pendingFloat.push_back((std::max)(-1.0f, (std::min)(finite, 1.0f)));
  }
  const size_t blocks = (state.pendingFloat.size() / 2) / 1024;
  if (blocks == 0) return true;
  const size_t handed = blocks * 1024 * 2;
  state.feed.assign(state.pendingFloat.begin(), state.pendingFloat.begin() + static_cast<std::ptrdiff_t>(handed));
  state.pendingFloat.erase(state.pendingFloat.begin(), state.pendingFloat.begin() + static_cast<std::ptrdiff_t>(handed));
  for (;;) {
    AudioBufferList outList{};
    outList.mNumberBuffers = 1;
    outList.mBuffers[0].mNumberChannels = 2;
    outList.mBuffers[0].mDataByteSize = static_cast<UInt32>(state.scratch.size());
    outList.mBuffers[0].mData = state.scratch.data();
    UInt32 produced = 1;
    AudioStreamPacketDescription description{};
    const OSStatus st = AudioConverterFillComplexBuffer(state.converter, &Impl::input, &state, &produced, &outList,
                                                        &description);
    if (st != noErr && st != kNoMoreInput) return false;
    if (produced == 0) break;
    const auto* bytes = static_cast<const uint8_t*>(outList.mBuffers[0].mData) + description.mStartOffset;
    const size_t size = description.mDataByteSize ? description.mDataByteSize : outList.mBuffers[0].mDataByteSize;
    // Packet n carries input samples starting at n*1024 - priming: the
    // converter's leading frames are subtracted so sampleIndex counts INPUT
    // samples (the MF path's meaning). Packets wholly before sample 0 hold only
    // priming and are dropped; 2112 is not a multiple of 1024, so the first
    // kept packet starts at sample 960, not exactly 0.
    const int64_t sampleIndex = state.packetsOut * 1024 - state.primingFrames;
    ++state.packetsOut;
    if (sampleIndex >= 0) {
      ProgramAacPacket packet;
      packet.adts.reserve(size + 7);
      appendAdtsHeader(packet.adts, size);
      packet.adts.insert(packet.adts.end(), bytes, bytes + size);
      packet.sampleIndex = sampleIndex;
      packet.pts100ns = sampleIndex * 10000000 / 48000;
      packets.push_back(std::move(packet));
    }
    if (st == kNoMoreInput) break;
  }
  state.nextSample += static_cast<int64_t>(blocks * 1024);
  return true;
#else
  (void)pcm; (void)channels; (void)sampleRate;
  return false;
#endif
}

}  // namespace corevideo::modules
