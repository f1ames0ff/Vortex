#pragma once

#include <filesystem>
#include <string>

namespace vortex::core {

/**
 * Validate a path that must stay inside a root. Rejects absolute paths, root
 * names, `..`, and (on Windows) components that Win32 would normalise to a
 * parent, a device name, or an alternate data stream.
 */
bool isSafeRelative(const std::filesystem::path& path, std::string& error);

/**
 * Resolve `relative` under `root` and reject symlinks in every component that
 * already exists. Missing components are allowed; callers create them. The
 * root itself may be a symlink.
 */
bool resolveNoSymlinks(const std::filesystem::path& root, const std::string& relative,
                       std::filesystem::path& result, std::string& error);

/**
 * Like resolveNoSymlinks, but also requires the leaf, when it exists, to be a
 * regular file. A missing leaf is allowed so the caller can report an optional
 * or required missing source.
 */
bool resolveSourceFile(const std::filesystem::path& root, const std::string& relative,
                       std::filesystem::path& result, std::string& error);

} // namespace vortex::core
