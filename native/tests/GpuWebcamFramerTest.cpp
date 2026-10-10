#include "modules/GpuWebcamFramer.h"
#include "modules/VirtualCameraFrame.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <limits>
#if defined(_WIN32)
#include <objbase.h>
#endif
using namespace corevideo::modules;
namespace {
struct ComScope {
#if defined(_WIN32)
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~ComScope() { if(SUCCEEDED(hr)) CoUninitialize(); }
#endif
};
constexpr int width=1920, height=1080;
std::shared_ptr<const std::vector<uint8_t>> cleanFrame() {
  auto bytes=std::make_shared<std::vector<uint8_t>>(width*height*3/2,170);
  std::fill(bytes->begin(),bytes->begin()+width*height,100);
  return bytes;
}
TEST(GpuWebcamFramer, PreservesAlphaAndCleanInputOrReportsUnavailable) {
  ComScope com;
  GpuWebcamFramer framer;
  const auto clean=cleanFrame();
  auto result=framer.apply(clean,width,height,false);
#if defined(_WIN32) && COREVIDEO_WITH_D3D11
  ASSERT_TRUE(result != nullptr);
  EXPECT_TRUE(framer.warning().empty());
  EXPECT_EQ(result->at(400*width+960),100); // completely clear center
  EXPECT_EQ(result->at(width*height+200*width+960),170); // unchanged clear chroma
  EXPECT_EQ(result->at(950*width+960),58); // original PNG alpha127 black: 100 ->58
  EXPECT_EQ(result->at(width*height+475*width+960),149); // neutral black chroma blend
  EXPECT_EQ(result->at(400*width+420),74); // opaque gray vertical guide
  EXPECT_EQ(clean->at(950*width+960),100); // recording/Program source remains clean
  EXPECT_EQ(clean->at(width*height+475*width+960),170);
#else
  EXPECT_TRUE(result == nullptr);
  EXPECT_FALSE(framer.warning().empty());
#endif
}
TEST(GpuWebcamFramer, GuidesRemainReadableAfterBackendMirror) {
  ComScope com;
  GpuWebcamFramer framer;
  auto variable=std::make_shared<std::vector<uint8_t>>(width*height*3/2);
  for(size_t i=0;i<variable->size();++i) (*variable)[i]=static_cast<uint8_t>(16+(i%width)*219/width);
  const std::shared_ptr<const std::vector<uint8_t>> clean=variable;
  auto reversed=*clean;
  mirrorNv12InPlace(reversed.data(),width,height);
  auto normal=framer.apply(std::make_shared<const std::vector<uint8_t>>(reversed),width,height,false);
  auto mirrored=framer.apply(clean,width,height,true);
#if defined(_WIN32) && COREVIDEO_WITH_D3D11
  ASSERT_TRUE(normal != nullptr); ASSERT_TRUE(mirrored != nullptr);
  auto final=*mirrored;
  mirrorNv12InPlace(final.data(),width,height);
  EXPECT_TRUE(final == *normal); // includes both labels and subsampled chroma
#else
  EXPECT_TRUE(normal == nullptr); EXPECT_TRUE(mirrored == nullptr);
#endif
}
TEST(GpuWebcamFramer, RejectsMalformedFrameWithoutModifyingIt) {
  ComScope com;
  GpuWebcamFramer framer;
  auto bytes=std::make_shared<const std::vector<uint8_t>>(7,77);
  EXPECT_TRUE(framer.apply(bytes,1920,1080,false) == nullptr);
  EXPECT_FALSE(framer.warning().empty());
  EXPECT_EQ(bytes->size(),7u); EXPECT_EQ(bytes->front(),77);
}
TEST(WebcamLoudnessOverlay, RequiresFullWindowAndFreshFiniteMeasurement) {
  WebcamLoudnessState state;
  const auto now=WebcamLoudnessState::Clock::time_point(std::chrono::seconds(10));
  EXPECT_FALSE(state.read(now).ready);
  state.update(-18.4,false,now); EXPECT_FALSE(state.read(now).ready);
  state.update(-18.4,true,now);
  EXPECT_TRUE(state.read(now+std::chrono::milliseconds(500)).ready);
  EXPECT_FALSE(state.read(now+std::chrono::milliseconds(501)).ready);
  state.update(std::numeric_limits<double>::quiet_NaN(),true,now);
  EXPECT_FALSE(state.read(now).ready);
  state.update(-120,true,now); EXPECT_TRUE(state.read(now).ready); // measured silence
}
TEST(GpuWebcamFramer, LiveLoudnessChangesOnlyCameraPanelAndMirrorsReadably) {
  ComScope com; GpuWebcamFramer framer;
  const auto clean=cleanFrame();
  const auto baseline=framer.apply(clean,width,height,false);
  const auto live=framer.apply(clean,width,height,false,{true,true,-18.4});
  const auto quiet=framer.apply(clean,width,height,false,{true,true,-30});
  const auto stale=framer.apply(clean,width,height,false,{true,false,-18.4});
#if defined(_WIN32) && COREVIDEO_WITH_D3D11
  ASSERT_TRUE(baseline!=nullptr); ASSERT_TRUE(live!=nullptr);
  ASSERT_TRUE(quiet!=nullptr); ASSERT_TRUE(stale!=nullptr);
  EXPECT_EQ((*live)[981*width+44],235); // first minus glyph
  EXPECT_TRUE(*live!=*quiet); EXPECT_TRUE(*live!=*stale);
  for(int y=0;y<height;++y) for(int x=0;x<width;++x)
    if(x<32 || x>=392 || y<932 || y>=1040)
      ASSERT_EQ((*live)[y*width+x],(*baseline)[y*width+x]);
  EXPECT_EQ((*clean)[981*width+44],100);
  const auto mirrored=framer.apply(clean,width,height,true,{true,true,-18.4});
  ASSERT_TRUE(mirrored!=nullptr);
  auto final=*mirrored; mirrorNv12InPlace(final.data(),width,height);
  EXPECT_TRUE(final==*live);
#else
  EXPECT_TRUE(live==nullptr);
#endif
}
TEST(GpuWebcamFramer, Warm1080pProcessingReportsCost) {
#if defined(_WIN32) && COREVIDEO_WITH_D3D11
  ComScope com;
  GpuWebcamFramer framer;
  const auto clean=cleanFrame();
  ASSERT_TRUE(framer.apply(clean,width,height,false) != nullptr);
  double worst=0,total=0;
  for(int i=0;i<120;++i) {
    const auto start=std::chrono::steady_clock::now();
    ASSERT_TRUE(framer.apply(clean,width,height,(i&1)!=0,{true,true,-18.4+(i%10)*.1}) != nullptr);
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    total+=ms; if(ms>worst) worst=ms;
  }
  std::printf("OH Framer + LUFS warm 1080p GPU upload/blend/readback: mean %.3f ms, worst %.3f ms (120 frames; not receiver cadence proof)\n",total/120,worst);
#endif
}
}
