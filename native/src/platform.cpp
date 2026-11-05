#include "vortex/deployment/platform.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace vortex::deployment::platform {

std::int64_t file_mtime_ms(const fs::path& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "GetFileAttributesExW");
    }
    ULARGE_INTEGER value{};
    value.LowPart = data.ftLastWriteTime.dwLowDateTime;
    value.HighPart = data.ftLastWriteTime.dwHighDateTime;
    constexpr std::uint64_t epoch_delta = 116444736000000000ULL;
    return static_cast<std::int64_t>((value.QuadPart - epoch_delta) / 10000ULL);
#else
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        throw std::system_error(errno, std::generic_category(), "stat");
    }
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000 +
           static_cast<std::int64_t>(st.st_mtim.tv_nsec) / 1000000;
#endif
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string operation_id() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream out;
    out << now_ms() << '-' << std::hex << dist(gen);
    return out.str();
}

bool is_case_insensitive(const fs::path& /*root*/) {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

std::string normalized_key(const fs::path& path, bool case_insensitive) {
    auto key = path.lexically_normal().generic_string();
    while (key.size() > 1 && key.back() == '/') {
        key.pop_back();
    }
    if (case_insensitive) {
        for (char& ch : key) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
    }
    return key;
}

bool is_same_file(const fs::path& lhs, const fs::path& rhs) {
    std::error_code ec;
    return fs::equivalent(lhs, rhs, ec) && !ec;
}

std::uintmax_t link_count(const fs::path& path) {
    std::error_code ec;
    const auto count = fs::hard_link_count(path, ec);
    return ec ? 0 : count;
}

bool is_hardlink_to(const fs::path& source, const fs::path& target) {
    if (link_count(target) < 2) {
        return false;
    }
    return is_same_file(source, target);
}

fs::path backup_path(const fs::path& target) {
    return fs::path(target.native() + fs::path(".vortex_backup").native());
}

void atomic_replace(const fs::path& source, const fs::path& destination) {
#ifdef _WIN32
    if (MoveFileExW(source.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "MoveFileExW");
    }
#else
    if (::rename(source.c_str(), destination.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "rename");
    }
#endif
}

void sync_file(const fs::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CreateFileW");
    }
    const BOOL ok = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "FlushFileBuffers");
    }
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open");
    }
    const int rc = ::fsync(fd);
    const int saved = errno;
    ::close(fd);
    if (rc != 0) {
        throw std::system_error(saved, std::generic_category(), "fsync");
    }
#endif
}

void remove_empty_parents(const fs::path& start, const fs::path& stop) {
    std::error_code ec;
    auto current = start;
    while (current != stop && current != current.parent_path()) {
        if (!fs::remove(current, ec)) {
            break;
        }
        ec.clear();
        current = current.parent_path();
    }
}

} // namespace vortex::deployment::platform
