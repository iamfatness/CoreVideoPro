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
#include <utility>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <thread>
#endif

namespace corevideo::core {

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
  std::thread worker([&] {
    try {
      result = std::forward<Body>(body)();
    } catch (...) {
      failure = std::current_exception();
    }
    done.store(true, std::memory_order_release);
    CFRunLoopStop(mainLoop);
    CFRunLoopWakeUp(mainLoop);
  });
  // Bounded slices: a stop that lands between two slices is caught by `done`.
  while (!done.load(std::memory_order_acquire)) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
  worker.join();
  CFRunLoopRemoveTimer(mainLoop, keepAlive, kCFRunLoopCommonModes);
  CFRelease(keepAlive);
  if (failure) std::rethrow_exception(failure);
  return result;
#else
  return std::forward<Body>(body)();
#endif
}

}  // namespace corevideo::core
