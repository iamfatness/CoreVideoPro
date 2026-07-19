#include "engine-video.h"
#include "engine-writer.h"
#if __has_include(<rawdata/zoom_rawdata_api.h>)
#include <rawdata/zoom_rawdata_api.h>
#else
#include <zoom_rawdata_api.h>
#endif
#include <cstring>
#include <limits>
#include <atomic>
#include <algorithm>
#include <iterator>
#include <tuple>
#include <vector>

static ZOOMSDK::ZoomSDKResolution sdk_resolution(uint32_t resolution)
{
    switch (resolution) {
    case 0: return ZOOMSDK::ZoomSDKResolution_360P;
    case 2: return ZOOMSDK::ZoomSDKResolution_1080P;
    case 1:
    default: return ZOOMSDK::ZoomSDKResolution_720P;
    }
}

static bool valid_i420_frame(YUVRawDataI420 *data, uint32_t w, uint32_t h, size_t &y_len)
{
    if (w == 0 || h == 0) return false;
    if (w > 8192 || h > 8192) return false;
    if ((w & 1) != 0 || (h & 1) != 0) return false;
    if (!data->GetYBuffer() || !data->GetUBuffer() || !data->GetVBuffer()) return false;

    const uint64_t pixels = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
    const uint64_t max_reasonable_i420 = 8192ull * 8192ull;
    constexpr uint64_t max_size_t_value = static_cast<uint64_t>(~size_t{0});
    if (pixels > max_reasonable_i420 || pixels > max_size_t_value) {
        return false;
    }

    y_len = static_cast<size_t>(pixels);
    return true;
}

ParticipantSubscription::ParticipantSubscription(uint32_t participant_id,
                                                 const std::string &initial_source_uuid,
                                                 IpcFd e2p_fd,
                                                 uint32_t resolution)
    : m_participant_id(participant_id)
{
    if (resolution > 2) resolution = 1;

    std::vector<uint32_t> attempts;
    for (int candidate = static_cast<int>(resolution); candidate >= 0; --candidate)
        attempts.push_back(static_cast<uint32_t>(candidate));

    for (const uint32_t candidate_resolution : attempts) {
        ZOOMSDK::SDKError err = ZOOMSDK::createRenderer(&m_renderer, this);
        if (err != ZOOMSDK::SDKERR_SUCCESS || !m_renderer) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"create_renderer_failed","source_uuid":")" +
                initial_source_uuid + R"(","participant_id":)" +
                std::to_string(m_participant_id) + R"(,"code":)" +
                std::to_string(static_cast<int>(err)) + R"(,"resolution":)" +
                std::to_string(candidate_resolution) + "}");
            continue;
        }

        m_resolution = candidate_resolution;
        const ZOOMSDK::SDKError res_err =
            m_renderer->setRawDataResolution(sdk_resolution(m_resolution));
        EngineIpc::write(
            R"({"cmd":"debug","stage":"set_resolution","source_uuid":")" +
            initial_source_uuid + R"(","participant_id":)" +
            std::to_string(m_participant_id) + R"(,"code":)" +
            std::to_string(static_cast<int>(res_err)) + R"(,"resolution":)" +
            std::to_string(m_resolution) + "}");

        err = m_renderer->subscribe(participant_id, ZOOMSDK::RAW_DATA_TYPE_VIDEO);
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_subscribe","source_uuid":")" +
            initial_source_uuid + R"(","participant_id":)" +
            std::to_string(m_participant_id) + R"(,"code":)" +
            std::to_string(static_cast<int>(err)) + R"(,"resolution":)" +
            std::to_string(m_resolution) + "}");
        if (err == ZOOMSDK::SDKERR_SUCCESS) {
            add_source(initial_source_uuid, e2p_fd);
            if (m_resolution != resolution) {
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"video_resolution_downgraded","source_uuid":")" +
                    initial_source_uuid + R"(","participant_id":)" +
                    std::to_string(m_participant_id) + R"(,"requested":)" +
                    std::to_string(resolution) + R"(,"actual":)" +
                    std::to_string(m_resolution) + "}");
            }
            return;
        }

        ZOOMSDK::destroyRenderer(m_renderer);
        m_renderer = nullptr;
    }

    EngineIpc::write(
        R"({"cmd":"debug","stage":"video_subscribe_failed_all","source_uuid":")" +
        initial_source_uuid + R"(","participant_id":)" +
        std::to_string(m_participant_id) + R"(,"requested":)" +
        std::to_string(resolution) + "}");
}

