#include "core/localbuild.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string_view>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

#include "core/game_data.h"
#include "core/platform.h"

namespace kuf::setup {

namespace {

using Clock = std::chrono::steady_clock;
double Since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// builder/recipe.txt, written by tools/package/make_builder.py: "<key> <value>" lines; repeated keys in order.
struct Recipe {
  std::map<std::string, std::vector<std::string>> values;
  const std::vector<std::string>& All(const std::string& key) const {
    static const std::vector<std::string> none;
    const auto it = values.find(key);
    return it == values.end() ? none : it->second;
  }
  std::string One(const std::string& key) const { return All(key).empty() ? "" : All(key).front(); }
};

bool ReadRecipe(const fs::path& path, Recipe* recipe) {
  std::string text;
  if (!ReadFile(path, &text)) return false;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();  // a list with Windows line ends
    if (line.empty() || line[0] == '#') continue;
    const size_t space = line.find(' ');
    if (space == std::string::npos) return false;
    recipe->values[line.substr(0, space)].push_back(line.substr(space + 1));
  }
  return !recipe->One("generation_id").empty() && !recipe->All("compile").empty() && !recipe->All("link").empty();
}

#if defined(_WIN32)
constexpr const char* kExe = ".exe";
#else
constexpr const char* kExe = "";
#endif

std::string U(const fs::path& p) { return PathUtf8(p); }

struct Child {
  ChildProcess process;
  std::string name;
};

void KillAll(std::vector<Child>& running) {
  for (Child& c : running) KillProcess(&c.process);
  running.clear();
}

// Runs one program to its end; 0, its exit status, or -1 when cancelled.
int RunOne(const std::vector<std::string>& argv, const fs::path& cwd, const fs::path& log,
           const std::vector<std::string>& env, const std::atomic<bool>& cancel) {
  ChildProcess child;
  if (!StartProcess(argv, cwd, log, env, &child)) return 1;
  for (;;) {
    int code = 0;
    bool oom = false;
    if (PollProcess(&child, &code, &oom)) return code;
    if (cancel) {
      KillProcess(&child);
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

// The last lines of the build log, for the error shown to the player.
std::string LogTail(const fs::path& log) {
  std::string text;
  if (!ReadFile(log, &text)) return "";
  size_t pos = text.size();
  for (int lines = 0; lines < 4 && pos != std::string::npos && pos > 0; ++lines) pos = text.rfind('\n', pos - 1);
  return pos == std::string::npos ? text : text.substr(pos + 1);
}

}  // namespace

int DefaultBuildJobs(unsigned processors, uint64_t available_memory) {
  const uint64_t by_memory = available_memory / kBuildJobMemory;
  return int(std::clamp<uint64_t>(std::min<uint64_t>(processors ? processors : 1, by_memory), 1, 64));
}

std::string ExpandRecipe(std::string text, const fs::path& builder, const fs::path& work) {
  // Forward slashes on every system: the compiler reads them on Windows too, and a backslash is an escape character
  // in its response files.
  const std::string b = PathUtf8(builder.generic_u8string()), w = PathUtf8(work.generic_u8string());
  for (const auto& [from, to] : {std::pair<std::string, std::string>{"@B@", b}, {"@W@", w}}) {
    for (size_t pos = 0; (pos = text.find(from, pos)) != std::string::npos; pos += to.size()) {
      text.replace(pos, from.size(), to);
    }
  }
  return text;
}

namespace {

// One build. *stale_cache: it linked from objects an earlier build had finished (the cache's stamp) and the link
// found them missing or unusable (a disk that lost files, a crash that damaged them, a broken copy of the folder).
bool LocalBuildOnce(const BuildRequest& request, const BuildProgress& progress, const std::atomic<bool>& cancel,
                    std::string* error, BuildStats* stats, bool* stale_cache) {
  std::error_code ec;
  const fs::path builder = fs::absolute(request.package / "builder", ec);
  Recipe recipe;
  if (!ReadRecipe(builder / "recipe.txt", &recipe)) {
    *error = "The package's build recipe is damaged (builder/recipe.txt).";
    return false;
  }
  const XexInfo xex = InspectXex(request.xex);
  if (xex.status != XexStatus::kOk) {
    *error = "The game's default.xex cannot be used for the build (" + U(request.xex) + ").";
    return false;
  }
  // What the objects depend on: the package's generator, compiler, flags and headers, and the player's game version.
  const std::string id = recipe.One("generation_id").substr(0, 16) + "-" + xex.image_sha256.substr(0, 16);
  const fs::path slot = fs::absolute(request.cache, ec) / id;
  const fs::path work = slot / "work", objects = slot / "obj", log = fs::absolute(request.cache, ec) / "build.log";
  fs::create_directories(objects, ec);
  if (ec) {
    *error = "Could not create the build folder " + U(objects) + ".";
    return false;
  }
  // A cache of another generation can never be used again.
  for (const auto& e : fs::directory_iterator(request.cache, ec)) {
    if (e.is_directory(ec) && e.path().filename() != id) fs::remove_all(e.path(), ec);
  }
  std::ofstream(log, std::ios::binary | std::ios::trunc) << "build " << id << "\n";
  BuildStats local;
  BuildStats& s = stats ? *stats : local;
  s.generation_id = id;
  const auto cancelled = [&] {
    *error = "Installation cancelled.";
    return false;
  };
  const auto failed = [&](const std::string& what) {
    *error = what + " The details are in " + U(log) + ".\n" + LogTail(log);
    return false;
  };

#if defined(_WIN32)
  const std::vector<std::string> env;  // the programs find their DLLs beside them
#else
  const std::vector<std::string> env = {
      "LD_LIBRARY_PATH=" + U(request.package / "bin") + ":" + U(request.package / "bin" / "compat")};
#endif

  // The compiler: the recipe's (Windows: llvm-mingw's clang, which finds its linker beside it), else the Linux
  // compiler, one program that acts by the name it is started under (clang++, ld.lld).
  const fs::path tools = work / "tools";
  std::string compiler;
  std::vector<std::string> link_tool_flags;
  if (!recipe.One("compiler").empty()) {
    compiler = ExpandRecipe(recipe.One("compiler"), builder, work);
  } else {
    fs::create_directories(tools, ec);
    for (const char* name : {"clang++", "ld.lld"}) {
      fs::remove(tools / name, ec);
      fs::create_symlink(builder / "toolchain" / "llvm", tools / name, ec);
      if (ec) {
        *error = "Could not prepare the compiler in " + U(tools) + ".";
        return false;
      }
    }
    compiler = U(tools / "clang++");
    link_tool_flags.push_back("--ld-path=" + U(tools / "ld.lld"));
  }
  const auto expand = [&](const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string arg;
    while (in >> arg) out.push_back(ExpandRecipe(arg, builder, work));
    return out;
  };

  // 1. Code generation (skipped when every object is already in the cache: a finished generation has a stamp).
  const fs::path generated = work / "generated" / "default";
  const fs::path done_stamp = slot / "objects.done";
  const bool have_objects = fs::exists(done_stamp, ec);
  auto start = Clock::now();
  if (!have_objects && !fs::exists(work / "generated.done", ec)) {
    if (progress) progress(BuildPhase::kGenerate, 0, "Reading the game's code");
    fs::remove_all(work / "generated", ec);
    fs::remove_all(work / "config", ec);
    fs::create_directories(work / "build" / "input" / "v1", ec);
    fs::copy(builder / "config", work / "config", fs::copy_options::recursive, ec);
    fs::copy_file(builder / "kuf_cod_manifest.toml", work / "kuf_cod_manifest.toml",
                  fs::copy_options::overwrite_existing, ec);
    // Removed first: a leftover from an interrupted build may be read-only.
    const fs::path staged = work / "build" / "input" / "v1" / "default.xex";
    fs::remove(staged, ec);
    fs::copy_file(request.xex, staged, ec);
    if (!ec) fs::permissions(staged, fs::perms(0644), ec);
    if (ec) {
      *error = "Could not prepare the code generator's folder.";
      return false;
    }
    const int rc = RunOne({U(request.package / "bin" / (std::string("rexgluerd") + kExe)), "codegen",
                           "kuf_cod_manifest.toml"}, work, log, env, cancel);
    if (rc < 0) return cancelled();
    if (rc != 0) return failed("The code generator failed.");
    std::ofstream(work / "generated.done", std::ios::binary) << "ok\n";
  }
  s.generate_seconds = Since(start);
  if (progress) progress(BuildPhase::kGenerate, 1, "");

  // 2. Compile: the precompiled header, then every generated file not yet in the cache.
  start = Clock::now();
  std::vector<fs::path> sources;
  if (!have_objects) {
    for (const auto& e : fs::directory_iterator(generated, ec)) {
      if (e.path().extension() == ".cpp") sources.push_back(e.path());
    }
    std::sort(sources.begin(), sources.end());
    const size_t expected = size_t(std::atol(recipe.One("sources").c_str()));
    if (sources.empty() || (expected != 0 && sources.size() != expected)) {
      return failed("The code generator wrote " + std::to_string(sources.size()) + " files, not the expected " +
                    std::to_string(expected) + ".");
    }
    const std::vector<std::string> flags = expand(recipe.One("compile"));
    const fs::path pch = work / "pch.hxx.pch";
    if (progress) progress(BuildPhase::kCompile, 0, "Preparing the compiler");
    // The precompiled header records its headers' absolute paths: it is only good for the builder folder it was made
    // from, and an interrupted installation continues from another staging folder.
    std::string pch_builder;
    ReadFile(work / "pch.done", &pch_builder);
    if (pch_builder != U(builder)) {
      std::vector<std::string> argv = {compiler};
      for (const std::string& a : expand(recipe.One("pch"))) argv.push_back(a);
      for (const std::string& a : flags) argv.push_back(a);
      argv.insert(argv.end(), {"-Xclang", "-emit-pch", "-x", "c++-header", "-o", U(pch), "-c",
                               U(generated / "kuf_cod_pch.h")});
      const int rc = RunOne(argv, work, log, env, cancel);
      if (rc < 0) return cancelled();
      if (rc != 0) return failed("Preparing the compiler failed.");
      std::ofstream(work / "pch.done", std::ios::binary) << U(builder);
    }

    std::vector<fs::path> todo;
    for (const fs::path& source : sources) {
      if (fs::exists(objects / (source.filename().string() + ".o"), ec)) {
        ++s.reused;
      } else {
        todo.push_back(source);
      }
    }
    int jobs = request.jobs > 0 ? request.jobs : DefaultBuildJobs(std::thread::hardware_concurrency(), AvailableMemory());
    s.jobs = jobs;
    size_t next = 0, finished = 0;
    std::vector<Child> running;

    int killed_retries = 0;
    while (next < todo.size() || !running.empty()) {
      if (cancel) {
        KillAll(running);
        return cancelled();
      }
      while (next < todo.size() && int(running.size()) < jobs) {
        const fs::path& source = todo[next++];
        const fs::path out = objects / (source.filename().string() + ".o");
        std::vector<std::string> argv = {compiler};
        for (const std::string& a : flags) argv.push_back(a);
        argv.insert(argv.end(), {"-Xclang", "-include-pch", "-Xclang", U(pch), "-c", U(source), "-o",
                                 U(out) + ".part"});
        Child c;
        c.name = U(source);
        if (!StartProcess(argv, work, log, env, &c.process)) {
          KillAll(running);
          return failed("The compiler could not be started.");
        }
        running.push_back(std::move(c));
      }
      // Only its own children: the setup may have others.
      int code = 0;
      bool oom = false;
      auto it = running.begin();
      for (; it != running.end(); ++it) {
        if (PollProcess(&it->process, &code, &oom)) break;
      }
      if (it == running.end()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      const fs::path source = PathFromUtf8(it->name);
      running.erase(it);
      const fs::path out = objects / (source.filename().string() + ".o");
      if (oom && killed_retries < 3) {
        // The system ran out of memory: again, with fewer at once.
        ++killed_retries;
        jobs = std::max(1, jobs / 2);
        s.jobs = jobs;
        fs::remove(PathFromUtf8(U(out) + ".part"), ec);
        todo.push_back(source);
        continue;
      }
      if (code != 0) {
        KillAll(running);
        return failed("Compiling " + source.filename().string() + " failed.");
      }
      fs::rename(PathFromUtf8(U(out) + ".part"), out, ec);
      if (ec) {
        KillAll(running);
        return failed("Could not store " + U(out) + ".");
      }
      ++finished;
      ++s.compiled;
      if (progress) {
        progress(BuildPhase::kCompile, double(finished + s.reused) / double(todo.size() + s.reused),
                 source.filename().string());
      }
    }
    // The list of objects, in order, for the link and for later links without the generated files. Binary: in text
    // mode Windows ends the lines with "\r\n" (the first Windows run read back "kuf_cod_init.cpp.o\r").
    std::ofstream list(slot / "objects.txt.part", std::ios::binary | std::ios::trunc);
    for (const fs::path& source : sources) list << source.filename().string() << ".o\n";
    list.close();
    fs::rename(slot / "objects.txt.part", slot / "objects.txt", ec);
    std::ofstream(done_stamp, std::ios::binary) << "ok\n";
  } else {
    std::ifstream listed(slot / "objects.txt", std::ios::binary);
    s.reused = int(std::count(std::istreambuf_iterator<char>(listed.rdbuf()), std::istreambuf_iterator<char>(), '\n'));
  }
  s.compile_seconds = Since(start);
  if (progress) progress(BuildPhase::kCompile, 1, "");

  // 3. Link, through a temporary name.
  start = Clock::now();
  if (progress) progress(BuildPhase::kLink, 0, "Linking the game");
  std::string list_text;
  if (!ReadFile(slot / "objects.txt", &list_text)) return failed("The object list is missing.");
  std::ofstream rsp(work / "objects.rsp", std::ios::binary | std::ios::trunc);
  std::istringstream names(list_text);
  std::string name;
  while (std::getline(names, name)) {
    if (!name.empty() && name.back() == '\r') name.pop_back();  // a list written with Windows line ends
    if (name.empty()) continue;
    if (!fs::exists(objects / name, ec)) {
      *stale_cache = have_objects;
      return failed("The object " + name + " is missing from the cache.");
    }
    rsp << '"' << PathUtf8((objects / name).generic_u8string()) << "\"\n";
  }
  rsp.close();
  fs::create_directories(request.output.parent_path(), ec);
  const fs::path output = fs::absolute(request.output, ec);
  fs::path temp = output;
  temp += ".part";
  std::vector<std::string> argv = {compiler};
  for (const std::string& a : link_tool_flags) argv.push_back(a);
  for (const std::string& a : expand(recipe.One("link"))) argv.push_back(a);
  argv.insert(argv.end(), {"@" + PathUtf8((work / "objects.rsp").generic_u8string()), "-o", U(temp),
                           "-L" + U(fs::absolute(request.package / "bin", ec))});
  for (const std::string& a : expand(recipe.One("link_libraries"))) argv.push_back(a);
  const int rc = RunOne(argv, work, log, env, cancel);
  if (rc < 0) {
    fs::remove(temp, ec);
    return cancelled();
  }
  if (rc != 0) {
    fs::remove(temp, ec);
    *stale_cache = have_objects;
    return failed("Linking the game failed.");
  }
  fs::permissions(temp, fs::perms(0755), ec);
  fs::rename(temp, output, ec);
  if (ec) return failed("Could not write " + U(output) + ".");
  s.link_seconds = Since(start);

  // The generated C++ and the temporary files are no longer needed; the objects stay for updates.
  fs::remove_all(work, ec);
  if (progress) progress(BuildPhase::kLink, 1, "");
  return true;
}

}  // namespace

bool LocalBuild(const BuildRequest& request, const BuildProgress& progress, const std::atomic<bool>& cancel,
                std::string* error, BuildStats* stats) {
  bool stale = false;
  if (LocalBuildOnce(request, progress, cancel, error, stats, &stale)) return true;
  if (!stale || cancel) return false;
  // The cached objects were unusable: they are built again once, from nothing (a failure then is a real one).
  std::error_code ec;
  const std::string first_error = *error;
  for (const auto& e : fs::directory_iterator(fs::absolute(request.cache, ec), ec)) {
    if (e.is_directory(ec)) fs::remove_all(e.path(), ec);
  }
  bool again = false;
  error->clear();
  if (LocalBuildOnce(request, progress, cancel, error, stats, &again)) return true;
  *error = "The game could not be prepared, also after building it again from nothing. " + *error +
           "\n(The first attempt, from the cached objects: " + first_error.substr(0, first_error.find('\n')) + ")";
  return false;
}

}  // namespace kuf::setup
