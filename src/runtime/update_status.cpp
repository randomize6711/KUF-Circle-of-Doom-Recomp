#include "runtime/update_status.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace kuf::updates {
namespace {

// A minimal reader for the updater's flat JSON objects (strings with escapes, numbers, booleans, null; nested
// arrays and objects are skipped).
struct Reader {
  std::string_view s;
  size_t i = 0;
  void Space() {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  }
  bool Eat(char c) {
    Space();
    if (i < s.size() && s[i] == c) {
      ++i;
      return true;
    }
    return false;
  }
  std::optional<std::string> String() {
    if (!Eat('"')) return std::nullopt;
    std::string out;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size()) return std::nullopt;
      c = s[i++];
      switch (c) {
        case 'n': out += '\n'; break;
        case 't': out += ' '; break;
        case 'r': break;
        case 'b': case 'f': break;
        case 'u': {
          if (i + 4 > s.size()) return std::nullopt;
          const unsigned v = unsigned(std::strtoul(std::string(s.substr(i, 4)).c_str(), nullptr, 16));
          i += 4;
          if (v < 0x80) {
            if (v >= 0x20) out += char(v);
          } else if (v < 0x800) {
            out += char(0xC0 | (v >> 6));
            out += char(0x80 | (v & 0x3F));
          } else {
            out += char(0xE0 | (v >> 12));
            out += char(0x80 | ((v >> 6) & 0x3F));
            out += char(0x80 | (v & 0x3F));
          }
          break;
        }
        default: out += c; break;  // \" \\ \/
      }
    }
    if (i >= s.size()) return std::nullopt;
    ++i;
    return out;
  }
  // Skips any value; returns its text for scalars.
  bool Value(std::string* scalar, std::string* text) {
    Space();
    if (i >= s.size()) return false;
    if (s[i] == '"') {
      auto v = String();
      if (!v) return false;
      *text = *v;
      return true;
    }
    if (s[i] == '[' || s[i] == '{') {
      const char open = s[i], close = open == '[' ? ']' : '}';
      int depth = 0;
      bool in_string = false;
      for (; i < s.size(); ++i) {
        const char c = s[i];
        if (in_string) {
          if (c == '\\') ++i;
          else if (c == '"') in_string = false;
        } else if (c == '"') {
          in_string = true;
        } else if (c == open) {
          ++depth;
        } else if (c == close && --depth == 0) {
          ++i;
          return true;
        }
      }
      return false;
    }
    const size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    *scalar = std::string(s.substr(start, i - start));
    return !scalar->empty();
  }
};

