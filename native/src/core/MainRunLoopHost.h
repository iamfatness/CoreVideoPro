#pragma once

// Keeps the Apple main run loop serviced for the life of a blocking body.
//
// CoreMediaIO applies device arrivals in a process on the MAIN run loop
// (verified on macOS 26: a device that appears after the process's first CMIO
// call is added only while the main thread runs CFRunLoop; setting
// kCMIOHardwarePropertyRunLoop ('rnlp') to NULL or to another thread's run loop
// is accepted and ignored). The core's main thread used to block in
// std::getline(std::cin) for the whole session, so a camera extension that
// (re)started after launch (update, approval, crash) was never seen again:
// CAMERA_EXTENSION_NOT_FOUND forever while every other process saw the camera.
//
// runWithMainRunLoop() runs `body` on a worker thread while the calling thread
// (which must be the main thread) runs the main run loop, and returns the
// body's result once it finishes; an exception from the body is rethrown on
// the caller. On other platforms the body runs inline on the caller, so the
// Windows thread/COM-apartment layout is unchanged.

#include <exception>
#include <type_traits>
#include <utility>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <stdexcept>
#endif

namespace corevideo::core {

#if defined(__APPLE__)
// The body used to run on the main thread (8 MiB stack); a std::thread gets
// 512 KiB on macOS. Keep the main thread's size so nothing that ran there
// (MediaCore construction/teardown, every RPC handler) loses headroom.
inline constexpr std::size_t kMainRunLoopBodyStackBytes = 8u * 1024u * 1024u;
#endif

template <typename Body>
int runWithMainRunLoop(Body&& body) {
#if defined(__APPLE__)
  std::atomic<bool> done{false};
  int result = 0;
  std::exception_ptr failure;
  CFRunLoopRef mainLoop = CFRunLoopGetMain();
  // A run loop with no sources returns at once; this idle timer keeps it alive
  // so the wait below sleeps in the kernel instead of spinning.
  CFRunLoopTimerRef keepAlive = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 3600.0,
                                                     3600.0, 0, 0, [](CFRunLoopTimerRef, void*) {}, nullptr);
  CFRunLoopAddTimer(mainLoop, keepAlive, kCFRunLoopCommonModes);
  struct Context {
    std::remove_reference_t<Body>* body;
    int* result;
    std::exception_ptr* failure;
    std::atomic<bool>* done;
    CFRunLoopRef mainLoop;
  } context{&body, &result, &failure, &done, mainLoop};
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, kMainRunLoopBodyStackBytes);
  pthread_t worker;
  const int created = pthread_create(&worker, &attributes, [](void* raw) -> void* {
    auto* c = static_cast<Context*>(raw);
    try {
      *c->result = std::forward<Body>(*c->body)();
    } catch (...) {
      *c->failure = std::current_exception();
    }
    c->done->store(true, std::memory_order_release);
    CFRunLoopStop(c->mainLoop);
    CFRunLoopWakeUp(c->mainLoop);
    return nullptr;
  }, &context);
  pthread_attr_destroy(&attributes);
  if (created != 0) {
    CFRunLoopRemoveTimer(mainLoop, keepAlive, kCFRunLoopCommonModes);
    CFRelease(keepAlive);
    throw std::runtime_error("runWithMainRunLoop: pthread_create failed");
  }
  // Bounded slices: a stop that lands between two slices is caught by `done`.
  while (!done.load(std::memory_order_acquire)) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
  pthread_join(worker, nullptr);
  CFRunLoopRemoveTimer(mainLoop, keepAlive, kCFRunLoopCommonModes);
  CFRelease(keepAlive);
  if (failure) std::rethrow_exception(failure);
  return result;
#else
  return std::forward<Body>(body)();
#endif
}

}  // namespace corevideo::core
