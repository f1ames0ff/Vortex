#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vortex::game {

/**
 * TOML syntax error. The message already contains the source name and the
 * 1-based line number so callers can surface it without extra formatting.
 */
class TomlError : public std::runtime_error {
public:
    TomlError(std::size_t line, const std::string& message);

    std::size_t line() const noexcept { return line_; }

private:
    std::size_t line_;
};

enum class TomlType {
    String,
    Integer,
    Boolean,
    Array,
    Table,
};

struct TomlValue;
using TomlArray = std::vector<TomlValue>;
using TomlTable = std::map<std::string, TomlValue>;

/**
 * A deliberately small subset of TOML.
 *
 * Supported: comments, blank lines, [table] headers, dotted keys, basic and
 * literal strings, integers, booleans and arrays (including multi-line
 * arrays). Floats, dates, inline tables and arrays of tables are rejected with
 * a line-numbered error.
 */
struct TomlValue {
    TomlType type = TomlType::String;
    std::string stringValue;
    std::int64_t integerValue = 0;
    bool booleanValue = false;
    TomlArray arrayValue;
    std::shared_ptr<TomlTable> tableValue;

    bool isString() const noexcept { return type == TomlType::String; }
    bool isInteger() const noexcept { return type == TomlType::Integer; }
    bool isBoolean() const noexcept { return type == TomlType::Boolean; }
    bool isArray() const noexcept { return type == TomlType::Array; }
    bool isTable() const noexcept { return type == TomlType::Table; }
};

/** Human-readable name used in descriptor type errors. */
const char* tomlTypeName(TomlType type);

/** Parse a TOML document. Throws TomlError on malformed input. */
TomlTable parseToml(std::string_view text, std::string_view sourceName = "<input>");

/**
 * Look up a value by a dotted path. Returns nullptr when the path does not
 * exist or an intermediate value is not a table.
 */
const TomlValue* findToml(const TomlTable& table, std::string_view path);
TomlValue* findToml(TomlTable& table, std::string_view path);

} // namespace vortex::game