ParticipantSubscription::~ParticipantSubscription()
{
    // Teardown must serialize with an in-flight onRawDataFrameReceived (the
    // ZoomISO rule: never destroy a renderer while a raw-data callback is
    // running). EngineShare gets this by taking m_mtx in both the callback and
    // the teardown; here we use the stopping-flag + drain variant instead of
    // holding m_targets_mtx across the SDK calls, because we cannot prove
    // unSubscribe()/destroyRenderer() never synchronously wait on a callback
    // that itself takes m_targets_mtx (holding it across them could deadlock).
    // Order: (1) flag so new callbacks bail before touching targets,
    // (2) acquire+release m_targets_mtx to drain a callback already inside,
    // (3) only then tear the renderer down, (4) free the SHM under the lock.
    m_stopping.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> drain(m_targets_mtx); }
    if (m_renderer) {
        m_renderer->unSubscribe();
        ZOOMSDK::destroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    for (auto &entry : m_targets) {
        if (entry.second) shm_region_destroy(entry.second->shm);
    }
}

void ParticipantSubscription::add_source(const std::string &source_uuid, IpcFd e2p_fd)
{
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    const auto [it, inserted] = m_targets.emplace(source_uuid, nullptr);
    if (inserted)
        it->second = std::make_unique<SourceTarget>(e2p_fd);
}

size_t ParticipantSubscription::target_count() const
{
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    return m_targets.size();
}

void ParticipantSubscription::remove_source(const std::string &source_uuid)
{
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    auto it = m_targets.find(source_uuid);
    if (it == m_targets.end()) return;
    if (it->second) shm_region_destroy(it->second->shm);
    m_targets.erase(it);
}

bool ParticipantSubscription::empty() const
{
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    return m_targets.empty();
}

std::vector<std::pair<std::string, IpcFd>> ParticipantSubscription::sources() const
{
    std::vector<std::pair<std::string, IpcFd>> result;
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    result.reserve(m_targets.size());
    for (const auto &entry : m_targets) {
        if (entry.second) result.emplace_back(entry.first, entry.second->e2p_fd);
    }
    return result;
}

bool ParticipantSubscription::ensure_shm(SourceTarget &target,
                                         const std::string &source_uuid,
                                         size_t y_len)
{
    const size_t total =
        sizeof(ShmFrameHeader) + y_len + y_len / 4 + y_len / 4;
    if (total < y_len) return false;
    if (target.shm.ptr && target.shm.size >= total) return true;

    const std::string region_name = EngineIpc::shm_prefix() + source_uuid;
    return shm_region_create(target.shm, region_name, total);
}

void ParticipantSubscription::onRawDataFrameReceived(YUVRawDataI420 *data)
{
    if (!data) return;
    if (m_stopping.load(std::memory_order_acquire)) return; // teardown in progress
    const uint32_t w     = data->GetStreamWidth();
    const uint32_t h     = data->GetStreamHeight();
    size_t y_len = 0;
    if (!valid_i420_frame(data, w, h, y_len)) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_frame_invalid","participant_id":)" +
            std::to_string(m_participant_id) + R"(,"w":)" +
            std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
        return;
    }

    std::lock_guard<std::mutex> lock(m_targets_mtx);
    // Re-check under the lock: the destructor may have set the flag between the
    // early check and this acquisition; past this point it drains behind us.
    if (m_stopping.load(std::memory_order_acquire)) return;
    for (auto &entry : m_targets) {
        const std::string &source_uuid = entry.first;
        SourceTarget &target = *entry.second;

        if (!ensure_shm(target, source_uuid, y_len) || !target.shm.ptr) {
            if (target.frame_count == 0) {
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"video_shm_create_failed","source_uuid":")" +
                    source_uuid + R"(","participant_id":)" +
                    std::to_string(m_participant_id) + R"(,"w":)" +
                    std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
            }
            continue;
        }

        auto *hdr    = static_cast<ShmFrameHeader *>(target.shm.ptr);
        auto *pixels = static_cast<char *>(target.shm.ptr) + sizeof(ShmFrameHeader);
        uint32_t seq = hdr->sequence + 1;
        if ((seq & 1u) == 0) ++seq;
        hdr->sequence = seq;
        std::atomic_thread_fence(std::memory_order_release);
        hdr->width = w;
        hdr->height = h;
        hdr->y_len = static_cast<uint32_t>(y_len);

        std::memcpy(pixels,                   data->GetYBuffer(), y_len);
        std::memcpy(pixels + y_len,           data->GetUBuffer(), y_len / 4);
        std::memcpy(pixels + y_len + y_len/4, data->GetVBuffer(), y_len / 4);
        std::atomic_thread_fence(std::memory_order_release);
        hdr->sequence = seq + 1;

        ++target.frame_count;
        if (target.frame_count == 1 || target.frame_count % 120 == 0) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"video_frame_received","source_uuid":")" +
                source_uuid + R"(","participant_id":)" +
                std::to_string(m_participant_id) + R"(,"count":)" +
                std::to_string(target.frame_count) + R"(,"w":)" +
                std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
        }

        // Video-beacon fix: the core reads frames on its render poll with a
        // sequence peek - the event is a DISCOVERY/dimension beacon. Per-frame
        // events at 30fps x N sources drowned the core command queue
        // (soak-measured queueWait 3.7s). Emit on first frame, dimension
        // change, and ~1/s as liveness.
        const bool dimsChanged = w != target.last_beacon_w || h != target.last_beacon_h;
        if (target.frame_count == 1 || dimsChanged || target.frame_count % 30 == 0) {
            target.last_beacon_w = w;
            target.last_beacon_h = h;
            EngineIpc::write(
                R"({"cmd":"frame","source_uuid":")" + source_uuid +
                R"(","participant_id":)" + std::to_string(m_participant_id) +
                R"(,"w":)" + std::to_string(w) + R"(,"h":)" + std::to_string(h) + "}");
        }
    }
}

