#include "modules/Interfaces.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER

#include <mfapi.h>

#include <memory>
#include <utility>

namespace corevideo::modules {
namespace {

class MediaFoundationEncoderSink final : public IEncoderSink {
 public:
  ~MediaFoundationEncoderSink() override { MFShutdown(); }

  OutputSession start(const std::vector<std::string>& destinations, const std::vector<std::string>& isoParticipantIds) override {
    session_.active = true;
    session_.destinations = destinations;
    session_.isoParticipantIds = isoParticipantIds;
    session_.encoderName = "media-foundation";
    session_.codec = "h264";
    session_.targetBitrateMbps = 18;
    session_.hardwareAccelerated = true;
    return session_;
  }

  void submit(const ProgramFrame&) override {
    if (session_.active) {
      ++session_.encodedFrameCount;
    }
  }

  OutputSession session() const override { return session_; }

 private:
  OutputSession session_;
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
