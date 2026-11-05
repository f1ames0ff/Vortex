#include "vortex/core/process.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace vortex::core {

namespace {

#ifdef _WIN32
std::string quoteWindowsArgument(const std::string& argument) {
    std::string result = "\"";
    std::size_t backslashes = 0;
    for (const char character : argument) {
        if (character == '\\') {
            ++backslashes;
            continue;
        }
        if (character == '"') {
            result.append(backslashes * 2 + 1, '\\');
            result.push_back('"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, '\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, '\\');
    result.push_back('"');
    return result;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const auto length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "MultiByteToWideChar");
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                        length);
    return result;
}
#endif

} // namespace

ProcessResult runProcess(const std::filesystem::path& executable,
                         const std::vector<std::string>& arguments) {
    ProcessResult result;

#ifdef _WIN32
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (CreatePipe(&readPipe, &writePipe, &attributes, 0) == 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CreatePipe");
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring commandLine = widen(quoteWindowsArgument(executable.string()));
    for (const auto& argument : arguments) {
        commandLine += L" " + widen(quoteWindowsArgument(argument));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;

    PROCESS_INFORMATION process{};
    const auto application = executable.wstring();
    if (CreateProcessW(application.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) == 0) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CreateProcessW");
    }

    CloseHandle(writePipe);

    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) != 0 && read > 0) {
        result.output.append(buffer, read);
    }
    CloseHandle(readPipe);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    int pipeDescriptors[2];
    if (::pipe(pipeDescriptors) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe");
    }

    const auto pid = ::fork();
    if (pid < 0) {
        ::close(pipeDescriptors[0]);
        ::close(pipeDescriptors[1]);
        throw std::system_error(errno, std::generic_category(), "fork");
    }

    if (pid == 0) {
        ::close(pipeDescriptors[0]);
        ::dup2(pipeDescriptors[1], STDOUT_FILENO);
        ::dup2(pipeDescriptors[1], STDERR_FILENO);
        ::close(pipeDescriptors[1]);

        std::vector<std::string> storage;
        storage.reserve(arguments.size() + 1);
        storage.push_back(executable.string());
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& value : storage) {
            argv.push_back(value.data());
        }
        argv.push_back(nullptr);

        ::execvp(executable.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(pipeDescriptors[1]);
    char buffer[4096];
    for (;;) {
        const auto count = ::read(pipeDescriptors[0], buffer, sizeof(buffer));
        if (count > 0) {
            result.output.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(pipeDescriptors[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "waitpid");
        }
    }
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    }
#endif

    return result;
}

} // namespace vortex::core
