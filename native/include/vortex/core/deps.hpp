#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vortex::core {

struct RepoReference {
    std::string repository;
    std::optional<std::string> modId;
    std::optional<std::string> fileId;
};

struct ModReference {
    std::optional<std::string> id;
    std::optional<std::string> idHint;
    std::optional<std::string> md5Hint;
    std::optional<std::string> archiveId;
    std::optional<std::string> fileMD5;
    std::optional<std::string> fileExpression;
    std::optional<std::string> logicalFileName;
    std::optional<std::string> tag;
    std::optional<std::string> gameId;
    std::optional<std::string> versionMatch;
    std::optional<RepoReference> repo;
};

struct LookupMod {
    std::string id;
    std::optional<std::string> fileMD5;
    std::optional<std::string> fileName;
    std::optional<std::string> name;
    std::optional<std::string> logicalFileName;
    std::optional<std::string> customFileName;
    std::optional<std::string> version;
    std::optional<std::string> fileId;
    std::optional<std::string> modId;
    std::optional<std::string> source;
    std::optional<std::string> referenceTag;
    std::optional<std::string> state;
    std::vector<std::string> additionalLogicalFileNames;
    std::vector<std::string> referenceTags;
    std::vector<std::string> game;
    nlohmann::json installerChoices;
    nlohmann::json fileList;
    nlohmann::json patches;
};

struct DownloadInfo {
    std::string id;
    std::optional<std::string> fileMD5;
    std::optional<std::string> localPath;
    std::optional<std::string> logicalFileName;
    std::optional<std::string> version;
    std::optional<std::string> fileId;
    std::optional<std::string> modId;
    std::optional<std::string> source;
    std::optional<std::string> referenceTag;
    std::vector<std::string> referenceTags;
    std::vector<std::string> game;
    std::string state;
    std::int64_t fileTime = 0;
};

struct RuleInfo {
    int index = 0;
    ModReference reference;
    std::string type;
    int phase = 0;
    bool ignored = false;
    std::optional<std::string> localPath;
    nlohmann::json installerChoices;
    nlohmann::json fileList;
    nlohmann::json patches;
};

struct DependencyEntry {
    int ruleIndex = 0;
    std::string ruleId;
    std::string type;
    int phase = 0;
    bool ignored = false;
    std::optional<std::string> modId;
    std::optional<std::string> downloadId;
    std::string status;
};

struct CollectionPlan {
    std::vector<DependencyEntry> members;
    std::vector<int> phases;
    int totalRequired = 0;
    int totalOptional = 0;
};

std::string referenceId(const ModReference& reference);
std::string ruleId(const RuleInfo& rule);

bool matchMod(const LookupMod& mod, const ModReference& reference,
              const RuleInfo* installSpec = nullptr);
std::optional<std::string> matchModId(const std::vector<LookupMod>& mods,
                                      const ModReference& reference,
                                      const RuleInfo* installSpec = nullptr);
std::optional<std::string> matchDownloadId(const std::vector<DownloadInfo>& downloads,
                                          const ModReference& reference);

std::vector<DependencyEntry> resolveDependencies(const std::vector<RuleInfo>& rules,
                                                 const std::vector<LookupMod>& mods,
                                                 const std::vector<DownloadInfo>& downloads);
CollectionPlan planCollection(const std::vector<RuleInfo>& rules,
                              const std::vector<LookupMod>& mods,
                              const std::vector<DownloadInfo>& downloads);

} // namespace vortex::core
