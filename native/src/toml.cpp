#include "vortex/game/toml.hpp"

#include <algorithm>
#include <charconv>
#include <set>
#include <system_error>
#include <utility>

namespace vortex::game {

TomlError::TomlError(std::size_t line, const std::string& message)
    : std::runtime_error(message), line_(line) {}

const char* tomlTypeName(TomlType type) {
    switch (type) {
    case TomlType::String:
        return "string";
    case TomlType::Integer:
        return "integer";
    case TomlType::Boolean:
        return "boolean";
    case TomlType::Array:
        return "array";
    case TomlType::Table:
        return "table";
    }
    return "unknown";
}

namespace {

bool isBareKeyChar(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-';
}

bool isDigit(char value) {
    return value >= '0' && value <= '9';
}

bool isHorizontalSpace(char value) {
    return value == ' ' || value == '\t';
}

std::string joinPath(const std::vector<std::string>& path) {
    std::string result;
    for (const auto& segment : path) {
        if (!result.empty()) {
            result.push_back('.');
        }
        result += segment;
    }
    return result;
}

class Parser {
public:
    Parser(std::string_view text, std::string source)
        : text_(text), source_(std::move(source)) {}

    TomlTable parse() {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            position_ = 3;
        }

        while (!atEnd()) {
            skipHorizontalSpace();
            if (atEnd()) {
                break;
            }
            if (peek() == '\n' || peek() == '\r') {
                consumeNewline();
                continue;
            }
            if (peek() == '#') {
                skipComment();
                continue;
            }
            if (peek() == '[') {
                parseTableHeader();
            } else {
                parseKeyValue();
            }
            finishStatement();
        }
        return std::move(root_);
    }

private:
    bool atEnd() const { return position_ >= text_.size(); }

    char peek() const { return text_[position_]; }

    void expect(char value, std::size_t line, const std::string& message) {
        if (atEnd() || peek() != value) {
            fail(line, message);
        }
        ++position_;
    }

    void skipHorizontalSpace() {
        while (!atEnd() && isHorizontalSpace(peek())) {
            ++position_;
        }
    }

    void skipComment() {
        while (!atEnd() && peek() != '\n' && peek() != '\r') {
            ++position_;
        }
    }

    void consumeNewline() {
        if (peek() == '\r') {
            ++position_;
            if (!atEnd() && peek() == '\n') {
                ++position_;
            }
        } else {
            ++position_;
        }
        ++line_;
    }

    [[noreturn]] void fail(std::size_t line, const std::string& message) const {
        const auto prefix =
            source_.empty() ? "line " + std::to_string(line) : source_ + ":" + std::to_string(line);
        throw TomlError(line, prefix + ": " + message);
    }

    void finishStatement() {
        skipHorizontalSpace();
        if (!atEnd() && peek() == '#') {
            skipComment();
        }
        if (atEnd()) {
            return;
        }
        if (peek() == '\n' || peek() == '\r') {
            consumeNewline();
            return;
        }
        fail(line_, "unexpected characters after value");
    }

    std::vector<std::string> parseKeyPath(char terminator, std::size_t startLine) {
        std::vector<std::string> path;
        for (;;) {
            skipHorizontalSpace();
            if (atEnd() || peek() == '\n' || peek() == '\r') {
                fail(startLine, std::string("expected a key before '") + terminator + "'");
            }
            if (peek() == terminator) {
                if (path.empty()) {
                    fail(startLine, std::string("expected a key before '") + terminator + "'");
                }
                return path;
            }
            path.push_back(parseKeySegment(startLine));
            skipHorizontalSpace();
            if (!atEnd() && peek() == '.') {
                ++position_;
                continue;
            }
            if (atEnd() || peek() != terminator) {
                fail(startLine,
                     std::string("expected '.' or '") + terminator + "' in key '" +
                         joinPath(path) + "'");
            }
            return path;
        }
    }

    std::string parseKeySegment(std::size_t startLine) {
        const auto start = position_;
        while (!atEnd() && isBareKeyChar(peek())) {
            ++position_;
        }
        if (position_ == start) {
            fail(startLine, "expected a bare key");
        }
        return std::string(text_.substr(start, position_ - start));
    }

