// What the setup checks about this computer before it offers an installation (installer/README.md "System check"):
// the graphics driver's Vulkan support for the game's renderer, and whether this is SteamOS / a Steam Deck.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kuf::setup {

namespace fs = std::filesystem;

// One Vulkan device and the capabilities the game's renderer requires that it lacks (empty: usable). The list is
// the ReXGlue SDK's (src/ui/vulkan/vulkan_device.cpp CreateIfSupported, vulkan_instance.cpp): Vulkan 1.0;
// the features independentBlend, fragmentStoresAndAtomics, vertexPipelineStoresAndAtomics, geometryShader,
// fillModeNonSolid; a queue family with graphics and compute; the VK_KHR_swapchain extension.
struct VulkanDevice {
  std::string name;
  std::string type;  // "integrated GPU", "discrete GPU", "CPU (software)", ...
  uint32_t api_version = 0;
  uint32_t vendor_id = 0;
  std::vector<std::string> missing;
};

struct VulkanReport {
  std::string error;  // the loader or the instance failed (no devices then)
  bool surface = false;  // the instance can present on X11 or Wayland (VK_KHR_surface + xcb or wayland surface)
  std::vector<VulkanDevice> devices;
  // A hardware device with nothing missing (software renderers such as llvmpipe are far too slow to count).
  bool Usable() const;
};

std::string VulkanVersionString(uint32_t api_version);

// Loads libvulkan.so.1 (dlopen, nothing linked) and asks every device. KUF_SETUP_PRETEND_NO_VULKAN (tests) reports
// that no Vulkan driver was found.
VulkanReport CheckVulkan();

// The problems to show for a report (empty when usable): plain sentences naming what is missing.
std::vector<std::string> VulkanProblems(const VulkanReport& report);

// The operating system and machine: os-release (ID=steamos) and the DMI board (Valve "Jupiter" is the LCD Steam
// Deck, "Galileo" the OLED one). `root` is "/" except in tests.
struct Platform {
  std::string os_name;  // PRETTY_NAME
  bool steamos = false;
  bool steam_deck = false;
  std::string deck_model;  // "LCD", "OLED" or ""
};
Platform DetectPlatform(const fs::path& root = "/");

// A one-line description of this computer for the log and --check-system ("SteamOS 3.7.13 on a Steam Deck (OLED)").
std::string PlatformSummary(const Platform& p);

}  // namespace kuf::setup
