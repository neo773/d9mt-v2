#include "d3d9_monitor.h"

#include "d3d9_format.h"

namespace dxvk {

  uint32_t GetMonitorFormatBpp(D3D9Format Format) {
    switch (Format) {
    case D3D9Format::A8R8G8B8:
    case D3D9Format::X8R8G8B8:
    case D3D9Format::A2R10G10B10:
      return 32;

    case D3D9Format::A1R5G5B5:
    case D3D9Format::X1R5G5B5:
    case D3D9Format::R5G6B5:
      return 16;

    default:
      Logger::warn(str::format(
        "GetMonitorFormatBpp: Unknown format: ",
        Format));
      return 32;
    }
  }


  bool IsSupportedAdapterFormat(
          D3D9Format Format) {
    return Format == D3D9Format::A2R10G10B10
        || Format == D3D9Format::X8R8G8B8
        || Format == D3D9Format::X1R5G5B5
        || Format == D3D9Format::R5G6B5;
  }


  bool IsSupportedModeFormat(
          D3D9Format Format) {
    // Native drivers never list X1R5G5B5 modes; some apps rely on it being absent.
    return Format == D3D9Format::A2R10G10B10
        || Format == D3D9Format::X8R8G8B8
        || Format == D3D9Format::R5G6B5;
  }


  bool IsSupportedBackBufferFormat(
          D3D9Format AdapterFormat,
          D3D9Format BackBufferFormat,
          BOOL       Windowed) {
    if (!Windowed) {
      // Native drivers never advertise X1R5G5B5 as a fullscreen adapter format.
      return (AdapterFormat == D3D9Format::A2R10G10B10 && BackBufferFormat == D3D9Format::A2R10G10B10)
          || (AdapterFormat == D3D9Format::X8R8G8B8    && BackBufferFormat == D3D9Format::X8R8G8B8)
          || (AdapterFormat == D3D9Format::X8R8G8B8    && BackBufferFormat == D3D9Format::A8R8G8B8)
          || (AdapterFormat == D3D9Format::R5G6B5      && BackBufferFormat == D3D9Format::R5G6B5);
    }

    // Native drivers never advertise A2R10G10B10 as a windowed backbuffer format.
    return BackBufferFormat == D3D9Format::A8R8G8B8
        || BackBufferFormat == D3D9Format::X8R8G8B8
        || BackBufferFormat == D3D9Format::A1R5G5B5
        || BackBufferFormat == D3D9Format::X1R5G5B5
        || BackBufferFormat == D3D9Format::R5G6B5
        || BackBufferFormat == D3D9Format::Unknown;
  }

  bool IsSupportedBackBufferFormat(
        D3D9Format BackBufferFormat) {
    return BackBufferFormat == D3D9Format::A2R10G10B10
        || BackBufferFormat == D3D9Format::A8R8G8B8
        || BackBufferFormat == D3D9Format::X8R8G8B8
        || BackBufferFormat == D3D9Format::A1R5G5B5
        || BackBufferFormat == D3D9Format::X1R5G5B5
        || BackBufferFormat == D3D9Format::R5G6B5
        || BackBufferFormat == D3D9Format::Unknown;
  }

}
