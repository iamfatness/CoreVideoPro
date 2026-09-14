#include "core/AtomicTakeJsonCodec.h"
#include <cmath>
#include <stdexcept>
#include <array>
#include <charconv>

namespace corevideo::core {
namespace {
using J=rpc::Json; using T=AtomicTakeCoordinator;
constexpr uint64_t maxSafe=9007199254740991ULL;
struct Invalid {};
bool utf8(const std::string& s) {
  for(size_t i=0;i<s.size();) {
    const auto c=static_cast<unsigned char>(s[i++]);if(c<0x80)continue;
    unsigned code=0;size_t n=0;unsigned minimum=0;
    if(c>=0xC2&&c<=0xDF){code=c&31;n=1;minimum=0x80;}
    else if(c>=0xE0&&c<=0xEF){code=c&15;n=2;minimum=0x800;}
    else if(c>=0xF0&&c<=0xF4){code=c&7;n=3;minimum=0x10000;}
    else return false;
    if(n>s.size()-i)return false;
    while(n--){auto next=static_cast<unsigned char>(s[i++]);if((next&0xC0)!=0x80)return false;code=(code<<6)|(next&63);}
    if(code<minimum||code>0x10FFFF||(code>=0xD800&&code<=0xDFFF))return false;
  }
  return true;
}
const J& field(const J& o,const char* key) { if(!o.isObject())throw Invalid{};auto p=o.get(key);if(!p)throw Invalid{};return *p; }
std::string text(const J& o,const char* key,size_t limit=512,bool empty=false) {
  const auto& v=field(o,key);if(!v.isString()||(!empty&&v.asString().empty())||v.asString().size()>limit||!utf8(v.asString()))throw Invalid{};return v.asString();
}
uint64_t number(const J& o,const char* key,bool positive=false) {
  const auto& v=field(o,key);if(!v.isNumber())throw Invalid{};const auto n=v.asNumber();
  if (const auto lexeme = v.numberLexeme()) {
    if (lexeme->empty() || lexeme->front() == '-' || lexeme->find_first_of(".eE") != std::string_view::npos) throw Invalid{};
    uint64_t exact = 0; const auto parsed = std::from_chars(lexeme->data(), lexeme->data() + lexeme->size(), exact);
    if (parsed.ec != std::errc{} || parsed.ptr != lexeme->data() + lexeme->size() || exact > maxSafe || (positive && exact == 0)) throw Invalid{};
    return exact;
  }
  if(!std::isfinite(n)||n<(positive?1:0)||n>static_cast<double>(maxSafe)||std::floor(n)!=n)throw Invalid{};
  return static_cast<uint64_t>(n);
}
bool boolean(const J& o,const char* key) {const auto& v=field(o,key);if(!v.isBool())throw Invalid{};return v.asBool();}
J integer(uint64_t n) {if(n>maxSafe)throw Invalid{};return J(static_cast<double>(n));}
ShowPlanStamp stamp(const J& v) {return {text(v,"authorityEpoch",256),text(v,"registryEpoch",256),number(v,"controlRevision"),number(v,"registryRevision"),text(v,"eligibilityIdentity",4*1024*1024)};}
J stampJson(const ShowPlanStamp& s) {return J::Object{{"authorityEpoch",s.authorityEpoch},{"registryEpoch",s.registryEpoch},{"controlRevision",integer(s.controlRevision)},{"registryRevision",integer(s.registryRevision)},{"eligibilityIdentity",s.eligibilityIdentity}};}
const std::array<const char*,14> errors={"none","invalid","authorityEpoch","operationConflict","operationExpired","capacity","staleRevision","stalePreview","notPrepared","busy","revisionExhausted","applyFailed","notApplied","staleObservation"};
// Explicit mapping avoids depending on enum ordinals as a wire contract.
const std::array<T::Error,14> errorValues={T::Error::None,T::Error::Invalid,T::Error::AuthorityEpoch,T::Error::OperationConflict,T::Error::OperationExpired,T::Error::Capacity,T::Error::StaleRevision,T::Error::StalePreview,T::Error::NotPrepared,T::Error::Busy,T::Error::RevisionExhausted,T::Error::ApplyFailed,T::Error::NotApplied,T::Error::StaleObservation};
T::Error errorValue(const std::string& s) {for(size_t i=0;i<errorValues.size();++i)if(s==errors[i])return errorValues[i];throw Invalid{};}
std::string errorText(T::Error e) {for(size_t i=0;i<errorValues.size();++i)if(e==errorValues[i])return errors[i];throw Invalid{};}
std::string kindText(T::Transition::Kind k) {switch(k){case T::Transition::Kind::Cut:return "cut";case T::Transition::Kind::Fade:return "fade";case T::Transition::Kind::Dip:return "dip";case T::Transition::Kind::Wipe:return "wipe";}throw Invalid{};}
T::Transition transition(const J& v) {
  T::Transition t;const auto k=text(v,"kind");t.durationNs=number(v,"durationNs");t.direction=text(v,"direction",32,true);t.dipColor=text(v,"dipColor",7,true);
  if(k=="cut")t.kind=T::Transition::Kind::Cut;else if(k=="fade")t.kind=T::Transition::Kind::Fade;else if(k=="dip")t.kind=T::Transition::Kind::Dip;else if(k=="wipe")t.kind=T::Transition::Kind::Wipe;else throw Invalid{};
  const bool timed=t.durationNs>0&&t.durationNs<=5000000000ULL;
  bool color=t.dipColor.size()==7&&t.dipColor[0]=='#';
  if(color)for(size_t i=1;i<7;++i)if(std::string("0123456789abcdefABCDEF").find(t.dipColor[i])==std::string::npos)color=false;
  const bool direction=t.direction=="left-to-right"||t.direction=="right-to-left"||t.direction=="top-to-bottom"||t.direction=="bottom-to-top";
  if(!((k=="cut"&&t.durationNs==0&&t.direction.empty()&&t.dipColor.empty())||
       (k=="fade"&&timed&&t.direction.empty()&&t.dipColor.empty())||
       (k=="dip"&&timed&&t.direction.empty()&&color)||
       (k=="wipe"&&timed&&direction&&t.dipColor.empty())))throw Invalid{};
  return t;
}
}
std::optional<T::Request> AtomicTakeJsonCodec::decodeRequest(const J& v) {
 try {
  if(text(v,"type")!="take")throw Invalid{};
  T::Request r;r.authorityEpoch=text(v,"authorityEpoch",256);r.operationId=text(v,"operationId");
  const auto& f=field(v,"fingerprint");auto& out=r.fingerprint;
  out.expectedRevision=number(f,"expectedRevision");out.previewRevision=number(f,"previewRevision");out.mediaProcessEpoch=text(f,"mediaProcessEpoch");out.mediaGeneration=number(f,"mediaGeneration",true);
  out.transition=transition(field(f,"transition"));out.expectedPlanStamp=stamp(field(f,"expectedPlanStamp"));out.expectedPlanId=text(f,"expectedPlanId",256);
  const auto& p=field(f,"preparation");const auto& b=field(p,"base");
  out.preparation={{text(b,"epoch",256),number(b,"revision"),number(b,"generation",true)},text(p,"planId",256),number(p,"planRevision",true),number(p,"transactionGeneration",true),stamp(field(p,"stamp"))};
  if(!validShowPreparationToken(out.preparation)||out.preparation.base.epoch!=r.authorityEpoch||out.preparation.base.revision!=out.expectedRevision||out.preparation.stamp!=out.expectedPlanStamp||out.preparation.planId!=out.expectedPlanId)throw Invalid{};
  return r;
 }catch(const Invalid&){return {};}
}
std::optional<J> AtomicTakeJsonCodec::encodeRequest(const T::Request& r) {
 try {
  const auto& f=r.fingerprint;const auto& p=f.preparation;
  J v=J::Object{{"type","take"},{"authorityEpoch",r.authorityEpoch},{"operationId",r.operationId},{"fingerprint",J::Object{
    {"expectedRevision",integer(f.expectedRevision)},{"previewRevision",integer(f.previewRevision)},{"mediaProcessEpoch",f.mediaProcessEpoch},{"mediaGeneration",integer(f.mediaGeneration)},
    {"transition",J::Object{{"kind",kindText(f.transition.kind)},{"durationNs",integer(f.transition.durationNs)},{"direction",f.transition.direction},{"dipColor",f.transition.dipColor}}},
    {"expectedPlanStamp",stampJson(f.expectedPlanStamp)},{"expectedPlanId",f.expectedPlanId},
    {"preparation",J::Object{{"base",J::Object{{"epoch",p.base.epoch},{"revision",integer(p.base.revision)},{"generation",integer(p.base.generation)}}},{"planId",p.planId},{"planRevision",integer(p.planRevision)},{"transactionGeneration",integer(p.transactionGeneration)},{"stamp",stampJson(p.stamp)}}}}}};
  if(!decodeRequest(v))return {};return v;
 }catch(const Invalid&){return {};}
}
std::optional<T::Outcome> AtomicTakeJsonCodec::decodeOutcome(const J& v) {
 try {
  T::Outcome o;o.authorityEpoch=text(v,"authorityEpoch",256);o.operationId=text(v,"operationId");o.error=errorValue(text(v,"error",64));
  o.pending=boolean(v,"pending");o.accepted=boolean(v,"accepted");o.applied=boolean(v,"applied");o.rendered=boolean(v,"rendered");o.delivered=boolean(v,"delivered");o.resultRevision=number(v,"resultRevision");o.failure=text(v,"failure",512,true);
  if((o.pending&&(!o.accepted||o.applied||o.rendered||o.delivered))||(o.applied&&!o.accepted)||(o.rendered&&!o.applied)||(o.delivered&&!o.rendered)||(o.error!=T::Error::None&&(o.applied||o.rendered||o.delivered)))throw Invalid{};
  return o;
 }catch(const Invalid&){return {};}
}
std::optional<J> AtomicTakeJsonCodec::encodeOutcome(const T::Outcome& o) {
 try {
  J v=J::Object{{"authorityEpoch",o.authorityEpoch},{"operationId",o.operationId},{"error",errorText(o.error)},{"pending",o.pending},{"accepted",o.accepted},{"applied",o.applied},{"rendered",o.rendered},{"delivered",o.delivered},{"resultRevision",integer(o.resultRevision)},{"failure",o.failure}};
  if(!decodeOutcome(v))return {};return v;
 }catch(const Invalid&){return {};}
}
}
