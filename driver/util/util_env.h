#pragma once

#include "util_string.h"

namespace dxvk::env {

#ifdef _WIN32
  constexpr char PlatformDirSlash = '\\';
#else
  constexpr char PlatformDirSlash = '/';
#endif

  constexpr bool is32BitHostPlatform() {
    return sizeof(void*) == 4;
  }

  bool isWineVulkan();

  std::string getEnvVar(const char* name);

  size_t matchFileExtension(const std::string& name, const char* ext);

  std::string getExeName();

  std::string getExeBaseName();

  std::string getExePath();

  void setThreadName(const std::string& name);

  bool createDirectory(const std::string& path);

}
