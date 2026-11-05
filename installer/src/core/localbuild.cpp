#include "core/localbuild.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string_view>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

#include "core/game_data.h"

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
    if (line.empty() || line[0] == '#') continue;
    const size_t space = line.find(' ');
    if (space == std::string::npos) return false;
    recipe->values[line.substr(0, space)].push_back(line.substr(space + 1));
  }
  return !recipe->One("generation_id").empty() && !recipe->All("compile").empty() && !recipe->All("link").empty();
}

struct Child {
  pid_t pid = -1;
  std::string name;
};

// Starts a program in its own process group, at low priority, its output appended to `log`. Everything the child uses
// is made before the fork (the setup has threads).
pid_t Spawn(const std::vector<std::string>& argv, const fs::path& cwd, const fs::path& log,
            const std::vector<std::string>& env) {
  std::vector<char*> args;
  for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  std::vector<char*> envp;
  for (const std::string& e : env) envp.push_back(const_cast<char*>(e.c_str()));
  for (char** e = environ; *e; ++e) {
    const bool replaced = std::any_of(env.begin(), env.end(), [&](const std::string& mine) {
      return std::string_view(*e).substr(0, mine.find('=') + 1) == std::string_view(mine).substr(0, mine.find('=') + 1);
    });
    if (!replaced) envp.push_back(*e);
  }
  envp.push_back(nullptr);
  const std::string program = argv[0], where = cwd.string(), log_path = log.string();
  const pid_t pid = fork();
  if (pid != 0) return pid;
  setpgid(0, 0);
  prctl(PR_SET_PDEATHSIG, SIGKILL);  // it does not outlive a killed setup
  setpriority(PRIO_PROCESS, 0, 5);  // the desktop stays responsive
  const int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd >= 0) {
    dup2(fd, 1);
    dup2(fd, 2);
  }
  if (!where.empty() && chdir(where.c_str()) != 0) _exit(126);
  execve(program.c_str(), args.data(), envp.data());
  _exit(127);
}

void KillAll(std::vector<Child>& running) {
  for (const Child& c : running) kill(-c.pid, SIGKILL);
  for (const Child& c : running) waitpid(c.pid, nullptr, 0);
  running.clear();
}

