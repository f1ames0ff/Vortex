#include "vortex/core/archive.hpp"

#include "vortex/core/path_safety.hpp"
#include "vortex/core/process.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace vortex::core {

namespace {

std::string trim(std::string value) {
    const auto notSpace = [](unsigned char character) { return std::isspace(character) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string normalizeArchivePath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

std::vector<std::string> errorsFromOutput(const std::string& output, int code) {
    std::vector<std::string> errors;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        const auto text = trim(line);
        if (text.find("ERROR") != std::string::npos ||
            text.find("Can not open") != std::string::npos ||
            text.find("Cannot open") != std::string::npos ||
            text.find("Wrong password") != std::string::npos ||
            text.find("Data Error") != std::string::npos) {
            errors.push_back(text);
            if (errors.size() >= 20) {
                break;
            }
        }
    }
    if (errors.empty() && code != 0) {
        errors.push_back("7-Zip exited with code " + std::to_string(code));
    }
    return errors;
}

std::vector<ArchiveEntry> parseEntries(const std::string& output) {
    std::vector<ArchiveEntry> entries;
    std::istringstream stream(output);
    std::string line;
    ArchiveEntry current;
    bool inEntry = false;

    const auto flush = [&] {
        if (!inEntry) {
            return;
        }
        current.path = normalizeArchivePath(trim(current.path));
        if (!current.path.empty()) {
            entries.push_back(current);
        }
        current = ArchiveEntry{};
        inEntry = false;
    };

    while (std::getline(stream, line)) {
        const auto text = trim(line);
        if (text.rfind("Path = ", 0) == 0) {
            flush();
            current.path = text.substr(7);
            inEntry = true;
            continue;
        }
        if (!inEntry) {
            continue;
        }
        if (text.rfind("Folder = ", 0) == 0) {
            current.directory = text.substr(9) == "+";
        } else if (text.rfind("Size = ", 0) == 0) {
            try {
                current.size = std::stoull(text.substr(7));
            } catch (const std::exception&) {
                current.size = 0;
            }
        } else if (text.rfind("Encrypted = ", 0) == 0) {
            current.encrypted = text.substr(12) == "+";
        } else if (text.rfind("Attributes = ", 0) == 0) {
            const auto attributes = trim(text.substr(13));
            current.symlink = !attributes.empty() && attributes.front() == 'l';
        }
    }
    flush();
    return entries;
}

std::vector<std::string> sevenZipArguments(const std::string& password) {
    if (password.empty()) {
        return {};
    }
    return {"-p" + password};
}

} // namespace

ArchiveListResult listArchive(const fs::path& archive, const fs::path& sevenZip,
                              const std::string& password) {
    ArchiveListResult result;
    if (archive.empty() || sevenZip.empty()) {
        throw std::runtime_error("archive and sevenZip paths are required");
    }

    std::vector<std::string> arguments = {"l", "-slt", "-ba"};
    const auto passwordArguments = sevenZipArguments(password);
    arguments.insert(arguments.end(), passwordArguments.begin(), passwordArguments.end());
    arguments.push_back("--");
    arguments.push_back(archive.string());

    const auto process = runProcess(sevenZip, arguments);
    result.code = process.exitCode;
    result.entries = parseEntries(process.output);
    result.errors = errorsFromOutput(process.output, process.exitCode);
    result.encrypted = std::any_of(result.entries.begin(), result.entries.end(),
                                   [](const ArchiveEntry& entry) { return entry.encrypted; });
    return result;
}

ArchiveExtractResult extractArchive(const fs::path& archive, const fs::path& destination,
                                    const fs::path& sevenZip, const std::string& password) {
    ArchiveExtractResult result;
    const auto listing = listArchive(archive, sevenZip, password);
    result.code = listing.code;
    result.entries = listing.entries;
    result.errors = listing.errors;

    if (listing.code < 0) {
        return result;
    }

    for (const auto& entry : listing.entries) {
        std::string error;
        if (!isSafeRelative(fs::path(entry.path), error)) {
            throw std::runtime_error("archive contains an unsafe path '" + entry.path +
                                     "': " + error);
        }
        if (entry.symlink) {
            throw std::runtime_error("archive contains a symbolic link: " + entry.path);
        }
    }

    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        throw std::runtime_error("create extraction directory: " + ec.message());
    }

    std::vector<std::string> arguments = {"x", "-y", "-snl-", "-bsp0", "-bso0",
                                          "-o" + destination.string()};
    const auto passwordArguments = sevenZipArguments(password);
    arguments.insert(arguments.end(), passwordArguments.begin(), passwordArguments.end());
    arguments.push_back("--");
    arguments.push_back(archive.string());

    const auto process = runProcess(sevenZip, arguments);
    result.code = process.exitCode;
    const auto processErrors = errorsFromOutput(process.output, process.exitCode);
    result.errors.insert(result.errors.end(), processErrors.begin(), processErrors.end());
    return result;
}

} // namespace vortex::core