void ParticipantSubscription::onRawDataStatusChanged(
    ZOOMSDK::IZoomSDKRendererDelegate::RawDataStatus status)
{
    if (m_stopping.load(std::memory_order_acquire)) return; // teardown in progress
    std::lock_guard<std::mutex> lock(m_targets_mtx);
    if (m_stopping.load(std::memory_order_acquire)) return;
    for (const auto &entry : m_targets) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_raw_status","source_uuid":")" +
            entry.first + R"(","participant_id":)" +
            std::to_string(m_participant_id) + R"(,"status":)" +
            std::to_string(static_cast<int>(status)) + "}");
    }
}

void ParticipantSubscription::onRendererBeDestroyed()
{
    m_renderer = nullptr;
}

void EngineVideo::subscribe(uint32_t participant_id,
                             const std::string &source_uuid,
                             IpcFd e2p_fd,
                             uint32_t resolution)
{
    // Declared BEFORE the lock so retired subscriptions destruct AFTER m_mtx
    // is released (locals destruct in reverse order): their destructors make
    // SDK calls (unSubscribe/destroyRenderer) that must not run under m_mtx.
    std::vector<std::unique_ptr<ParticipantSubscription>> retired;
    std::unique_lock<std::mutex> lock(m_mtx);

    if (participant_id == 0) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_subscribe_skipped","source_uuid":")" +
            source_uuid + R"(","participant_id":0,"reason":"missing_participant"})");
        if (auto r = unsubscribe_locked(source_uuid)) retired.push_back(std::move(r));
        return;
    }

    // Bound the number of distinct video source UUIDs. Each source backs a
    // shared-memory region, so without a cap a misbehaving or runaway plugin
    // could create unbounded SHM regions and exhaust memory. The product
    // targets up to 8 feeds plus active-speaker/spotlight/screenshare slots;
    // 32 leaves generous headroom while staying bounded. Re-subscribing an
    // existing source is always allowed.
    constexpr size_t kMaxVideoSources = 32;
    if (m_source_participants.find(source_uuid) == m_source_participants.end() &&
        m_source_participants.size() >= kMaxVideoSources) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_subscribe_rejected_capacity","source_uuid":")" +
            source_uuid + R"(","participant_id":)" +
            std::to_string(participant_id) + R"(,"limit":)" +
            std::to_string(kMaxVideoSources) + "}");
        return;
    }

    auto existing_source = m_source_participants.find(source_uuid);
    if (existing_source != m_source_participants.end()) {
        if (existing_source->second.participant_id == participant_id) {
            auto existing_sub = m_subs.find(participant_id);
            if (existing_sub != m_subs.end() && existing_sub->second &&
                existing_sub->second->active()) {
                const uint32_t current_resolution = existing_sub->second->resolution();
                if (resolution <= current_resolution) {
                    EngineIpc::write(
                        R"({"cmd":"debug","stage":"video_subscribe_noop_existing","source_uuid":")" +
                        source_uuid + R"(","participant_id":)" +
                        std::to_string(participant_id) + R"(,"requested":)" +
                        std::to_string(resolution) + R"(,"active":)" +
                        std::to_string(current_resolution) + "}");
                    return;
                }
            } else {
                if (auto r = unsubscribe_locked(source_uuid)) retired.push_back(std::move(r));
            }
        } else {
            if (auto r = unsubscribe_locked(source_uuid)) retired.push_back(std::move(r));
        }
    }

    if (!m_raw_media_active) {
        m_source_participants[source_uuid] = {
            participant_id,
            resolution,
            e2p_fd
        };
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_subscribe_deferred","source_uuid":")" +
            source_uuid + R"(","participant_id":)" +
            std::to_string(participant_id) + R"(,"requested":)" +
            std::to_string(resolution) + R"(,"reason":"raw_media_not_ready"})");
        return;
    }

    auto it = m_subs.find(participant_id);
    if (it != m_subs.end() && it->second) {
        if (it->second->active()) {
            if (resolution > it->second->resolution()) {
                auto targets = it->second->sources();
                const auto already_targeted =
                    std::find_if(targets.begin(), targets.end(),
                                 [&source_uuid](const auto &target) {
                                     return target.first == source_uuid;
                                 }) != targets.end();
                if (!already_targeted)
                    targets.emplace_back(source_uuid, e2p_fd);
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"video_upgrade_subscription","source_uuid":")" +
                    source_uuid + R"(","participant_id":)" +
                    std::to_string(participant_id) + R"(,"requested":)" +
                    std::to_string(resolution) + R"(,"previous":)" +
                    std::to_string(it->second->resolution()) + R"(,"active_targets":)" +
                    std::to_string(targets.size()) + "}");

                retired.push_back(std::move(it->second));
                m_subs.erase(it);
                for (const auto &target : targets)
                    m_source_participants.erase(target.first);

                // Destroy the old renderer and build the replacement OUTSIDE
                // m_mtx (ctor/dtor make SDK calls); destroy-before-create so
                // the participant never has two live renderers.
                lock.unlock();
                retired.clear();
                auto upgraded = std::make_unique<ParticipantSubscription>(
                    participant_id, targets.front().first,
                    targets.front().second, resolution);
                lock.lock();
                if (!upgraded || upgraded->empty()) {
                    if (upgraded) retired.push_back(std::move(upgraded));
                    return;
                }
                for (size_t i = 1; i < targets.size(); ++i)
                    upgraded->add_source(targets[i].first, targets[i].second);
                // A concurrent subscribe may have re-created an entry while we
                // were unlocked - extract it so it is destroyed off-lock too.
                auto clash = m_subs.find(participant_id);
                if (clash != m_subs.end()) {
                    if (clash->second) retired.push_back(std::move(clash->second));
                    m_subs.erase(clash);
                }
                it = m_subs.emplace(participant_id, std::move(upgraded)).first;
                for (const auto &target : targets) {
                    m_source_participants[target.first] = {
                        participant_id,
                        it->second->resolution(),
                        target.second
                    };
                }
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"video_subscription_upgraded","source_uuid":")" +
                    source_uuid + R"(","participant_id":)" +
                    std::to_string(participant_id) + R"(,"requested":)" +
                    std::to_string(resolution) + R"(,"actual":)" +
                    std::to_string(it->second->resolution()) + R"(,"active_targets":)" +
                    std::to_string(it->second->target_count()) + "}");
                return;
            }
            it->second->add_source(source_uuid, e2p_fd);
            m_source_participants[source_uuid] = {
                participant_id,
                it->second->resolution(),
                e2p_fd
            };
            EngineIpc::write(
                R"({"cmd":"debug","stage":"video_source_attached_existing_subscription","source_uuid":")" +
                source_uuid + R"(","participant_id":)" +
                std::to_string(participant_id) + R"(,"resolution":)" +
                std::to_string(it->second->resolution()) + R"(,"active_targets":)" +
                std::to_string(it->second->target_count()) + "}");
            return;
        }
        retired.push_back(std::move(it->second));
        m_subs.erase(it);
        it = m_subs.end();
    }

    if (it == m_subs.end()) {
        // Build (and destroy any retired predecessor) OUTSIDE m_mtx: the
        // ParticipantSubscription ctor/dtor call into the SDK.
        lock.unlock();
        retired.clear();
        auto created = std::make_unique<ParticipantSubscription>(
            participant_id, source_uuid, e2p_fd, resolution);
        lock.lock();
        if (!created || created->empty()) {
            if (created) retired.push_back(std::move(created));
            return;
        }
        auto clash = m_subs.find(participant_id);
        if (clash != m_subs.end()) {
            if (clash->second) retired.push_back(std::move(clash->second));
            m_subs.erase(clash);
        }
        it = m_subs.emplace(participant_id, std::move(created)).first;
    } else {
        it->second->add_source(source_uuid, e2p_fd);
    }
    m_source_participants[source_uuid] = {
        participant_id,
        it->second->resolution(),
        e2p_fd
    };
    EngineIpc::write(
        R"({"cmd":"debug","stage":"video_source_bound","source_uuid":")" +
        source_uuid + R"(","participant_id":)" +
        std::to_string(participant_id) + R"(,"requested":)" +
        std::to_string(resolution) + R"(,"actual":)" +
        std::to_string(it->second->resolution()) + R"(,"participant_subscriptions":)" +
        std::to_string(m_subs.size()) + R"(,"source_bindings":)" +
        std::to_string(m_source_participants.size()) + "}");
}

