# An idea for having cake and eating it too.   
  
The idea is to add per game, patches and custom GPU drivers. For example, a patch that runs Halo 3 differently and with a different GPU custom driver than NFS Carbon. Long press a game to add custom settings and patches. Add a framework sample of how patches need to be written and add it to GitHub so people can create new patches.   
  
Here is the proposed architecture plan that integrates **Per-Game Custom GPU Driver Loading** (using dynamic linking hooks like libadrenotools for custom Turnip/Mesa drivers) alongside Title-ID configs and the patch engine.  
## Architecture Overview  
Every Xbox 360 title is assigned a unique **Title ID** (e.g., Halo 3 is 4D5307E6 and Need for Speed: Carbon is 454107EC).  
When a game boots:  
1. The emulator reads the .xex header to identify the **Title ID** and **Module Hash**.  
2. **Driver Initialization**: The emulator checks the per-game config for a designated **custom Vulkan driver** (e.g., Turnip/Mesa v24.x) and hooks it *before* initializing the Vulkan instance.  
3. **Settings Override**: Loads game-specific flags from /config/<TitleID>.toml.  
4. **Memory Patch Engine**: Applies target memory modifications from /patches/<TitleID> - <Name>.patch.toml.  
## Step 1: Storage & Folder Structure  
```
/sdcard/Android/data/com.xenia.aep/files/
├── drivers/
│   ├── system_default/
│   ├── turnip_v24.1.0/      <-- Extracted Adreno/Turnip custom driver zip
│   └── turnip_v23.3.0/
├── config/
│   ├── 4D5307E6.toml        (Halo 3 Config: Custom Turnip Driver A)
│   └── 454107EC.toml        (NFS Carbon Config: System Driver)
└── patches/
    ├── 4D5307E6 - Halo 3.patch.toml
    └── 454107EC - Need for Speed Carbon.patch.toml

```
## Step 2: Custom GPU Driver Hooking (libadrenotools)  
Custom drivers (like Mesa Turnip for Qualcomm Snapdragon devices) must be hooked **before** Vulkan functions are loaded (vulkan::VulkanProvider::Initialize).  
**C++ Native Driver Loader (gpu_driver_manager.cpp)**  
```
#include <dlfcn.h>
#include <adrenotools/driver.h>
#include "xenia/base/logging.h"

class GPUDriverManager {
public:
    static bool LoadCustomDriver(const std::string& driver_dir, const std::string& library_name) {
        if (driver_dir.empty() || driver_dir == "default") {
            XELOGI("Using system default GPU driver.");
            return true;
        }

        XELOGI("Hooking custom GPU driver from: {}/{}", driver_dir, library_name);

        // Uses adrenotools to hook custom Vulkan ICD dynamic libraries into the namespace
        void* handle = adrenotools_open_libvulkan(
            RTLD_NOW,
            ADRENOTOOLS_GPU_MAPPING_IMPORT_HARDWARE_BUFFER,
            driver_dir.c_str(),
            library_name.c_str(),
            nullptr,  // hook_dir
            nullptr   // custom_feature_flags
        );

        if (!handle) {
            XELOGE("Failed to load custom Vulkan driver from path: {}", driver_dir);
            return false;
        }

        XELOGI("Successfully hooked custom GPU driver into process space.");
        return true;
    }
};

```
## Step 3: Per-Game Configuration (.toml)  
The per-game config now specifies both GPU drivers and graphics settings.  
**Halo 3 Settings (/config/4D5307E6.toml)**  
```
[gpu]
# Custom Driver Path
custom_driver_path = "drivers/turnip_v24.1.0"
custom_driver_library = "vulkan.adreno.so"

# Game-Specific Graphics Toggles
draw_resolution_scale = 1
query_occlusion_fake_results = true

```
**NFS Carbon Settings (/config/454107EC.toml)**  
```
[gpu]
# Use default system driver for NFS
custom_driver_path = "default"
custom_driver_library = ""

# Speedhack toggles safe for NFS Carbon
vsync = false
gpu_allow_invalid_fetch_constants = true

```
**Native Execution Pipeline**  
```
void BootTitle(const std::string& xex_path) {
    std::string title_id = ExtractTitleID(xex_path);
    
    // 1. Load Per-Game Config File
    std::string config_path = fmt::format("config/{}.toml", title_id);
    toml::table config = toml::parse_file(config_path);
    
    // 2. Hook Custom Driver BEFORE initializing Vulkan Instance
    std::string driver_path = config["gpu"]["custom_driver_path"].value_or("default");
    std::string driver_lib  = config["gpu"]["custom_driver_library"].value_or("vulkan.adreno.so");
    
    GPUDriverManager::LoadCustomDriver(driver_path, driver_lib);
    
    // 3. Initialize Vulkan Render Hardware Interface (RHI)
    xenia::gpu::vulkan::Initialize();
    
    // 4. Apply Game Memory Patches
    ApplyPatchesForTitle(title_id);
}

```
## Step 4: Memory Patch Engine  
For targeted code fixes (e.g., lighting glitches, model fixes, or disabling post-processing):  
```
4D5307E6 - Halo 3.patch.toml
title_name = "Halo 3"
title_id = "4D5307E6"
hash = "E2F1A02B19842C02"

[[patch]]
name = "Disable Post-Process Blur / Character Model Fix"
author = "Xenia-Community"
is_enabled = true

[[patch.be32]]
address = 0x824A1000
value = 0x60000000 # Replace target instruction with NOP

```
**C++ Patch Application Logic**  
```
void ApplyPatchesForTitle(const std::string& title_id) {
    std::string patch_path = fmt::format("patches/{} - *.patch.toml", title_id);
    auto patch_file = FindPatchFile(patch_path);
    
    if (!patch_file.empty()) {
        auto patches = ParsePatchTOML(patch_file);
        for (const auto& patch : patches) {
            if (patch.is_enabled) {
                WriteGuestMemory(patch.address, patch.value, patch.type);
            }
        }
    }
}

```
## Step 5: Android Frontend (Kotlin UI)  
In the Android app UI, add options to manage custom driver packages and assign them to specific games:  
```
data class GameSettings(
    val titleId: String,
    var customDriverPath: String = "default",
    var resolutionScale: Int = 1,
    var enabledPatches: MutableList<String> = mutableListOf()
)

class GameConfigManager(private val context: Context) {

    fun saveGameConfig(settings: GameSettings) {
        val configFile = File(context.getExternalFilesDir(null), "config/${settings.titleId}.toml")
        
        val tomlContent = """
            [gpu]
            custom_driver_path = "${settings.customDriverPath}"
            draw_resolution_scale = ${settings.resolutionScale}
        """.trimIndent()
        
        configFile.writeText(tomlContent)
    }
}

```
