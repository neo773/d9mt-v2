#pragma once

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "../thread.h"

namespace dxvk {

  /* ONE diagnostic-logging switch for the whole driver. Set D9MT_TRACE=1 to
   * enable every gated Logger::info trace line (clear/load-action, D3D9
   * interface queries, etc). Cached once. Behaviour toggles that CHANGE
   * rendering for bisection (D9MT_NOCOLORCLEAR / D9MT_NOPSOCACHE) stay separate
   * — those are not logging. */
  inline bool d9mtTrace() {
    static const bool s_on = std::getenv("D9MT_TRACE") != nullptr;
    return s_on;
  }

  enum class LogLevel : uint32_t {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    None  = 5,
  };
  
#ifdef _WIN32
  using PFN_wineLogOutput = int (__cdecl *)(const char *);
#endif

  /**
   * \brief Logger
   * 
   * Logger for one DLL. Creates a text file and
   * writes all log messages to that file.
   */
  class Logger {
    
  public:
    
    Logger(const std::string& file_name);
    ~Logger();
    
    static void trace(const std::string& message);
    static void debug(const std::string& message);
    static void info (const std::string& message);
    static void warn (const std::string& message);
    static void err  (const std::string& message);
    static void log  (LogLevel level, const std::string& message);
    
    static LogLevel logLevel() {
      return s_instance.m_minLevel;
    }
    
  private:
    
    static Logger     s_instance;
    
    const LogLevel    m_minLevel;
    const std::string m_fileName;
    
    dxvk::mutex       m_mutex;
    std::ofstream     m_fileStream;

    bool              m_initialized = false;
#ifdef _WIN32
    PFN_wineLogOutput m_wineLogOutput = nullptr;
#endif

    void emitMsg(LogLevel level, const std::string& message);
    
    std::string getFileName(
      const std::string& base);

    static LogLevel getMinLogLevel();

  };
  
}
