#pragma once

namespace corevideo::core {

// Keep the process MTA alive until every worker and COM-owned media resource
// has been destroyed. A worker's apartment must not be the last COM lifetime
// when its WIC/MF objects are owned by modules destroyed after that worker exits.
class ComApartmentLifetime {
 public:
  ComApartmentLifetime();
  ~ComApartmentLifetime();
  ComApartmentLifetime(const ComApartmentLifetime&) = delete;
  ComApartmentLifetime& operator=(const ComApartmentLifetime&) = delete;
  bool initialized() const { return initialized_; }

 private:
#if defined(_WIN32)
  bool initialized_ = false;
#else
  bool initialized_ = true;
#endif
};

}  // namespace corevideo::core
