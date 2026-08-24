#include "engine/process_diagnostics.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#endif

namespace {

bool contains_expected_record(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream input(entry.path(), std::ios::binary);
        const std::string content{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        if (content.find("noemancer.process-diagnostics.v1") != std::string::npos &&
            content.find("process.terminate") != std::string::npos &&
            content.find("intentional fatal diagnostics probe") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool contains_expected_minidump(const std::filesystem::path& directory) {
#if defined(_WIN32)
    for(const auto& entry:std::filesystem::directory_iterator(directory)) {
        std::error_code error;
        if(entry.path().extension()==".dmp"&&std::filesystem::file_size(entry.path(),error)>0U&&!error)return true;
    }
    return false;
#else
    static_cast<void>(directory);
    return true;
#endif
}

bool expected_child_status(const int status) {
#if defined(_WIN32)
    return status == 134;
#else
    return status == 134 || status == (134 << 8);
#endif
}

int run_child(const char* executable, const std::vector<std::string>& arguments) {
#if defined(_WIN32)
    std::vector<const char*> child_argv;
    child_argv.reserve(arguments.size() + 2U);
    child_argv.push_back(executable);
    for (const auto& argument : arguments) {
        child_argv.push_back(argument.c_str());
    }
    child_argv.push_back(nullptr);
    return static_cast<int>(_spawnv(_P_WAIT, executable, child_argv.data()));
#else
    std::string command{"\""};
    command += executable;
    command += '"';
    for (const auto& argument : arguments) {
        command += " \"";
        command += argument;
        command += '"';
    }
    return std::system(command.c_str());
#endif
}

} // namespace

int main(const int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--abort-child") {
        noemancer::configure_process_diagnostics("test.diagnostics-abort");
        std::abort();
    }
    if (argc == 2 && std::string_view{argv[1]} == "--verify-abort") {
        const auto child_status = run_child(argv[0], {"--abort-child"});
        if (!expected_child_status(child_status)) {
            std::cerr << "abort verification failed: status=" << child_status << '\n';
            return 1;
        }
        return 0;
    }
    if (argc == 3 && std::string_view{argv[1]} == "--sidecar-child") {
        noemancer::configure_process_diagnostics(noemancer::process_diagnostics_options{
            "test.diagnostics-sidecar", argv[2]});
        try {
            throw std::runtime_error("intentional fatal diagnostics probe");
        } catch (...) {
            std::terminate();
        }
    }

    const auto verification_directory = std::filesystem::temp_directory_path() /
        ("noemancer-process-diagnostics-" + std::to_string(std::rand()));
    std::filesystem::create_directories(verification_directory);
    const auto child_status = run_child(
        argv[0], {"--sidecar-child", verification_directory.string()});
    if (!expected_child_status(child_status) || !contains_expected_record(verification_directory) ||
        !contains_expected_minidump(verification_directory)) {
        std::cerr << "fatal diagnostics sidecar probe failed: status=" << child_status << '\n';
        std::filesystem::remove_all(verification_directory);
        return 1;
    }
    std::filesystem::remove_all(verification_directory);
    return 0;
}
