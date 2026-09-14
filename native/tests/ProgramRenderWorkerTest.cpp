#include "core/ProgramRenderWorker.h"
#include <gtest/gtest.h>
#include <condition_variable>
#include <stdexcept>
#include <future>
using namespace corevideo::core;
namespace {
using Worker = ProgramRenderWorker;
// Generous on purpose. A correct worker satisfies every wait below in
// milliseconds and returns immediately, so a large budget costs a passing run
// nothing. Under a sanitizer on a loaded CI agent the same work takes an order
// of magnitude longer, and a tight budget makes thread scheduling -- not the
// code under test -- decide the verdict. Waits also sleep rather than yield:
// a yield-spin on a small runner starves the very worker thread it waits for.
constexpr auto kWait = std::chrono::seconds(10);
inline void waitTick() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
Worker::Generation generation(uint64_t number = 1) { return {"show", "clock", number, 1}; }
Worker::Config config() { Worker::Config c; c.enabled = true; c.generation = generation(); c.stallThreshold = std::chrono::milliseconds(1); return c; }
std::shared_ptr<const Worker::Work> work(int64_t slot, uint64_t gen = 1, uint64_t revision = 1) {
  ShowPlans plans; plans.render.stamp = plans.audio.stamp = plans.output.stamp = {"show", "registry", revision, 0, "eligibility"};
  return std::make_shared<const Worker::Work>(generation(gen), revision, slot,
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() + 1'000'000'000, plans);
}
struct Fixture {
  std::mutex mutex; std::condition_variable changed;
  bool entered{false}, release{false}, cooperative{true}, destroyed{false};
  std::thread::id createdThread, renderedThread, destroyedThread;
  size_t created{0}, calls{0};
  bool waitEntered() { std::unique_lock lock(mutex); return changed.wait_for(lock, kWait, [&]{return entered;}); }
  bool waitDestroyed() { std::unique_lock lock(mutex); return changed.wait_for(lock, kWait, [&]{return destroyed;}); }
  void unblock() { {std::lock_guard lock(mutex); release = true;} changed.notify_all(); }
};
struct Renderer final : Worker::Renderer {
  explicit Renderer(std::shared_ptr<Fixture> state) : f(std::move(state)) {
    std::lock_guard lock(f->mutex); f->createdThread = std::this_thread::get_id(); ++f->created;
  }
  ~Renderer() override {
    {std::lock_guard lock(f->mutex); f->destroyedThread = std::this_thread::get_id(); f->destroyed = true;} f->changed.notify_all();
  }
  bool render(const Worker::Work&, const std::atomic<bool>& cancelled) override {
    std::unique_lock lock(f->mutex); f->renderedThread = std::this_thread::get_id(); ++f->calls; f->entered = true; f->changed.notify_all();
    while (!f->release && !(f->cooperative && cancelled)) f->changed.wait_for(lock, std::chrono::milliseconds(1));
    return !cancelled;
  }
  std::shared_ptr<Fixture> f;
};
Worker::Factory factory(std::shared_ptr<Fixture> f) { return [f](const auto&, const auto&) {return std::make_unique<Renderer>(f);}; }
// submit() returns Contended rather than waiting when the worker holds its own
// lock. That is deliberate -- rule 3, no real-time worker waits -- so Contended
// is a legitimate outcome the CALLER must handle, never a failure. A renderer
// that returns immediately makes the worker cycle its lock tightly, and under a
// sanitizer that window is wide enough to hit routinely: a test that ignored the
// return silently never queued its work and then timed out waiting to render.
// Retry only on Contended; every other admission is returned as-is so tests that
// assert Stale or Disabled still assert exactly that.
Worker::Admission submitRetry(Worker& worker, std::shared_ptr<const Worker::Work> work);
bool waitRendered(Worker& worker, uint64_t count) {
  const auto until = std::chrono::steady_clock::now() + kWait;
  while (std::chrono::steady_clock::now() < until) {
    if (worker.diagnostics().rendered >= count) return true;
    waitTick();
  }
  return false;
}
Worker::Admission submitRetry(Worker& worker, std::shared_ptr<const Worker::Work> work) {
  const auto until = std::chrono::steady_clock::now() + kWait;
  for (;;) {
    const auto admission = worker.submit(work);
    if (admission != Worker::Admission::Contended) return admission;
    if (std::chrono::steady_clock::now() >= until) return admission;
    waitTick();
  }
}
}
TEST(ProgramRenderWorker, DisabledCreatesNoContextAndDoesNotSubmit) {
  int created = 0;
  Worker worker({}, [&](const auto&, const auto&) -> std::unique_ptr<Worker::Renderer> {++created; return {};});
  EXPECT_EQ(worker.submit(work(0)), Worker::Admission::Disabled);
  EXPECT_TRUE(worker.shutdown()); EXPECT_EQ(created, 0);
  EXPECT_TRUE(worker.shutdown());
}
TEST(ProgramRenderWorker, BlockedRendererCannotBlockSubmitAndQueueRemainsBounded) {
  auto f = std::make_shared<Fixture>(); Worker worker(config(), factory(f));
  ASSERT_EQ(submitRetry(worker, work(0)), Worker::Admission::Accepted); ASSERT_TRUE(f->waitEntered());
  auto one = work(1), two = work(2), three = work(3);
  auto producer = std::async(std::launch::async, [&] {
    return std::vector<Worker::Admission>{worker.submit(one), worker.submit(two), worker.submit(three)};
  });
  const bool nonblocking = producer.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
  if (!nonblocking) f->unblock(); // A blocking regression fails without hanging the suite.
  const auto admissions = producer.get();
  EXPECT_TRUE(nonblocking);
  EXPECT_EQ(admissions[0], Worker::Admission::Accepted);
  EXPECT_EQ(admissions[1], Worker::Admission::Accepted);
  EXPECT_EQ(admissions[2], Worker::Admission::Coalesced);
  const auto stallLimit = std::chrono::steady_clock::now() + kWait;
  while (!worker.diagnostics().stalled && std::chrono::steady_clock::now() < stallLimit) waitTick();
  EXPECT_EQ(worker.diagnostics().stalled, 1ULL);
  auto d = worker.diagnostics(); EXPECT_EQ(d.queued, 2U); EXPECT_EQ(d.maximumQueued, 2U); EXPECT_EQ(d.coalesced, 1ULL); EXPECT_EQ(d.skipped, 1ULL);
  f->unblock(); ASSERT_TRUE(waitRendered(worker, 3)); EXPECT_EQ(worker.lastRendered()->slot, 3);
  EXPECT_TRUE(worker.shutdown());
}
TEST(ProgramRenderWorker, ContextLifecycleBelongsToSingleWorkerThread) {
  auto f = std::make_shared<Fixture>(); f->release = true;
  Worker worker(config(), factory(f)); submitRetry(worker, work(0)); ASSERT_TRUE(waitRendered(worker, 1));
  ASSERT_TRUE(worker.shutdown()); ASSERT_TRUE(f->waitDestroyed());
  EXPECT_EQ(f->createdThread, f->renderedThread); EXPECT_EQ(f->createdThread, f->destroyedThread);
  EXPECT_FALSE(f->createdThread == std::this_thread::get_id());
}
TEST(ProgramRenderWorker, GenerationResetFencesQueuedAndInflightCompletions) {
  auto f = std::make_shared<Fixture>(); Worker worker(config(), factory(f));
  submitRetry(worker, work(0)); ASSERT_TRUE(f->waitEntered()); submitRetry(worker, work(1));
  EXPECT_EQ(worker.reset(generation(), generation(2)), Worker::Admission::Accepted);
  EXPECT_EQ(submitRetry(worker, work(2)), Worker::Admission::Stale);
  EXPECT_EQ(submitRetry(worker, work(0, 2)), Worker::Admission::Accepted);
  EXPECT_FALSE(worker.lastRendered()); f->unblock(); ASSERT_TRUE(waitRendered(worker, 1));
  EXPECT_EQ(worker.lastRendered()->generation.renderer, 2ULL);
  EXPECT_EQ(worker.diagnostics().staleCompletions, 1ULL); EXPECT_EQ(worker.diagnostics().skipped, 2ULL);
  ASSERT_TRUE(worker.shutdown()); EXPECT_EQ(f->created, 2U);
}
TEST(ProgramRenderWorker, NoncooperativeRenderShutdownIsBoundedAndOwnerSurvivesUntilExit) {
  auto f = std::make_shared<Fixture>(); f->cooperative = false;
  auto c = config(); c.shutdownBudget = std::chrono::milliseconds(5);
  bool completed; std::chrono::steady_clock::duration elapsed;
  {
    Worker worker(c, factory(f)); submitRetry(worker, work(0)); ASSERT_TRUE(f->waitEntered());
    const auto start = std::chrono::steady_clock::now(); completed = worker.shutdown(); elapsed = std::chrono::steady_clock::now() - start;
  }
  f->unblock(); ASSERT_TRUE(f->waitDestroyed());
  EXPECT_FALSE(completed); EXPECT_TRUE(elapsed < std::chrono::milliseconds(200));
  EXPECT_EQ(f->createdThread, f->destroyedThread);
}
TEST(ProgramRenderWorker, ImmutableWorkAndStaleRevisionCannotOverwriteRenderedPlan) {
  ShowPlans plans; plans.render.stamp = plans.audio.stamp = plans.output.stamp = {"show", "registry", 4, 0, "original"};
  auto frozen = std::make_shared<const Worker::Work>(generation(), 4, 0, 0, plans);
  plans.render.stamp.eligibilityIdentity = "mutated"; EXPECT_EQ(frozen->plans.render.stamp.eligibilityIdentity, "original");
  auto f = std::make_shared<Fixture>(); f->release = true; Worker worker(config(), factory(f));
  submitRetry(worker, frozen); ASSERT_TRUE(waitRendered(worker, 1));
  EXPECT_EQ(submitRetry(worker, work(1, 1, 3)), Worker::Admission::Stale);
  EXPECT_EQ(submitRetry(worker, work(0, 1, 4)), Worker::Admission::Stale);
  EXPECT_EQ(worker.diagnostics().deadlineMisses, 1ULL);
  EXPECT_TRUE(worker.shutdown());
}
TEST(ProgramRenderWorker, FactoryFailureIsContainedAndWorkMetadataIsBounded) {
  Worker worker(config(), [](const auto&, const auto&) -> std::unique_ptr<Worker::Renderer> {throw std::runtime_error("factory");});
  submitRetry(worker, work(0));
  const auto until = std::chrono::steady_clock::now() + kWait;
  while (!worker.diagnostics().failed && std::chrono::steady_clock::now() < until) waitTick();
  EXPECT_EQ(worker.diagnostics().failed, 1ULL); EXPECT_TRUE(worker.shutdown());
  ShowPlans oversized; oversized.render.inputs.reserve(4097);
  bool rejected = false; try { Worker::Work invalid(generation(), 1, 0, 0, oversized); } catch (const std::invalid_argument&) { rejected = true; }
  EXPECT_TRUE(rejected);
}
TEST(ProgramRenderWorker, ShutdownReleasesQueuedAndLatestOwnersEvenWhenRenderIsStuck) {
  auto f = std::make_shared<Fixture>(); f->release = true; f->cooperative = false;
  auto c = config(); c.shutdownBudget = std::chrono::milliseconds(5);
  Worker worker(c, factory(f));
  auto first = work(0); std::weak_ptr<const Worker::Work> latest = first;
  submitRetry(worker, first); first.reset(); ASSERT_TRUE(waitRendered(worker, 1));
  EXPECT_FALSE(latest.expired());
  {std::lock_guard lock(f->mutex); f->release = false; f->entered = false;}
  auto second = work(1); std::weak_ptr<const Worker::Work> inflight = second;
  submitRetry(worker, second); second.reset(); ASSERT_TRUE(f->waitEntered());
  auto third = work(2); std::weak_ptr<const Worker::Work> queued = third;
  submitRetry(worker, third); third.reset();
  const bool done = worker.shutdown();
  const bool latestReleased = latest.expired(), queuedReleased = queued.expired(), inflightRetained = !inflight.expired();
  f->unblock(); ASSERT_TRUE(f->waitDestroyed());
  EXPECT_FALSE(done); EXPECT_TRUE(latestReleased); EXPECT_TRUE(queuedReleased); EXPECT_TRUE(inflightRetained);
  EXPECT_TRUE(inflight.expired());
  const auto completionLimit = std::chrono::steady_clock::now() + kWait;
  while (!worker.diagnostics().shutdownComplete && std::chrono::steady_clock::now() < completionLimit) waitTick();
  EXPECT_TRUE(worker.shutdown());
  EXPECT_FALSE(worker.lastRendered()); EXPECT_EQ(worker.diagnostics().queued, 0U);
}
