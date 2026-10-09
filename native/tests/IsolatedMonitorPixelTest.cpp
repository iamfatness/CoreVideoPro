#include "modules/Interfaces.h"
#include "modules/DeliveryCounterPattern.h"
#include "compositor/AdvancedGrade.h"
#include "core/ComApartmentLifetime.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <future>
#include <algorithm>

#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "compositor/ComPtrLite.h"
#include "modules/D3DVideoFrame.h"
#include "modules/CpuSourcePreparation.h"

namespace {
using namespace corevideo::modules;

std::unique_ptr<ICompositor> isolatedCompositor() {
  const char* raw = std::getenv("COREVIDEO_ISOLATE_MONITORS");
  const std::string previous = raw ? raw : "";
  _putenv_s("COREVIDEO_ISOLATE_MONITORS", "1");
  auto result = createD3D11Compositor();
  _putenv_s("COREVIDEO_ISOLATE_MONITORS", previous.c_str());
  return result;
}

// Independently opens and consumes the exported pixels, rather than trusting
// the job's metadata or the compositor's submission counter.
template <typename Texture>
uint32_t consumeCenter(const Texture& exported, float x = .5f) {
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      device.put(), nullptr, context.put()))) return 0;
  const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(
      std::stoull(exported.sharedHandleHex, nullptr, 16)));
  ComPtrLite<ID3D11Texture2D> texture, staging;
  ComPtrLite<IDXGIKeyedMutex> key;
  if (FAILED(device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put()))) ||
      FAILED(texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(key.put())))) return 0;
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put())) || key->AcquireSync(1, 1000) != S_OK) return 0;
  context->CopyResource(staging.get(), texture.get());
  context->Flush();
  key->ReleaseSync(0);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return 0;
  const auto* p = static_cast<const uint8_t*>(mapped.pData) + (desc.Height / 2) * mapped.RowPitch + static_cast<size_t>(desc.Width * x) * 4;
  const uint32_t pixel = (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
  context->Unmap(staging.get(), 0);
  return pixel;
}

MonitorRenderRequest requestAtSize(int size) {
  MonitorRenderRequest request;
  request.previewActive = request.multiviewActive = true;
  request.previewPlan.width = request.previewPlan.height = size;
  request.previewPlan.skipCpuReadback = true;
  CompositorRenderPlanLayer layer;
  layer.kind = "participant-video"; layer.participantId = "test";
  layer.sourceId = "test"; layer.layerId = "monitor:test";
  layer.rect = {0, 0, 1, 1}; layer.borderStyle = "none";
  request.previewPlan.layers.push_back(layer);
  request.multiviewPlan = request.programPlan = request.previewPlan;
  VideoFrame frame;
  frame.participantId = "test"; frame.frameId = 1;
  frame.width = frame.pixelWidth = frame.naturalWidth = 64;
  frame.height = frame.pixelHeight = frame.naturalHeight = 64;
  frame.pixelStride = 256;
  auto bytes = std::make_shared<std::vector<uint8_t>>(64 * 64 * 4);
  for (size_t i = 0; i < bytes->size(); i += 4) {
    (*bytes)[i] = 33; (*bytes)[i + 1] = 99; (*bytes)[i + 2] = 177; (*bytes)[i + 3] = 255;
  }
  frame.pixels = bytes;
  request.frames.push_back(frame);
  return request;
}
}


