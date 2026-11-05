#include "vortex/core/deps.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

using nlohmann::json;

namespace vortex::core {

namespace {

bool hasValue(const std::optional<std::string>& value) {
    return value.has_value() && !value->empty();
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string trim(std::string value) {
    const auto notSpace = [](unsigned char character) { return std::isspace(character) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string sanitizeExpression(const std::string& fileName) {
    const auto slash = fileName.find_last_of("/\\");
    std::string base = slash == std::string::npos ? fileName : fileName.substr(slash + 1);
    const auto dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        base.erase(dot);
    }
    static const std::regex duplicateSuffix(R"(\.\d+$)");
    static const std::regex browserSuffix(R"( \(\d+\)$)");
    base = std::regex_replace(base, duplicateSuffix, "");
    base = std::regex_replace(base, browserSuffix, "");
    return base;
}

bool globMatch(const std::string& value, const std::string& pattern) {
    std::size_t valueIndex = 0;
    std::size_t patternIndex = 0;
    std::size_t star = std::string::npos;
    std::size_t match = 0;

    while (valueIndex < value.size()) {
        if (patternIndex < pattern.size() &&
            (pattern[patternIndex] == '?' || pattern[patternIndex] == value[valueIndex])) {
            ++valueIndex;
            ++patternIndex;
        } else if (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
            star = patternIndex++;
            match = valueIndex;
        } else if (star != std::string::npos) {
            patternIndex = star + 1;
            valueIndex = ++match;
        } else {
            return false;
        }
    }
    while (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
        ++patternIndex;
    }
    return patternIndex == pattern.size();
}

struct Version {
    std::vector<int> parts;
    bool valid = false;
};

Version parseVersion(const std::string& input) {
    Version version;
    std::size_t index = 0;
    while (index < input.size() && version.parts.size() < 3) {
        while (index < input.size() && !std::isdigit(static_cast<unsigned char>(input[index]))) {
            ++index;
        }
        if (index >= input.size()) {
            break;
        }
        int value = 0;
        while (index < input.size() && std::isdigit(static_cast<unsigned char>(input[index]))) {
            value = value * 10 + (input[index] - '0');
            ++index;
        }
        version.parts.push_back(value);
        if (index < input.size() && input[index] == '.') {
            ++index;
        } else {
            break;
        }
    }
    version.valid = !version.parts.empty();
    return version;
}

int compareVersions(const Version& lhs, const Version& rhs) {
    const auto count = std::max(lhs.parts.size(), rhs.parts.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto left = index < lhs.parts.size() ? lhs.parts[index] : 0;
        const auto right = index < rhs.parts.size() ? rhs.parts[index] : 0;
        if (left != right) {
            return left < right ? -1 : 1;
        }
    }
    return 0;
}

bool isFuzzyVersion(const std::string& input) {
    if (input.empty()) {
        return false;
    }
    if (input == "*" || input.ends_with("+prefer")) {
        return true;
    }
    if (input.find_first_of("^~<>=|xX*") != std::string::npos) {
        return true;
    }
    // A full three-part numeric version is exact; everything else is a range.
    static const std::regex fullVersion(R"(^\d+\.\d+\.\d+$)");
    return !std::regex_match(input, fullVersion);
}

std::string coerceVersion(const std::string& input) {
    static const std::regex fullVersion(R"(^(\d+)\.(\d+)\.(\d+)(.*)$)");
    std::smatch match;
    if (std::regex_match(input, match, fullVersion)) {
        auto suffix = trim(match[4].str());
        if (!suffix.empty()) {
            suffix.erase(0, suffix.find_first_not_of(".-+"));
            if (!suffix.empty()) {
                return match[1].str() + "." + match[2].str() + "." + match[3].str() + "-" +
                       suffix;
            }
        }
        return match[1].str() + "." + match[2].str() + "." + match[3].str();
    }
    const auto parsed = parseVersion(input);
    if (!parsed.valid) {
        return input;
    }
    std::ostringstream stream;
    stream << parsed.parts[0] << '.' << (parsed.parts.size() > 1 ? parsed.parts[1] : 0) << '.'
           << (parsed.parts.size() > 2 ? parsed.parts[2] : 0);
    return stream.str();
}

bool versionSatisfies(const std::string& versionText, const std::string& rangeText) {
    const auto version = parseVersion(versionText);
    if (!version.valid) {
        return false;
    }

    std::stringstream alternatives(rangeText);
    std::string range;
    while (std::getline(alternatives, range, '|')) {
        range = trim(range);
        if (range.empty()) {
            continue;
        }
        if (range == "*" || range.find('x') != std::string::npos ||
            range.find('X') != std::string::npos || range.find('*') != std::string::npos) {
            const auto base = parseVersion(range);
            if (!base.valid) {
                return true;
            }
            bool matches = true;
            for (std::size_t index = 0; index < base.parts.size() && index < version.parts.size();
                 ++index) {
                if (base.parts[index] != version.parts[index]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return true;
            }
            continue;
        }

        char operation = '=';
        std::string value = range;
        if (range.rfind(">=", 0) == 0 || range.rfind("<=", 0) == 0 ||
            range.rfind("^", 0) == 0 || range.rfind("~", 0) == 0 || range.rfind(">", 0) == 0 ||
            range.rfind("<", 0) == 0 || range.rfind("=", 0) == 0) {
            operation = range[0];
            value = trim(range.substr(1));
            if ((operation == '>' || operation == '<') && !range.empty() && range[1] == '=') {
                operation = range[0];
                value = trim(range.substr(2));
            }
        }

        const auto target = parseVersion(value);
        if (!target.valid) {
            continue;
        }
        const auto comparison = compareVersions(version, target);
        switch (operation) {
        case '>':
            if (comparison > 0) {
                return true;
            }
            break;
        case '<':
            if (comparison < 0) {
                return true;
            }
            break;
        case '^': {
            if (comparison < 0) {
                break;
            }
            const auto major = target.parts[0];
            const auto minor = target.parts.size() > 1 ? target.parts[1] : 0;
            const auto patch = target.parts.size() > 2 ? target.parts[2] : 0;
            Version upper;
            if (major > 0) {
                upper.parts = {major + 1, 0, 0};
            } else if (minor > 0) {
                upper.parts = {0, minor + 1, 0};
            } else {
                upper.parts = {0, 0, patch + 1};
            }
            upper.valid = true;
            if (compareVersions(version, upper) < 0) {
                return true;
            }
            break;
        }
        case '~': {
            if (comparison < 0) {
                break;
            }
            const auto major = target.parts[0];
            const auto minor = target.parts.size() > 1 ? target.parts[1] : 0;
            Version upper{{major, minor + 1, 0}, true};
            if (compareVersions(version, upper) < 0) {
                return true;
            }
            break;
        }
        default:
            if (target.parts.size() == 3) {
                if (comparison == 0) {
                    return true;
                }
            } else {
                bool prefix = version.parts.size() >= target.parts.size();
                for (std::size_t index = 0; index < target.parts.size(); ++index) {
                    if (version.parts[index] != target.parts[index]) {
                        prefix = false;
                        break;
                    }
                }
                if (prefix) {
                    return true;
                }
            }
            break;
        }
    }
    return false;
}

bool hasMarker(const LookupMod& mod, const ModReference& reference, bool fuzzy, bool allowTag) {
    return (hasValue(reference.id) && !mod.id.empty()) ||
           (!fuzzy && hasValue(reference.fileMD5) && hasValue(mod.fileMD5)) ||
           (hasValue(reference.fileExpression) && (hasValue(mod.fileName) || hasValue(mod.name))) ||
           (hasValue(reference.logicalFileName) && hasValue(mod.logicalFileName)) ||
           (reference.repo.has_value() && hasValue(mod.source)) ||
           (allowTag && hasValue(reference.tag) &&
            (hasValue(mod.referenceTag) || !mod.referenceTags.empty()));
}

bool tagMatches(const LookupMod& mod, const std::string& tag) {
    return (mod.referenceTag.has_value() && *mod.referenceTag == tag) || contains(mod.referenceTags, tag);
}

bool matchInstallSpec(const LookupMod& mod, const RuleInfo& rule) {
    const auto check = [](const json& spec, const json& installed) {
        if (spec.is_null() || !spec.is_object() || spec.empty()) {
            return true;
        }
        return installed == spec;
    };
    if (!check(rule.installerChoices, mod.installerChoices)) {
        return false;
    }
    if (rule.fileList.is_array() && !rule.fileList.empty() && mod.fileList != rule.fileList) {
        return false;
    }
    if (!check(rule.patches, mod.patches)) {
        return false;
    }
    return true;
}

bool matchReference(const LookupMod& mod, const ModReference& reference) {
    if (hasValue(reference.id) && reference.id != mod.id) {
        return false;
    }

    const auto fuzzy = isFuzzyVersion(reference.versionMatch.value_or(""));
    if (!hasMarker(mod, reference, fuzzy, true)) {
        return false;
    }

    if (hasValue(reference.tag)) {
        if (tagMatches(mod, *reference.tag)) {
            return true;
        }
        if (!hasMarker(mod, reference, fuzzy, false)) {
            return false;
        }
    }

    if (hasValue(reference.fileMD5) && !fuzzy && reference.fileMD5 != mod.fileMD5) {
        return false;
    }

    if (reference.repo.has_value()) {
        if (!mod.source.has_value() || reference.repo->repository != *mod.source) {
            return false;
        }
        const auto refModId = reference.repo->modId.value_or("-1");
        const auto modId = mod.modId.value_or("-1");
        if (refModId != modId) {
            return false;
        }
        if (!fuzzy) {
            return reference.repo->fileId.value_or("-1") == mod.fileId.value_or("-1");
        }
    } else if (hasValue(reference.fileMD5) && reference.fileMD5 == mod.fileMD5) {
        return true;
    }

    if (hasValue(reference.logicalFileName)) {
        const auto& logical = *reference.logicalFileName;
        if (!mod.additionalLogicalFileNames.empty()) {
            if (!contains(mod.additionalLogicalFileNames, logical) &&
                !(mod.logicalFileName.has_value() && *mod.logicalFileName == logical) &&
                !(mod.customFileName.has_value() && *mod.customFileName == logical) &&
                !hasValue(reference.fileExpression)) {
                return false;
            }
        } else if (!(mod.logicalFileName.has_value() && *mod.logicalFileName == logical) &&
                   !(mod.customFileName.has_value() && *mod.customFileName == logical) &&
                   !hasValue(reference.fileExpression)) {
            return false;
        }
    }

    if (hasValue(reference.fileExpression)) {
        if (!mod.fileName.has_value()) {
            if (!mod.name.has_value() || *mod.name != *reference.fileExpression) {
                return false;
            }
        } else {
            const auto base = sanitizeExpression(*mod.fileName);
            if (base != *reference.fileExpression && !globMatch(base, *reference.fileExpression)) {
                return false;
            }
        }
    }

    if (hasValue(reference.versionMatch) && *reference.versionMatch != "*" &&
        hasValue(mod.version)) {
        const auto versionMatch = reference.versionMatch->substr(
            0, reference.versionMatch->find('+'));
        const auto sameFile =
            hasValue(reference.fileMD5) && reference.fileMD5 == mod.fileMD5;
        const auto sameVersion = *mod.version == *reference.versionMatch ||
                                 coerceVersion(*mod.version) == coerceVersion(versionMatch);
        if (!sameFile && !sameVersion) {
            const auto coerced = coerceVersion(*mod.version);
            if (coerced.empty() || !versionSatisfies(coerced, versionMatch)) {
                return false;
            }
        }
    }

    if (hasValue(reference.gameId) && !mod.game.empty() && !contains(mod.game, *reference.gameId)) {
        return false;
    }

    return true;
}

LookupMod downloadLookup(const DownloadInfo& download) {
    LookupMod mod;
    mod.id = download.id;
    mod.fileMD5 = download.fileMD5;
    mod.fileName = download.localPath;
    mod.name = download.logicalFileName;
    mod.logicalFileName = download.logicalFileName;
    mod.version = download.version;
    mod.fileId = download.fileId;
    mod.modId = download.modId;
    mod.source = download.source;
    mod.referenceTag = download.referenceTag;
    mod.referenceTags = download.referenceTags;
    mod.game = download.game;
    return mod;
}

bool matchIdentifiers(const DownloadInfo& download, const ModReference& reference) {
    if (reference.repo.has_value() && hasValue(download.modId)) {
        if (reference.repo->modId.has_value() &&
            *reference.repo->modId != *download.modId) {
            return false;
        }
        if (reference.repo->fileId.has_value() && hasValue(download.fileId)) {
            return *reference.repo->fileId == *download.fileId;
        }
    }
    if (!hasValue(reference.fileExpression) && hasValue(reference.logicalFileName) &&
        download.logicalFileName.has_value()) {
        if (*download.logicalFileName == *reference.logicalFileName) {
            return true;
        }
    }
    if (hasValue(reference.fileExpression) && download.localPath.has_value()) {
        const auto base = sanitizeExpression(*download.localPath);
        if (base == *reference.fileExpression || globMatch(base, *reference.fileExpression)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string referenceId(const ModReference& reference) {
    if (hasValue(reference.tag)) {
        return *reference.tag;
    }
    if (hasValue(reference.fileMD5)) {
        return *reference.fileMD5;
    }
    if (hasValue(reference.id)) {
        return *reference.id;
    }
    if (hasValue(reference.logicalFileName)) {
        return *reference.logicalFileName;
    }
    if (hasValue(reference.fileExpression)) {
        return *reference.fileExpression;
    }
    return {};
}

std::string ruleId(const RuleInfo& rule) {
    const auto id = referenceId(rule.reference);
    // JavaScript's `type + "_" + undefined` yields the literal "undefined".
    return rule.type + "_" + (id.empty() ? "undefined" : id);
}

bool matchMod(const LookupMod& mod, const ModReference& reference, const RuleInfo* installSpec) {
    if (!matchReference(mod, reference)) {
        return false;
    }
    return installSpec == nullptr || matchInstallSpec(mod, *installSpec);
}

std::optional<std::string> matchModId(const std::vector<LookupMod>& mods,
                                      const ModReference& reference, const RuleInfo* installSpec) {
    if (hasValue(reference.idHint)) {
        const auto found = std::find_if(mods.begin(), mods.end(), [&](const LookupMod& mod) {
            return mod.id == *reference.idHint && matchMod(mod, reference, installSpec);
        });
        if (found != mods.end()) {
            return found->id;
        }
    }

    if (hasValue(reference.md5Hint)) {
        const auto found = std::find_if(mods.begin(), mods.end(), [&](const LookupMod& mod) {
            return mod.fileMD5.has_value() && *mod.fileMD5 == *reference.md5Hint &&
                   (installSpec == nullptr || matchInstallSpec(mod, *installSpec));
        });
        if (found != mods.end()) {
            return found->id;
        }
    }

    for (const auto& mod : mods) {
        if (matchMod(mod, reference, installSpec)) {
            return mod.id;
        }
    }
    return std::nullopt;
}

std::optional<std::string> matchDownloadId(const std::vector<DownloadInfo>& downloads,
                                           const ModReference& reference) {
    if (hasValue(reference.md5Hint)) {
        const auto found = std::find_if(downloads.begin(), downloads.end(), [&](const auto& d) {
            return d.fileMD5.has_value() && *d.fileMD5 == *reference.md5Hint;
        });
        if (found != downloads.end()) {
            return found->id;
        }
    }

    auto effective = reference;
    if (isFuzzyVersion(reference.versionMatch.value_or("")) && hasValue(reference.fileMD5) &&
        (hasValue(reference.logicalFileName) || hasValue(reference.fileExpression))) {
        effective.fileMD5.reset();
    }

    std::vector<const DownloadInfo*> matches;
    for (const auto& download : downloads) {
        if (download.state == "failed") {
            continue;
        }
        const auto lookup = downloadLookup(download);
        if (matchReference(lookup, effective) || matchIdentifiers(download, effective)) {
            matches.push_back(&download);
        }
    }

    if (matches.empty()) {
        return std::nullopt;
    }
    std::sort(matches.begin(), matches.end(),
              [](const DownloadInfo* lhs, const DownloadInfo* rhs) {
                  return lhs->fileTime > rhs->fileTime;
              });
    return matches.front()->id;
}

std::vector<DependencyEntry> resolveDependencies(const std::vector<RuleInfo>& rules,
                                                 const std::vector<LookupMod>& mods,
                                                 const std::vector<DownloadInfo>& downloads) {
    std::vector<DependencyEntry> result;
    for (const auto& rule : rules) {
        if (rule.type != "requires" && rule.type != "recommends") {
            continue;
        }
        DependencyEntry entry;
        entry.ruleIndex = rule.index;
        entry.ruleId = ruleId(rule);
        entry.type = rule.type;
        entry.phase = rule.phase;
        entry.ignored = rule.ignored;
        entry.modId = matchModId(mods, rule.reference, &rule);
        entry.downloadId = matchDownloadId(downloads, rule.reference);

        if (rule.ignored) {
            entry.status = "ignored";
        } else if (entry.modId.has_value()) {
            const auto found = std::find_if(mods.begin(), mods.end(), [&](const LookupMod& mod) {
                return mod.id == *entry.modId;
            });
            entry.status = found != mods.end() && found->state.has_value()
                               ? *found->state
                               : "installed";
        } else if (entry.downloadId.has_value()) {
            const auto found = std::find_if(
                downloads.begin(), downloads.end(),
                [&](const DownloadInfo& download) { return download.id == *entry.downloadId; });
            if (found != downloads.end()) {
                if (found->state == "failed") {
                    entry.status = "failed";
                } else if (found->state == "finished") {
                    entry.status = "downloaded";
                } else {
                    entry.status = "downloading";
                }
            } else {
                entry.status = "pending";
            }
        } else if (hasValue(rule.localPath)) {
            entry.status = "downloaded";
        } else {
            entry.status = "pending";
        }
        result.push_back(std::move(entry));
    }
    return result;
}

CollectionPlan planCollection(const std::vector<RuleInfo>& rules,
                              const std::vector<LookupMod>& mods,
                              const std::vector<DownloadInfo>& downloads) {
    CollectionPlan plan;
    plan.members = resolveDependencies(rules, mods, downloads);

    std::set<int> phases;
    for (const auto& member : plan.members) {
        phases.insert(member.phase);
        if (member.type == "recommends") {
            ++plan.totalOptional;
        } else {
            ++plan.totalRequired;
        }
    }
    plan.phases.assign(phases.begin(), phases.end());
    return plan;
}

} // namespace vortex::core
