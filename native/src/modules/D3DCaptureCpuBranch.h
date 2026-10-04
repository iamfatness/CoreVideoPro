#pragma once
#include "modules/CaptureFrameWorker.h"
#include "modules/D3DVideoFrame.h"
#include "modules/Interfaces.h"
#include "core/FrameAllocation.h"
#include <algorithm>
#include <cstring>

namespace corevideo::modules {
// All new source residency, including CPU mirrors/staging, shares the declared
// 512 MiB ingress limit. Reservations survive every outstanding frame lease.
struct D3DCaptureCpuReservation {
  size_t bytes = 0;
  ~D3DCaptureCpuReservation() { D3DVideoImage::residentBytes.fetch_sub(bytes); }
  static std::shared_ptr<D3DCaptureCpuReservation> reserve(size_t bytes) {
    auto result = std::make_shared<D3DCaptureCpuReservation>();
    auto used = D3DVideoImage::residentBytes.load();
    do {
      if (bytes > 512u * 1024u * 1024u || used > 512u * 1024u * 1024u - bytes) return {};
    } while (!D3DVideoImage::residentBytes.compare_exchange_weak(used, used + bytes));
    result->bytes = bytes;
    return result;
  }
};

// A private three-image CPU branch. Capture submits copies on its OWN context;
// the conversion worker reads completed shared images on a SEPARATE device and
// immediate context. CPU/ISO pressure cannot retain a production/monitor slot.
class D3DCaptureCpuBranch {
 public:
  struct Stats { uint64_t copied = 0, admitted = 0, converted = 0, capacityRefused = 0,
    queueRefused = 0, failed = 0, outputRefused = 0, preparationRefused = 0; size_t ready = 0; };
  D3DCaptureCpuBranch(ID3D11Device* producer, std::string sourceId,
                      std::function<void()> beforeConvert = {}, std::function<void()> beforePrepare = {})
      : producer_(producer), sourceId_(std::move(sourceId)), beforeConvert_(std::move(beforeConvert)),
        beforePrepare_(std::move(beforePrepare)) {
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter;
    if (FAILED(producer->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
          D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
          &reader_, nullptr, &context_))) return;
    worker_ = std::make_unique<CaptureFrameWorker<Work>>(
        [this](const Work& work) { convert(work); },
        [this] { retireReads(); }, [this] { finishReads(); });
    preparationWorker_ = std::make_unique<CaptureFrameWorker<Preparation>>(
        [this](const Preparation& work) {
          // Free unleased/stale generations on the preparation owner before
          // allocating their replacement. Never admit a third generation.
          work.retiring->generations.clear();
          std::unique_ptr<Generation> next;
          try { if (beforePrepare_) beforePrepare_(); next = initialize(work.desc); } catch (...) { ++failed_; }
          if (!next) ++capacityRefused_;
          std::lock_guard<std::mutex> lock(preparationMutex_);
          prepared_ = std::move(next); preparing_ = false;
        });
  }
  ~D3DCaptureCpuBranch() {
    if (preparationWorker_) preparationWorker_->stop();
    if (worker_) worker_->stop();
  }
  bool valid() const { return worker_ != nullptr && preparationWorker_ != nullptr; }

