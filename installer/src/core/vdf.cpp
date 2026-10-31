#include "core/vdf.h"

#include <cctype>
#include <cstring>

namespace kuf::setup::vdf {

namespace {

bool EqualNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

size_t FixedSize(uint8_t type) {
  switch (type) {
    case kInt32:
    case kFloat:
    case kPointer:
    case kColor: return 4;
    case kUInt64:
    case kInt64: return 8;
    default: return 0;
  }
}

struct Reader {
  std::string_view d;
  size_t i = 0;
  std::string* error;
  bool CString(std::string* out) {
    const size_t end = d.find('\0', i);
    if (end == std::string_view::npos) {
      *error = "a string runs past the end of the file";
      return false;
    }
    *out = std::string(d.substr(i, end - i));
    i = end + 1;
    return true;
  }
  // Nodes up to (and including) a kEnd; `top` also accepts the end of the data.
  bool List(std::vector<Node>* out, int depth, bool top) {
    if (depth > 64) {
      *error = "nested too deeply";
      return false;
    }
    while (true) {
      if (i >= d.size()) {
        if (top) return true;
        *error = "the file ends inside a map";
        return false;
      }
      const uint8_t type = uint8_t(d[i++]);
      if (type == kEnd) return true;
      Node n;
      n.type = Type(type);
      if (!CString(&n.key)) return false;
      if (type == kMap) {
        if (!List(&n.children, depth + 1, false)) return false;
      } else if (type == kString) {
        if (!CString(&n.str)) return false;
      } else if (const size_t size = FixedSize(type)) {
        if (i + size > d.size()) {
          *error = "a value runs past the end of the file";
          return false;
        }
        n.raw.assign(d.begin() + ptrdiff_t(i), d.begin() + ptrdiff_t(i + size));
        i += size;
      } else {
        *error = "an unknown value type " + std::to_string(type) + " (not changed: unsupported format)";
        return false;
      }
      out->push_back(std::move(n));
    }
  }
};

void Write(const Node& n, std::string* out) {
  *out += char(n.type);
  *out += n.key;
  *out += '\0';
  if (n.type == kMap) {
    for (const Node& c : n.children) Write(c, out);
    *out += char(kEnd);
  } else if (n.type == kString) {
    *out += n.str;
    *out += '\0';
  } else {
    out->append(reinterpret_cast<const char*>(n.raw.data()), n.raw.size());
  }
}

}  // namespace

Node* Node::Child(std::string_view k) {
  for (Node& c : children) {
    if (c.key == k) return &c;
  }
  return nullptr;
}
const Node* Node::Child(std::string_view k) const {
  for (const Node& c : children) {
    if (c.key == k) return &c;
  }
  return nullptr;
}
const Node* Node::ChildNoCase(std::string_view k) const {
  for (const Node& c : children) {
    if (EqualNoCase(c.key, k)) return &c;
  }
  return nullptr;
}
int32_t Node::Int32() const {
  if (raw.size() < 4) return 0;
  return int32_t(uint32_t(raw[0]) | uint32_t(raw[1]) << 8 | uint32_t(raw[2]) << 16 | uint32_t(raw[3]) << 24);
}
Node Node::String(std::string key, std::string value) {
  Node n;
  n.type = kString;
  n.key = std::move(key);
  n.str = std::move(value);
  return n;
}
Node Node::Int(std::string key, int32_t value) {
  Node n;
  n.type = kInt32;
  n.key = std::move(key);
  const uint32_t v = uint32_t(value);
  n.raw = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
  return n;
}
Node Node::Map(std::string key) {
  Node n;
  n.type = kMap;
  n.key = std::move(key);
  return n;
}

std::optional<Node> ParseBinary(std::string_view data, std::string* error) {
  std::string ignored;
  Reader r{data, 0, error ? error : &ignored};
  Node root;
  if (!r.List(&root.children, 0, true)) return std::nullopt;
  if (r.i != data.size()) {
    *r.error = "data after the end of the file's map";
    return std::nullopt;
  }
  return root;
}

std::string WriteBinary(const Node& root) {
  std::string out;
  for (const Node& c : root.children) Write(c, &out);
  out += char(kEnd);
  return out;
}

std::optional<Node> ParseText(std::string_view t) {
  size_t i = 0;
  auto space = [&] {
    while (i < t.size()) {
      if (std::isspace(static_cast<unsigned char>(t[i]))) {
        ++i;
      } else if (t.substr(i, 2) == "//") {
        while (i < t.size() && t[i] != '\n') ++i;
      } else {
        break;
      }
    }
  };
  auto token = [&](std::string* out) -> int {  // 1 a string, 2 '{', 3 '}', 0 the end, -1 an error
    space();
    if (i >= t.size()) return 0;
    if (t[i] == '{') {
      ++i;
      return 2;
    }
    if (t[i] == '}') {
      ++i;
      return 3;
    }
    if (t[i] != '"') return -1;
    ++i;
    out->clear();
    while (i < t.size() && t[i] != '"') {
      if (t[i] == '\\' && i + 1 < t.size()) {
        ++i;
        *out += t[i] == 'n' ? '\n' : t[i] == 't' ? '\t' : t[i];
      } else {
        *out += t[i];
      }
      ++i;
    }
    if (i >= t.size()) return -1;
    ++i;
    return 1;
  };
  std::vector<Node*> stack;
  Node root;
  stack.push_back(&root);
  std::string key, value;
  for (int guard = 0; guard < 1000000; ++guard) {
    const int k = token(&key);
    if (k == 0) return stack.size() == 1 ? std::optional<Node>(root) : std::nullopt;
    if (k == 3) {
      if (stack.size() == 1) return std::nullopt;
      stack.pop_back();
      continue;
    }
    if (k != 1) return std::nullopt;
    const int v = token(&value);
    if (v == 1) {
      stack.back()->children.push_back(Node::String(key, value));
    } else if (v == 2) {
      stack.back()->children.push_back(Node::Map(key));
      stack.push_back(&stack.back()->children.back());
    } else {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

}  // namespace kuf::setup::vdf
