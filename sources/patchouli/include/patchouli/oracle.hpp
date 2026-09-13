#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <cerrno>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#else
#error "patchouli external oracle supports only Windows, macOS, and Linux"
#endif

namespace patchouli {

class Oracle {
public:
    virtual ~Oracle() = default;
    virtual std::vector<bool> accepts_batch(const std::vector<std::string>& values) = 0;
};

class ExternalOracle final : public Oracle {
public:
    explicit ExternalOracle(std::filesystem::path executable)
        : executable_(std::move(executable)) {}

    std::vector<bool> accepts_batch(const std::vector<std::string>& values) override {
        if (values.empty()) return {};
        const auto request=nlohmann::json(values).dump();
#if defined(_WIN32)
        const auto output=accepts_windows(request);
#else
        const auto output=accepts_posix(request);
#endif
        const auto result=nlohmann::json::parse(output);
        if (!result.is_array() || result.size()!=values.size())
            throw std::runtime_error("oracle result count mismatch: expected boolean array");
        std::vector<bool> accepted;
        for (const auto& item:result) {
            if (!item.is_boolean()) throw std::runtime_error("oracle result is not boolean");
            accepted.push_back(item.get<bool>());
        }
        return accepted;
    }

private:
#if defined(_WIN32)
    class Handle {
    public:
        Handle() = default;
        explicit Handle(HANDLE value) : value_(value) {}
        ~Handle() { reset(); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        Handle(Handle&& other) noexcept : value_(other.release()) {}
        Handle& operator=(Handle&& other) noexcept {
            if (this != &other) {
                reset(other.release());
            }
            return *this;
        }
        HANDLE get() const noexcept { return value_; }
        HANDLE release() noexcept {
            HANDLE result = value_;
            value_ = nullptr;
            return result;
        }
        void reset(HANDLE replacement = nullptr) noexcept {
            if (value_ && value_ != INVALID_HANDLE_VALUE) {
                CloseHandle(value_);
            }
            value_ = replacement;
        }

    private:
        HANDLE value_{nullptr};
    };

    static std::string windows_error(const char* operation) {
        return std::string(operation) + " failed with Windows error " +
               std::to_string(GetLastError());
    }

    std::string accepts_windows(std::string_view value) const {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;

        HANDLE raw_read = nullptr;
        HANDLE raw_write = nullptr;
        if (!CreatePipe(&raw_read, &raw_write, &security, 0)) {
            throw std::runtime_error(windows_error("CreatePipe"));
        }
        Handle input_read(raw_read);
        Handle input_write(raw_write);
        if (!SetHandleInformation(input_write.get(), HANDLE_FLAG_INHERIT, 0)) {
            throw std::runtime_error(windows_error("SetHandleInformation"));
        }

        if (!CreatePipe(&raw_read, &raw_write, &security, 0))
            throw std::runtime_error(windows_error("CreatePipe(stdout)"));
        Handle output_read(raw_read);
        Handle output_write(raw_write);
        if (!SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0))
            throw std::runtime_error(windows_error("SetHandleInformation(stdout)"));

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input_read.get();
        startup.hStdOutput = output_write.get();
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};

        const std::wstring executable = executable_.wstring();
        std::wstring command_line = L"\"" + executable + L"\"";
        if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            throw std::runtime_error(windows_error("CreateProcessW"));
        }
        Handle process_handle(process.hProcess);
        Handle thread_handle(process.hThread);
        input_read.reset();
        output_write.reset();
        // On any I/O exception terminate and reap the child before closing handles.
        struct ChildGuard {
            HANDLE process;
            bool done=false;
            ~ChildGuard() { if (!done) { TerminateProcess(process, 2); WaitForSingleObject(process, INFINITE); } }
        } child{process_handle.get()};

