#include "modules/Interfaces.h"

#include <memory>

namespace corevideo::modules {

std::unique_ptr<IOutputSender> createSrtOutputSender() {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_SRT_OUTPUT
  // REQUIRES DEV MACHINE: Haivision libsrt integration belongs behind this gate.
  // The default alpha build continues to use SyntheticOutputSender, which already
  // exposes isolated "srt" sender diagnostics for program-output contract testing.
  return nullptr;
#else
  return nullptr;
#endif
}

}  // namespace corevideo::modules
