#include "engine/managed_debug_protocol.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(const int argc,char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin),_O_BINARY);
    _setmode(_fileno(stdout),_O_BINARY);
#endif
    if(argc==2&&std::string_view(argv[1])=="--hang") {
        std::this_thread::sleep_for(std::chrono::seconds(10));return 0;
    }
    std::cerr<<"fake-adapter-ready\n";
    const std::string input{std::istreambuf_iterator<char>(std::cin),std::istreambuf_iterator<char>()};
    noemancer::DapStreamDecoder decoder;
    const auto decoded=decoder.feed(input);
    if(decoded.error)return 2;
    std::uint64_t sequence=100;
    for(const auto& raw:decoded.messages) {
        const auto request=nlohmann::json::parse(raw);
        const auto response=nlohmann::json{{"seq",sequence++},{"type","response"},{"request_seq",request.at("seq")},
            {"success",true},{"command",request.at("command")},{"body",{{"fake",true}}}};
        std::cout<<noemancer::dap_message_frame(response.dump());
    }
    return 0;
}
