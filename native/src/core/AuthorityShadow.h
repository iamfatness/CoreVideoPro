#pragma once
#include "core/LegacyShowCommandProjector.h"
#include "core/ShowPlanGenerator.h"
#include "core/ShadowExactSourceFrames.h"
#include "core/ZoomSourceAuthorityAdapter.h"
#include "core/SceneVersionShadow.h"
#include <atomic>
#include <condition_variable>
#include <thread>

namespace corevideo::core {
class AuthorityShadow final {
 public:
  struct Config {
    bool enabled{false};
    std::string nativeProcessEpoch, legacyAuthorityEpoch, authorityEpoch, registryEpoch;
    // Required when enabled, positive and at most 60 seconds.
    int64_t videoFreshnessWindowNs{0};
    size_t maxRecords{64}, maxBytes{2 * 1024 * 1024}, maxEntities{4096};
    // Deterministic offline mode; production keeps the dedicated worker enabled.
    bool workerThread{true};
    bool versionedScenes{false}; // Startup-fixed; no legacy fallback once enabled.
    SceneVersionStore::Limits sceneVersionLimits;
    // Startup-fixed opt-in; never affects plans, subscriptions or live rendering.
    bool exactFrameComparison{false};
  };
  struct Basis {
    std::string nativeProcessEpoch, legacyAuthorityEpoch;
    uint64_t captureSequence{0}, legacyRevision{0}, sourceSequence{0}, clockGeneration{1};
    int64_t capturedAtNs{0};
  };
  struct Record {
    Basis basis;
    bool completeCheckpoint{false};
    LegacyShowCommandDto desired;
    ZoomSourceAuthorityAdapter::Observation sources;
    struct SceneVersions {
      struct Definition { SceneVersionRef reference; ShowSceneIntent scene; };
      std::optional<SceneVersionRef> program, preview;
      // Complete payloads for the referenced buses, unique by exact reference.
      // At most two definitions; both buses may share one.
      std::vector<Definition> definitions;
    };
    std::optional<SceneVersions> sceneVersions;
    std::shared_ptr<const ShadowExactSourceFrames::Checkpoint> exactFrames;
  };
  enum class Admission { Accepted, Disabled, Stopped, Contended, Full, Oversized, PrivateMetadata, ModeMismatch };
  enum class Status { Disabled, AwaitingCheckpoint, Ready, BasisGap, AdapterInvalid, DisabledError, Stopped };
  struct Diagnostics {
    bool enabled{false}, continuous{false};
    Status status{Status::Disabled};
    uint64_t captured{0}, processed{0}, queueDrops{0}, oversized{0}, privateMetadata{0},
      basisGaps{0}, adapterInvalid{0}, exceptions{0}, lastProcessedSequence{0},
      controlRevision{0}, registryRevision{0};
    size_t queueDepth{0}, queueBytes{0}, maximumQueueDepth{0};
    // Plans are intent/identity projections only; no legacy comparison implemented.
    uint64_t compared{0}, matched{0};
  };
  struct ExactFrameComparison {
    enum class State { Disabled, Unavailable, Complete };
    State state{State::Disabled};
    // Historical eligibility at the captured basis, NOT rendering/delivery proof.
    uint64_t evaluated{0}, eligible{0}, missing{0}, expired{0};
  };
  struct Evidence {
    Basis basis;
    std::shared_ptr<const ShowPlans> plans;
    std::shared_ptr<const SceneVersionShadowEvidence> sceneVersions;
    ExactFrameComparison exactFrames;
  };
  explicit AuthorityShadow(Config config);
  ~AuthorityShadow();
  AuthorityShadow(const AuthorityShadow&) = delete;
  AuthorityShadow& operator=(const AuthorityShadow&) = delete;
  Admission submit(Record record);
  Diagnostics diagnostics() const;
  std::shared_ptr<const Evidence> latest() const;
  // Only valid with workerThread=false; same processing path as the worker.
  bool processOne();
  void stop();
 private:
  struct Envelope { Record record; size_t bytes{0}; uint64_t lossEpoch{0}; };
  struct Published { Evidence evidence; uint64_t lossEpoch{0}; };
  static size_t retainedBytes(const Record&, size_t limit);
  static bool hasPrivateMetadata(const Record&);
  bool consumeOne();
  void process(Envelope envelope);
  void run();
  void lose();
  Config config_;
  std::unique_ptr<ZoomSourceAuthorityAdapter> sources_;
  std::unique_ptr<ShowStateOwner> show_;
  std::unique_ptr<SceneVersionStore> sceneVersions_;
  mutable std::mutex queueMutex_;
  std::mutex processingMutex_, stopMutex_;
  std::atomic<size_t> queueDepthGauge_{0}, queueBytesGauge_{0}, maxDepthGauge_{0};
  std::condition_variable wake_;
  std::vector<std::optional<Envelope>> queue_;
  size_t head_{0}, tail_{0}, depth_{0}, bytes_{0}, maximumDepth_{0};
  std::atomic<bool> stopped_{false}, error_{false};
  std::atomic<uint64_t> lossEpoch_{0}, captured_{0}, processed_{0}, drops_{0}, oversized_{0},
    private_{0}, gaps_{0}, invalid_{0}, exceptions_{0}, lastSequence_{0};
  std::atomic<Status> status_{Status::Disabled};
  // Use portable shared_ptr atomic free functions; atomic<shared_ptr> is not
  // available in the deployment libc++ used by the macOS build.
  std::shared_ptr<const Published> published_;
  std::thread worker_;
};
} // namespace corevideo::core