// Markup-safe text for the game's message box: no '@' (markup) or '\' (the box's own line breaks), new lines as the
// box's "\n", other control characters dropped.
std::string BoxText(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '\n') {
      out += "\\n";
    } else if (c == '@' || c == '\\' || (static_cast<unsigned char>(c) < 0x20)) {
      continue;
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

std::optional<Reply> ParseLine(std::string_view line) {
  Reader r{line};
  if (!r.Eat('{')) return std::nullopt;
  Reply out;
  if (r.Eat('}')) return out;
  do {
    auto key = r.String();
    if (!key || !r.Eat(':')) return std::nullopt;
    std::string scalar, text;
    if (!r.Value(&scalar, &text)) return std::nullopt;
    const std::string& k = *key;
    if (k == "exit") out.exit = std::atoi(scalar.c_str());
    else if (k == "update") out.update = scalar == "true";
    else if (k == "current") out.current = text;
    else if (k == "latest") out.latest = text;
    else if (k == "published") out.published = text;
    else if (k == "notes") out.notes = text;
    else if (k == "summary") out.summary = text;
    else if (k == "page") out.page = text;
    else if (k == "error") out.error = text;
    else if (k == "stage") out.stage = text;
    else if (k == "progress") out.progress = std::atoll(scalar.c_str());
    else if (k == "total") out.total = std::atoll(scalar.c_str());
  } while (r.Eat(','));
  if (!r.Eat('}')) return std::nullopt;
  return out;
}

std::pair<std::string, std::string> AnswerLabels(Dialog dialog) {
  switch (dialog) {
    case Dialog::kOffer: return {"Select", "Cancel"};
    case Dialog::kRestart: return {"Restart Now", "Later"};
    case Dialog::kCancelAsk: return {"Cancel Download", "Keep Downloading"};
    default: return {"OK", "Close"};
  }
}

std::string ReleasePage(std::string_view reported) {
  const std::string prefix = std::string(kReleasesPage) + "/tag/v";
  if (reported.size() > prefix.size() && reported.size() <= prefix.size() + 64 &&
      reported.substr(0, prefix.size()) == prefix) {
    const std::string_view version = reported.substr(prefix.size());
    const bool ok = std::all_of(version.begin(), version.end(), [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-';
    });
    if (ok && std::isdigit(static_cast<unsigned char>(version.front()))) return std::string(reported);
  }
  return kReleasesPage;
}

std::string DialogText(Dialog dialog, const Reply& reply, std::string_view installed) {
  const std::string current = BoxText(reply.current.empty() ? installed : std::string_view(reply.current));
  const std::string latest = BoxText(reply.latest);
  std::string t;
  switch (dialog) {
    case Dialog::kOffer:
      t = "UPDATE AVAILABLE\\n\\nInstalled Version: " + current + "\\nNew Version: " + latest +
          "\\n\\nDownload and install the update now?";
      for (int i = 0; i < kOfferButtonLines; ++i) t += "\\n ";
      break;
    case Dialog::kUpToDate:
      t = "NO UPDATE AVAILABLE\\n\\nInstalled Version: " + current + "\\nThis is the newest version.";
      break;
    case Dialog::kFailed:
      t = "UPDATE FAILED\\n\\nInstalled Version: " + current + "\\n" +
          (reply.error.empty() ? std::string("The updater gave no reason.") : BoxText(reply.error)) +
          "\\nNothing was changed.";
      break;
    case Dialog::kNotSetUp:
      t = "UPDATES NOT AVAILABLE\\n\\nInstalled Version: " + current +
          "\\nThis copy was not installed with the setup, so it cannot update itself.";
      break;
    case Dialog::kCancelAsk:
      t = "CANCEL THE DOWNLOAD?\\n\\nNothing is changed until the update is verified.";
      break;
    case Dialog::kCancelled:
      t = "DOWNLOAD CANCELLED\\n\\nNothing was changed.";
      break;
    case Dialog::kRestart:
      t = "RESTART REQUIRED\\n\\nVersion " + latest + " is installed. Restart now to use it?";
      break;
    case Dialog::kInstalledNoRestart:
      t = "UPDATE INSTALLED\\n\\nVersion " + latest + " is used from the next start of the game.";
      break;
  }
  return t;
}

std::string RelabelAnswers(std::string_view text, std::string_view a, std::string_view b) {
  std::string out(text);
  auto rename = [&](std::string_view icon, std::string_view word, std::string_view label) {
    size_t at = out.find(icon);
    if (at == std::string::npos) return false;
    at += icon.size();
    while (at < out.size() && out[at] == ' ') ++at;
    const size_t end = at + word.size();
    if (out.compare(at, word.size(), word) != 0 || (end < out.size() && std::isalpha(static_cast<unsigned char>(out[end])))) {
      return false;
    }
    out.replace(at, word.size(), label);
    return true;
  };
  if (!rename("@(A)", "Yes", a) || !rename("@(B)", "No", b)) return std::string(text);
  return out;
}

std::string ProgressLabel(const Reply& p) {
  if (p.stage == "verifying") return "VERIFYING THE UPDATE...";
  if (p.stage == "installing") return "INSTALLING THE UPDATE...";
  if (p.progress < 0) return "DOWNLOADING...";
  const int percent = p.total > 0 ? int(p.progress * 100 / p.total) : 0;
  return "DOWNLOADING " + std::to_string(percent) + "%";
}

}  // namespace kuf::updates
