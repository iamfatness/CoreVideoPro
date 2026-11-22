#pragma once
// The Metal grade-preview worker (grading Phase 2, G5/G6; spec §5.5-5.7). The macOS twin of the
// D3D11 grade worker (D3D11CompositorAdapter.cpp:223-238 + renderGradeBatch): the private,
// per-editor preview of one source with the draft grade (or the original for Compare), and the
// three scopes, rendered OFF the Program render path on its own thread, its own Metal command
// queue and its own textures. MetalCompositorAdapter only delegates the four ICompositor grade
// preview calls here, so this work never touches the render thread's state.
//
// Threading contract (spec §5.7): submit() runs on the render thread under coreMutex; it only
// swaps a pointer into the shared single-slot MonitorRenderWorker (pending capacity 1, a newer job
// supersedes the pending one) and never waits, allocates per pixel, or calls Metal. Everything
// Metal happens on the worker thread, which waits only on its OWN command buffers.
//
// Giving way (plan P4): while the core is shedding monitors (divisor d > 1) only every d-th submit
// is forwarded; after a job whose GPU time exceeded kGpuBudgetMs the next submit is skipped.
// An empty request (demand ended) is never shed: it releases the backend on the worker.

#include <cstdint>
#include <functional>
#include <memory>

#include "modules/Interfaces.h"

namespace corevideo::modules {

class MetalGradePreview {
 public:
  static constexpr double kGpuBudgetMs = 4.0;
  static constexpr int kMaxPreviewWidth = 1920, kMaxPreviewHeight = 1080;

  virtual ~MetalGradePreview() = default;
  // nullptr when this build has no Metal adapters.
  static std::unique_ptr<MetalGradePreview> create();

  virtual void submit(MonitorRenderRequest request) = 0;
  [[nodiscard]] virtual std::shared_ptr<const MonitorRenderResult> latest() const = 0;
  [[nodiscard]] virtual MonitorRenderDiagnostics diagnostics() const = 0;
  virtual void setMonitorShedDivisor(int divisor) = 0;
  // Submits not forwarded because of shedding or the GPU budget (tests and the log line).
  [[nodiscard]] virtual uint64_t shedCount() const = 0;
  [[nodiscard]] virtual double lastGpuMs() const = 0;

  // TEST ONLY: runs at the start of every worker job, on the worker thread. No product caller.
  static void setRenderStallForTest(std::function<void()> stall);
  // TEST ONLY: pretend the last job cost this much GPU time (exercises the budget rule).
  virtual void setLastGpuMsForTest(double ms) = 0;
};

}  // namespace corevideo::modules
