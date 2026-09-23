#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <initializer_list>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace oracle_process {

// Replacing a complete snapshot keeps the previous one readable if the harness
// kills us during a write. This survives process termination (not a power loss).
inline void persist_metrics(std::uint64_t calls, std::uint64_t elapsed,
                            std::uint64_t submitted, std::uint64_t active_started) {
    const char* configured = std::getenv("ORACLE_METRICS_PATH");
    const std::string name = configured ? configured : "oracle-metrics.json";
    const auto path = std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(name.data()), name.size()));
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream << "{\"oracle_total_calls\":" << calls
           << ",\"oracle_execution_time_ns\":" << elapsed
           << ",\"oracle_candidates_submitted\":" << submitted
           << ",\"active_started_unix_ns\":" << active_started << "}\n";
    stream.close();
    if (!stream) throw std::runtime_error("cannot persist oracle metrics");
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("cannot replace oracle metrics snapshot");
#else
    std::filesystem::rename(temporary, path);
#endif
}

// Every adapter starts this before preparing its request and destroys it after
// decoding the verdict and cleaning up. Empty batches do not create a scope.
class Invocation {
public:
    Invocation(std::uint64_t& calls, std::uint64_t& elapsed,
               std::uint64_t& submitted, std::size_t count)
        : calls_(calls), elapsed_(elapsed), submitted_(submitted),
          started_(std::chrono::steady_clock::now()) {
        ++calls;
        submitted += count;
        const auto active_started = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        persist_metrics(calls_, elapsed_, submitted_, active_started);
    }
    ~Invocation() {
        elapsed_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started_).count());
        try {
            persist_metrics(calls_, elapsed_, submitted_, 0);
        } catch (const std::exception& error) {
            std::cerr << "oracle metrics: " << error.what() << '\n';
        }
    }
    Invocation(const Invocation&) = delete;
    Invocation& operator=(const Invocation&) = delete;
private:
    std::uint64_t& calls_;
    std::uint64_t& elapsed_;
    std::uint64_t& submitted_;
    std::chrono::steady_clock::time_point started_;
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
#if defined(_WIN32)
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                ("oracle-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(path_)) return;
        }
        throw std::runtime_error("cannot create oracle temporary directory");
#else
        auto pattern = (std::filesystem::temp_directory_path() / "oracle-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (!mkdtemp(buffer.data())) throw std::runtime_error("cannot create oracle temporary directory");
        path_ = buffer.data();
#endif
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        for (const char* name : {"candidate", "stdin", "stdout", "stderr"})
            std::filesystem::remove(path_ / name, ignored);
        std::filesystem::remove(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    std::filesystem::path file(const char* name) const { return path_ / name; }
private:
    std::filesystem::path path_;
};

inline void write_file(const std::filesystem::path& path, std::string_view value) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    stream.close();
    if (!stream) throw std::runtime_error("cannot write oracle input file");
}

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read oracle output file");
    std::string value{std::istreambuf_iterator<char>(stream), {}};
    if (stream.bad()) throw std::runtime_error("cannot read oracle output file");
    return value;
}

struct Result {
    int exit_code;
    std::string output;
    std::string error;
};

#if defined(_WIN32)
class Handle {
public:
    explicit Handle(HANDLE handle) : handle_(handle) {
        if (!handle || handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("cannot open oracle process handle");
    }
    ~Handle() { CloseHandle(handle_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return handle_; }
private:
    HANDLE handle_;
};

inline std::wstring widen(const std::string& value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size())).wstring();
}

inline std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result.push_back(character);
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L"\"";
}
#endif

// Direct launch, no shell. File-backed standard streams avoid pipe-capacity
// deadlocks and let timeout handling cover arbitrarily large requests/replies.
// The executable still reads stdin/writes stdout using its unchanged protocol.
inline Result run(const std::vector<std::string>& command,
                  std::string_view input = {}, int timeout_ms = -1) {
    if (command.empty() || command.front().empty())
        throw std::runtime_error("empty oracle command");
    TemporaryDirectory files;
    write_file(files.file("stdin"), input);
#if defined(_WIN32)
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle input_handle(CreateFileW(files.file("stdin").c_str(), GENERIC_READ,
        FILE_SHARE_READ, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle output_handle(CreateFileW(files.file("stdout").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle error_handle(CreateFileW(files.file("stderr").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input_handle.get();
    startup.hStdOutput = output_handle.get();
    startup.hStdError = error_handle.get();
    std::wstring arguments;
    for (const auto& argument : command) {
        if (!arguments.empty()) arguments += L' ';
        arguments += quote(widen(argument));
    }
    PROCESS_INFORMATION process{};
    const auto executable = widen(command.front());
    if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr,
                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        throw std::runtime_error("cannot launch oracle: " + command.front());
    Handle process_handle(process.hProcess);
    Handle thread_handle(process.hThread);
    const auto waited = WaitForSingleObject(process_handle.get(),
        timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process_handle.get(), 2);
        WaitForSingleObject(process_handle.get(), INFINITE);
        throw std::runtime_error(waited == WAIT_TIMEOUT ? "oracle timed out" : "oracle wait failed");
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process_handle.get(), &code))
        throw std::runtime_error("cannot read oracle exit code");
    const int exit_code = static_cast<int>(code);
#else
    const auto started = std::chrono::steady_clock::now();
    struct Actions {
        posix_spawn_file_actions_t value;
        Actions() {
            if (posix_spawn_file_actions_init(&value))
                throw std::runtime_error("cannot initialize oracle spawn actions");
        }
        ~Actions() { posix_spawn_file_actions_destroy(&value); }
    } actions;
    const auto redirect = [&](int descriptor, const char* name, int flags) {
        if (posix_spawn_file_actions_addopen(&actions.value, descriptor,
                files.file(name).c_str(), flags, 0600))
            throw std::runtime_error("cannot configure oracle standard streams");
    };
    redirect(STDIN_FILENO, "stdin", O_RDONLY);
    redirect(STDOUT_FILENO, "stdout", O_WRONLY | O_CREAT | O_TRUNC);
    redirect(STDERR_FILENO, "stderr", O_WRONLY | O_CREAT | O_TRUNC);
    std::vector<char*> arguments;
    for (const auto& argument : command) arguments.push_back(const_cast<char*>(argument.c_str()));
    arguments.push_back(nullptr);
    pid_t pid = 0;
    const int spawn_error = posix_spawnp(&pid, command.front().c_str(), &actions.value,
                                       nullptr, arguments.data(), environ);
    if (spawn_error) throw std::runtime_error("cannot launch oracle: " + command.front());
    struct Child {
        pid_t pid;
        bool reaped = false;
        ~Child() {
            if (!reaped) {
                kill(pid, SIGKILL);
                while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            }
        }
    } child{pid};
    int status = 0;
    for (;;) {
        const auto waited = waitpid(pid, &status, timeout_ms < 0 ? 0 : WNOHANG);
        if (waited == pid) { child.reaped = true; break; }
        if (waited < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("oracle wait failed");
        }
        if (std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(timeout_ms))
            throw std::runtime_error("oracle timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!WIFEXITED(status)) throw std::runtime_error("oracle terminated abnormally");
    const int exit_code = WEXITSTATUS(status);
#endif
    return {exit_code, read_file(files.file("stdout")), read_file(files.file("stderr"))};
}

inline void require_exit_code(const Result& result, std::initializer_list<int> allowed) {
    for (int code : allowed) if (result.exit_code == code) return;
    throw std::runtime_error("oracle exited with unexpected code " +
                            std::to_string(result.exit_code) + ": " + result.error);
}

} // namespace oracle_process
