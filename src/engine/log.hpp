#pragma once

#include <string_view>

namespace noemancer {

enum class LogFormat {
    Human,
    Json
};

class Logger final {
public:
    explicit Logger(LogFormat format = LogFormat::Human) noexcept;

    void info(std::string_view event, std::string_view message) const;
    void error(std::string_view event, std::string_view message) const;

private:
    void write(std::string_view level, std::string_view event, std::string_view message) const;
    LogFormat format_;
};

} // namespace noemancer

