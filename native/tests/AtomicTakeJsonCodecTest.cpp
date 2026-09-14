#include "core/AtomicTakeJsonCodec.h"
#include <gtest/gtest.h>
#include <limits>
using namespace corevideo::core;
namespace {
using J=corevideo::rpc::Json;
// Property names/values match NativeTakeRequest.Serialize(); JSON object ordering is immaterial.
constexpr const char* golden=R"({"type":"take","authorityEpoch":"show","operationId":"caller-stable-id","fingerprint":{"expectedRevision":10,"previewRevision":8,"mediaProcessEpoch":"media-process","mediaGeneration":7,"transition":{"kind":"fade","durationNs":300000000,"direction":"","dipColor":""},"expectedPlanStamp":{"authorityEpoch":"show","registryEpoch":"registry","controlRevision":10,"registryRevision":12,"eligibilityIdentity":"eligibility-v1"},"expectedPlanId":"plan","preparation":{"base":{"epoch":"show","revision":10,"generation":7},"planId":"plan","planRevision":11,"transactionGeneration":3,"stamp":{"authorityEpoch":"show","registryEpoch":"registry","controlRevision":10,"registryRevision":12,"eligibilityIdentity":"eligibility-v1"}}}})";
J request(){return *J::parse(golden);}
J replace(const J& value,std::vector<std::string> path,J changed,bool remove=false) {
 auto copy=value.asObject();auto key=path.front();path.erase(path.begin());
 if(path.empty()){if(remove)copy.erase(key);else copy[key]=std::move(changed);}
 else copy[key]=replace(copy.at(key),path,std::move(changed),remove);
 return copy;
}
}
TEST(AtomicTakeJsonCodec, CSharpGoldenRoundTripsWithoutFingerprintNormalization) {
 const auto parsed=AtomicTakeJsonCodec::decodeRequest(request());ASSERT_TRUE(parsed.has_value());
 EXPECT_EQ(parsed->operationId,"caller-stable-id");EXPECT_EQ(parsed->fingerprint.preparation.transactionGeneration,3ULL);
 const auto encoded=AtomicTakeJsonCodec::encodeRequest(*parsed);ASSERT_TRUE(encoded.has_value());
 EXPECT_EQ(encoded->stringify(),request().stringify());
 auto withUnknown=replace(request(),{"futureField"},"future");
 withUnknown=replace(withUnknown,{"fingerprint","transition","futureField"},true);
 ASSERT_TRUE(AtomicTakeJsonCodec::decodeRequest(withUnknown).has_value());
 auto mixed=replace(request(),{"fingerprint","transition"},J::Object{{"kind","dip"},{"durationNs",300000000},{"direction",""},{"dipColor","#aBcDeF"}});
 const auto dip=AtomicTakeJsonCodec::decodeRequest(mixed);ASSERT_TRUE(dip.has_value());
 EXPECT_EQ(dip->fingerprint.transition.dipColor,"#aBcDeF");
 EXPECT_EQ(AtomicTakeJsonCodec::encodeRequest(*dip)->stringify(),mixed.stringify());
}
TEST(AtomicTakeJsonCodec, RequiredFieldsWrongTypesAndUnsafeNumbersFailClosed) {
 for(const auto& path:std::vector<std::vector<std::string>>{{"operationId"},{"fingerprint","transition","direction"},{"fingerprint","preparation","base","generation"},{"fingerprint","expectedPlanStamp","eligibilityIdentity"}})
   EXPECT_FALSE(AtomicTakeJsonCodec::decodeRequest(replace(request(),path,J(),true)).has_value());
 for(const auto& value:std::vector<J>{J(),J(true),J("10"),J(10.5),J(-1),J(9007199254740992.0),J((std::numeric_limits<double>::infinity)())})
   EXPECT_FALSE(AtomicTakeJsonCodec::decodeRequest(replace(request(),{"fingerprint","expectedRevision"},value)).has_value());
 EXPECT_FALSE(AtomicTakeJsonCodec::decodeRequest(replace(request(),{"fingerprint","mediaGeneration"},0)).has_value());
 EXPECT_FALSE(AtomicTakeJsonCodec::decodeRequest(replace(request(),{"operationId"},std::string(513,'x'))).has_value());
 EXPECT_FALSE(AtomicTakeJsonCodec::decodeRequest(replace(request(),{"fingerprint","preparation","stamp","registryEpoch"},"Registry")).has_value());
 EXPECT_FALSE(AtomicTakeJsonCodec::decodeRequest(replace(request(),{"fingerprint","preparation","planRevision"},12)).has_value());
 std::string rounded = golden;
 const auto numberAt = rounded.find("\"mediaGeneration\":7");
 ASSERT_NE(numberAt, std::string::npos);
 rounded.replace(numberAt, std::string("\"mediaGeneration\":7").size(),
                 "\"mediaGeneration\":9007199254740990.5");
 const auto parsedRounded = J::parse(rounded);
 ASSERT_TRUE(parsedRounded.has_value());
 EXPECT_FALSE(AtomicTakeJsonCodec::decodeRequest(*parsedRounded).has_value());
 std::string malformed = golden;
 const auto idAt = malformed.find("caller-stable-id");
 ASSERT_NE(idAt, std::string::npos);
 malformed.replace(idAt, std::string("caller-stable-id").size(), "\\uD800");
 EXPECT_FALSE(J::parse(malformed).has_value());
 std::string duplicate = golden;
 const auto objectAt = duplicate.find('{');
 duplicate.insert(objectAt + 1, "\"operationId\":\"ambiguous\",");
 EXPECT_FALSE(J::parse(duplicate).has_value());
}
TEST(AtomicTakeJsonCodec, TransitionFieldsAreStrictAndNotSilentlyCoerced) {
 for(const auto& value:std::vector<J>{J::Object{{"kind","Cut"},{"durationNs",0},{"direction",""},{"dipColor",""}},
     J::Object{{"kind","cut"},{"durationNs",1},{"direction",""},{"dipColor",""}},
     J::Object{{"kind","wipe"},{"durationNs",100},{"direction","diagonal"},{"dipColor",""}},
     J::Object{{"kind","dip"},{"durationNs",100},{"direction",""},{"dipColor","#xxxxxx"}}})
   EXPECT_FALSE(AtomicTakeJsonCodec::decodeRequest(replace(request(),{"fingerprint","transition"},value)).has_value());
 auto native=*AtomicTakeJsonCodec::decodeRequest(request());native.fingerprint.expectedRevision=9007199254740992ULL;
 EXPECT_FALSE(AtomicTakeJsonCodec::encodeRequest(native).has_value());
}
TEST(AtomicTakeJsonCodec, OutcomePreservesStagesAndRejectsMissingOrContradictoryEvidence) {
 AtomicTakeCoordinator::Outcome outcome;outcome.authorityEpoch="show";outcome.operationId="id";
 outcome.pending=true;outcome.accepted=true;outcome.resultRevision=11;
 const auto encoded=AtomicTakeJsonCodec::encodeOutcome(outcome);ASSERT_TRUE(encoded.has_value());
 const auto decoded=AtomicTakeJsonCodec::decodeOutcome(*encoded);ASSERT_TRUE(decoded.has_value());
 EXPECT_TRUE(decoded->pending);EXPECT_FALSE(decoded->applied);EXPECT_FALSE(decoded->delivered);
 EXPECT_FALSE(AtomicTakeJsonCodec::decodeOutcome(replace(*encoded,{"applied"},J(),true)).has_value());
 EXPECT_FALSE(AtomicTakeJsonCodec::decodeOutcome(replace(*encoded,{"delivered"},true)).has_value());
 EXPECT_FALSE(AtomicTakeJsonCodec::decodeOutcome(replace(*encoded,{"accepted"},1)).has_value());
 EXPECT_FALSE(AtomicTakeJsonCodec::decodeOutcome(replace(*encoded,{"resultRevision"},11.5)).has_value());
 EXPECT_FALSE(AtomicTakeJsonCodec::decodeOutcome(replace(*encoded,{"error"},"futureUnknownError")).has_value());
 outcome.pending=false;outcome.applied=true;outcome.rendered=true;outcome.delivered=true;
 EXPECT_TRUE(AtomicTakeJsonCodec::encodeOutcome(outcome).has_value());
 outcome.error=AtomicTakeCoordinator::Error::ApplyFailed;
 EXPECT_FALSE(AtomicTakeJsonCodec::encodeOutcome(outcome).has_value());
}