namespace {
GradePreviewSurface awaitGrade(ICompositor& compositor, MonitorRenderRequest request, const std::string& id, int64_t revision) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < end) {
    compositor.submitGradePreviews(request);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto result = compositor.latestGradePreviews();
    if (!result) continue;
    for (const auto& surface : result->gradePreviews)
      if (surface.demand.instanceId == id && surface.demand.revision == revision &&
          !surface.texture.sharedHandleHex.empty()) return surface;
  }
  return {};
}
uint32_t previewCenter(const ProgramFrame& frame) {
  const auto& bytes = frame.preview.bgra;
  const auto at = ((frame.preview.height / 2) * frame.preview.width + frame.preview.width / 2) * 4;
  if (bytes.size() < static_cast<size_t>(at + 4)) return 0;
  return (uint32_t(bytes[at+3]) << 24) | (uint32_t(bytes[at+2]) << 16) | (uint32_t(bytes[at+1]) << 8) | bytes[at];
}
void gradePixelParity(bool i420, bool advanced = false, bool fullRange = true, bool bt601 = false) {
  auto compositor = isolatedCompositor(); ASSERT_TRUE(compositor != nullptr);
  ASSERT_TRUE(compositor->supportsGradePreview());
  auto request = requestAtSize(64); request.programPlan.skipCpuReadback = false;
  auto& source = request.frames.front(); source.sourceEpoch = 8; source.captureTimestamp100ns = 1234;
  if (i420) {
    source.pixels.reset(); source.pixelStride = 0;
    source.i420 = std::make_shared<std::vector<uint8_t>>(64*64*3/2, 110);
    source.i420Width = source.i420Height = 64; source.i420FullRange=fullRange;source.i420Bt601=bt601;
  }
  const auto neutral = previewCenter(compositor->render(request.programPlan, request.frames)); ASSERT_NE(neutral, 0u);
  request.previewActive = request.multiviewActive = false;
  CompositorColorGrade warm; warm.lut = "warm-film";
  if(advanced) {
    const corevideo::rpc::Json line=corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{{"x",0},{"y",0}},corevideo::rpc::Json::Object{{"x",1},{"y",1}}};
    ASSERT_TRUE(readAdvancedGrade(corevideo::rpc::Json::Object{{"version",2},{"colorSpace","rec709-sdr"},
        {"operations",corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{{"id","primary"},{"kind","primaries"},{"exposureStops",.5},
        {"curves",corevideo::rpc::Json::Array{line,line,line,line}}}}}},warm.advanced));
  }
  request.gradePreviews.push_back({"draft", "test", 1, warm});
  auto grade = awaitGrade(*compositor, request, "draft", 1);
  ASSERT_EQ(grade.demand.revision, 1); EXPECT_EQ(grade.sourceEpoch, 8u);
  EXPECT_EQ(grade.sourceFrameId, source.frameId); EXPECT_EQ(grade.captureTimestamp100ns, 1234);
  const auto pixel = consumeCenter(grade.texture); ASSERT_NE(pixel, 0u); EXPECT_NE(pixel, neutral);
  // Editing a private preview leaves Program unchanged.
  EXPECT_EQ(previewCenter(compositor->render(request.programPlan, request.frames)), neutral);
  request.programPlan.layers[0].hasColorGrade = true; request.programPlan.layers[0].colorGrade = warm;
  const auto applied = previewCenter(compositor->render(request.programPlan, request.frames));
  for (int shift : {0, 8, 16}) EXPECT_LE(std::abs(int((pixel >> shift) & 255) - int((applied >> shift) & 255)), 1);
  // A held source is regraded without changing its source frame identity.
  request.gradePreviews[0].revision = 2; request.gradePreviews[0].grade = {};
  grade = awaitGrade(*compositor, request, "draft", 2);
  ASSERT_EQ(grade.demand.revision, 2); EXPECT_EQ(grade.sourceFrameId, source.frameId);
  const auto reset = consumeCenter(grade.texture);
  for (int shift : {0, 8, 16}) EXPECT_LE(std::abs(int((reset >> shift) & 255) - int((neutral >> shift) & 255)), 1);
  // A second editor has its own export; its churn cannot prune the held first editor.
  request.gradePreviews.push_back({"other", "test", 1, warm});
  auto other = awaitGrade(*compositor, request, "other", 1);
  ASSERT_NE(other.texture.sharedHandleHex, grade.texture.sharedHandleHex);
  for (int revision = 2; revision < 38; ++revision) {
    request.gradePreviews[1].revision = revision;
    ASSERT_EQ(awaitGrade(*compositor, request, "other", revision).demand.revision, revision);
  }
  ASSERT_EQ(awaitGrade(*compositor, request, "draft", 2).demand.instanceId, "draft");
  request.frames.clear();
  grade = awaitGrade(*compositor, request, "draft", 2);
  ASSERT_EQ(grade.status, "held"); EXPECT_EQ(grade.sourceEpoch, 8u);
  compositor->submitGradePreviews({});
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < end) {
    auto latest = compositor->latestGradePreviews();
    if (latest && latest->gradePreviews.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(compositor->latestGradePreviews()->gradePreviews.empty());
  EXPECT_EQ(compositor->gradePreviewDiagnostics().retainedInputs, 0u);
}
}
TEST(GradePreviewPixels, BgraDraftMatchesNativeProgramAndRetiresPrivateEditors) { gradePixelParity(false); }
TEST(GradePreviewPixels, I420DraftMatchesNativeProgramAndRetiresPrivateEditors) { gradePixelParity(true); }
TEST(GradePreviewPixels, AdvancedBgraDraftMatchesProgramAndHeldEdits) { gradePixelParity(false,true); }
TEST(GradePreviewPixels, AdvancedI420DraftMatchesProgramAndHeldEdits) { for(bool full:{false,true}) for(bool bt601:{false,true}) gradePixelParity(true,true,full,bt601); }
TEST(GradePreviewPixels, AdvancedCurvesPreserveChannelsAndNativeScopesHaveIndependentTap) {
  auto compositor=isolatedCompositor(); ASSERT_TRUE(compositor);
  auto request=requestAtSize(64); request.programPlan.skipCpuReadback=false;
  const auto points=[](float end) {return corevideo::rpc::Json::Array{
    corevideo::rpc::Json::Object{{"x",0},{"y",0}},corevideo::rpc::Json::Object{{"x",1},{"y",double(end)}}};};
  const corevideo::rpc::Json doc=corevideo::rpc::Json::Object{{"version",2},{"colorSpace","rec709-sdr"},{"operations",corevideo::rpc::Json::Array{
    corevideo::rpc::Json::Object{{"id","red-half"},{"kind","curves"},{"curves",corevideo::rpc::Json::Array{points(1),points(.5f),points(1),points(1)}}}}}};
  CompositorColorGrade grade;ASSERT_TRUE(readAdvancedGrade(doc,grade.advanced));
  request.programPlan.layers[0].hasColorGrade=true; request.programPlan.layers[0].colorGrade=grade;
  const auto pixel=previewCenter(compositor->render(request.programPlan,request.frames));
  EXPECT_LE(std::abs(int((pixel>>16)&255)-89),1); EXPECT_EQ((pixel>>8)&255,99u); EXPECT_EQ(pixel&255,33u);
  GradePreviewDemand demand{"advanced","test",1,grade};demand.scopesEnabled=true;request.gradePreviews.push_back(demand);
  GradePreviewSurface surface; const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
  do {surface=awaitGrade(*compositor,request,"advanced",1); if(!surface.scopes.texture.sharedHandleHex.empty()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));} while(std::chrono::steady_clock::now()<end);
  ASSERT_FALSE(surface.scopes.texture.sharedHandleHex.empty()); EXPECT_EQ(surface.scopes.sourceFrameId,request.frames[0].frameId);
  const auto monitor=consumeCenter(surface.texture);for(int shift:{0,8,16}) EXPECT_LE(std::abs(int((monitor>>shift)&255)-int((pixel>>shift)&255)),1);
  const auto redCode=(monitor>>16)&255;
  auto histogram=consumeCenter(surface.scopes.texture,(float(redCode)+.5f)/256.f/3.f);
  EXPECT_GT((histogram>>16)&255,128u);
  request.gradePreviews[0].revision=2;request.gradePreviews[0].scopesOriginal=true;
  do {surface=awaitGrade(*compositor,request,"advanced",2);if(surface.scopes.revision==2 && !surface.scopes.texture.sharedHandleHex.empty()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));} while(std::chrono::steady_clock::now()<end);
  ASSERT_EQ(surface.scopes.revision,2); EXPECT_TRUE(surface.scopes.original);
  histogram=consumeCenter(surface.scopes.texture,(177.5f)/256.f/3.f);EXPECT_GT((histogram>>16)&255,128u);
  EXPECT_EQ(previewCenter(compositor->render(request.programPlan,request.frames)),pixel);
  compositor->submitGradePreviews({});
}

