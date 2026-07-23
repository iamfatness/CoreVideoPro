#pragma once

#include <utility>

// Minimal move-only RAII wrapper for COM interface pointers used by the D3D11
// compositor and its extracted helper units. Extracted VERBATIM from the
// anonymous namespace of D3D11CompositorAdapter.cpp so both that adapter and
// CompositorOverlayRaster share ONE definition (move-only refactor, no behavior
// change).
//
// putBlob() names ID3DBlob** so callers can hand the pointer straight to
// D3DCompile. We forward-declare the SDK's blob type here (mirroring
// d3dcommon.h's `typedef ID3D10Blob ID3DBlob;`) so this header stays free of the
// heavy <d3dcompiler.h> include and composes with it in any order; putBlob()'s
// body is only instantiated by TUs that already include the real D3D headers.
struct ID3D10Blob;
using ID3DBlob = ID3D10Blob;

namespace corevideo::modules {

template <typename T>
class ComPtrLite {
 public:
  ComPtrLite() = default;
  ~ComPtrLite() { reset(); }
  ComPtrLite(const ComPtrLite&) = delete;
  ComPtrLite& operator=(const ComPtrLite&) = delete;
  ComPtrLite(ComPtrLite&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  ComPtrLite& operator=(ComPtrLite&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }

  T** put() {
    reset();
    return &value_;
  }

  ID3DBlob** putBlob() { return reinterpret_cast<ID3DBlob**>(put()); }

  T* get() const { return value_; }
  T* operator->() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }

 private:
  void reset() {
    if (value_) {
      value_->Release();
      value_ = nullptr;
    }
  }

  T* value_ = nullptr;
};

}  // namespace corevideo::modules
