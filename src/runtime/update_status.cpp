#include "runtime/update_status.h"

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
    else if (k == "error") out.error = text;
    else if (k == "stage") out.stage = text;
    else if (k == "progress") out.progress = std::atoll(scalar.c_str());
    else if (k == "total") out.total = std::atoll(scalar.c_str());
  } while (r.Eat(','));
  if (!r.Eat('}')) return std::nullopt;
  return out;
}

std::string DialogText(Dialog dialog, const Reply& reply, std::string_view installed, size_t max_notes) {
  const std::string current = BoxText(reply.current.empty() ? installed : std::string_view(reply.current));
  const std::string latest = BoxText(reply.latest);
  std::string t;
  switch (dialog) {
    case Dialog::kOffer: {
      t = "Update available\\nInstalled: " + current + "\\nAvailable: " + latest;
      if (!reply.published.empty()) t += " (released " + BoxText(reply.published) + ")";
      std::string notes = BoxText(reply.notes);
      if (notes.size() > max_notes) {
        notes.resize(max_notes);
        while (!notes.empty() && notes.back() == '\\') notes.pop_back();  // never half a "\n"
        notes += "...";
      }
      if (!notes.empty()) t += "\\n\\n" + notes;
      t += "\\n\\nDownload and install it now? @(A)Install  @(B)Close";
      break;
    }
    case Dialog::kUpToDate:
      t = "No update available\\nInstalled: " + current + (latest.empty() ? "" : "\\nLatest release: " + latest) +
          "\\n\\nThis is the newest version.";
      break;
    case Dialog::kFailed:
      t = "The update did not complete\\nInstalled: " + current + "\\n\\n" +
          (reply.error.empty() ? std::string("The updater gave no reason.") : BoxText(reply.error)) +
          "\\n\\nNothing was changed. Check the network and try again.";
      break;
    case Dialog::kNotSetUp:
      t = "Updates are not set up\\nInstalled: " + current +
          "\\n\\nThis copy was not installed from a release package (or no release source is configured), so it "
          "cannot update itself.";
      break;
    case Dialog::kCancelAsk:
      t = "Cancel the download of " + latest + "?\\nNothing is changed until the download is complete and verified."
          " @(A)Cancel download  @(B)Continue";
      break;
    case Dialog::kCancelled:
      t = "Download cancelled\\nInstalled: " + current + "\\n\\nNothing was changed.";
      break;
    case Dialog::kRestart:
      t = "Version " + latest + " is installed\\nIt is used from the next start. Restart now? (the game closes and "
          "starts again; save first if needed) @(A)Restart  @(B)Later";
      break;
    case Dialog::kInstalledNoRestart:
      t = "Version " + latest + " is installed\\nIt is used from the next start of the game.";
      break;
  }
  return t;
}

std::string ProgressLabel(const Reply& p) {
  if (p.stage == "verifying") return "VERIFYING THE UPDATE...";
  if (p.progress < 0) return "DOWNLOADING...";
  const int percent = p.total > 0 ? int(p.progress * 100 / p.total) : 0;
  return "DOWNLOADING " + std::to_string(percent) + "%";
}

}  // namespace kuf::updates
