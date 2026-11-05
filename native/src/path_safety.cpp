#include "vortex/core/path_safety.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace vortex::core {

namespace {

bool isMissing(const std::error_code& ec) {
    return ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory;
}

#ifdef _WIN32
std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isReservedWindowsName(const std::string& name) {
    const auto dot = name.find('.');
    const auto base = lowerAscii(dot == std::string::npos ? name : name.substr(0, dot));
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" ||
        base == "conout$") {
        return true;
    }
    if (base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0)) {
        return base[3] >= '1' && base[3] <= '9';
    }
    return false;
}
#endif

} // namespace

bool isSafeRelative(const fs::path& path, std::string& error) {
    if (path.empty()) {
        error = "path must not be empty";
        return false;
    }
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        error = "path must be relative: " + path.string();
        return false;
    }

    for (const auto& part : path) {
        std::string name;
        try {
            name = part.string();
        } catch (const std::exception& err) {
            error = std::string("invalid path component: ") + err.what();
            return false;
        }
        if (name.find('\0') != std::string::npos) {
            error = "path contains a NUL byte";
            return false;
        }
#ifdef _WIN32
        while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
            name.pop_back();
        }
        if (name.empty() || name == "." || name == "..") {
            error = "path component is not a valid directory name";
            return false;
        }
        if (name.find(':') != std::string::npos) {
            error = "path component contains ':'";
            return false;
        }
        if (isReservedWindowsName(name)) {
            error = "path component is a reserved Windows name";
            return false;
        }
#else
        if (name == "..") {
            error = "path must not contain '..'";
            return false;
        }
#endif
    }
    return true;
}

bool resolveNoSymlinks(const fs::path& root, const std::string& relative, fs::path& result,
                       std::string& error) {
    const fs::path child(relative);
    if (!isSafeRelative(child, error)) {
        return false;
    }

    fs::path current = root;
    for (auto iterator = child.begin(); iterator != child.end(); ++iterator) {
        current /= *iterator;
        std::error_code ec;
        const auto status = fs::symlink_status(current, ec);
        if (ec) {
            if (isMissing(ec)) {
                // The rest of the destination does not exist yet. The caller
                // creates only the suffix below this point.
                result = root / child;
                return true;
            }
            error = "cannot inspect path: " + current.string() + ": " + ec.message();
            return false;
        }
        if (fs::is_symlink(status)) {
            error = "symlinks are not allowed in install paths: " + current.string();
            return false;
        }
        const auto isLast = std::next(iterator) == child.end();
        if (!isLast && !fs::is_directory(status)) {
            error = "path parent is not a directory: " + current.string();
            return false;
        }
    }

    result = root / child;
    return true;
}

bool resolveSourceFile(const fs::path& root, const std::string& relative, fs::path& result,
                       std::string& error) {
    if (!resolveNoSymlinks(root, relative, result, error)) {
        return false;
    }

    std::error_code ec;
    const auto status = fs::symlink_status(result, ec);
    if (ec) {
        if (isMissing(ec)) {
            return true;
        }
        error = "cannot inspect source: " + result.string() + ": " + ec.message();
        return false;
    }
    if (fs::is_symlink(status)) {
        error = "source symlinks are not allowed: " + result.string();
        return false;
    }
    if (!fs::is_regular_file(status)) {
        error = "source is not a regular file: " + result.string();
        return false;
    }
    return true;
}

} // namespace vortex::core