    void parseTableHeader() {
        const auto startLine = line_;
        expect('[', startLine, "expected '['");
        if (!atEnd() && peek() == '[') {
            fail(startLine, "arrays of tables are not supported");
        }
        auto path = parseKeyPath(']', startLine);
        expect(']', startLine, "expected ']' after table name");

        const auto name = joinPath(path);
        if (!declaredTables_.insert(name).second) {
            fail(startLine, "duplicate table [" + name + "]");
        }
        (void)ensureTable(root_, path, startLine);
        currentPath_ = std::move(path);
    }

    void parseKeyValue() {
        const auto startLine = line_;
        auto path = parseKeyPath('=', startLine);
        expect('=', startLine, "expected '=' after key");
        skipHorizontalSpace();
        if (atEnd() || peek() == '\n' || peek() == '\r') {
            fail(startLine, "missing value for key '" + joinPath(path) + "'");
        }

        auto value = parseValue();
        auto& table = ensureTable(root_, currentPath_, startLine);
        TomlTable* target = &table;
        for (std::size_t index = 0; index + 1 < path.size(); ++index) {
            auto found = target->find(path[index]);
            if (found == target->end()) {
                TomlValue nested;
                nested.type = TomlType::Table;
                nested.tableValue = std::make_shared<TomlTable>();
                found = target->emplace(path[index], std::move(nested)).first;
            } else if (!found->second.isTable() || !found->second.tableValue) {
                fail(startLine,
                     "key '" + joinPath(path) + "' conflicts with an existing value");
            }
            target = found->second.tableValue.get();
        }

        if (target->contains(path.back())) {
            fail(startLine, "duplicate key '" + joinPath(path) + "'");
        }
        target->emplace(path.back(), std::move(value));
    }

    TomlTable& ensureTable(TomlTable& root, const std::vector<std::string>& path,
                           std::size_t line) {
        TomlTable* table = &root;
        for (const auto& segment : path) {
            auto found = table->find(segment);
            if (found == table->end()) {
                TomlValue nested;
                nested.type = TomlType::Table;
                nested.tableValue = std::make_shared<TomlTable>();
                found = table->emplace(segment, std::move(nested)).first;
            } else if (!found->second.isTable() || !found->second.tableValue) {
                fail(line, "table '" + segment + "' conflicts with an existing value");
            }
            table = found->second.tableValue.get();
        }
        return *table;
    }

    TomlValue parseValue() {
        skipHorizontalSpace();
        if (atEnd()) {
            fail(line_, "expected a value");
        }
        switch (peek()) {
        case '"':
            return parseBasicString();
        case '\'':
            return parseLiteralString();
        case '[':
            return parseArray();
        case 't':
        case 'f':
            return parseBoolean();
        default:
            if (peek() == '+' || peek() == '-' || isDigit(peek())) {
                return parseInteger();
            }
            fail(line_, "unsupported value; expected a string, integer, boolean or array");
        }
    }

    TomlValue parseBasicString() {
        const auto startLine = line_;
        expect('"', startLine, "expected a string");
        std::string result;
        while (!atEnd()) {
            const auto value = peek();
            if (value == '"') {
                ++position_;
                TomlValue parsed;
                parsed.type = TomlType::String;
                parsed.stringValue = std::move(result);
                return parsed;
            }
            if (value == '\n' || value == '\r') {
                fail(startLine, "unterminated string");
            }
            if (value == '\\') {
                ++position_;
                if (atEnd()) {
                    fail(startLine, "unterminated string");
                }
                const auto escaped = peek();
                ++position_;
                switch (escaped) {
                case '"':
                    result.push_back('"');
                    break;
                case '\\':
                    result.push_back('\\');
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                default:
                    fail(line_, std::string("unsupported escape sequence '\\") + escaped + "'");
                }
                continue;
            }
            result.push_back(value);
            ++position_;
        }
        fail(startLine, "unterminated string");
    }

    TomlValue parseLiteralString() {
        const auto startLine = line_;
        expect('\'', startLine, "expected a string");
        std::string result;
        while (!atEnd()) {
            const auto value = peek();
            if (value == '\'') {
                ++position_;
                TomlValue parsed;
                parsed.type = TomlType::String;
                parsed.stringValue = std::move(result);
                return parsed;
            }
            if (value == '\n' || value == '\r') {
                fail(startLine, "unterminated literal string");
            }
            result.push_back(value);
            ++position_;
        }
        fail(startLine, "unterminated literal string");
    }

