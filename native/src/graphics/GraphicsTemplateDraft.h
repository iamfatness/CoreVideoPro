#pragma once
#include "graphics/GraphicsTemplate.h"
#include <condition_variable>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace corevideo::graphics {
// One coalescing private authoring compiler. Scratch revisions never enter the
// immutable installed catalog and can only reuse their parent's verified assets.
class GraphicsTemplateDraft {
 public:
  struct Publication { std::string key,error; std::shared_ptr<const GraphicsTemplate> definition; rpc::Json layout; };
  GraphicsTemplateDraft()=default;
  ~GraphicsTemplateDraft();
  bool submit(std::string manifest,const std::string& expectedHash,std::shared_ptr<const GraphicsTemplate> parent,std::string& key,rpc::Json fields = {},rpc::Json brand = {});
  std::shared_ptr<const Publication> poll() const { return std::atomic_load(&publication_); }
 private:
  struct Request { std::string manifest,expectedHash,key; std::shared_ptr<const GraphicsTemplate> parent; rpc::Json fields,brand; };
  void run();
  std::mutex mutex_; std::condition_variable wake_; std::thread worker_;
  std::shared_ptr<const Request> pending_; std::string wanted_; bool stopping_=false;
  std::shared_ptr<const Publication> publication_;
};
// Returns a cached immutable resolved input once its exact private revision is
// validated. A pending/invalid revision never falls back to a different graphic.
std::shared_ptr<const GraphicsTemplateInput> resolveGraphicsTemplateInput(const std::shared_ptr<const GraphicsTemplateInput>&,std::string& reason);
}
