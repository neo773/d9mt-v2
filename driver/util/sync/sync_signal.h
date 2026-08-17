#pragma once

#include <atomic>
#include <functional>
#include <list>

#include "../rc/util_rc.h"

#include "../thread.h"

namespace dxvk::sync {

  class Signal : public RcObject {

  public:

    virtual ~Signal() { }

    virtual uint64_t value() const = 0;

    virtual void signal(uint64_t value) = 0;

    virtual void wait(uint64_t value) = 0;

  };


  class SyncPoint {

  public:

    SyncPoint() = default;
    SyncPoint(Rc<Signal> signal, uint64_t value)
    : m_signal(std::move(signal)), m_value(value) { }

    void synchronize() {
      if (m_signal) {
        m_signal->wait(m_value);
        m_signal = nullptr;
      }
    }

  private:

    Rc<Signal>  m_signal;
    uint64_t    m_value = 0u;

  };


  class Fence final : public Signal {

  public:

    Fence()
    : m_value(0ull) { }

    explicit Fence(uint64_t value)
    : m_value(value) { }

    uint64_t value() const {
      return m_value.load(std::memory_order_acquire);
    }

    void signal(uint64_t value) {
      std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_value.store(value, std::memory_order_release);
      m_cond.notify_all();
    }

    void wait(uint64_t value) {
      if (value <= m_value.load(std::memory_order_acquire))
        return;

      std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_cond.wait(lock, [this, value] {
        return value <= m_value.load(std::memory_order_acquire);
      });
    }

  private:

    std::atomic<uint64_t>    m_value;
    dxvk::mutex              m_mutex;
    dxvk::condition_variable m_cond;

  };


  class CallbackFence final : public Signal {

  public:

    CallbackFence()
    : m_value(0ull) { }

    explicit CallbackFence(uint64_t value)
    : m_value(value) { }

    uint64_t value() const {
      return m_value.load(std::memory_order_acquire);
    }

    void signal(uint64_t value) {
      std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_value.store(value, std::memory_order_release);
      m_cond.notify_all();

      for (auto i = m_callbacks.begin(); i != m_callbacks.end(); ) {
        if (value >= i->first) {
          i->second();
          i = m_callbacks.erase(i);
        } else {
          i++;
        }
      }
    }

    void wait(uint64_t value) {
      if (value <= m_value.load(std::memory_order_acquire))
        return;

      std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_cond.wait(lock, [this, value] {
        return value <= m_value.load(std::memory_order_acquire);
      });
    }

    template<typename Fn>
    void setCallback(uint64_t value, Fn&& proc) {
      if (value <= this->value()) {
        proc();
        return;
      }

      std::unique_lock<dxvk::mutex> lock(m_mutex);

      if (value > this->value())
        m_callbacks.emplace_back(std::piecewise_construct,
          std::make_tuple(value),
          std::make_tuple(proc));
      else {
        lock.unlock();
        proc();
      }
    }

  private:

    std::atomic<uint64_t>    m_value;
    dxvk::mutex              m_mutex;
    dxvk::condition_variable m_cond;

    std::list<std::pair<uint64_t, std::function<void ()>>> m_callbacks;

  };

}
