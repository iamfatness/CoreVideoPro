#pragma once
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace corevideo::modules {
// One outstanding read and one retained reply. MF callbacks own no session or
// adapter pointer, so cancellation fences late replies from retired readers.
class UvcAsyncRead final : public IMFSourceReaderCallback {
 public:
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(IMFSourceReaderCallback)) return E_NOINTERFACE;
    *out = static_cast<IMFSourceReaderCallback*>(this); AddRef(); return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override { const auto left = --references_; if (!left) delete this; return left; }
  STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG pts, IMFSample* sample) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (retired_) return S_OK;
      // A duplicate callback must not replace the owned, undrained reply.
      if (!pending_ || ready_) return S_OK;
      if (sample) sample->AddRef();
      sample_ = sample; status_ = status; flags_ = flags; pts_ = pts;
      pending_ = false; ready_ = true;
    }
    changed_.notify_all(); return S_OK;
  }
  STDMETHODIMP OnFlush(DWORD) override {
    IMFSample* old = nullptr;
    { std::lock_guard<std::mutex> lock(mutex_); pending_ = ready_ = false; old = sample_; sample_ = nullptr; }
    if (old) old->Release();
    changed_.notify_all(); return S_OK;
  }
  STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
  void cancel() {
    IMFSample* old = nullptr;
    { std::lock_guard<std::mutex> lock(mutex_); retired_ = true; pending_ = ready_ = false; old = sample_; sample_ = nullptr; }
    if (old) old->Release();
    changed_.notify_all();
  }
  template<class Request>
  HRESULT take(Request&& request, std::chrono::milliseconds timeout,
      DWORD* flags, LONGLONG* pts, IMFSample** sample) {
    if (!flags || !pts || !sample) return E_POINTER;
    *sample = nullptr; *flags = 0; *pts = 0;
    bool submit = false;
    { std::lock_guard<std::mutex> lock(mutex_);
      if (retired_) return MF_E_SHUTDOWN;
      if (!ready_ && !pending_) { pending_ = true; submit = true; }
    }
    if (submit) {
      const auto hr = request(); // Async ReadSample; no callback lock held.
      if (FAILED(hr)) { std::lock_guard<std::mutex> lock(mutex_); pending_ = false; return hr; }
    }
    HRESULT status;
    bool prime = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait_for(lock, timeout, [&] { return retired_ || ready_ || !pending_; });
      if (retired_) return MF_E_SHUTDOWN;
      if (!ready_) return S_FALSE;
      status = status_; *flags = flags_; *pts = pts_; *sample = sample_; sample_ = nullptr; ready_ = false;
      if (SUCCEEDED(status) && !(flags_ & MF_SOURCE_READERF_ENDOFSTREAM)) { pending_ = true; prime = true; }
    }
    // Keep decode/conversion off the request cadence: prime one next sample.
    if (prime) {
      const auto hr = request();
      if (FAILED(hr)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!retired_ && !ready_) { pending_ = false; ready_ = true; status_ = hr; flags_ = 0; pts_ = 0; }
      }
    }
    return status;
  }
 private:
  ~UvcAsyncRead() { if (sample_) sample_->Release(); }
  std::atomic<ULONG> references_{1};
  std::mutex mutex_; std::condition_variable changed_;
  bool retired_ = false, pending_ = false, ready_ = false;
  HRESULT status_ = S_OK; DWORD flags_ = 0; LONGLONG pts_ = 0;
  IMFSample* sample_ = nullptr;
};
}
#endif