TEST(AdvancedGradePixels, ExposurePrimariesStackOrderLutDomainAndGlobalIntensityHaveIndependentExpectedValues) {
  auto compositor=isolatedCompositor(); ASSERT_TRUE(compositor);
  auto request=requestAtSize(64);request.programPlan.skipCpuReadback=false;
  using J=corevideo::rpc::Json;
  const J::Array line{J::Object{{"x",0},{"y",0}},J::Object{{"x",1},{"y",1}}};
  const J::Array curves{line,line,line,line};
  auto primary=J::Object{{"id","primary"},{"kind","primaries"},{"curves",curves},{"exposureStops",1}};
  auto half=J::Object{{"id","half"},{"kind","curves"},{"curves",J::Array{line,
      J::Array{J::Object{{"x",0},{"y",0}},J::Object{{"x",1},{"y",.5}}},line,line}}};
  auto render=[&](J::Array operations,float intensity=1,float legacyExposure=0,bool bypass=false) {
    CompositorColorGrade grade; grade.exposure=legacyExposure;
    ASSERT_TRUE(readAdvancedGrade(J::Object{{"version",2},{"colorSpace","rec709-sdr"},{"operations",operations},
        {"intensity",double(intensity)},{"bypass",bypass}},grade.advanced));
    request.programPlan.layers[0].hasColorGrade=true; request.programPlan.layers[0].colorGrade=grade;
    return previewCenter(compositor->render(request.programPlan,request.frames));
  };
  // Independent Rec.709 transfer reference, never calls the production translator.
  const auto exposure=[](double encoded) {double l=encoded<.081?encoded/4.5:std::pow((encoded+.099)/1.099,1/.45);
    l*=2;return std::clamp(l<.018?4.5*l:1.099*std::pow(l,.45)-.099,0.,1.);};
  const auto code=[](uint32_t p,int shift){return int((p>>shift)&255);};
  const auto check=[&](uint32_t pixel,int shift,double expected){EXPECT_LE(std::abs(code(pixel,shift)-int(std::round(expected*255))),1);};
  auto pixel=render({primary});for(auto [shift,value]:{std::pair{0,33},std::pair{8,99},std::pair{16,177}}) check(pixel,shift,exposure(value/255.));
  pixel=render({primary,half});check(pixel,16,exposure(177/255.)*.5);
  const auto reverse=render({half,primary});check(reverse,16,exposure(177/255.*.5)); EXPECT_NE(code(pixel,16),code(reverse,16));
  primary["exposureStops"]=0;primary["saturation"]=0;
  pixel=render({primary}); const double luma=(.2126*177+.7152*99+.0722*33)/255.;for(int shift:{0,8,16}) check(pixel,shift,luma);
  // An empty advanced stack still controls legacy intensity and bypass.
  pixel=render({},0,20);check(pixel,16,177/255.);check(pixel,8,99/255.);
  pixel=render({},1,20,true);check(pixel,16,177/255.);check(pixel,8,99/255.);
  // 2^3 RGB-red-fastest cube, explicit domain, asymmetric output catches axis swaps.
  std::string text="LUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n";
  for(int b=0;b<2;++b) for(int g=0;g<2;++g) for(int r=0;r<2;++r) text+=std::to_string(b)+" "+std::to_string(r)+" "+std::to_string(g)+"\n";
  const auto digest=hashing::sha256(reinterpret_cast<const uint8_t*>(text.data()),text.size());
  std::ostringstream hash;hash<<std::hex<<std::setfill('0');for(auto b:digest) hash<<std::setw(2)<<int(b);
  auto cube=parseGradeCube(text,hash.str());ASSERT_TRUE(cube);
  const J::Object lut{{"id","cube"},{"kind","cube"},{"curves",curves},{"cubeText",text},{"cubeSha256",cube->hash}};
  pixel=render({lut});check(pixel,16,33/255./2);check(pixel,8,177/255./2);check(pixel,0,99/255./2);
}

TEST(PreparedSourcePixels, BgraGpuViewKeepsCpuI420ForIsoWithoutSelectingYuvShader) {
  const char* gpuRaw = std::getenv("COREVIDEO_GPU_CAPTURE");
  const std::string gpuPrevious = gpuRaw ? gpuRaw : "";
  const char* monitorRaw = std::getenv("COREVIDEO_ISOLATE_MONITORS");
  const std::string monitorPrevious = monitorRaw ? monitorRaw : "";
  _putenv_s("COREVIDEO_GPU_CAPTURE", "1");
  _putenv_s("COREVIDEO_ISOLATE_MONITORS", "0");
  auto compositor = createD3D11Compositor();
  _putenv_s("COREVIDEO_GPU_CAPTURE", gpuPrevious.c_str());
  _putenv_s("COREVIDEO_ISOLATE_MONITORS", monitorPrevious.c_str());
  ASSERT_TRUE(compositor != nullptr);
  ComPtrLite<ID3D11Device> producer;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      producer.put(), nullptr, context.put())));
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtrLite<ID3D11Texture2D> texture;
  ComPtrLite<ID3D11RenderTargetView> target;
  ASSERT_TRUE(SUCCEEDED(producer->CreateTexture2D(&desc, nullptr, texture.put())));
  ASSERT_TRUE(SUCCEEDED(producer->CreateRenderTargetView(texture.get(), nullptr, target.put())));
  const float red[] = {201.f / 255, 0, 0, 1};
  context->ClearRenderTargetView(target.get(), red);
  D3DVideoFramePool pool;
  ASSERT_TRUE(pool.initialize(producer.get(), 64, 64, 1));
  const int slot = pool.beginCopy(context.get(), texture.get());
  ASSERT_GE(slot, 0); context->Flush();
  auto request = requestAtSize(64);
  auto& frame = request.frames.front();
  frame.pixels.reset(); frame.pixelWidth = frame.pixelHeight = frame.pixelStride = 0;
  // Deliberately different representations prove both shader selection and
  // export sizing use the admitted GPU view. CPU planes must remain available.
  auto cpu = std::make_shared<std::vector<uint8_t>>(128 * 128 * 3 / 2, 128);
  frame.i420 = cpu; frame.i420Width = frame.i420Height = 128;
  frame.sourceEpoch = 7; frame.frameId = 19; frame.captureTimestamp100ns = 123456;
  const auto readyBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!(frame.gpuPixels = pool.completed(context.get(), slot)) && std::chrono::steady_clock::now() < readyBy)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(frame.gpuPixels != nullptr);
  auto plan = request.programPlan; plan.skipCpuReadback = false;
  auto result = compositor->render(plan, request.frames);
  ASSERT_FALSE(result.preview.bgra.empty());
  const size_t center = (result.preview.height / 2 * result.preview.width + result.preview.width / 2) * 4;
  EXPECT_EQ(result.preview.bgra[center], 0);
  EXPECT_EQ(result.preview.bgra[center + 1], 0);
  EXPECT_NEAR(result.preview.bgra[center + 2], 201, 1);
  uint32_t exportedPixel = 0;
  const auto exportBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    result = compositor->render(plan, request.frames);
    if (!result.participantSharedTextures.empty()) {
      const auto& source = result.participantSharedTextures.front();
      EXPECT_EQ(source.width, 64); EXPECT_EQ(source.height, 64);
      ProgramFrameSharedTexture exported;
      exported.sharedHandleHex = source.sharedHandleHex;
      exportedPixel = consumeCenter(exported);
    }
    if (exportedPixel != 0xffc90000u) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (exportedPixel != 0xffc90000u && std::chrono::steady_clock::now() < exportBy);
  EXPECT_EQ(exportedPixel, 0xffc90000u);
  auto gpuOnly = frame;
  gpuOnly.i420.reset(); gpuOnly.i420Width = gpuOnly.i420Height = 0;
  ++gpuOnly.frameId;
  result = compositor->render(plan, {gpuOnly});
  ASSERT_FALSE(result.preview.bgra.empty());
  EXPECT_NEAR(result.preview.bgra[center + 2], 201, 1);
  exportedPixel = 0;
  const auto gpuOnlyBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    result = compositor->render(plan, {gpuOnly});
    if (!result.participantSharedTextures.empty()) {
      const auto& source = result.participantSharedTextures.front();
      EXPECT_EQ(source.width, 64); EXPECT_EQ(source.height, 64);
      ProgramFrameSharedTexture exported;
      exported.sharedHandleHex = source.sharedHandleHex;
      exportedPixel = consumeCenter(exported);
    }
    if (exportedPixel != 0xffc90000u) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (exportedPixel != 0xffc90000u && std::chrono::steady_clock::now() < gpuOnlyBy);
  EXPECT_EQ(exportedPixel, 0xffc90000u);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(frame.i420, cpu); EXPECT_EQ(frame.sourceEpoch, 7u);
  EXPECT_EQ(frame.frameId, 19); EXPECT_EQ(frame.captureTimestamp100ns, 123456);
  // An image without an imported consumer view must keep the legacy CPU
  // fallback usable. The shader follows what was actually admitted.
  auto unavailable = std::make_shared<GpuVideoFrame>();
  unavailable->width = unavailable->height = 64;
  auto fallback = frame; fallback.gpuPixels = unavailable; ++fallback.frameId;
  result = compositor->render(plan, {fallback});
  ASSERT_FALSE(result.preview.bgra.empty());
  for (size_t channel = 0; channel < 3; ++channel)
    EXPECT_NEAR(result.preview.bgra[center + channel], 128, 12);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 1u);
}

