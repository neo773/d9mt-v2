#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace dxvk {

  enum class Tristate : int32_t {
    Auto  = -1,
    False =  0,
    True  =  1,
  };

  class Config {
    using OptionMap = std::unordered_map<std::string, std::string>;
  public:

    Config();
    Config(OptionMap&& options);
    ~Config();

    void merge(const Config& other);

    void setOption(
      const std::string& key,
      const std::string& value);

    template<typename T>
    T getOption(const char* option, T fallback = T()) const {
      const std::string& value = getOptionValue(option);

      T result = fallback;
      parseOptionValue(value, result);
      return result;
    }

    void logOptions() const;

    static Config getAppConfig(const std::string& appName);

    static Config getUserConfig();

    static std::string toLower(std::string str);

  private:

    OptionMap m_options;

    std::string getOptionValue(
      const char*         option) const;

    static bool parseOptionValue(
      const std::string&  value,
            std::string&  result);

    static bool parseOptionValue(
      const std::string&  value,
            bool&         result);

    static bool parseOptionValue(
      const std::string&  value,
            int32_t&      result);

    static bool parseOptionValue(
      const std::string&  value,
            float&        result);

    static bool parseOptionValue(
      const std::string&  value,
            Tristate&     result);

    template<typename I, typename V>
    static bool parseStringOption(
            std::string   str,
            I             begin,
            I             end,
            V&            value);

  };


  inline void applyTristate(bool& option, Tristate state) {
    option &= state != Tristate::False;
    option |= state == Tristate::True;
  }

}