    TomlValue parseBoolean() {
        TomlValue parsed;
        parsed.type = TomlType::Boolean;
        if (text_.substr(position_, 4) == "true") {
            position_ += 4;
            parsed.booleanValue = true;
            return parsed;
        }
        if (text_.substr(position_, 5) == "false") {
            position_ += 5;
            parsed.booleanValue = false;
            return parsed;
        }
        fail(line_, "invalid boolean; expected true or false");
    }

    TomlValue parseInteger() {
        const auto startLine = line_;
        std::string token;
        if (!atEnd() && peek() == '-') {
            token.push_back('-');
            ++position_;
        } else if (!atEnd() && peek() == '+') {
            ++position_;
        }

        bool sawDigit = false;
        bool lastWasUnderscore = false;
        while (!atEnd()) {
            const auto value = peek();
            if (isDigit(value)) {
                token.push_back(value);
                sawDigit = true;
                lastWasUnderscore = false;
                ++position_;
            } else if (value == '_') {
                if (!sawDigit || lastWasUnderscore) {
                    fail(startLine, "invalid integer");
                }
                lastWasUnderscore = true;
                ++position_;
            } else {
                break;
            }
        }
        if (!sawDigit || lastWasUnderscore) {
            fail(startLine, "invalid integer");
        }

        token.erase(std::remove(token.begin(), token.end(), '_'), token.end());
        TomlValue parsed;
        parsed.type = TomlType::Integer;
        const auto result =
            std::from_chars(token.data(), token.data() + token.size(), parsed.integerValue);
        if (result.ec == std::errc::result_out_of_range) {
            fail(startLine, "integer out of range");
        }
        if (result.ec != std::errc() || result.ptr != token.data() + token.size()) {
            fail(startLine, "invalid integer");
        }
        return parsed;
    }

    TomlValue parseArray() {
        const auto startLine = line_;
        expect('[', startLine, "expected '['");
        TomlValue parsed;
        parsed.type = TomlType::Array;
        for (;;) {
            skipArraySpace();
            if (atEnd()) {
                fail(startLine, "unterminated array");
            }
            if (peek() == ']') {
                ++position_;
                return parsed;
            }
            parsed.arrayValue.push_back(parseValue());
            skipArraySpace();
            if (atEnd()) {
                fail(startLine, "unterminated array");
            }
            if (peek() == ',') {
                ++position_;
                continue;
            }
            if (peek() == ']') {
                ++position_;
                return parsed;
            }
            fail(line_, "expected ',' or ']' in array");
        }
    }

    void skipArraySpace() {
        for (;;) {
            skipHorizontalSpace();
            if (atEnd()) {
                return;
            }
            if (peek() == '#') {
                skipComment();
                continue;
            }
            if (peek() == '\n' || peek() == '\r') {
                consumeNewline();
                continue;
            }
            return;
        }
    }

    std::string_view text_;
    std::string source_;
    std::size_t position_ = 0;
    std::size_t line_ = 1;
    TomlTable root_;
    std::vector<std::string> currentPath_;
    std::set<std::string> declaredTables_;
};

} // namespace

TomlTable parseToml(std::string_view text, std::string_view sourceName) {
    Parser parser(text, std::string(sourceName));
    return parser.parse();
}

const TomlValue* findToml(const TomlTable& table, std::string_view path) {
    const TomlTable* current = &table;
    std::size_t start = 0;
    for (;;) {
        const auto dot = path.find('.', start);
        const auto segment = path.substr(start, dot == std::string_view::npos ? dot : dot - start);
        const auto found = current->find(std::string(segment));
        if (found == current->end()) {
            return nullptr;
        }
        if (dot == std::string_view::npos) {
            return &found->second;
        }
        if (!found->second.isTable() || !found->second.tableValue) {
            return nullptr;
        }
        current = found->second.tableValue.get();
        start = dot + 1;
    }
}

TomlValue* findToml(TomlTable& table, std::string_view path) {
    return const_cast<TomlValue*>(findToml(std::as_const(table), path));
}

} // namespace vortex::game