TEST(IsolatedMonitorPixels, UnavailableInputHoldsItsPixelsWhileOtherInputsAdvanceAndRecover) {
  const char* raw = std::getenv("COREVIDEO_GPU_CAPTURE"); const std::string previous = raw ? raw : "";
  _putenv_s("COREVIDEO_GPU_CAPTURE", "1");
  auto compositor = isolatedCompositor(); _putenv_s("COREVIDEO_GPU_CAPTURE", previous.c_str());
  ASSERT_TRUE(compositor != nullptr);
  const auto readyBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (compositor->monitorDiagnostics().readiness == "starting" && std::chrono::steady_clock::now() < readyBy)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_EQ(compositor->monitorDiagnostics().readiness, "ready");
  auto request = requestAtSize(128);
  auto fault = request.frames.front(); fault.participantId = "fault"; fault.sourceEpoch = 1;
  auto bytes = std::make_shared<std::vector<uint8_t>>(64 * 64 * 4, 0);
  for (size_t i = 0; i < bytes->size(); i += 4) { (*bytes)[i] = 211; (*bytes)[i + 3] = 255; }
  ComPtrLite<ID3D11Device> producer; ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, producer.put(), nullptr, context.put())));
  D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtrLite<ID3D11Texture2D> source; ComPtrLite<ID3D11RenderTargetView> target;
  ASSERT_TRUE(SUCCEEDED(producer->CreateTexture2D(&desc, nullptr, source.put())));
  ASSERT_TRUE(SUCCEEDED(producer->CreateRenderTargetView(source.get(), nullptr, target.put())));
  const float blue[] = {0, 0, 211.f / 255, 1}; context->ClearRenderTargetView(target.get(), blue);
  D3DVideoFramePool privatePool; ASSERT_TRUE(privatePool.initialize(producer.get(), 64, 64, 9, true));
  const int slot = privatePool.beginCopy(context.get(), source.get()); ASSERT_GE(slot, 0); context->Flush();
  std::shared_ptr<const GpuVideoFrame> image;
  const auto copyBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!(image = privatePool.completed(context.get(), slot)) && std::chrono::steady_clock::now() < copyBy)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(image != nullptr);
  auto production = std::make_shared<GpuVideoFrame>(); production->width = production->height = 64; production->generation = 44;
  fault.pixels.reset(); fault.gpuPixels = production; fault.monitorGpuPixels = image;
  request.frames.push_back(fault);
  request.sourceExports.push_back({"fault", SourceMonitorConsumer::Inspector, "qa"});
  request.multiviewPlan.layers.front().rect = {0, 0, .5f, 1};
  auto layer = request.multiviewPlan.layers.front(); layer.participantId = layer.sourceId = "fault";
  layer.layerId = "monitor:fault"; layer.rect = {.5f, 0, .5f, 1};
  request.multiviewPlan.layers.push_back(layer);
  int64_t sequence = 0;
  const auto publish = [&](uint64_t held, uint64_t ready) {
    std::shared_ptr<const MonitorRenderResult> result;
    const auto firstSequence = sequence + 1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
      request.sequence = ++sequence; compositor->submitMonitors(request);
      std::this_thread::sleep_for(std::chrono::milliseconds(5)); result = compositor->latestMonitors();
    } while ((!result || result->heldInputs != held || result->readyInputs != ready ||
        result->sequence < request.sequence - 1 || result->preview.sharedHandleHex.empty() ||
        result->preview.frameNumber < firstSequence || result->multiview.frameNumber < firstSequence ||
        result->sources.empty() || result->sources.front().frameNumber == 0 ||
        result->multiview.sharedHandleHex.empty()) && std::chrono::steady_clock::now() < deadline);
    return result;
  };
  auto initial = publish(0, 2); ASSERT_TRUE(initial != nullptr);
  EXPECT_EQ(consumeCenter(initial->multiview, .75f), 0xff0000d3u);
  // Lose only the optional private copy, while production itself remains valid.
  request.frames[1].pixels.reset(); request.frames[1].monitorGpuPixels.reset(); request.frames[1].gpuPixels = production;
  request.frames[1].sourceEpoch = 2; request.frames[1].frameId = 22;
  auto green = std::make_shared<std::vector<uint8_t>>(64 * 64 * 4, 0);
  for (size_t i = 0; i < green->size(); i += 4) { (*green)[i + 1] = 201; (*green)[i + 3] = 255; }
  request.frames[0].pixels = green; request.frames[0].frameId = 2;
  auto mixed = publish(1, 1); ASSERT_TRUE(mixed != nullptr);
  EXPECT_EQ(mixed->heldInputs, 1u); EXPECT_EQ(mixed->readyInputs, 1u);
  EXPECT_EQ(consumeCenter(mixed->preview), 0xff00c900u);
  EXPECT_EQ(consumeCenter(mixed->multiview, .25f), 0xff00c900u);
  // Reading the same export twice requires a new publication/key transfer.
  mixed = publish(1, 1);
  EXPECT_EQ(consumeCenter(mixed->multiview, .75f), 0xff0000d3u);
  auto identity = std::find_if(mixed->inputs.begin(), mixed->inputs.end(), [](const auto& input) { return input.sourceId == "fault"; });
  ASSERT_TRUE(identity != mixed->inputs.end());
  EXPECT_EQ(identity->state, "held"); EXPECT_EQ(identity->sourceEpoch, 1u);
  EXPECT_EQ(identity->requestedEpoch, 2u); EXPECT_EQ(identity->frameId, 1);
  request.frames[0].pixels.reset(); request.frames[0].gpuPixels = production;
  auto allHeld = publish(2, 0); ASSERT_TRUE(allHeld != nullptr);
  EXPECT_EQ(allHeld->heldInputs, 2u); EXPECT_EQ(consumeCenter(allHeld->preview), 0xff00c900u);
  request.frames[0].gpuPixels.reset(); request.frames[0].pixels = green;
  request.frames[1].gpuPixels.reset(); request.frames[1].pixels = green;
  request.frames[1].frameId = 1; // reconnect can restart IDs; epoch must invalidate export dedup
  auto recovered = publish(0, 2); ASSERT_TRUE(recovered != nullptr);
  EXPECT_EQ(recovered->heldInputs, 0u); EXPECT_EQ(recovered->readyInputs, 2u);
  uint32_t sourcePixel = 0;
  const auto exportBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    recovered = publish(0, 2);
    if (!recovered->sources.empty()) {
      ProgramFrameSharedTexture exported; const auto& source = recovered->sources.front();
      exported.sharedHandleHex = source.sharedHandleHex; exported.width = source.width; exported.height = source.height;
      sourcePixel = consumeCenter(exported);
    }
  } while (sourcePixel != 0xff00c900u && std::chrono::steady_clock::now() < exportBy);
  EXPECT_EQ(sourcePixel, 0xff00c900u);
  EXPECT_EQ(compositor->monitorDiagnostics().failed, 0u);
  request = {}; request.sequence = ++sequence; compositor->submitMonitors(request);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (compositor->monitorDiagnostics().lastSequence != sequence && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(compositor->monitorDiagnostics().retainedInputs, 0u);
}

