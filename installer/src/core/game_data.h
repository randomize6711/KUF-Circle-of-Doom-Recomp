// The player's original game files (installer/README.md "Game data"): the supported version's reference list
// (installer/assets/game_v1.tsv: SHA-256, size and path of each file the game reads; the console's $SystemUpdate
// folder is not needed) and the checks against a chosen folder or disc image. Both are only ever read. default.xex is
// compared by its decompressed memory image and must be unencrypted: the setup carries no console keys and does not
// decrypt, unless the build carries the retail key (core/xex_decrypt.h): then an encrypted default.xex that decrypts
// to the supported image is accepted (CheckGame: `decrypt`). Otherwise a retail dump can be combined with a separately
// supplied unencrypted default.xex (docs/RETAIL_IMPORT_PLAN.md A4).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/util.h"
#include "core/xiso.h"

namespace kuf::setup {

struct GameFile {
  std::string sha256;
  uint64_t size = 0;
  std::string path;  // relative, '/'-separated
};

struct GameManifest {
  std::string name;  // "Xbox 360, base version (default.xex v1)"
  std::vector<GameFile> files;
  uint64_t total = 0;
  // From the list's '@' lines (0 / empty when absent): "@title <title ID> <media ID> <version>" (hex) and
  // "@retail-xex <sha256> <size>" (the original encrypted retail default.xex, recognised by its hash).
  uint32_t title_id = 0, media_id = 0, version = 0;
  std::string retail_xex_sha256;
  uint64_t retail_xex_size = 0;
  const GameFile* Find(std::string_view path) const;
};
// Parses the reference list (tab-separated: sha256, size, path; '#' comment lines; '@' lines as above). Empty on a
// malformed list.
GameManifest ParseGameManifest(std::string_view text, std::string name);

// A default.xex looked at without any key: its format, identity and, when it is unencrypted, its memory image's
// SHA-256.
enum class XexStatus { kUnreadable, kNotXex, kEncrypted, kUnsupportedCompression, kOk };
struct XexInfo {
  XexStatus status = XexStatus::kUnreadable;
  std::string image_sha256;  // only for kOk
  std::string file_sha256;   // of the whole file, whenever it was read
  uint16_t encryption = 0, compression = 0;
  uint32_t module_flags = 0;
  bool has_execution_info = false;
  uint32_t title_id = 0, media_id = 0, version = 0, base_version = 0;
  uint8_t disc_number = 0, disc_count = 0;
};
constexpr uint64_t kMaxXexBytes = uint64_t(256) << 20;
XexInfo InspectXexData(std::string_view data);
XexInfo InspectXex(const fs::path& path);
// "1.0.1234.5" (major.minor.build.qfe, as the console packs it).
std::string XexVersionString(uint32_t v);

// What an executable is, for this setup (docs/RETAIL_IMPORT_PLAN.md A1), in the order it is decided.
enum class Executable {
  kMissing,
  kUnreadable,
  kNotXex,
  kOtherGame,               // another title ID
  kTitleUpdate,             // a patch module (default.xexp)
  kOtherVersion,            // this game, another version or release (media ID)
  kRetailEncrypted,         // the supported retail file, encrypted (recognised by its SHA-256)
  kEncrypted,               // encrypted, not the known retail file
  kUnsupportedCompression,  // LZX
  kModified,                // unencrypted, the right version, a different image
  kDecryptedMismatch,       // decrypted by the setup, but not the supported image
  kSupported,
};
Executable JudgeExecutable(const XexInfo& info, const GameManifest& manifest);
// One or two plain-text sentences for the player ("" for kSupported).
std::string ExecutableMessage(Executable e, const XexInfo& info, const GameManifest& manifest);
// True when the dump is usable with a separate unencrypted default.xex (its own one is encrypted).
inline bool NeedsSeparateExecutable(Executable e) {
  return e == Executable::kRetailEncrypted || e == Executable::kEncrypted;
}

// Where the game files come from: a folder (an extracted dump) or an Xbox 360 disc image. Paths of the reference list
// are found exactly, else ignoring ASCII case (dump tools differ: "Default.xex").
class GameSource {
 public:
  enum class Kind { kNone, kFolder, kIso };
  struct Entry {
    bool found = false;
    fs::path file;        // the file (a folder's) or the image (an ISO's)
    uint64_t offset = 0;  // in `file`
    uint64_t size = 0;
    std::string path;     // as found in the source
  };
  // A folder as given (no search), or an ISO file.
  static GameSource Folder(const fs::path& folder);
  static GameSource Iso(const fs::path& image);
  // What the player picked: an ISO file, else FindGameRoot(picked) (a picked default.xex stands for its folder).
  static GameSource Open(const fs::path& picked);

  Kind kind() const { return kind_; }
  const fs::path& path() const { return path_; }
  bool ok() const { return ok_; }                  // a readable folder, or a valid image
  const std::string& error() const { return error_; }  // why an image was refused
  Entry Find(std::string_view rel) const;
  bool HasRetailLayout() const;                    // the console's $SystemUpdate folder is present
  // The entry's bytes (false if larger than `max` or unreadable).
  bool Read(const Entry& e, std::string* out, uint64_t max) const;
  // Copies the entry to `to` (through `to`.part) and returns the SHA-256 of what was copied ("" on failure).
  std::string Copy(const Entry& e, const fs::path& to, const std::function<void(uint64_t)>& progress,
                   const std::function<bool()>& cancelled) const;

 private:
  Kind kind_ = Kind::kNone;
  fs::path path_;
  bool ok_ = false;
  std::string error_;
  std::vector<IsoFile> iso_;
};

// The quick check made when the game files are chosen: every file present with its size, and the executable's
// verdict (the executable decides the version; the rest is hashed while it is copied). `executable`, when given, is
// a separate unencrypted default.xex used instead of the dump's own.
struct GameCheck {
  bool folder_ok = false;  // a readable folder or a valid disc image
  GameSource::Kind kind = GameSource::Kind::kNone;
  std::string source_error;   // a refused disc image: why
  bool retail_layout = false; // a console disc dump ($SystemUpdate)
  bool executable_found = false;
  XexInfo dump_xex;
  Executable dump_verdict = Executable::kMissing;
  bool separate = false;  // a separate executable was given
  XexInfo separate_xex;
  Executable separate_verdict = Executable::kMissing;
  Executable verdict = Executable::kMissing;  // the one used: the separate executable's, else the dump's
  bool decrypt = false;  // the dump's encrypted default.xex is decrypted by the setup into the installation
  bool executable_encrypted = false;  // the dump's default.xex is encrypted and no usable separate one is given
  bool executable_known = false;      // the executable used is the supported version
  std::vector<std::string> missing;
  std::vector<std::string> wrong_size;
  uint64_t bytes = 0;  // of the files found with their sizes
  bool Ok() const { return folder_ok && executable_known && missing.empty() && wrong_size.empty(); }
};
GameCheck CheckGame(const GameSource& source, const GameManifest& manifest, const fs::path& executable = {});
GameCheck CheckGameFolder(const fs::path& folder, const GameManifest& manifest);
// Why the check fails, for the player ("" when it passes): the source, then the executable, then the files.
std::string GameCheckProblem(const GameCheck& c, const GameManifest& manifest);

// The folder a player picked may be one level above the game (a disc dump's parent): the folder itself, else its
// only subfolder that holds default.xex. A picked default.xex gives its folder; another file (a disc image) is
// returned unchanged.
fs::path FindGameRoot(const fs::path& picked);

}  // namespace kuf::setup
