#include "core/system_check.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include "core/util.h"

namespace kuf::setup {

namespace {

const char* DeviceType(VkPhysicalDeviceType t) {
  switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU (software)";
    default: return "other";
  }
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
  return std::any_of(list.begin(), list.end(),
                     [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

// The check itself, in this process.
VulkanReport Query() {
  VulkanReport r;
  if (!Env("KUF_SETUP_PRETEND_NO_VULKAN").empty()) {
    r.error = "no Vulkan driver was found (libvulkan.so.1 could not be loaded)";
    return r;
  }
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    r.error = "no Vulkan driver was found (libvulkan.so.1 could not be loaded)";
    return r;
  }
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
  if (!gipa) {
    r.error = "the Vulkan loader is damaged (no vkGetInstanceProcAddr)";
    return r;
  }
#define KUF_GLOBAL(name) auto name = reinterpret_cast<PFN_##name>(gipa(VK_NULL_HANDLE, #name))
  KUF_GLOBAL(vkCreateInstance);
  KUF_GLOBAL(vkEnumerateInstanceExtensionProperties);
  KUF_GLOBAL(vkEnumerateInstanceVersion);  // Vulkan 1.1 loaders
#undef KUF_GLOBAL
  if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties) {
    r.error = "the Vulkan loader is damaged (no vkCreateInstance)";
    return r;
  }
  uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> instance_ext(count);
  if (count) vkEnumerateInstanceExtensionProperties(nullptr, &count, instance_ext.data());
  r.surface = HasExtension(instance_ext, "VK_KHR_surface") &&
              (HasExtension(instance_ext, "VK_KHR_xcb_surface") || HasExtension(instance_ext, "VK_KHR_wayland_surface"));
  uint32_t loader_version = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loader_version);

  VkApplicationInfo app = {};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "kuf-setup";
  // As the game: 1.1 when the loader has it ("Vulkan 1.0 implementations were required to return
  // VK_ERROR_INCOMPATIBLE_DRIVER if apiVersion was larger than 1.0").
  app.apiVersion = loader_version >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
  VkInstanceCreateInfo ici = {};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  const VkResult created = vkCreateInstance(&ici, nullptr, &instance);
  if (created != VK_SUCCESS) {
    r.error = created == VK_ERROR_INCOMPATIBLE_DRIVER
                  ? "no Vulkan driver is installed for this graphics card (VK_ERROR_INCOMPATIBLE_DRIVER)"
                  : "the Vulkan driver could not be started (vkCreateInstance error " + std::to_string(created) + ")";
    return r;
  }
#define KUF_INSTANCE(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
  KUF_INSTANCE(vkDestroyInstance);
  KUF_INSTANCE(vkEnumeratePhysicalDevices);
  KUF_INSTANCE(vkGetPhysicalDeviceProperties);
  KUF_INSTANCE(vkGetPhysicalDeviceFeatures);
  KUF_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
  KUF_INSTANCE(vkEnumerateDeviceExtensionProperties);
#undef KUF_INSTANCE
  if (!vkDestroyInstance || !vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties ||
      !vkGetPhysicalDeviceFeatures || !vkGetPhysicalDeviceQueueFamilyProperties ||
      !vkEnumerateDeviceExtensionProperties) {
    r.error = "the Vulkan driver is missing core functions";
    return r;
  }
  count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> physical(count);
  if (count) vkEnumeratePhysicalDevices(instance, &count, physical.data());
  for (VkPhysicalDevice pd : physical) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(pd, &f);
    VulkanDevice d;
    d.name = props.deviceName;
    d.type = DeviceType(props.deviceType);
    d.api_version = props.apiVersion;
    d.vendor_id = props.vendorID;
    if (!f.independentBlend) d.missing.push_back("independentBlend");
    if (!f.fragmentStoresAndAtomics) d.missing.push_back("fragmentStoresAndAtomics");
    if (!f.vertexPipelineStoresAndAtomics) d.missing.push_back("vertexPipelineStoresAndAtomics");
    if (!f.geometryShader) d.missing.push_back("geometryShader");
    if (!f.fillModeNonSolid) d.missing.push_back("fillModeNonSolid");
    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(qcount);
    if (qcount) vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, queues.data());
    const VkQueueFlags gc = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    if (std::none_of(queues.begin(), queues.end(), [&](const VkQueueFamilyProperties& q) {
          return (q.queueFlags & gc) == gc;
        })) {
      d.missing.push_back("a graphics and compute queue");
    }
    uint32_t ecount = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ecount, nullptr);
    std::vector<VkExtensionProperties> ext(ecount);
    if (ecount) vkEnumerateDeviceExtensionProperties(pd, nullptr, &ecount, ext.data());
    if (!HasExtension(ext, "VK_KHR_swapchain")) d.missing.push_back("VK_KHR_swapchain");
    r.devices.push_back(std::move(d));
  }
  vkDestroyInstance(instance, nullptr);
  // The loader stays loaded: unloading drivers at exit is where some of them crash.
  return r;
}

