#pragma once

// The single NV12 video stream of the virtual-camera media source
// (docs/virtual-camera-spec.md V2b, modeled on smourier/VCamSample). Implements
// IMFMediaStream over an MF event queue; on RequestSample it produces one NV12
// sample from the shared-memory slot (or a standby slate when the core is not
// publishing), timestamped on a fixed frame cadence.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <cstdint>
#include <mutex>
#include <vector>

#include "SharedFrameReader.h"

namespace corevideo::virtualcam {

class MediaSource;  // forward

class MediaStream
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          // Expose the base IMFMediaEventGenerator to QueryInterface too.
          Microsoft::WRL::ChainInterfaces<IMFMediaStream, IMFMediaEventGenerator>> {
 public:
  MediaStream() = default;

  HRESULT RuntimeClassInitialize(MediaSource* source, IMFStreamDescriptor* descriptor,
                                 UINT32 width, UINT32 height, UINT32 fps);

  // IMFMediaEventGenerator
  IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
  IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
  IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
  IFACEMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                            const PROPVARIANT* value) override;

  // IMFMediaStream
  IFACEMETHODIMP GetMediaSource(IMFMediaSource** source) override;
  IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
  IFACEMETHODIMP RequestSample(IUnknown* token) override;

  HRESULT Start();
  HRESULT Stop();
  void Shutdown();

 private:
  HRESULT CreateSample(IUnknown* token, IMFSample** sample);
  HRESULT FillFromSharedMemoryOrSlate(BYTE* dst, DWORD dstLen);

  Microsoft::WRL::ComPtr<IMFMediaEventQueue> events_;
  Microsoft::WRL::ComPtr<IMFStreamDescriptor> descriptor_;
  Microsoft::WRL::ComPtr<IMFMediaSource> source_;  // weak-ish: parent owns us
  SharedFrameReader reader_;
  std::mutex mutex_;
  std::vector<std::uint8_t> scratch_;
  // Last complete frame we served, held so a transient read miss (seqlock
  // collision / a tick the core didn't publish) re-serves the previous frame
  // instead of flashing the standby slate. Only after a sustained absence do we
  // fall back to the slate.
  std::vector<std::uint8_t> lastGood_;
  int missStreak_ = 0;
  UINT32 width_ = 1920;
  UINT32 height_ = 1080;
  UINT32 fps_ = 60;
  LONGLONG nextPts_ = 0;
  LONGLONG frameDuration_ = 166667;  // 100ns units, set from fps (60fps default)
  bool running_ = false;
  bool shutdown_ = false;
};

}  // namespace corevideo::virtualcam
