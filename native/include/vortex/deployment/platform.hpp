#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace vortex::deployment::platform {

std::int64_t file_mtime_ms(const std::filesystem::path& path);
std::int64_t now_ms();
std::string operation_id();

bool is_case_insensitive(const std::filesystem::path& root);
std::string normalized_key(const std::filesystem::path& path, bool case_insensitive);

bool is_same_file(const std::filesystem::path& lhs, const std::filesystem::path& rhs);
std::uintmax_t link_count(const std::filesystem::path& path);
bool is_hardlink_to(const std::filesystem::path& source, const std::filesystem::path& target);
bool is_symlink_to(const std::filesystem::path& source, const std::filesystem::path& target);

/** Map an activator id (`move_activator`) to its on-disk method (`move`). */
std::string normalize_deployment_method(const std::string& method);

std::filesystem::path backup_path(const std::filesystem::path& target);
void atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination);
void sync_file(const std::filesystem::path& path);
void remove_empty_parents(const std::filesystem::path& start, const std::filesystem::path& stop);

} // namespace vortex::deployment::platform
