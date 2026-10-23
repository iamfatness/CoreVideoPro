#include "graphics/GraphicsTemplateDraft.h"
#include "modules/Sha256.h"
#include <algorithm>
#include <atomic>

namespace corevideo::graphics {
using rpc::Json;
namespace {
std::string hex(const std::array<uint8_t,32>& bytes) {
  static constexpr char digits[]="0123456789abcdef"; std::string text; text.reserve(64);
  for(auto byte:bytes) {text+=digits[byte>>4];text+=digits[byte&15];} return text;
}
}
GraphicsTemplateDraft::~GraphicsTemplateDraft() { {std::lock_guard lock(mutex_);stopping_=true;} wake_.notify_one(); if(worker_.joinable()) worker_.join(); }
bool GraphicsTemplateDraft::submit(std::string manifest,const std::string& expectedHash,std::shared_ptr<const GraphicsTemplate> parent,std::string& key,Json fields,Json brand) {
  if(!parent || manifest.empty() || manifest.size()>256*1024 || expectedHash.size()!=64 ||
      !std::all_of(expectedHash.begin(),expectedHash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})) return false;
  std::string assets; for(const auto& entry:parent->assets()) assets+=entry.first+":"+entry.second->sha256+";";
  const auto values=fields.stringify()+brand.stringify();
  key=expectedHash+":"+hex(parent->hash())+":"+hex(modules::hashing::sha256(reinterpret_cast<const uint8_t*>(assets.data()),assets.size()))+":"+hex(modules::hashing::sha256(reinterpret_cast<const uint8_t*>(values.data()),values.size()));
  std::unique_lock lock(mutex_,std::try_to_lock); if(!lock.owns_lock() || stopping_) return false;
  if(wanted_==key) return true;
  wanted_=key; pending_=std::make_shared<const Request>(Request{std::move(manifest),expectedHash,key,std::move(parent),std::move(fields),std::move(brand)});
  if(!worker_.joinable()) worker_=std::thread([this]{run();}); wake_.notify_one(); return true;
}
void GraphicsTemplateDraft::run() {
  for(;;) {
    std::shared_ptr<const Request> request;
    {std::unique_lock lock(mutex_);wake_.wait(lock,[&]{return stopping_||pending_;});if(stopping_) return;request=std::move(pending_);}
    auto result=std::make_shared<Publication>(); result->key=request->key;
    if(hex(modules::hashing::sha256(reinterpret_cast<const uint8_t*>(request->manifest.data()),request->manifest.size()))!=request->expectedHash) result->error="private-template-hash-mismatch";
    else {
      auto parsed=hex(request->parent->hash())==request->expectedHash ? request->parent : GraphicsTemplate::load(request->manifest,result->error);
      if(parsed==request->parent) result->definition=parsed;
      else if(parsed) {
        GraphicsAssets assets;
        if(const auto* declarations=parsed->document().get("assets")) for(const auto& declaration:declarations->asArray()) {
          const auto found=request->parent->assets().find(declaration.getString("id"));
          if(found!=request->parent->assets().end()) assets.emplace(found->first,found->second);
        }
        result->definition=GraphicsTemplate::load(request->manifest,result->error,std::move(assets));
      }
    }
    if(result->definition && request->fields.isObject()) {
      GraphicsDrawing drawing;
      if(!result->definition->resolve(request->fields,request->brand,drawing,result->error)) result->definition.reset();
      else {
        Json::Array nodes;
        for(const auto& node:drawing.nodes) nodes.emplace_back(Json::Object{{"id",node.id},{"parent",node.parent},{"type",node.type},
          {"x",node.box.x},{"y",node.box.y},{"width",node.box.width},{"height",node.box.height},
          {"clipX",node.clip.x},{"clipY",node.clip.y},{"clipWidth",node.clip.width},{"clipHeight",node.clip.height},{"visible",node.visible}});
        result->layout=Json::Object{{"width",drawing.width},{"height",drawing.height},{"nodes",std::move(nodes)}};
      }
    }
    {std::lock_guard lock(mutex_); if(stopping_) return; if(wanted_==request->key) std::atomic_store(&publication_,std::shared_ptr<const Publication>(std::move(result)));}
  }
}
std::shared_ptr<const GraphicsTemplateInput> resolveGraphicsTemplateInput(const std::shared_ptr<const GraphicsTemplateInput>& input,std::string& reason) {
  if(!input || !input->draft) return input;
  if(auto cached=std::atomic_load(&input->resolvedDraft)) return cached;
  auto publication=input->draft->poll();
  if(!publication || publication->key!=input->draftKey) {reason="validating-private-template";return {};}
  if(!publication->definition) {reason=publication->error;return {};}
  auto result=std::make_shared<GraphicsTemplateInput>(); result->definition=publication->definition; result->fields=input->fields; result->brand=input->brand; result->placement=input->placement; result->logo=input->logo;
  std::shared_ptr<const GraphicsTemplateInput> immutable=result;
  std::atomic_store(&input->resolvedDraft,immutable); return immutable;
}
}
