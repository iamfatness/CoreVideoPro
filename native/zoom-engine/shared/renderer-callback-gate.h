#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>

// SDK renderer calls can synchronously invoke delegates, or wait for delegates
// on another thread. Drain callback access under the state mutex, then suspend
// new callbacks and release that mutex for the whole SDK operation.
class RendererCallbackGate {
public:
    bool suspended() const { return m_suspended.load(std::memory_order_acquire); }

    std::unique_lock<std::mutex> lifecycle(std::mutex &mutex) {
        std::unique_lock<std::mutex> lock(mutex);
        m_ready.wait(lock, [this] { return !suspended(); });
        return lock;
    }

    std::unique_lock<std::mutex> enter(std::mutex &mutex) const {
        if (suspended()) return {};
        std::unique_lock<std::mutex> lock(mutex);
        if (suspended()) return {};
        return lock;
    }

    class Transition {
    public:
        Transition(RendererCallbackGate &gate, std::unique_lock<std::mutex> &lock)
            : m_gate(gate), m_lock(lock) {
            m_gate.m_suspended.store(true, std::memory_order_release);
            m_lock.unlock();
        }
        ~Transition() {
            m_lock.lock();
            m_gate.m_suspended.store(false, std::memory_order_release);
            m_gate.m_ready.notify_all();
        }
        Transition(const Transition &) = delete;
        Transition &operator=(const Transition &) = delete;
    private:
        RendererCallbackGate &m_gate;
        std::unique_lock<std::mutex> &m_lock;
    };
private:
    std::atomic<bool> m_suspended{false};
    std::condition_variable m_ready;
};