void EngineVideo::set_raw_media_active(bool active)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_raw_media_active == active) return;
    m_raw_media_active = active;
    EngineIpc::write(
        R"({"cmd":"debug","stage":"video_raw_media_state","active":)" +
        std::string(active ? "true" : "false") +
        R"(,"pending_sources":)" +
        std::to_string(m_source_participants.size()) + "}");
}

void EngineVideo::unsubscribe(const std::string &source_uuid)
{
    std::unique_ptr<ParticipantSubscription> retired;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        retired = unsubscribe_locked(source_uuid);
    }
    // retired (if any) destructs here: unSubscribe/destroyRenderer off-lock.
}

std::unique_ptr<ParticipantSubscription>
EngineVideo::unsubscribe_locked(const std::string &source_uuid)
{
    auto source_it = m_source_participants.find(source_uuid);
    if (source_it == m_source_participants.end()) return nullptr;

    const uint32_t participant_id = source_it->second.participant_id;
    m_source_participants.erase(source_it);

    auto sub_it = m_subs.find(participant_id);
    if (sub_it == m_subs.end() || !sub_it->second) return nullptr;

    sub_it->second->remove_source(source_uuid);
    if (!sub_it->second->empty()) return nullptr;
    auto retired = std::move(sub_it->second);
    m_subs.erase(sub_it);
    return retired;
}

