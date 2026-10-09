#include "compositor/AdvancedGrade.h"
#include "core/SourceGradeState.h"
#include "core/GradePreviewController.h"
#include <gtest/gtest.h>
using namespace corevideo;
namespace {
rpc::Json document() {
  const rpc::Json points=rpc::Json::Array{rpc::Json::Object{{"x",0},{"y",0}},rpc::Json::Object{{"x",.333},{"y",.7}},rpc::Json::Object{{"x",1},{"y",1}}};
  return rpc::Json::Object{{"version",2},{"colorSpace","rec709-sdr"},{"intensity",1},{"operations",rpc::Json::Array{
    rpc::Json::Object{{"id","curve"},{"kind","curves"},{"curves",rpc::Json::Array{points,points,points,points}}}}}};
}
}
TEST(AdvancedGrade, ValidatesDocumentAndPreservesExactOffGridCurveKnots) {
  std::shared_ptr<const modules::AdvancedGradeDocument> grade; ASSERT_TRUE(modules::readAdvancedGrade(document(),grade));
  ASSERT_EQ(grade->operations.size(),1u); EXPECT_NEAR(modules::evaluateGradeCurve(grade->operations[0].curves[0],.333f),.7f,.000001f);
  modules::CompositorColorGrade wrapped; wrapped.advanced=grade;
  const auto points=modules::compileGradeCurves(wrapped);
  EXPECT_NEAR(points[2],.333f,.000001f); EXPECT_NEAR(points[3],.7f,.000001f);
  EXPECT_FALSE(modules::colorGradeIsIdentity(wrapped));
  auto boundary=document().asObject();auto operations=boundary["operations"].asArray();auto operation=operations[0].asObject();
  operation["kind"]="primaries";operation["gamma"]=.1;operations[0]=operation;boundary["operations"]=operations;
  EXPECT_TRUE(modules::readAdvancedGrade(boundary,grade));
  auto bad=document().asObject(); bad["version"]=3; EXPECT_FALSE(modules::readAdvancedGrade(bad,grade));
  bad=document().asObject(); bad["colorSpace"]="hdr"; EXPECT_FALSE(modules::readAdvancedGrade(bad,grade));
  bad=document().asObject(); bad["intensity"]=2; EXPECT_FALSE(modules::readAdvancedGrade(bad,grade));
}
TEST(AdvancedGrade, CubeHashDomainAndSampleCountAreValidated) {
  const std::string cube="LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
  const auto bytes=modules::hashing::sha256(reinterpret_cast<const uint8_t*>(cube.data()),cube.size());
  std::ostringstream hash;hash<<std::hex<<std::setfill('0');for(auto b:bytes)hash<<std::setw(2)<<int(b);
  const auto parsed=modules::parseGradeCube(cube,hash.str()); ASSERT_TRUE(parsed); EXPECT_EQ(parsed->size,2); EXPECT_EQ(parsed->rgba.size(),32u);
  EXPECT_FALSE(modules::parseGradeCube(cube,std::string(64,'0')));
  EXPECT_FALSE(modules::parseGradeCube(cube+"nan 0 0\n",hash.str()));
  const std::string abc="abc";const auto digest=modules::hashing::sha256(reinterpret_cast<const uint8_t*>(abc.data()),3);
  std::ostringstream expected;expected<<std::hex<<std::setfill('0');for(auto b:digest)expected<<std::setw(2)<<int(b);
  EXPECT_EQ(expected.str(),"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
TEST(AdvancedGrade, SourceApplyIsRevisionedAndFencesIdentityForTilesAndExports) {
  core::SourceGradeState state;modules::VideoFrame frame;frame.participantId="p1";frame.sourceEpoch=7;state.observe({frame});
  const rpc::Json request=rpc::Json::Object{{"sourceId","p1"},{"sourceEpoch",7},{"expectedRevision",0}};
  modules::CompositorColorGrade grade;grade.exposure=3;
  auto accepted=state.apply(request,grade);ASSERT_TRUE(accepted.get("accepted")->asBool());EXPECT_EQ(accepted.getNumber("revision"),1);
  EXPECT_EQ(state.apply(request,{}).getString("reason"),"grade-revision-conflict");
  modules::CompositorRenderPlan plan;modules::CompositorRenderPlanLayer tile;tile.participantId="p1";tile.kind="tiles-member";plan.layers.push_back(tile);
  state.applyTo(plan,{frame});ASSERT_TRUE(plan.layers[0].hasColorGrade);EXPECT_EQ(plan.layers[0].colorGrade.exposure,3);
  EXPECT_EQ(plan.sourceGrades.at("p1").exposure,3);
  frame.sourceEpoch=8;state.observe({frame});state.applyTo(plan,{frame});EXPECT_EQ(plan.layers[0].colorGrade.exposure,0);
  EXPECT_EQ(state.apply(request,grade).getString("reason"),"source-identity-changed");
}
TEST(AdvancedGrade, LeaseRenewalCannotChangeDraftAndExpiredLeaseRequiresResend) {
  core::GradePreviewController control;const auto now=core::GradePreviewController::Clock::now();
  const rpc::Json request=rpc::Json::Object{{"instanceId","a"},{"sourceId","p1"},{"revision",4}};
  ASSERT_TRUE(control.configure(request,{},true,now));EXPECT_TRUE(control.renew(request,now+std::chrono::milliseconds(500)));
  EXPECT_FALSE(control.renew(request,now+std::chrono::seconds(4)));
  auto wrong=request.asObject();wrong["revision"]=5;EXPECT_FALSE(control.renew(wrong,now));
}

TEST(AdvancedGrade, NonCurveOperationsHaveNeutralCurveResources) {
  auto document=std::make_shared<modules::AdvancedGradeDocument>();
  modules::GradeOperation primary;primary.kind="primaries";
  document->operations.push_back(primary);
  modules::CompositorColorGrade grade;grade.advanced=document;
  const auto points=modules::compileGradeCurves(grade);
  EXPECT_EQ(points[0],1.f);EXPECT_EQ(points[1],1.f);
}
