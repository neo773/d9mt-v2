#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "com_include.h"

namespace dxvk {

  enum class ComPrivateDataType : uint32_t {
    None,
    Data,
    Iface,
  };

  class ComPrivateDataEntry {

  public:

    ComPrivateDataEntry();
    ComPrivateDataEntry(
            REFGUID   guid,
            UINT      size,
      const void*     data);
    ComPrivateDataEntry(
            REFGUID   guid,
      const IUnknown* iface);
    ~ComPrivateDataEntry();

    ComPrivateDataEntry             (ComPrivateDataEntry&& other);
    ComPrivateDataEntry& operator = (ComPrivateDataEntry&& other);

    REFGUID guid() const {
      return m_guid;
    }

    bool hasGuid(REFGUID guid) const {
      return m_guid == guid;
    }

    HRESULT get(UINT& size, void* data) const;

  private:

    GUID                m_guid  = __uuidof(IUnknown);
    ComPrivateDataType  m_type  = ComPrivateDataType::None;
    UINT                m_size  = 0;
    void*               m_data  = nullptr;
    IUnknown*           m_iface = nullptr;

    void destroy();

  };


  class ComPrivateData {

  public:

    HRESULT setData(
            REFGUID   guid,
            UINT      size,
      const void*     data);

    HRESULT setInterface(
            REFGUID   guid,
      const IUnknown* iface);

    HRESULT getData(
            REFGUID   guid,
            UINT*     size,
            void*     data);

  private:

    std::vector<ComPrivateDataEntry> m_entries;

    ComPrivateDataEntry* findEntry(REFGUID guid);
    void insertEntry(ComPrivateDataEntry&& entry);

  };

}