void EngineVideo::resubscribe_all()
{
    std::vector<std::tuple<std::string, uint32_t, IpcFd, uint32_t>> current;
    std::unordered_map<uint32_t,
                       std::unique_ptr<ParticipantSubscription>> retired;
    bool deferred = false;
    size_t pending = 0;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        for (const auto &entry : m_source_participants) {
            if (entry.second.e2p_fd != kIpcInvalidFd) {
                current.emplace_back(entry.first,
                                     entry.second.participant_id,
                                     entry.second.e2p_fd,
                                     entry.second.resolution);
            }
        }
        if (current.empty()) {
            for (const auto &entry : m_subs) {
                if (entry.second) {
                    const uint32_t participant_id = entry.second->participant_id();
                    const uint32_t resolution = entry.second->resolution();
                    const auto sources = entry.second->sources();
                    std::transform(sources.begin(), sources.end(),
                                   std::back_inserter(current),
                                   [participant_id, resolution](const auto &source) {
                                       return std::make_tuple(source.first,
                                                              participant_id,
                                                              source.second,
                                                              resolution);
                                   });
                }
            }
        }

        retired.swap(m_subs);
        m_source_participants.clear();
        if (!m_raw_media_active) {
            std::for_each(current.begin(), current.end(), [this](const auto &entry) {
                const auto &[source_uuid, participant_id, e2p_fd, resolution] = entry;
                m_source_participants[source_uuid] = {
                    participant_id,
                    resolution,
                    e2p_fd
                };
            });
            deferred = true;
            pending = m_source_participants.size();
        }
    }
    // Old renderers tear down OUTSIDE m_mtx (SDK calls), and BEFORE the new
    // subscriptions are built (each subscribe() re-locks internally).
    retired.clear();
    if (deferred) {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"video_resubscribe_deferred","pending_sources":)" +
            std::to_string(pending) +
            R"(,"reason":"raw_media_not_ready"})");
        return;
    }
    std::for_each(current.begin(), current.end(), [this](const auto &entry) {
        const auto &[source_uuid, participant_id, e2p_fd, resolution] = entry;
        subscribe(participant_id, source_uuid, e2p_fd, resolution);
    });
}

void EngineVideo::unsubscribe_all()
{
    std::unordered_map<uint32_t,
                       std::unique_ptr<ParticipantSubscription>> retired;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        retired.swap(m_subs);
        m_source_participants.clear();
    }
    // Renderer teardown (unSubscribe/destroyRenderer per subscription) runs
    // here, outside m_mtx.
}