  // Called only by the capture context owner. No wait for readback/conversion.
  void copy(ID3D11DeviceContext* captureContext, ID3D11Texture2D* source,
            int64_t sequence, uint64_t epoch, int64_t capture100ns) {
    if (!worker_) { ++failed_; return; }
    publishReady(captureContext);
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.Width == 0 || desc.Height == 0 ||
        desc.Width > 7680 || desc.Height > 4320 || desc.MipLevels != 1 || desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1) { ++failed_; return; }
    if (!current_ || current_->width != desc.Width || current_->height != desc.Height) {
      if (!prepareGeneration(desc)) return;
    }
    for (auto& slot : current_->slots) {
      if (slot.pending || slot.image.use_count() != 1) continue;
      captureContext->CopyResource(slot.image->producer.Get(), source);
      captureContext->End(slot.ready.Get());
      slot.work = {slot.image, sequence, epoch, capture100ns};
      slot.pending = true; ++copied_; return;
    }
    ++capacityRefused_;
  }
  void publishReady(ID3D11DeviceContext* captureContext) {
    std::array<Slot*, 6> completed{}; size_t count = 0;
    const auto collect = [&](Generation& generation) {
      for (auto& slot : generation.slots) {
        if (!slot.pending) continue;
        BOOL ready = FALSE;
        if (captureContext->GetData(slot.ready.Get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !ready) continue;
        completed[count++] = &slot;
      }
    };
    // Older admitted copies precede new dimensions. Never monitor-supersede ISO.
    if (retired_) collect(*retired_);
    if (current_) collect(*current_);
    std::sort(completed.begin(), completed.begin() + count,
        [](const Slot* a, const Slot* b) { return a->work.sequence < b->work.sequence; });
    for (size_t i = 0; i < count; ++i) {
      auto& slot = *completed[i]; slot.pending = false;
      if (!worker_->submit(std::move(slot.work), true)) ++queueRefused_;
      slot.work = {};
    }
  }
  void stopOnCaptureOwner(ID3D11DeviceContext* captureContext) {
    if (preparationWorker_) preparationWorker_->stop();
    if (worker_) worker_->stop();
    captureContext->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const auto retire = [&](Generation& generation) {
      for (auto& slot : generation.slots) if (slot.pending) {
        BOOL ready = FALSE;
        if (captureContext->GetData(slot.ready.Get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && ready) {
          slot.pending = false; slot.work = {};
        }
      }
    };
    const auto pending = [&] {
      for (const auto* generation : {current_.get(), retired_.get()})
        if (generation) for (const auto& slot : generation->slots) if (slot.pending) return true;
      return false;
    };
    while (pending() && std::chrono::steady_clock::now() < deadline) {
      if (retired_) retire(*retired_); if (current_) retire(*current_);
      if (pending()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (pending()) {
      std::lock_guard<std::mutex> lock(quarantineMutex_);
      for (auto* generation : {current_.get(), retired_.get()})
        if (generation) for (auto& slot : generation->slots) if (slot.pending)
          quarantine_.push_back({slot.image, slot.ready});
    }
  }
  std::vector<VideoFrame> take() {
    std::vector<VideoFrame> result;
    std::lock_guard<std::mutex> lock(outputMutex_); result.swap(output_); return result;
  }
  Stats stats() const {
    std::lock_guard<std::mutex> lock(outputMutex_);
    const auto work = worker_ ? worker_->stats() : CaptureFrameWorker<Work>::Stats{};
    return {copied_.load(), work.accepted, converted_.load(), capacityRefused_.load(), queueRefused_.load(),
      failed_.load() + work.failed, outputRefused_.load(), preparationRefused_.load(), output_.size()};
  }
 private:
  struct Image {
    ComPtr<ID3D11Texture2D> producer, reader;
    std::shared_ptr<D3DCaptureCpuReservation> reservation;
    UINT width = 0, height = 0;
  };
  struct Work { std::shared_ptr<Image> image; int64_t sequence = 0;
    uint64_t epoch = 0; int64_t capture100ns = 0; };
  struct Slot { std::shared_ptr<Image> image; ComPtr<ID3D11Query> ready;
    Work work; bool pending = false; };
  struct Generation { UINT width = 0, height = 0; std::array<Slot, 3> slots; };
  struct Retirement { std::vector<std::shared_ptr<Generation>> generations; };
  struct Preparation { D3D11_TEXTURE2D_DESC desc{}; std::shared_ptr<Retirement> retiring; };
  struct Reading { std::shared_ptr<Image> image; ComPtr<ID3D11Query> ready; };
  bool prepareGeneration(const D3D11_TEXTURE2D_DESC& desc) {
    if (retired_ && !idle(*retired_)) { ++capacityRefused_; return false; }
    Preparation work; work.desc = desc;
    {
      std::lock_guard<std::mutex> lock(preparationMutex_);
      if (preparing_) { ++preparationRefused_; return false; }
      if (prepared_ && prepared_->width == desc.Width && prepared_->height == desc.Height) {
        retired_ = std::move(current_); current_ = std::move(prepared_); return true;
      }
      work.retiring = std::make_shared<Retirement>();
      if (retired_) work.retiring->generations.push_back(std::move(retired_));
      if (prepared_) work.retiring->generations.push_back(std::move(prepared_));
      preparing_ = true;
    }
    // Only one preparation is ever admitted. Intermediate dimensions may be
    // refused until it completes; stale resources then retire on that owner.
    if (!preparationWorker_->submit(std::move(work), true)) {
      std::lock_guard<std::mutex> lock(preparationMutex_); preparing_ = false; ++failed_;
    }
    ++preparationRefused_; return false;
  }
  std::unique_ptr<Generation> initialize(D3D11_TEXTURE2D_DESC desc) {
    auto generation = std::make_unique<Generation>();
    generation->width = desc.Width; generation->height = desc.Height;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0; desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1; desc.SampleDesc.Quality = 0;
    for (auto& slot : generation->slots) {
      auto image = std::make_shared<Image>(); image->width = desc.Width; image->height = desc.Height;
      image->reservation = D3DCaptureCpuReservation::reserve(static_cast<size_t>(desc.Width) * desc.Height * 4);
      if (!image->reservation || FAILED(producer_->CreateTexture2D(&desc, nullptr, &image->producer))) return {};
      ComPtr<IDXGIResource> resource; HANDLE handle = nullptr;
      if (FAILED(image->producer.As(&resource)) || FAILED(resource->GetSharedHandle(&handle)) ||
          FAILED(reader_->OpenSharedResource(handle, IID_PPV_ARGS(&image->reader)))) return {};
      D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
      if (FAILED(producer_->CreateQuery(&query, &slot.ready))) return {};
      slot.image = std::move(image);
    }
    return generation;
  }
  static bool idle(const Generation& generation) {
    for (const auto& slot : generation.slots) {
      if (slot.pending || slot.work.image || slot.image.use_count() != 1) return false;
    }
    return true;
  }
  void convert(const Work& work) {
    if (beforeConvert_) beforeConvert_();
    const auto& image = work.image;
    const size_t bytes = static_cast<size_t>(image->width) * image->height * 4;
    if (!staging_ || stagingWidth_ != image->width || stagingHeight_ != image->height) {
      auto reservation = D3DCaptureCpuReservation::reserve(bytes);
      if (!reservation) { ++capacityRefused_; return; }
      D3D11_TEXTURE2D_DESC desc{}; image->reader->GetDesc(&desc);
      desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ComPtr<ID3D11Texture2D> next;
      if (FAILED(reader_->CreateTexture2D(&desc, nullptr, &next))) { ++failed_; return; }
      staging_ = std::move(next); stagingReservation_ = std::move(reservation);
      stagingWidth_ = image->width; stagingHeight_ = image->height;
    }
    D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0}; ComPtr<ID3D11Query> query;
    if (FAILED(reader_->CreateQuery(&queryDesc, &query))) { ++failed_; return; }
    context_->CopyResource(staging_.Get(), image->reader.Get()); context_->End(query.Get()); context_->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{}; HRESULT result = DXGI_ERROR_WAS_STILL_DRAWING;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    do {
      result = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      if (result != DXGI_ERROR_WAS_STILL_DRAWING) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    if (FAILED(result)) { reads_.push_back({image, query}); ++failed_; return; }
    auto reservation = D3DCaptureCpuReservation::reserve(bytes);
    std::unique_ptr<std::vector<uint8_t>> pixels;
    try { if (reservation) pixels = std::make_unique<std::vector<uint8_t>>(); }
    catch (...) { }
    if (!pixels || !core::tryResizeFrameBuffer(*pixels, bytes)) {
      context_->Unmap(staging_.Get(), 0); ++capacityRefused_; return;
    }
    for (UINT row = 0; row < image->height; ++row)
      std::memcpy(pixels->data() + static_cast<size_t>(row) * image->width * 4,
          static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
          static_cast<size_t>(image->width) * 4);
    context_->Unmap(staging_.Get(), 0);
    VideoFrame frame; frame.participantId = sourceId_;
    frame.width = frame.naturalWidth = frame.pixelWidth = image->width;
    frame.height = frame.naturalHeight = frame.pixelHeight = image->height;
    frame.pixelStride = image->width * 4; frame.frameId = work.sequence;
    frame.sourceEpoch = work.epoch; frame.captureTimestamp100ns = work.capture100ns;
    frame.timestampMs = work.capture100ns / 10000;
    frame.pixels = std::shared_ptr<const std::vector<uint8_t>>(pixels.release(),
        [reservation = std::move(reservation)](const std::vector<uint8_t>* value) { delete value; });
    std::lock_guard<std::mutex> lock(outputMutex_);
    if (output_.size() == 4) { ++outputRefused_; return; }
    output_.push_back(std::move(frame)); ++converted_;
  }
  void retireReads() {
    for (auto it = reads_.begin(); it != reads_.end();) {
      BOOL ready = FALSE;
      if (context_->GetData(it->ready.Get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && ready) it = reads_.erase(it);
      else ++it;
    }
  }
  void finishReads() {
    context_->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!reads_.empty() && std::chrono::steady_clock::now() < deadline) {
      retireReads(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!reads_.empty()) {
      // Device failure: preserve leases until process teardown. Their bytes stay
      // in the global budget, so faults cannot create unbounded new residency.
      std::lock_guard<std::mutex> lock(quarantineMutex_);
      for (auto& read : reads_) quarantine_.push_back(std::move(read));
      reads_.clear();
    }
  }
  ComPtr<ID3D11Device> producer_, reader_;
  ComPtr<ID3D11DeviceContext> context_;
  std::string sourceId_;
  std::function<void()> beforeConvert_;
  std::function<void()> beforePrepare_;
  std::unique_ptr<CaptureFrameWorker<Work>> worker_;
  std::unique_ptr<CaptureFrameWorker<Preparation>> preparationWorker_;
  std::shared_ptr<Generation> current_, retired_, prepared_;
  std::mutex preparationMutex_; bool preparing_ = false;
  ComPtr<ID3D11Texture2D> staging_;
  std::shared_ptr<D3DCaptureCpuReservation> stagingReservation_;
  UINT stagingWidth_ = 0, stagingHeight_ = 0;
  std::vector<Reading> reads_;
  inline static std::mutex quarantineMutex_;
  inline static std::vector<Reading> quarantine_;
  mutable std::mutex outputMutex_;
  std::vector<VideoFrame> output_;
  std::atomic<uint64_t> copied_{0}, converted_{0}, capacityRefused_{0}, queueRefused_{0}, failed_{0}, outputRefused_{0}, preparationRefused_{0};
};
}
