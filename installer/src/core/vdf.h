// Valve's key-value files as Steam keeps them (installer/README.md "Steam"): the binary form of
// userdata/<id>/config/shortcuts.vdf (the non-Steam games) and the text form of config/loginusers.vdf (the accounts'
// names). The binary reader keeps every node it reads (unknown fixed-size values byte for byte), so writing back
// changes only what was edited; a node type it cannot size makes it refuse the file (nothing is written then).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kuf::setup::vdf {

enum Type : uint8_t {
  kMap = 0x00,
  kString = 0x01,
  kInt32 = 0x02,
  kFloat = 0x03,
  kPointer = 0x04,
  kColor = 0x06,
  kUInt64 = 0x07,
  kEnd = 0x08,
  kInt64 = 0x0A,
};

struct Node {
  Type type = kMap;
  std::string key;
  std::string str;                 // kString
  std::vector<uint8_t> raw;        // the fixed-size values (little-endian), kept as read
  std::vector<Node> children;      // kMap

  Node* Child(std::string_view k);
  const Node* Child(std::string_view k) const;
  // Case-insensitive lookup (Steam writes "AppName" / "appname" / "exe" differently across versions).
  const Node* ChildNoCase(std::string_view k) const;
  int32_t Int32() const;
  static Node String(std::string key, std::string value);
  static Node Int(std::string key, int32_t value);
  static Node Map(std::string key);
};

// The top level of a binary file is a list of nodes ending with kEnd: returned as a map with an empty key.
std::optional<Node> ParseBinary(std::string_view data, std::string* error);
std::string WriteBinary(const Node& root);

// A text VDF ("key" "value" / "key" { ... }): the same tree with string values only.
std::optional<Node> ParseText(std::string_view text);

}  // namespace kuf::setup::vdf
