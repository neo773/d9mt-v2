#pragma once

#include <atomic>

#include "com_include.h"

#include "../util_likely.h"

namespace dxvk {

  template<typename T>
  class NoWrapper : public T {

  public:

    virtual ~NoWrapper() { }

  };

  template<typename Base>
  class ComObject : public Base {

  public:

    virtual ~ComObject() { }

    ULONG STDMETHODCALLTYPE AddRef() {
      uint32_t refCount = m_refCount++;
      if (unlikely(!refCount))
        AddRefPrivate();
      return refCount + 1;
    }

    ULONG STDMETHODCALLTYPE Release() {
      uint32_t refCount = --m_refCount;
      if (unlikely(!refCount))
        ReleasePrivate();
      return refCount;
    }


    void AddRefPrivate() {
      ++m_refPrivate;
    }


    void ReleasePrivate() {
      uint32_t refPrivate = --m_refPrivate;
      if (unlikely(!refPrivate)) {
        m_refPrivate += 0x80000000;
        delete this;
      }
    }

    ULONG GetPrivateRefCount() const {
      return m_refPrivate.load();
    }

    bool HasLiveReferences() const {
      return bool(m_refCount.load() | (m_refPrivate.load() & 0x7FFFFFFF));
    }

  protected:

    std::atomic<uint32_t> m_refCount   = { 0ul };
    std::atomic<uint32_t> m_refPrivate = { 0ul };

  };

  // Clamps the ref count at zero instead of wrapping around; some
  // games rely on D3D9's COM implementation behaving this way.
  template<typename Base>
  class ComObjectClamp : public ComObject<Base> {

  public:

    ULONG STDMETHODCALLTYPE Release() {
      ULONG refCount = this->m_refCount;
      if (likely(refCount != 0ul)) {
        this->m_refCount--;
        refCount--;

        if (refCount == 0ul)
          this->ReleasePrivate();
      }

      return refCount;
    }

  };

  template<typename T>
  inline void InitReturnPtr(T** ptr) {
    if (ptr != nullptr)
      *ptr = nullptr;
  }

}