// Runs one program to its end; 0, its exit status, or -1 when cancelled.
int RunOne(const std::vector<std::string>& argv, const fs::path& cwd, const fs::path& log,
           const std::vector<std::string>& env, const std::atomic<bool>& cancel) {
  const pid_t pid = Spawn(argv, cwd, log, env);
  if (pid < 0) return 1;
  for (;;) {
    int status = 0;
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (r < 0) return 1;
    if (cancel) {
      kill(-pid, SIGKILL);
      waitpid(pid, nullptr, 0);
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

uint64_t AvailableMemory() {
  std::ifstream in("/proc/meminfo");
  std::string key;
  uint64_t kib = 0;
  std::string unit;
  while (in >> key >> kib) {
    std::getline(in, unit);
    if (key == "MemAvailable:") return kib << 10;
  }
  return uint64_t(4) << 30;
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
  for (const auto& [from, to] : {std::pair<std::string, std::string>{"@B@", builder.string()}, {"@W@", work.string()}}) {
    for (size_t pos = 0; (pos = text.find(from, pos)) != std::string::npos; pos += to.size()) {
      text.replace(pos, from.size(), to);
    }
  }
  return text;
}

bool LocalBuild(const BuildRequest& request, const BuildProgress& progress, const std::atomic<bool>& cancel,
                std::string* error, BuildStats* stats) {
  std::error_code ec;
  const fs::path builder = fs::absolute(request.package / "builder", ec);
  Recipe recipe;
  if (!ReadRecipe(builder / "recipe.txt", &recipe)) {
    *error = "The package's build recipe is damaged (builder/recipe.txt).";
    return false;
  }
  const XexInfo xex = InspectXex(request.xex);
  if (xex.status != XexStatus::kOk) {
    *error = "The game's default.xex cannot be used for the build (" + request.xex.string() + ").";
    return false;
  }
  // What the objects depend on: the package's generator, compiler, flags and headers, and the player's game version.
  const std::string id = recipe.One("generation_id").substr(0, 16) + "-" + xex.image_sha256.substr(0, 16);
  const fs::path slot = fs::absolute(request.cache, ec) / id;
  const fs::path work = slot / "work", objects = slot / "obj", log = fs::absolute(request.cache, ec) / "build.log";
  fs::create_directories(objects, ec);
  if (ec) {
    *error = "Could not create the build folder " + objects.string() + ".";
    return false;
  }
  // A cache of another generation can never be used again.
  for (const auto& e : fs::directory_iterator(request.cache, ec)) {
    if (e.is_directory(ec) && e.path().filename() != id) fs::remove_all(e.path(), ec);
  }
  std::ofstream(log, std::ios::trunc) << "build " << id << "\n";
  BuildStats local;
  BuildStats& s = stats ? *stats : local;
  s.generation_id = id;
  const auto cancelled = [&] {
    *error = "Installation cancelled.";
    return false;
  };
  const auto failed = [&](const std::string& what) {
    *error = what + " The details are in " + log.string() + ".\n" + LogTail(log);
    return false;
  };

  const std::vector<std::string> env = {
      "LD_LIBRARY_PATH=" + (request.package / "bin").string() + ":" + (request.package / "bin" / "compat").string()};

  // The tools by name (the compiler is one program that acts by the name it is started under).
  const fs::path tools = work / "tools";
  fs::create_directories(tools, ec);
  for (const char* name : {"clang++", "ld.lld"}) {
    fs::remove(tools / name, ec);
    fs::create_symlink(builder / "toolchain" / "llvm", tools / name, ec);
    if (ec) {
      *error = "Could not prepare the compiler in " + tools.string() + ".";
      return false;
    }
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
    const int rc = RunOne({(request.package / "bin" / "rexgluerd").string(), "codegen", "kuf_cod_manifest.toml"}, work,
                          log, env, cancel);
    if (rc < 0) return cancelled();
    if (rc != 0) return failed("The code generator failed.");
    std::ofstream(work / "generated.done") << "ok\n";
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
    if (pch_builder != builder.string()) {
      std::vector<std::string> argv = {(tools / "clang++").string()};
      for (const std::string& a : expand(recipe.One("pch"))) argv.push_back(a);
      for (const std::string& a : flags) argv.push_back(a);
      argv.insert(argv.end(), {"-Xclang", "-emit-pch", "-x", "c++-header", "-o", pch.string(), "-c",
                               (generated / "kuf_cod_pch.h").string()});
      const int rc = RunOne(argv, work, log, env, cancel);
      if (rc < 0) return cancelled();
      if (rc != 0) return failed("Preparing the compiler failed.");
      std::ofstream(work / "pch.done") << builder.string();
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
    std::map<pid_t, fs::path> sources_of;
    int killed_retries = 0;
    while (next < todo.size() || !running.empty()) {
      if (cancel) {
        KillAll(running);
        return cancelled();
      }
      while (next < todo.size() && int(running.size()) < jobs) {
        const fs::path& source = todo[next++];
        const fs::path out = objects / (source.filename().string() + ".o");
        std::vector<std::string> argv = {(tools / "clang++").string()};
        for (const std::string& a : flags) argv.push_back(a);
        argv.insert(argv.end(), {"-Xclang", "-include-pch", "-Xclang", pch.string(), "-c", source.string(), "-o",
                                 out.string() + ".part"});
        const pid_t pid = Spawn(argv, work, log, env);
        if (pid < 0) {
          KillAll(running);
          return failed("The compiler could not be started.");
        }
        running.push_back({pid, source.filename().string()});
        sources_of[pid] = source;
      }
      // Only its own children: the setup may have others.
      int status = 0;
      pid_t r = 0;
      auto it = running.begin();
      for (; it != running.end(); ++it) {
        r = waitpid(it->pid, &status, WNOHANG);
        if (r == it->pid) break;
      }
      if (it == running.end()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      const fs::path source = sources_of[r];
      running.erase(it);
      sources_of.erase(r);
      const fs::path out = objects / (source.filename().string() + ".o");
      if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL && killed_retries < 3) {
        // The system ran out of memory: again, with fewer at once.
        ++killed_retries;
        jobs = std::max(1, jobs / 2);
        s.jobs = jobs;
        fs::remove(out.string() + ".part", ec);
        todo.push_back(source);
        continue;
      }
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        KillAll(running);
        return failed("Compiling " + source.filename().string() + " failed.");
      }
      fs::rename(out.string() + ".part", out, ec);
      if (ec) {
        KillAll(running);
        return failed("Could not store " + out.string() + ".");
      }
      ++finished;
      ++s.compiled;
      if (progress) {
        progress(BuildPhase::kCompile, double(finished + s.reused) / double(todo.size() + s.reused),
                 source.filename().string());
      }
    }
    // The list of objects, in order, for the link and for later links without the generated files.
    std::ofstream list(slot / "objects.txt.part", std::ios::trunc);
    for (const fs::path& source : sources) list << source.filename().string() << ".o\n";
    list.close();
    fs::rename(slot / "objects.txt.part", slot / "objects.txt", ec);
    std::ofstream(done_stamp) << "ok\n";
  } else {
    s.reused = int(std::count(std::istreambuf_iterator<char>(std::ifstream(slot / "objects.txt").rdbuf()),
                              std::istreambuf_iterator<char>(), '\n'));
  }
  s.compile_seconds = Since(start);
  if (progress) progress(BuildPhase::kCompile, 1, "");

  // 3. Link, through a temporary name.
  start = Clock::now();
  if (progress) progress(BuildPhase::kLink, 0, "Linking the game");
  std::string list_text;
  if (!ReadFile(slot / "objects.txt", &list_text)) return failed("The object list is missing.");
  std::ofstream rsp(work / "objects.rsp", std::ios::trunc);
  std::istringstream names(list_text);
  std::string name;
  while (std::getline(names, name)) {
    if (!fs::exists(objects / name, ec)) return failed("The object " + name + " is missing from the cache.");
    rsp << '"' << (objects / name).string() << "\"\n";
  }
  rsp.close();
  fs::create_directories(request.output.parent_path(), ec);
  const fs::path output = fs::absolute(request.output, ec);
  const fs::path temp = output.string() + ".part";
  std::vector<std::string> argv = {(tools / "clang++").string(), "--ld-path=" + (tools / "ld.lld").string()};
  for (const std::string& a : expand(recipe.One("link"))) argv.push_back(a);
  argv.insert(argv.end(), {"@" + (work / "objects.rsp").string(), "-o", temp.string(),
                           "-L" + fs::absolute(request.package / "bin", ec).string()});
  for (const std::string& a : expand(recipe.One("link_libraries"))) argv.push_back(a);
  const int rc = RunOne(argv, work, log, env, cancel);
  if (rc < 0) {
    fs::remove(temp, ec);
    return cancelled();
  }
  if (rc != 0) {
    fs::remove(temp, ec);
    return failed("Linking the game failed.");
  }
  fs::permissions(temp, fs::perms(0755), ec);
  fs::rename(temp, output, ec);
  if (ec) return failed("Could not write " + output.string() + ".");
  s.link_seconds = Since(start);

  // The generated C++ and the temporary files are no longer needed; the objects stay for updates.
  fs::remove_all(work, ec);
  if (progress) progress(BuildPhase::kLink, 1, "");
  return true;
}

}  // namespace kuf::setup
