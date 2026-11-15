#include "core/update/json.h"

#include <cctype>

namespace kuf::setup::update {
namespace {

struct Parser {
  const std::string& s;
  size_t at = 0;
  int depth = 0;

  void Space() {
    while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) ++at;
  }
  bool Eat(char c) {
    Space();
    if (at < s.size() && s[at] == c) {
      ++at;
      return true;
    }
    return false;
  }
  bool Word(const char* w) {
    const size_t n = std::char_traits<char>::length(w);
    if (s.compare(at, n, w) != 0) return false;
    at += n;
    return true;
  }
  static void Utf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(char(cp));
    } else if (cp < 0x800) {
      out->push_back(char(0xC0 | (cp >> 6)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(char(0xE0 | (cp >> 12)));
      out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(char(0xF0 | (cp >> 18)));
      out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    }
  }
  bool Hex4(uint32_t* v) {
    if (at + 4 > s.size()) return false;
    *v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s[at++];
      *v <<= 4;
      if (c >= '0' && c <= '9') *v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') *v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') *v |= uint32_t(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool String(std::string* out) {
    Space();
    if (at >= s.size() || s[at] != '"') return false;
    ++at;
    while (at < s.size()) {
      const char c = s[at++];
      if (c == '"') return true;
      if (uint8_t(c) < 0x20) return false;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (at >= s.size()) return false;
      const char e = s[at++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          uint32_t cp;
          if (!Hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {  // a surrogate pair
            uint32_t low;
            if (!Word("\\u") || !Hex4(&low) || low < 0xDC00 || low > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
          }
          Utf8(cp, out);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool Number(std::string* out) {
    const size_t start = at;
    if (at < s.size() && s[at] == '-') ++at;
    if (at >= s.size() || !std::isdigit(uint8_t(s[at]))) return false;
    if (s[at] == '0') {
      ++at;
    } else {
      while (at < s.size() && std::isdigit(uint8_t(s[at]))) ++at;
    }
    if (at < s.size() && s[at] == '.') {
      ++at;
      if (at >= s.size() || !std::isdigit(uint8_t(s[at]))) return false;
      while (at < s.size() && std::isdigit(uint8_t(s[at]))) ++at;
    }
    if (at < s.size() && (s[at] == 'e' || s[at] == 'E')) {
      ++at;
      if (at < s.size() && (s[at] == '+' || s[at] == '-')) ++at;
      if (at >= s.size() || !std::isdigit(uint8_t(s[at]))) return false;
      while (at < s.size() && std::isdigit(uint8_t(s[at]))) ++at;
    }
    *out = s.substr(start, at - start);
    return true;
  }
  bool Value(Json* v) {
    if (++depth > 64) return false;
    Space();
    if (at >= s.size()) return false;
    bool ok = true;
    const char c = s[at];
    if (c == '{') {
      ++at;
      v->type = Json::kObject;
      if (!Eat('}')) {
        do {
          std::string key;
          Json item;
          if (!String(&key) || !Eat(':') || !Value(&item)) return false;
          v->fields[key] = std::move(item);
        } while (Eat(','));
        ok = Eat('}');
      }
    } else if (c == '[') {
      ++at;
      v->type = Json::kArray;
      if (!Eat(']')) {
        do {
          Json item;
          if (!Value(&item)) return false;
          v->items.push_back(std::move(item));
        } while (Eat(','));
        ok = Eat(']');
      }
    } else if (c == '"') {
      v->type = Json::kString;
      ok = String(&v->text);
    } else if (Word("true")) {
      v->type = Json::kBool;
      v->boolean = true;
    } else if (Word("false")) {
      v->type = Json::kBool;
    } else if (Word("null")) {
      v->type = Json::kNull;
    } else {
      v->type = Json::kNumber;
      ok = Number(&v->text);
    }
    --depth;
    return ok;
  }
};

}  // namespace

const Json* Json::Get(const std::string& key) const {
  if (type != kObject) return nullptr;
  const auto it = fields.find(key);
  return it == fields.end() ? nullptr : &it->second;
}

std::string Json::String(const std::string& key, const std::string& fallback) const {
  const Json* v = Get(key);
  return v && v->type == kString ? v->text : fallback;
}

bool Json::Bool(const std::string& key) const {
  const Json* v = Get(key);
  return v && v->type == kBool && v->boolean;
}

bool Json::IsInteger() const {
  if (type != kNumber || text.empty() || text.size() > 19) return false;
  for (char c : text) {
    if (!std::isdigit(uint8_t(c))) return false;
  }
  return true;
}

std::optional<uint64_t> Json::Integer(const std::string& key) const {
  const Json* v = Get(key);
  if (!v || !v->IsInteger()) return std::nullopt;
  return std::stoull(v->text);
}

std::optional<Json> ParseJson(const std::string& text) {
  Parser p{text};
  Json v;
  if (!p.Value(&v)) return std::nullopt;
  p.Space();
  if (p.at != text.size()) return std::nullopt;
  return v;
}

std::string JsonQuote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (uint8_t(c) < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out += hex[uint8_t(c) >> 4];
          out += hex[uint8_t(c) & 15];
        } else {
          out.push_back(c);
        }
    }
  }
  return out + "\"";
}

}  // namespace kuf::setup::update
