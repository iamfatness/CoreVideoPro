#include "graphics/GraphicsAutomation.h"
#include "gtest/gtest.h"
using corevideo::graphics::GraphicsAutomation;
using corevideo::rpc::Json;
namespace {
Json::Object config(double dwell=100,double duration=500) {
 return Json::Object{{"rules",Json::Array{Json::Object{{"id","intro"},{"instanceId","guest"},{"dwellMs",dwell},{"durationMs",duration}}}}};
}
GraphicsAutomation::Subject guest(int64_t generation=1,std::string name="Guest") {
 return {{"zoom:42","zoom-42-generation-"+std::to_string(generation),"core:zoom:1",generation},name};
}
void activate(GraphicsAutomation& a,const std::string& mode="automatic") {
 std::string error;ASSERT_TRUE(a.configure(config(),a.revision(),error));ASSERT_TRUE(a.control("set-mode",mode,"",true,error));
 if(mode!="suggest") ASSERT_TRUE(a.control("arm","","",true,error));
}
TEST(GraphicsAutomation, DwellRequiresDrawnSubjectAndAutoHideStartsAtFirstDraw) {
 GraphicsAutomation a;activate(a);auto g=guest();
 EXPECT_TRUE(a.tick({},"scene",false,0).empty());EXPECT_TRUE(a.tick({g},"scene",false,10).empty());
 auto decision=a.tick({g},"scene",false,110);ASSERT_EQ(decision.size(),1u);EXPECT_EQ(decision[0].action,"show");
 a.result(decision[0],true,"queued",2,110);
 EXPECT_TRUE(a.tick({g},"scene",false,5000).empty());
 a.composed("guest",2,5000);EXPECT_TRUE(a.tick({g},"scene",false,5499).empty());
 auto hide=a.tick({g},"scene",false,5500);ASSERT_EQ(hide.size(),1u);EXPECT_EQ(hide[0].action,"hide");
 EXPECT_TRUE(a.tick({g},"scene",false,9000).empty());
 EXPECT_TRUE(a.tick({},"scene",false,9010).empty());EXPECT_TRUE(a.tick({g},"scene",false,10000).empty());
 EXPECT_TRUE(a.tick({g},"scene",false,11000).empty());
}
TEST(GraphicsAutomation, ManualClearReconnectAndAmbiguityNeverReveal) {
 GraphicsAutomation a;activate(a);auto g=guest();
 EXPECT_TRUE(a.tick({g,guest(2)},"scene",false,1000).empty());
 a.manual("guest");EXPECT_TRUE(a.tick({g},"scene",false,2000).empty());
 std::string error;ASSERT_TRUE(a.control("resume","","guest",true,error));
 EXPECT_TRUE(a.tick({g},"scene",false,2000).empty());auto d=a.tick({g},"scene",false,2100);ASSERT_EQ(d.size(),1u);
 a.result(d[0],true,"queued",2,2100);a.clear();EXPECT_TRUE(a.tick({g},"scene",false,5000).empty());
 EXPECT_TRUE(a.snapshot().get("inhibited")->asBool());
 ASSERT_TRUE(a.control("arm","","",true,error));a.observeEpoch(1);a.observeEpoch(2);
 EXPECT_FALSE(a.snapshot().get("armed")->asBool());EXPECT_TRUE(a.tick({guest(2)},"scene",false,8000).empty());
}
TEST(GraphicsAutomation, TextOverrideIsQualifiedToExactIncarnationAndMetadataUpdateDoesNotRevealAgain) {
 GraphicsAutomation a;std::string error;auto c=config();auto g=guest();
 c["sourceTexts"]=Json::Object{{"zoom:42",Json::Object{{"binding",corevideo::contracts::toJson(g.identity)},{"name","Operator name"},{"secondary",""}}}};
 ASSERT_TRUE(a.configure(c,1,error));ASSERT_TRUE(a.control("set-mode","automatic","",true,error));ASSERT_TRUE(a.control("arm","","",true,error));
 a.tick({g},"scene",false,0);auto d=a.tick({g},"scene",false,100);ASSERT_EQ(d.size(),1u);EXPECT_EQ(d[0].name,"Operator name");EXPECT_EQ(d[0].secondary,"");a.result(d[0],true,"queued",2,100);
 auto next=guest(2,"New guest");a.tick({next},"scene",false,200);d=a.tick({next},"scene",false,300);ASSERT_EQ(d.size(),1u);EXPECT_EQ(d[0].name,"New guest");a.result(d[0],true,"queued",3,300);
 next.name="Renamed guest";d=a.tick({next},"scene",false,350);ASSERT_EQ(d.size(),1u);EXPECT_EQ(d[0].action,"update");EXPECT_EQ(d[0].name,"Renamed guest");
}
TEST(GraphicsAutomation, SuggestionsDoNotConsumeAutomaticShowAndSavingDoesNotArm) {
 GraphicsAutomation a;activate(a,"suggest");auto g=guest();a.tick({g},"scene",false,0);EXPECT_TRUE(a.tick({g},"scene",false,100).empty());
 std::string error;ASSERT_TRUE(a.control("set-mode","automatic","",true,error));EXPECT_TRUE(a.tick({g},"scene",false,1000).empty());
 ASSERT_TRUE(a.control("arm","","",true,error));a.tick({g},"scene",false,1100);auto d=a.tick({g},"scene",false,1200);ASSERT_EQ(d.size(),1u);
 auto edited=config(200);ASSERT_TRUE(a.configure(edited,a.revision(),error));EXPECT_FALSE(a.snapshot().get("armed")->asBool());
}
TEST(GraphicsAutomation, ShareSuppressionAndRehearsalNeverShowProgram) {
 GraphicsAutomation a;activate(a,"rehearsal");auto g=guest();EXPECT_TRUE(a.tick({g},"scene",true,0).empty());a.tick({g},"scene",false,10);
 auto d=a.tick({g},"scene",false,110);ASSERT_EQ(d.size(),1u);EXPECT_EQ(d[0].action,"cue");
}
TEST(GraphicsAutomation, SuggestionsFollowCurrentMetadataAndExpireBeforeReplacementDwell) {
 GraphicsAutomation a;activate(a,"suggest");auto g=guest();a.tick({g},"scene",false,0);a.tick({g},"scene",false,100);
 ASSERT_EQ(a.snapshot().get("suggestions")->asArray().size(),1u);const auto version=a.traceSequence();g.name="Renamed";a.tick({g},"scene",false,110);
 EXPECT_GT(a.traceSequence(),version);EXPECT_EQ(a.snapshot().get("suggestions")->asArray()[0].getString("name"),"Renamed");
 a.tick({guest(2)},"scene",false,120);EXPECT_TRUE(a.snapshot().get("suggestions")->asArray().empty());
}
TEST(GraphicsAutomation, ConfigurationRejectsUnqualifiedTextAndWrongTypesAtomically) {
 GraphicsAutomation a;std::string error;ASSERT_TRUE(a.configure(config(),1,error));const auto before=a.snapshot().stringify();
 auto invalid=config();invalid["sourceTexts"]=Json::Object{{"zoom:42",Json::Object{{"name","Recycled name"}}}};
 EXPECT_FALSE(a.configure(invalid,a.revision(),error));EXPECT_EQ(a.snapshot().stringify(),before);
 invalid=config();invalid["rules"]=Json::Array{Json::Object{{"id","intro"},{"instanceId","guest"},{"sceneId",42}}};
 EXPECT_FALSE(a.configure(invalid,a.revision(),error));EXPECT_EQ(a.snapshot().stringify(),before);
}
}