TEST(IsolatedMonitorPixels, IndependentDeviceReceivesPreviewAndMultiviewAcrossResizeAndRetirement) {
  auto compositor = isolatedCompositor();
  ASSERT_TRUE(compositor != nullptr);
  ASSERT_TRUE(compositor->hasIsolatedMonitors());
  int64_t sequence = 0;
  for (int size : {64, 96, 64}) {
    auto request = requestAtSize(size);
    std::shared_ptr<const MonitorRenderResult> result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
      request.sequence = ++sequence;
      compositor->submitMonitors(request);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      result = compositor->latestMonitors();
    } while ((!result || result->preview.width != size || result->multiview.width != size ||
              result->preview.sharedHandleHex.empty() || result->multiview.sharedHandleHex.empty()) &&
             std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(result != nullptr);
    ASSERT_FALSE(result->preview.sharedHandleHex.empty());
    ASSERT_FALSE(result->multiview.sharedHandleHex.empty());
    EXPECT_EQ(result->preview.width, size);
    EXPECT_EQ(result->multiview.width, size);
    EXPECT_EQ(consumeCenter(result->preview), 0xffb16321u);
    EXPECT_EQ(consumeCenter(result->multiview), 0xffb16321u);
    EXPECT_TRUE(result->sources.empty()); // composite demand does not export individual sources
  }
  MonitorRenderRequest retired;
  retired.sequence = ++sequence;
  compositor->submitMonitors(retired);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (compositor->latestMonitors()->sequence != sequence && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(compositor->latestMonitors()->sequence, sequence);
  EXPECT_TRUE(compositor->latestMonitors()->preview.sharedHandleHex.empty());
  EXPECT_TRUE(compositor->latestMonitors()->multiview.sharedHandleHex.empty());
  EXPECT_EQ(compositor->monitorDiagnostics().failed, 0u);
}

TEST(GpuCaptureIngress, IndependentProducerImageComposesWithoutCpuPixelsOrAnUpload) {
  const char* raw = std::getenv("COREVIDEO_GPU_CAPTURE");
  const std::string previous = raw ? raw : "";
  _putenv_s("COREVIDEO_GPU_CAPTURE", "1");
  auto compositor = isolatedCompositor();
  _putenv_s("COREVIDEO_GPU_CAPTURE", previous.c_str());
  ASSERT_TRUE(compositor != nullptr);
  ComPtrLite<ID3D11Device> producer;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      producer.put(), nullptr, context.put())));
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtrLite<ID3D11Texture2D> source;
  ComPtrLite<ID3D11RenderTargetView> target;
  ASSERT_TRUE(SUCCEEDED(producer->CreateTexture2D(&desc, nullptr, source.put())));
  ASSERT_TRUE(SUCCEEDED(producer->CreateRenderTargetView(source.get(), nullptr, target.put())));
  const float original[] = {177.f / 255, 99.f / 255, 33.f / 255, 1};
  context->ClearRenderTargetView(target.get(), original);
  D3DVideoFramePool pool;
  ASSERT_TRUE(pool.initialize(producer.get(), 64, 64, 1));
  const int slot = pool.beginCopy(context.get(), source.get());
  ASSERT_GE(slot, 0);
  context->Flush();
  std::shared_ptr<const GpuVideoFrame> image;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!(image = pool.completed(context.get(), slot)) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(image != nullptr);
  const float overwritten[] = {0, 0, 0, 1};
  context->ClearRenderTargetView(target.get(), overwritten);
  context->Flush();
  auto request = requestAtSize(64);
  auto frame = request.frames.front();
  frame.pixels.reset();
  frame.gpuPixels = image;
  request.programPlan.skipCpuReadback = false;
  const auto result = compositor->render(request.programPlan, {frame});
  ASSERT_FALSE(result.preview.bgra.empty());
  const auto offset = static_cast<size_t>((result.preview.height / 2) * result.preview.width + result.preview.width / 2) * 4;
  EXPECT_EQ(result.preview.bgra[offset], 33);
  EXPECT_EQ(result.preview.bgra[offset + 1], 99);
  EXPECT_EQ(result.preview.bgra[offset + 2], 177);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  // All three leases held => no writer may overwrite one to make room.
  std::vector<std::shared_ptr<const GpuVideoFrame>> held{image};
  for (int i = 0; i < 2; ++i) {
    const int next = pool.beginCopy(context.get(), source.get());
    ASSERT_GE(next, 0);
    context->Flush();
    std::shared_ptr<const GpuVideoFrame> lease;
    while (!(lease = pool.completed(context.get(), next)) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(lease != nullptr);
    held.push_back(std::move(lease));
  }
  EXPECT_EQ(pool.beginCopy(context.get(), source.get()), -1);
  held.pop_back();
  EXPECT_GE(pool.beginCopy(context.get(), source.get()), 0);
  context->Flush();

  // A separately colored private image proves the monitor actually samples its
  // branch, while a fake production lease must never survive monitor admission.
  const auto monitorDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  const auto monitorRegistered = [] {
    const auto consumers = D3DVideoConsumers::snapshot();
    return std::any_of(consumers.begin(), consumers.end(), [](const auto& consumer) { return consumer->monitor; });
  };
  while (!monitorRegistered() && std::chrono::steady_clock::now() < monitorDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(monitorRegistered());
  D3DVideoFramePool monitorPool;
  ASSERT_TRUE(monitorPool.initialize(producer.get(), 64, 64, 2, true));
  const float privateColor[] = {0, 1, 0, 1};
  context->ClearRenderTargetView(target.get(), privateColor);
  const int privateSlot = monitorPool.beginCopy(context.get(), source.get());
  ASSERT_GE(privateSlot, 0);
  context->Flush();
  std::shared_ptr<const GpuVideoFrame> privateImage;
  while (!(privateImage = monitorPool.completed(context.get(), privateSlot)) &&
      std::chrono::steady_clock::now() < monitorDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(privateImage != nullptr);
  auto productionOnly = std::make_shared<GpuVideoFrame>();
  productionOnly->width = productionOnly->height = 64;
  std::weak_ptr<const GpuVideoFrame> productionLease = productionOnly;
  auto monitorRequest = requestAtSize(64);
  monitorRequest.frames.front().pixels.reset();
  monitorRequest.frames.front().gpuPixels = std::move(productionOnly);
  monitorRequest.frames.front().monitorGpuPixels = privateImage;
  monitorRequest.sequence = 99;
  compositor->submitMonitors(std::move(monitorRequest));
  EXPECT_TRUE(productionLease.expired());
  std::shared_ptr<const MonitorRenderResult> monitorResult;
  do {
    monitorResult = compositor->latestMonitors();
    if (monitorResult && monitorResult->sequence == 99 && !monitorResult->preview.sharedHandleHex.empty()) break;
    auto again = requestAtSize(64);
    again.sequence = 99;
    again.frames.front().pixels.reset();
    again.frames.front().gpuPixels = image;
    again.frames.front().monitorGpuPixels = privateImage;
    compositor->submitMonitors(std::move(again));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < monitorDeadline);
  ASSERT_TRUE(monitorResult != nullptr);
  ASSERT_FALSE(monitorResult->preview.sharedHandleHex.empty());
  EXPECT_EQ(consumeCenter(monitorResult->preview), 0xff00ff00u);
}

TEST(IsolatedMonitorPixels, CameraIdentityBelongsToTheDeliveredNv12Packet) {
  const char* counter = std::getenv("COREVIDEO_QA_PROGRAM_COUNTER");
  const std::string previousCounter = counter ? counter : "";
  _putenv_s("COREVIDEO_QA_PROGRAM_COUNTER", "1");
  auto compositor = isolatedCompositor();
  _putenv_s("COREVIDEO_QA_PROGRAM_COUNTER", previousCounter.c_str());
  ASSERT_TRUE(compositor != nullptr);
  compositor->configureProgramBuffer(2);
  compositor->prepareProgramBuffer(1920, 1080);
  struct Observation {
    int64_t sequence = 0, deliveredAt = 0;
    std::shared_ptr<const std::vector<uint8_t>> pixels;
  } observed;
  std::promise<void> arrived;
  auto arrival = arrived.get_future();
  compositor->setIdentifiedVcamFrameSink([&](auto bytes, int, int, int64_t sequence, int64_t deliveredAt) {
    observed = {sequence, deliveredAt, std::move(bytes)};
    arrived.set_value();
  });
  auto request = requestAtSize(64);
  request.programPlan.width = 1920;
  request.programPlan.height = 1080;
  request.programPlan.fullProgramReadback = true;
  const auto anchor = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  compositor->setProgramProductionTiming(0,
      std::chrono::duration_cast<std::chrono::nanoseconds>(anchor.time_since_epoch()).count());
  const auto produced = compositor->render(request.programPlan, request.frames);
  ProgramFrame delivered;
  const bool received = compositor->takeDeliveredProgramFrame(delivered, 2000);
  const bool notified = arrival.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  compositor->setVcamFrameSink({}); // also clears the identified sink; callee teardown barrier
  ASSERT_TRUE(received);
  ASSERT_TRUE(notified);
  EXPECT_EQ(observed.sequence, produced.frameNumber);
  EXPECT_EQ(observed.sequence, delivered.frameNumber);
  EXPECT_EQ(observed.deliveredAt, delivered.deliveredAt100ns);
  EXPECT_GT(observed.deliveredAt, 0);
  EXPECT_TRUE(observed.pixels != nullptr);
  EXPECT_TRUE(observed.pixels == delivered.programNv12Shared);
  const auto decoded = decodeDeliveryCounter(observed.pixels->data(), observed.pixels->size(), 1920, 1080, 1920);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, static_cast<uint32_t>(observed.sequence));
}

#if COREVIDEO_WITH_WGC
namespace {
// Own a small non-activating moving window: WGC need not produce another frame
// for a static desktop. This generates changes without operator interaction.
class WgcTestMotion {
 public:
  WgcTestMotion() {
    std::promise<bool> initialized; auto ready = initialized.get_future();
    thread_ = std::thread([this, initialized = std::move(initialized)]() mutable {
      POINT origin{20, 20};
      EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFO info{}; info.cbSize = sizeof(info);
        if (GetMonitorInfo(monitor, &info)) {
          auto* point = reinterpret_cast<POINT*>(data);
          point->x = info.rcMonitor.left + 20; point->y = info.rcMonitor.top + 20;
        }
        return FALSE;
      }, reinterpret_cast<LPARAM>(&origin));
      const auto window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
          L"STATIC", L"CoreVideo capture validation", WS_POPUP,
          origin.x, origin.y, 160, 90, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      if (window) ShowWindow(window, SW_SHOWNOACTIVATE);
      initialized.set_value(window != nullptr);
      unsigned sequence = 0;
      while (window && !stopping_.load()) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&message); DispatchMessageW(&message);
        }
        const auto dc = GetDC(window);
        RECT rect{0, 0, 160, 90};
        const auto brush = CreateSolidBrush(RGB(++sequence % 256, 64, 192));
        FillRect(dc, &rect, brush); DeleteObject(brush); ReleaseDC(window, dc);
        GdiFlush(); // flush this thread's batched GDI writes before sleeping
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
      }
      if (window) DestroyWindow(window);
    });
    valid_ = ready.get();
  }
  ~WgcTestMotion() { stopping_.store(true); if (thread_.joinable()) thread_.join(); }
  bool valid() const { return valid_; }
 private:
  std::atomic<bool> stopping_{false}; bool valid_ = false; std::thread thread_;
};
}
TEST(WgcCpuPreparation, OptInRealCpuCaptureMatchesReferencePixelsWithoutProgramUploads) {
  const char* enabled = std::getenv("COREVIDEO_CAPTURE_TESTS");
  if (!enabled || std::string(enabled) != "1") {
    std::fprintf(stderr, "[capture-test] SKIPPED CPU WGC pixel test; enable COREVIDEO_CAPTURE_TESTS=1\n");
    return;
  }
  struct Flags {
    std::string cpu, gpu, monitor;
    Flags() {
      const char* raw = std::getenv("COREVIDEO_CPU_SOURCE_PREPARATION"); cpu = raw ? raw : "";
      raw = std::getenv("COREVIDEO_GPU_CAPTURE"); gpu = raw ? raw : "";
      raw = std::getenv("COREVIDEO_ISOLATE_MONITORS"); monitor = raw ? raw : "";
      _putenv_s("COREVIDEO_GPU_CAPTURE", "0"); _putenv_s("COREVIDEO_ISOLATE_MONITORS", "0");
    }
    ~Flags() {
      _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", cpu.c_str());
      _putenv_s("COREVIDEO_GPU_CAPTURE", gpu.c_str());
      _putenv_s("COREVIDEO_ISOLATE_MONITORS", monitor.c_str());
    }
  } flags;
  WgcTestMotion motion; ASSERT_TRUE(motion.valid());
  corevideo::core::ComApartmentLifetime apartment;
  _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "0");
  auto reference = createD3D11Compositor(); ASSERT_TRUE(reference);
  _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "1");
  auto prepared = createD3D11Compositor(); ASSERT_TRUE(prepared);
  auto owner = std::make_shared<CpuSourcePreparation>(true);
  auto capture = createWgcScreenCaptureDevice(owner); ASSERT_TRUE(capture);
  const auto devices = capture->enumerate(); ASSERT_FALSE(devices.empty());
  capture->connect(devices.front().id);
  struct Consumer : ICaptureVideoConsumer {
    VideoFrame frame;
    void publish(VideoFrame value) override { frame = std::move(value); }
    void end(const std::string&) override {}
  } consumer;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!consumer.frame.preparedGpu && std::chrono::steady_clock::now() < deadline) {
    capture->deliverVideo(consumer, 0); std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(consumer.frame.hasPixels()); ASSERT_TRUE(consumer.frame.preparedGpu);
  EXPECT_FALSE(consumer.frame.hasGpuPixels());
  const auto cpu = consumer.frame.pixels;
  const auto observed = consumer.frame.captureTimestamp100ns;
  ASSERT_GT(observed, 0);
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!consumer.frame.preparedGpu->acquire(true) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(consumer.frame.preparedGpu->acquire(false));
  auto request = requestAtSize(64);
  request.programPlan.skipCpuReadback = false;
  auto& layer = request.programPlan.layers.front();
  layer.participantId = layer.sourceId = consumer.frame.participantId;
  auto original = consumer.frame; original.preparedGpu.reset();
  const auto expected = reference->render(request.programPlan, {original});
  const auto actual = prepared->render(request.programPlan, {consumer.frame});
  ASSERT_FALSE(actual.preview.bgra.empty()); EXPECT_EQ(actual.preview.bgra, expected.preview.bgra);
  EXPECT_EQ(actual.sourceAdmissions.front().state, "ready");
  EXPECT_EQ(prepared->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(prepared->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(prepared->sourceTexStats().textureCreates, 0u);
  EXPECT_EQ(consumer.frame.pixels, cpu); EXPECT_EQ(consumer.frame.captureTimestamp100ns, observed);
  EXPECT_EQ(consumer.frame.sourceEpoch, consumer.frame.preparedGpu->sourceEpoch);
  const auto firstId = consumer.frame.frameId;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (consumer.frame.frameId <= firstId && std::chrono::steady_clock::now() < deadline) {
    capture->deliverVideo(consumer, 0); std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_GT(consumer.frame.frameId, firstId);
  std::fprintf(stderr, "[capture-test] CPU WGC prepared real %dx%d pixels; source advanced; Program uploads=0\n",
      consumer.frame.pixelWidth, consumer.frame.pixelHeight);
  capture->disconnect(devices.front().id);
  capture->connect(devices.front().id);
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (consumer.frame.sourceEpoch <= original.sourceEpoch && std::chrono::steady_clock::now() < deadline) {
    capture->deliverVideo(consumer, 0); std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(consumer.frame.sourceEpoch, original.sourceEpoch);
  ASSERT_TRUE(consumer.frame.preparedGpu);
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!consumer.frame.preparedGpu->acquire(true) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(consumer.frame.preparedGpu->acquire(false));
  // Use a fresh independent reference so a restarted capture sequence cannot
  // reuse the legacy comparator's prior frame-id texture cache.
  _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "0");
  auto reconnectReference = createD3D11Compositor(); ASSERT_TRUE(reconnectReference);
  _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "1");
  auto freshCpu = consumer.frame; freshCpu.preparedGpu.reset();
  const auto expectedReconnect = reconnectReference->render(request.programPlan, {freshCpu});
  const auto actualReconnect = prepared->render(request.programPlan, {consumer.frame});
  EXPECT_EQ(actualReconnect.preview.bgra, expectedReconnect.preview.bgra);
  EXPECT_EQ(actualReconnect.sourceAdmissions.front().actualEpoch, consumer.frame.sourceEpoch);
  EXPECT_EQ(actualReconnect.sourceAdmissions.front().actualFrameId, consumer.frame.frameId);
  EXPECT_EQ(prepared->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(prepared->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(prepared->sourceTexStats().textureCreates, 0u);
  EXPECT_EQ(original.pixels, cpu); EXPECT_EQ(original.captureTimestamp100ns, observed);
  capture->disconnect(devices.front().id);
}

TEST(GpuCaptureIngress, OptInRealWgcFrameUsesPreparedGpuViewAndIndependentCpuConsumers) {
  const char* enabled = std::getenv("COREVIDEO_CAPTURE_TESTS");
  if (!enabled || std::string(enabled) != "1") {
    std::fprintf(stderr, "[capture-test] SKIPPED real WGC capture; enable COREVIDEO_CAPTURE_TESTS=1 on an interactive rig\n");
    return;
  }
  WgcTestMotion motion;
  ASSERT_TRUE(motion.valid());
  corevideo::core::ComApartmentLifetime apartment;
  const char* raw = std::getenv("COREVIDEO_GPU_CAPTURE");
  const std::string previous = raw ? raw : "";
  _putenv_s("COREVIDEO_GPU_CAPTURE", "1");
  auto compositor = isolatedCompositor();
  auto capture = createWgcScreenCaptureDevice();
  ASSERT_TRUE(compositor != nullptr);
  ASSERT_TRUE(capture != nullptr);
  const auto devices = capture->enumerate();
  ASSERT_FALSE(devices.empty());
  capture->connect(devices.front().id);
  _putenv_s("COREVIDEO_GPU_CAPTURE", previous.c_str());
  struct Consumer : ICaptureVideoConsumer {
    VideoFrame frame;
    void publish(VideoFrame value) override { frame = std::move(value); }
    void end(const std::string&) override {}
  } consumer;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!consumer.frame.hasGpuPixels() && std::chrono::steady_clock::now() < deadline) {
    capture->deliverVideo(consumer, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(consumer.frame.hasGpuPixels());
  EXPECT_FALSE(consumer.frame.hasPixels());
  std::vector<VideoFrame> cpuFrames;
  const auto cpuDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (cpuFrames.empty() && std::chrono::steady_clock::now() < cpuDeadline) {
    cpuFrames = capture->takeCpuVideoFrames();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_FALSE(cpuFrames.empty());
  EXPECT_TRUE(cpuFrames.front().hasPixels());
  EXPECT_EQ(cpuFrames.front().sourceEpoch, consumer.frame.sourceEpoch);
  EXPECT_GT(cpuFrames.front().captureTimestamp100ns, 0);
  EXPECT_EQ(cpuFrames.front().participantId, consumer.frame.participantId);
  EXPECT_EQ(consumer.frame.gpuPixels->width, consumer.frame.pixelWidth);
  EXPECT_EQ(consumer.frame.gpuPixels->height, consumer.frame.pixelHeight);
  auto request = requestAtSize(64);
  auto& layer = request.programPlan.layers.front();
  layer.participantId = layer.sourceId = consumer.frame.participantId;
  request.programPlan.skipCpuReadback = false;
  const auto output = compositor->render(request.programPlan, {consumer.frame});
  EXPECT_TRUE(output.gpuComposed);
  EXPECT_FALSE(output.preview.bgra.empty());
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  std::fprintf(stderr, "[capture-test] real WGC %dx%d GPU view consumed; separate CPU arrival verified; uploads=%llu\n",
      consumer.frame.pixelWidth, consumer.frame.pixelHeight,
      static_cast<unsigned long long>(compositor->sourceTexStats().cachedUploads));
  const auto mirroredId = consumer.frame.frameId;
  capture->setVideoConsumerDemand({});
  capture->takeCpuVideoFrames(); // drain work admitted before demand release
  const auto gpuOnlyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < gpuOnlyDeadline) {
    capture->deliverVideo(consumer, 0);
    if (consumer.frame.frameId > mirroredId && consumer.frame.hasGpuPixels() && !consumer.frame.hasPixels()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_GT(consumer.frame.frameId, mirroredId);
  ASSERT_TRUE(consumer.frame.hasGpuPixels());
  ASSERT_FALSE(consumer.frame.hasPixels());
  const auto gpuOnly = compositor->render(request.programPlan, {consumer.frame});
  EXPECT_TRUE(gpuOnly.gpuComposed);
  EXPECT_FALSE(gpuOnly.preview.bgra.empty());
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  const auto gpuOnlyId = consumer.frame.frameId;
  // Exhaust only the optional monitor pool. Production must continue receiving
  // GPU-only pictures rather than borrowing those slots or falling back to CPU.
  std::vector<std::shared_ptr<const GpuVideoFrame>> heldMonitors;
  const auto retainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (heldMonitors.size() < 3 && std::chrono::steady_clock::now() < retainDeadline) {
    capture->deliverVideo(consumer, 0);
    const auto& image = consumer.frame.monitorGpuPixels;
    if (image && std::find(heldMonitors.begin(), heldMonitors.end(), image) == heldMonitors.end())
      heldMonitors.push_back(image);
    if (consumer.frame.hasGpuPixels()) compositor->render(request.programPlan, {consumer.frame});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(heldMonitors.size(), 3u);
  auto lastId = consumer.frame.frameId;
  int advanced = 0;
  bool allGpuOnly = true;
  int polls = 0; int64_t renderUs = 0, captureUs = 0;
  const auto pressureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (advanced < 20 && std::chrono::steady_clock::now() < pressureDeadline) {
    const auto captureStart = std::chrono::steady_clock::now();
    capture->deliverVideo(consumer, 0);
    captureUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - captureStart).count();
    ++polls;
    if (consumer.frame.frameId > lastId) {
      lastId = consumer.frame.frameId;
      ++advanced;
      allGpuOnly = allGpuOnly && consumer.frame.hasGpuPixels() && !consumer.frame.hasPixels();
    }
    // Production keeps rendering held pictures. That also retires completed
    // GPU reads; only rendering on source changes can strand the test's leases.
    const auto renderStart = std::chrono::steady_clock::now();
    if (consumer.frame.hasGpuPixels()) compositor->render(request.programPlan, {consumer.frame});
    renderUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - renderStart).count();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  heldMonitors.clear();
  std::fprintf(stderr, "[capture-test] pressure advanced=%d latest=%lld allGpuOnly=%d\n",
      advanced, static_cast<long long>(lastId), allGpuOnly ? 1 : 0);
  std::fprintf(stderr, "[capture-test] polls=%d captureUs=%lld renderUs=%lld\n", polls,
      static_cast<long long>(captureUs), static_cast<long long>(renderUs));
  EXPECT_EQ(advanced, 20);
  EXPECT_TRUE(allGpuOnly);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  capture->setVideoConsumerDemand({{consumer.frame.participantId, SourceVideoConsumer::Iso,
      "recording", SourceVideoRepresentation::Cpu}});
  cpuFrames.clear();
  const auto isoDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < isoDeadline) {
    capture->deliverVideo(consumer, 0);
    if (consumer.frame.hasGpuPixels()) compositor->render(request.programPlan, {consumer.frame});
    auto arrived = capture->takeCpuVideoFrames();
    for (auto& frame : arrived) if (frame.frameId > lastId) cpuFrames.push_back(std::move(frame));
    if (!cpuFrames.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GT(consumer.frame.frameId, gpuOnlyId);
  ASSERT_FALSE(cpuFrames.empty());
  EXPECT_TRUE(cpuFrames.front().hasPixels());
  EXPECT_EQ(cpuFrames.front().sourceEpoch, consumer.frame.sourceEpoch);
  EXPECT_GT(cpuFrames.front().captureTimestamp100ns, 0);
  EXPECT_FALSE(consumer.frame.hasPixels());
  EXPECT_TRUE(consumer.frame.hasGpuPixels());
  std::fprintf(stderr, "[capture-test] GPU-only/ISO transitions passed; %d new production frames with all monitor slots retained\n", advanced);
  for (int cycle = 0; cycle < 3; ++cycle) {
    const auto priorEpoch = consumer.frame.sourceEpoch;
    capture->disconnect(devices.front().id);
    capture->connect(devices.front().id);
    const auto reconnectBy = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (consumer.frame.sourceEpoch <= priorEpoch && std::chrono::steady_clock::now() < reconnectBy) {
      capture->deliverVideo(consumer, 0);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GT(consumer.frame.sourceEpoch, priorEpoch);
    ASSERT_TRUE(consumer.frame.hasGpuPixels());
    cpuFrames.clear();
    while (cpuFrames.empty() && std::chrono::steady_clock::now() < reconnectBy) {
      capture->deliverVideo(consumer, 0);
      compositor->render(request.programPlan, {consumer.frame});
      cpuFrames = capture->takeCpuVideoFrames();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_FALSE(cpuFrames.empty());
    EXPECT_EQ(cpuFrames.front().sourceEpoch, consumer.frame.sourceEpoch);
  }
  std::fprintf(stderr, "[capture-test] three asynchronous reconnects produced fresh GPU/CPU epochs\n");
  capture->disconnect(devices.front().id);
}
#endif
#endif