// One line per field, tab-separated, for the child process's answer.
std::string Serialize(const VulkanReport& r) {
  std::ostringstream o;
  o << "error\t" << r.error << "\nsurface\t" << (r.surface ? 1 : 0) << "\n";
  for (const auto& d : r.devices) {
    o << "device\t" << d.name << "\t" << d.type << "\t" << d.api_version << "\t" << d.vendor_id << "\t";
    for (size_t i = 0; i < d.missing.size(); ++i) o << (i ? "," : "") << d.missing[i];
    o << "\n";
  }
  return o.str();
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t at; (at = s.find(sep, start)) != std::string::npos; start = at + 1) out.push_back(s.substr(start, at - start));
  out.push_back(s.substr(start));
  return out;
}

VulkanReport Deserialize(const std::string& text) {
  VulkanReport r;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const auto f = Split(line, '\t');
    if (f[0] == "error" && f.size() >= 2) r.error = f[1];
    if (f[0] == "surface" && f.size() >= 2) r.surface = f[1] == "1";
    if (f[0] == "device" && f.size() >= 6) {
      VulkanDevice d;
      d.name = f[1];
      d.type = f[2];
      d.api_version = uint32_t(std::stoul(f[3]));
      d.vendor_id = uint32_t(std::stoul(f[4]));
      if (!f[5].empty()) d.missing = Split(f[5], ',');
      r.devices.push_back(std::move(d));
    }
  }
  return r;
}

std::string OsReleaseValue(const std::string& text, const std::string& key) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(key + "=", 0) != 0) continue;
    std::string v = line.substr(key.size() + 1);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    return v;
  }
  return {};
}

std::string Trimmed(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s;
}

}  // namespace

bool VulkanReport::Usable() const {
  if (!error.empty() || !surface) return false;
  return std::any_of(devices.begin(), devices.end(),
                     [](const VulkanDevice& d) { return d.missing.empty() && d.type != "CPU (software)"; });
}

std::string VulkanVersionString(uint32_t v) {
  return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
         std::to_string(VK_API_VERSION_PATCH(v));
}

VulkanReport CheckVulkan() {
  // In a child process: a driver that crashes while being asked must not take the setup with it. Called before
  // anything starts threads (fork, then dlopen in the child).
  int fds[2];
  if (pipe(fds) != 0) return Query();
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return Query();
  }
  if (pid == 0) {
    close(fds[0]);
    const std::string out = Serialize(Query());
    size_t done = 0;
    while (done < out.size()) {
      const ssize_t n = write(fds[1], out.data() + done, out.size() - done);
      if (n <= 0) break;
      done += size_t(n);
    }
    _exit(0);
  }
  close(fds[1]);
  std::string text;
  char buf[4096];
  for (ssize_t n; (n = read(fds[0], buf, sizeof(buf))) != 0;) {
    if (n < 0) break;
    text.append(buf, size_t(n));
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || text.empty()) {
    VulkanReport r;
    r.error = WIFSIGNALED(status) ? "the Vulkan driver crashed while being asked about the graphics card (signal " +
                                        std::to_string(WTERMSIG(status)) + ")"
                                  : "the Vulkan driver could not be asked about the graphics card";
    return r;
  }
  return Deserialize(text);
}

std::vector<std::string> VulkanProblems(const VulkanReport& r) {
  if (r.Usable()) return {};
  if (!r.error.empty()) return {r.error};
  std::vector<std::string> out;
  if (!r.surface) out.push_back("the Vulkan driver cannot show a window on X11 or Wayland (no VK_KHR_surface with "
                                "VK_KHR_xcb_surface or VK_KHR_wayland_surface)");
  if (r.devices.empty()) {
    out.push_back("the Vulkan driver reports no graphics card");
    return out;
  }
  for (const auto& d : r.devices) {
    if (d.type == "CPU (software)") {
      out.push_back(d.name + " is a software renderer (no graphics card driver)");
      continue;
    }
    if (d.missing.empty()) continue;
    std::string m;
    for (size_t i = 0; i < d.missing.size(); ++i) m += (i ? ", " : "") + d.missing[i];
    out.push_back(d.name + " (Vulkan " + VulkanVersionString(d.api_version) + ") lacks what the game needs: " + m);
  }
  return out;
}

Platform DetectPlatform(const fs::path& root) {
  Platform p;
  std::string os;
  if (!ReadFile(root / "etc/os-release", &os)) ReadFile(root / "usr/lib/os-release", &os);
  p.os_name = OsReleaseValue(os, "PRETTY_NAME");
  if (p.os_name.empty()) p.os_name = OsReleaseValue(os, "NAME");
  const std::string id = OsReleaseValue(os, "ID");
  p.steamos = id == "steamos" || OsReleaseValue(os, "VARIANT_ID") == "steamdeck";
  std::string vendor, board;
  ReadFile(root / "sys/class/dmi/id/board_vendor", &vendor);
  ReadFile(root / "sys/class/dmi/id/board_name", &board);
  vendor = Trimmed(vendor);
  board = Trimmed(board);
  if (vendor == "Valve" && (board == "Jupiter" || board == "Galileo")) {
    p.steam_deck = true;
    p.deck_model = board == "Galileo" ? "OLED" : "LCD";
  }
  return p;
}

std::string PlatformSummary(const Platform& p) {
  std::string s = p.os_name.empty() ? "an unknown Linux" : p.os_name;
  if (p.steam_deck) s += " on a Steam Deck (" + p.deck_model + ")";
  return s;
}

}  // namespace kuf::setup
