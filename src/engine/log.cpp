#include "engine/log.hpp"

#include <iostream>
#include <string>

namespace noemancer {
namespace {

std::string escape_json(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += ch; break;
        }
    }
    return result;
}

} // namespace

Logger::Logger(const LogFormat format) noexcept : format_(format) {}

void Logger::info(const std::string_view event, const std::string_view message) const {
    write("info", event, message);
}

void Logger::error(const std::string_view event, const std::string_view message) const {
    write("error", event, message);
}

void Logger::write(
    const std::string_view level,
    const std::string_view event,
    const std::string_view message) const {
    auto& stream = level == "error" ? std::cerr : std::cout;
    if (format_ == LogFormat::Json) {
        stream << "{\"level\":\"" << level
               << "\",\"event\":\"" << escape_json(event)
               << "\",\"message\":\"" << escape_json(message) << "\"}\n";
        return;
    }
    stream << '[' << level << "] " << event << ": " << message << '\n';
}

} // namespace noemancer