        std::size_t written_total = 0;
        while (written_total < value.size()) {
            const std::size_t remaining = value.size() - written_total;
            const DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!WriteFile(input_write.get(), value.data() + written_total, chunk, &written,
                           nullptr)) {
                const DWORD error = GetLastError();
                input_write.reset();

                SetLastError(error);
                throw std::runtime_error(windows_error("WriteFile(oracle stdin)"));
            }
            if (written == 0) {
                throw std::runtime_error("zero-byte write to oracle stdin");
            }
            written_total += written;
        }
        input_write.reset();

        std::string output;
        char buffer[8192];
        for (;;) {
            DWORD count=0;
            if (!ReadFile(output_read.get(), buffer, sizeof(buffer), &count, nullptr)) {
                if (GetLastError()==ERROR_BROKEN_PIPE) break;
                throw std::runtime_error(windows_error("ReadFile(oracle stdout)"));
            }
            if (!count) break;
            output.append(buffer,count);
        }
        output_read.reset();
        if (WaitForSingleObject(process_handle.get(), INFINITE) != WAIT_OBJECT_0) {
            throw std::runtime_error(windows_error("WaitForSingleObject"));
        }
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(process_handle.get(), &exit_code)) {
            throw std::runtime_error(windows_error("GetExitCodeProcess"));
        }
        child.done=true;
        if (exit_code!=0)
            throw std::runtime_error("oracle exited with unexpected code " + std::to_string(exit_code));
        return output;
    }
#else
    std::string accepts_posix(std::string_view value) const {
        static const bool sigpipe_ignored = signal(SIGPIPE, SIG_IGN) != SIG_ERR;
        if (!sigpipe_ignored) throw std::runtime_error("failed to ignore SIGPIPE");
        struct Pipe {
            int fd[2]{-1,-1};
            Pipe() { if (pipe(fd)!=0) throw std::runtime_error("oracle pipe failed"); }
            void reset(int i) { if (fd[i]>=0) { close(fd[i]); fd[i]=-1; } }
            ~Pipe() { reset(0); reset(1); }
        } input, output;
        struct Actions {
            posix_spawn_file_actions_t value;
            Actions() { if (posix_spawn_file_actions_init(&value)!=0) throw std::runtime_error("spawn actions init failed"); }
            ~Actions() { posix_spawn_file_actions_destroy(&value); }
        } actions;
        const auto check=[](int code) { if (code) throw std::runtime_error("spawn action failed"); };
        check(posix_spawn_file_actions_adddup2(&actions.value,input.fd[0],STDIN_FILENO));
        check(posix_spawn_file_actions_adddup2(&actions.value,output.fd[1],STDOUT_FILENO));
        for (int fd:{input.fd[0],input.fd[1],output.fd[0],output.fd[1]})
            if (fd!=STDIN_FILENO && fd!=STDOUT_FILENO)
                check(posix_spawn_file_actions_addclose(&actions.value,fd));
        const auto executable=executable_.string();
        char* argv[]={const_cast<char*>(executable.c_str()),nullptr};
        pid_t pid{};
        if (posix_spawn(&pid,executable.c_str(),&actions.value,nullptr,argv,environ)!=0)
            throw std::runtime_error("oracle posix_spawn failed");
        struct ChildGuard {
            pid_t pid;
            bool done=false;
            ~ChildGuard() {
                if (!done) { kill(pid,SIGKILL); while (waitpid(pid,nullptr,0)<0 && errno==EINTR) {} }
            }
        } child{pid};
        input.reset(0);
        output.reset(1);
        std::size_t offset=0;
        while (offset<value.size()) {
            const auto count=write(input.fd[1],value.data()+offset,
                std::min<std::size_t>(value.size()-offset,65536));
            if (count<0 && errno==EINTR) continue;
            if (count<=0) throw std::runtime_error("oracle stdin write failed");
            offset+=static_cast<std::size_t>(count);
        }
        input.reset(1);
        std::string response;
        char buffer[8192];
        for (;;) {
            const auto count=read(output.fd[0],buffer,sizeof(buffer));
            if (count<0 && errno==EINTR) continue;
            if (count<0) throw std::runtime_error("oracle stdout read failed");
            if (!count) break;
            response.append(buffer,static_cast<std::size_t>(count));
        }
        output.reset(0);
        int status{};
        pid_t waited;
        do { waited=waitpid(pid,&status,0); } while (waited<0 && errno==EINTR);
        if (waited<0) throw std::runtime_error("oracle waitpid failed");
        child.done=true;
        if (!WIFEXITED(status) || WEXITSTATUS(status)!=0)
            throw std::runtime_error("oracle exited unsuccessfully");
        return response;
    }
#endif

    std::filesystem::path executable_;
};

}  // namespace patchouli
