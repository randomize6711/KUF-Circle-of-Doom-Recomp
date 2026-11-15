// A small JSON reader (RFC 8259) for the updater: GitHub's release list and the signed update manifest (M17G).
// Depth and size are bounded; numbers are kept as text and read as integers where needed.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kuf::setup::update {

struct Json {
  enum Type { kNull, kBool, kNumber, kString, kArray, kObject } type = kNull;
  bool boolean = false;
  std::string text;  // a string's value, or a number's text
  std::vector<Json> items;
  std::map<std::string, Json> fields;

  const Json* Get(const std::string& key) const;  // nullptr unless an object with that key
  std::string String(const std::string& key, const std::string& fallback = "") const;
  bool Bool(const std::string& key) const;  // false unless true
  // A non-negative integer field (no fraction, no exponent); nullopt otherwise.
  std::optional<uint64_t> Integer(const std::string& key) const;
  bool IsInteger() const;
};

// nullopt on anything that is not one complete JSON value (trailing text, depth over 64, bad escapes).
std::optional<Json> ParseJson(const std::string& text);

// A string as a JSON string literal (for the updater's own --json lines).
std::string JsonQuote(const std::string& s);

}  // namespace kuf::setup::update
