# Migration Guide: DeviceStatsModule to New Firmware

Complete guide for migrating custom DeviceStatsModule functionality to a new Meshtastic firmware version.

---

## Overview

This guide describes ALL modifications needed to add DeviceStatsModule to a fresh Meshtastic firmware.

**Goals achieved:**
1. ✅ **Cross-platform compatibility** - works on any Meshtastic device (NRF52, ESP32, ESP32S3, STM32WL)
2. ✅ **Monitor device behavior** - real-time statistics via text commands
3. ✅ **Optional firmware size reduction** - exclude unused modules for rak4631_lite variant

**Architecture:**
- **Weak linkage** for `dynamic_max_nodes` - variants can override defaults
- **Hardcoded PIN** - no variant configuration needed
- **Automatic MAX_NUM_NODES** - uses architecture-specific defaults from `mesh-pb-constants.h`

**Total changes:** ~15 files modified, 4 files added

---

## PART 1: Core Module Files (Copy & Paste)

### Step 1.1: Add New Module Files

Copy these **4 files** directly to new firmware:

```
src/modules/DeviceStatsModule.cpp    (~810 lines) - Main module implementation
src/modules/DeviceStatsModule.h      (~67 lines)  - Module header
src/modules/DeviceStatsModule.md     (~240 lines) - Documentation
src/mesh/DynamicNodes.cpp             (~18 lines)  - Weak linkage for dynamic_max_nodes
```

**Action:** Direct copy - NO modifications needed

**Why DynamicNodes.cpp?**
- Defines `DEFAULT_MAX_NODES` and `dynamic_max_nodes` with weak linkage
- Uses `MAX_NUM_NODES` from `mesh-pb-constants.h` (architecture-specific defaults)
- Allows variants to override by defining stronger symbols in `variant.cpp`
- Makes DeviceStatsModule work on ANY device without variant-specific configuration

---

## PART 2: Core Integration (Minimal Edits)

### Step 2.1: Register Module in Modules.cpp

**File:** `src/modules/Modules.cpp`

**Add include** (after other module includes):
```cpp
#if !MESHTASTIC_EXCLUDE_DEVICESTATS
#include "modules/DeviceStatsModule.h"
#endif
```

**Add initialization** (in `setupModules()` function, after textMessageModule):
```cpp
#if !MESHTASTIC_EXCLUDE_DEVICESTATS
    // Custom device statistics and monitoring module
    deviceStatsModule = new DeviceStatsModule();
#endif
```

**Lines changed:** 2 blocks (5 lines total)

---

### Step 2.2: Add Periodic Work in main.cpp

**File:** `src/main.cpp`

**Add include** (after other module includes):
```cpp
#if !MESHTASTIC_EXCLUDE_DEVICESTATS
#include "modules/DeviceStatsModule.h"
#endif
```

**Add periodic call** (in `loop()` function, after `service->loop()`):
```cpp
#if !MESHTASTIC_EXCLUDE_DEVICESTATS
    // Handle periodic device statistics monitoring
    if (deviceStatsModule) {
        deviceStatsModule->doPeriodicWork();
    }
#endif
```

**Lines changed:** 2 blocks (8 lines total)

---

## PART 3: Core Enhancements (Required by DeviceStatsModule)

### Step 3.1: Add PacketHistory Statistics

**File:** `src/mesh/PacketHistory.h`

**Add to public section** (after existing methods):
```cpp
// Statistics methods for monitoring packet history
uint32_t getPacketCount() const;
uint32_t getOldestPacketAge() const;  // Age of oldest packet in seconds
```

**File:** `src/mesh/PacketHistory.cpp`

**Add at end of file** (after existing methods):
```cpp
uint32_t PacketHistory::getPacketCount() const
{
    return recentPackets.size();
}

uint32_t PacketHistory::getOldestPacketAge() const
{
    if (recentPackets.empty()) {
        return 0;
    }
    uint32_t currentTime = millis();
    uint32_t oldestTime = currentTime;
    for (const auto& packet : recentPackets) {
        if (packet.rxTimeMsec < oldestTime) {
            oldestTime = packet.rxTimeMsec;
        }
    }
    return (currentTime - oldestTime) / 1000; // Convert to seconds
}
```

**Lines changed:** 2 methods in .h (2 lines), 2 implementations in .cpp (~20 lines)

**Why needed:** `/debug` command monitors DupeCache packet ages to detect memory overflow risk

---

### Step 3.2: Add Router Queue Monitoring

**File:** `src/mesh/Router.h`

**Add to public section** (after `getQueueStatus()`):
```cpp
/// Get network queue statistics for monitoring (safe read-only access)
int getFromRadioQueueSize() { return fromRadioQueue.numUsed(); }
int getFromRadioQueueFree() { return fromRadioQueue.numFree(); }

/// Get PacketHistory (DupeCache) statistics for monitoring
uint32_t getPacketCount() { return PacketHistory::getPacketCount(); }
uint32_t getOldestPacketAge() { return PacketHistory::getOldestPacketAge(); }
```

**Lines changed:** 6 lines

**Why needed:** `/debug` command displays RX/TX queue status and DupeCache stats

---

### Step 3.3: Fix nRF52 Memory Monitoring

**File:** `src/memGet.cpp`

**Add FreeRTOS include at top:**
```cpp
#include "freertosinc.h"
```

**Find the `#ifdef ARCH_NRF52` section and update `getFreeHeap()`:**
```cpp
#ifdef ARCH_NRF52
uint32_t getFreeHeap() {
#ifdef HAS_FREE_RTOS
    return xPortGetFreeHeapSize();
#else
    // Fallback estimate if FreeRTOS not available
    return 150 * 1024; // 150KB estimate
#endif
}
```

**Update `getHeapSize()`:**
```cpp
uint32_t getHeapSize() {
    return 200 * 1024; // nRF52840 has ~200KB RAM for heap
}
```

**Update `getFlashTotal()`:**
```cpp
uint32_t getFlashTotal() {
    return 815104; // nRF52840 flash size
}
```

**Add `getFlashUsed()`:**
```cpp
uint32_t getFlashUsed() {
    // Calculate used flash from linker symbols
    extern uint32_t __data_start__;
    extern uint32_t __bss_end__;
    return (uint32_t)&__bss_end__ - (uint32_t)&__data_start__;
}
#endif
```

**File:** `src/memGet.h`

**Add declaration:**
```cpp
uint32_t getFlashUsed();
```

**Why needed:** Without this, memory monitoring returns 0 or incorrect values on nRF52

**Lines changed:** ~30 lines in .cpp, 1 line in .h

---

### Step 3.4: Update NodeDB for Dynamic Max Nodes

**File:** `src/mesh/NodeDB.cpp`

**Add extern declarations at top** (after includes):
```cpp
// dynamic_max_nodes defined in DynamicNodes.cpp with weak linkage
// Variants can override by defining in variant.cpp
extern const uint32_t DEFAULT_MAX_NODES;
extern uint32_t dynamic_max_nodes;
```

**Find and replace throughout file:**
- `MAX_NUM_NODES` → `dynamic_max_nodes` (in multiple places)

**Key locations to update:**
1. `installDefaultDeviceState()` - vector initialization
2. `loadFromDisk()` - bounds checking and resize
3. `isFull()` - capacity check
4. `getOrCreateMeshNode()` - capacity check

**Why needed:** Support `/setmaxnodes` command and dynamic node management

**Lines changed:** ~10 replacements

---

### Step 3.5: Update mesh-pb-constants.h

**File:** `src/mesh/mesh-pb-constants.h`

**Change static_assert** (increase from 200 to 250):
```cpp
static_assert(sizeof(meshtastic_NodeInfoLite) <= 250, "NodeInfoLite size increased. Reconsider impact on MAX_NUM_NODES.");
```

**Why needed:** Support 256+ nodes (was limited to 200 bytes/node)

**Lines changed:** 1 line

---

## PART 4: Optional Variant Configuration (rak4631_lite Example)

**NOTE:** This section is OPTIONAL. DeviceStatsModule works on ALL devices without variant configuration.
Only needed if you want to:
- Create a custom variant with aggressive memory optimizations
- Override default `MAX_NUM_NODES` for specific variant
- Reduce firmware size by excluding unused modules

### Step 4.1: Create Custom Variant (rak4631_lite)

**Directory structure:**
```
variants/rak4631_lite/
  ├── platformio.ini    - Build configuration with module exclusions
  ├── variant.h         - Hardware pins and feature flags
  └── variant.cpp       - Override dynamic_max_nodes (256 instead of 80)
```

**File:** `variants/rak4631_lite/variant.cpp`

**Add at end of file:**
```cpp
// Override default MAX_NUM_NODES for RAK4631_LITE
// This overrides the weak symbol from DynamicNodes.cpp
const uint32_t DEFAULT_MAX_NODES = 256;  // Aggressive limit for RAK4631_LITE
uint32_t dynamic_max_nodes = DEFAULT_MAX_NODES;
```

**File:** `variants/rak4631_lite/variant.h`

**Add declarations:**
```cpp
// Override default MAX_NUM_NODES for RAK4631 - make it dynamic
extern const uint32_t DEFAULT_MAX_NODES;  // Safe limit for RAK4631 (256)
extern uint32_t dynamic_max_nodes;        // Current limit (can only increase from default)
#define MAX_NUM_NODES dynamic_max_nodes
```

**File:** `variants/rak4631_lite/platformio.ini`

**Key settings:**
```ini
[env:rak4631_lite]
extends = nrf52840_base
board = wiscore_rak4631

build_flags = 
    ${nrf52840_base.build_flags}
    -DHAS_ETHERNET=0            # Must be early to prevent RAK13800 include
    -Ivariants/rak4631_lite     # Include variant directory
    
    # Aggressive module exclusions for firmware size reduction
    -DMESHTASTIC_EXCLUDE_GPS=1
    -DMESHTASTIC_EXCLUDE_SCREEN=1
    -DMESHTASTIC_EXCLUDE_WIFI=1
    -DMESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR=1
    -DMESHTASTIC_EXCLUDE_MQTT=1
    ... (see full file for complete list)
```

**Why needed:**
- Override default 80 nodes (NRF52 default) to 256 nodes
- Aggressive firmware size reduction through module exclusions
- Custom hardware configuration

---

### Step 4.2: Set Default Environment

**File:** `platformio.ini` (root)

**Change default_envs:**
```ini
[platformio]
default_envs = rak4631_lite
```

**Why needed:** Convenience - sets default build target

---

## PART 5: Optional Size Reduction (Conditional Compilation)

Only needed if you're excluding modules via `-DMESHTASTIC_EXCLUDE_*` flags.

### Step 5.1: Add RTTTL Conditionals

**File:** `src/ButtonThread.cpp`
```cpp
#ifndef MESHTASTIC_EXCLUDE_RTTTL
    playShutdownMelody();
#endif
```

**File:** `src/shutdown.h`
```cpp
#ifndef MESHTASTIC_EXCLUDE_RTTTL
    playShutdownMelody();
#endif
```

**File:** `src/input/ExpressLRSFiveWay.cpp`
```cpp
#ifndef MESHTASTIC_EXCLUDE_RTTTL
    playBeep();
#endif
```

**Lines changed:** ~12 lines total

---

### Step 5.2: Add GPS Conditionals

**File:** `src/gps/GPS.h`
```cpp
#ifndef TINYGPS_OPTION_NO_STATISTICS
    uint32_t lastChecksumFailCount = 0;
#endif
```

**Lines changed:** ~3 lines

---

## PART 6: Architecture-Specific Defaults

DeviceStatsModule automatically uses the correct defaults for each architecture:

| Architecture | Default MAX_NUM_NODES | Source |
|--------------|----------------------|--------|
| **ARCH_NRF52** | 80 | `mesh-pb-constants.h` |
| **ARCH_STM32WL** | 10 | `mesh-pb-constants.h` |
| **ESP32S3** (16MB flash) | 250 | `mesh-pb-constants.h` (dynamic) |
| **ESP32S3** (8MB flash) | 200 | `mesh-pb-constants.h` (dynamic) |
| **ESP32S3** (<8MB flash) | 100 | `mesh-pb-constants.h` (dynamic) |
| **Other (ESP32, RP2040, etc)** | 100 | `mesh-pb-constants.h` |

**How it works:**
1. `DynamicNodes.cpp` reads `MAX_NUM_NODES` from `mesh-pb-constants.h`
2. Defines weak symbols for `DEFAULT_MAX_NODES` and `dynamic_max_nodes`
3. Variants can override by defining stronger symbols in `variant.cpp`
4. If no override, uses architecture-specific defaults

**Example:**
- `rak4631_eth_gw` (standard NRF52) → uses 80 nodes (no override)
- `rak4631_lite` (custom variant) → uses 256 nodes (override in variant.cpp)
- `tbeam` (ESP32) → uses 100 nodes (no override)
- `station-g2` (ESP32S3, 16MB) → uses 250 nodes (no override)

---

## PART 7: Step-by-Step Migration Process

### Phase 1: Prepare New Firmware
1. Download/clone new Meshtastic firmware version
2. Create branch: `git checkout -b custom-devicestats`

### Phase 2: Add DeviceStatsModule (Required)
1. Copy `src/modules/DeviceStatsModule.*` (3 files)
2. Copy `src/mesh/DynamicNodes.cpp` (1 file)
3. Edit `src/modules/Modules.cpp` - add registration (Step 2.1)
4. Edit `src/main.cpp` - add periodic call (Step 2.2)
5. Test build: `pio run -e <your_variant>`

### Phase 3: Add Core Enhancements (Required)
1. Edit `src/mesh/PacketHistory.h` - add methods (Step 3.1)
2. Edit `src/mesh/PacketHistory.cpp` - add implementations (Step 3.1)
3. Edit `src/mesh/Router.h` - add getters (Step 3.2)
4. Edit `src/memGet.cpp` - fix nRF52 functions (Step 3.3)
5. Edit `src/memGet.h` - add declaration (Step 3.3)
6. Edit `src/mesh/NodeDB.cpp` - add extern and replace MAX_NUM_NODES (Step 3.4)
7. Edit `src/mesh/mesh-pb-constants.h` - update assert (Step 3.5)
8. Test build: `pio run -e <your_variant>`

### Phase 4: Optional Custom Variant (if needed)
1. Copy existing variant directory (e.g., `rak4631_eth_gw` → `rak4631_lite`)
2. Edit `variant.cpp` - override `dynamic_max_nodes` (Step 4.1)
3. Edit `variant.h` - add declarations (Step 4.1)
4. Edit `platformio.ini` variant - add exclusions (Step 4.1)
5. Edit root `platformio.ini` - set default_envs (Step 4.2)
6. Test build: `pio run -e rak4631_lite`

### Phase 5: Optional Size Reduction (if excluding modules)
1. Edit `src/ButtonThread.cpp` - add RTTTL check (Step 5.1)
2. Edit `src/shutdown.h` - add RTTTL check (Step 5.1)
3. Edit `src/gps/GPS.h` - add GPS stats check (Step 5.2)
4. Edit `src/input/ExpressLRSFiveWay.cpp` - add RTTTL check (Step 5.1)
5. Test build: `pio run -e rak4631_lite`

### Phase 6: Final Testing
1. Build: `pio run -e <your_variant>`
2. Flash device: `pio run -e <your_variant> -t upload`
3. Enable in phone app: Settings → Module Config → Range Test → Enable
4. Test commands: Send `/mem`, `/debug`, `/help` as text message

---

## PART 8: Quick Checklist

### Minimum Required (DeviceStatsModule only - works on ALL devices):
- [ ] Copy DeviceStatsModule.* files (3 files)
- [ ] Copy DynamicNodes.cpp (1 file)
- [ ] Edit Modules.cpp (add registration)
- [ ] Edit main.cpp (add periodic call)
- [ ] Edit PacketHistory.h/cpp (add statistics)
- [ ] Edit Router.h (add getters)
- [ ] Edit memGet.cpp/h (fix nRF52)
- [ ] Edit NodeDB.cpp (add extern, replace MAX_NUM_NODES)
- [ ] Edit mesh-pb-constants.h (250 bytes limit)

### Optional Custom Variant (only if you want to override defaults):
- [ ] Create/copy variant directory
- [ ] Edit variant.cpp (override dynamic_max_nodes)
- [ ] Edit variant.h (add declarations)
- [ ] Edit platformio.ini variant (add exclusions)
- [ ] Edit root platformio.ini (set default_envs)

### Optional Size Reduction (only if excluding modules):
- [ ] Edit ButtonThread.cpp (RTTTL conditional)
- [ ] Edit shutdown.h (RTTTL conditional)
- [ ] Edit GPS.h (GPS stats conditional)
- [ ] Edit ExpressLRSFiveWay.cpp (RTTTL conditional)

---

## PART 9: File Summary

### Files to COPY (4 files):
```
src/modules/DeviceStatsModule.cpp    - Main module implementation
src/modules/DeviceStatsModule.h      - Module header
src/modules/DeviceStatsModule.md     - Documentation
src/mesh/DynamicNodes.cpp             - Weak linkage for dynamic_max_nodes
```

### Files to EDIT - Core Integration (2 files, ~13 lines):
```
src/modules/Modules.cpp          - Add 5 lines (registration)
src/main.cpp                     - Add 8 lines (periodic work + include)
```

### Files to EDIT - Core Enhancements (6 files, ~60 lines):
```
src/mesh/PacketHistory.h         - Add 2 lines (method declarations)
src/mesh/PacketHistory.cpp       - Add ~20 lines (implementations)
src/mesh/Router.h                - Add 6 lines (getters)
src/memGet.cpp                   - Add ~30 lines (nRF52 fixes)
src/memGet.h                     - Add 1 line (declaration)
src/mesh/NodeDB.cpp              - Add 4 lines (extern), replace MAX_NUM_NODES (~10 places)
src/mesh/mesh-pb-constants.h     - Change 1 line (200→250)
```

### Files to EDIT - Optional Custom Variant (3-4 files):
```
variants/<custom>/variant.cpp         - Add 3 lines (override dynamic_max_nodes)
variants/<custom>/variant.h           - Add 4 lines (declarations)
variants/<custom>/platformio.ini      - Add ~300 lines (module exclusions)
platformio.ini (root)                 - Change 1 line (default_envs)
```

### Files to EDIT - Optional Size Reduction (4 files, ~15 lines):
```
src/ButtonThread.cpp             - Add ~4 lines (RTTTL conditional)
src/shutdown.h                   - Add ~4 lines (RTTTL conditional)
src/gps/GPS.h                    - Add ~3 lines (GPS stats conditional)
src/input/ExpressLRSFiveWay.cpp  - Add ~4 lines (RTTTL conditional)
```

---

## PART 10: Testing Checklist

After migration:

1. **Build Test (any device):**
   ```bash
   pio run -e tbeam           # ESP32
   pio run -e station-g2      # ESP32S3
   pio run -e rak4631_eth_gw  # NRF52 standard
   pio run -e rak4631_lite    # NRF52 custom (if created)
   ```
   Should see: `SUCCESS`

2. **Flash Test:**
   ```bash
   pio run -e <your_variant> -t upload
   ```
   Device should boot normally

3. **Enable Module:**
   - Open Meshtastic app
   - Settings → Module Config → Range Test
   - Toggle "Enable" ON

4. **Command Test:**
   - Send text message `/help` to device
   - Should reply with command list
   - Send `/mem` - should show memory stats
   - Send `/debug` - should show DupeCache info

5. **Monitoring Test:**
   - Send `/monstart 30,123456` (30s interval, PIN: 123456)
   - Should receive memory stats every 30 seconds
   - Send `/monstop 123456` to stop

---

## PART 11: Troubleshooting

### Build fails with "deviceStatsModule not declared"
**Fix:** Add `#include "modules/DeviceStatsModule.h"` in main.cpp and Modules.cpp

### Build fails with "getPacketCount not found"
**Fix:** Copy PacketHistory methods (Step 3.1)

### Build fails with "dynamic_max_nodes not defined"
**Fix:** Copy DynamicNodes.cpp (Step 2, Phase 2)

### Commands not working
**Check:** Range Test module enabled in phone app?

### Memory stats show 0 on nRF52
**Fix:** Copy nRF52 memory fixes (Step 3.3)

### "/setmaxnodes not working"
**Check:** NodeDB.cpp updated with dynamic_max_nodes? (Step 3.4)

### Build fails with "RAK13800_W5100S.h not found" (rak4631_lite)
**Fix:** Ensure `-DHAS_ETHERNET=0` is EARLY in build_flags in platformio.ini

---

## PART 12: Total Lines Changed

| Category | Files | Lines Added | Lines Removed |
|----------|-------|-------------|---------------|
| **Required (all devices)** |
| New Files | 4 | ~900 | 0 |
| Integration | 2 | 13 | 0 |
| Core Enhancements | 6 | ~70 | 0 |
| **Optional (custom variant)** |
| Variant Config | 4 | ~310 | 0 |
| **Optional (size reduction)** |
| Size Reduction | 4 | ~15 | 0 |
| **TOTAL** | **20** | **~1,308** | **~0** |

**Minimum for any device:** 12 files, ~983 lines (4 new files + 8 edited files)

---

## PART 13: Minimal Migration (Universal - Works on ALL Devices)

For DeviceStatsModule on ANY device without custom variant:

### Required Files (12 files, ~983 lines):
1. Copy DeviceStatsModule.*, DynamicNodes.cpp (4 files)
2. Edit Modules.cpp (5 lines)
3. Edit main.cpp (8 lines)
4. Edit PacketHistory.h/cpp (~22 lines)
5. Edit Router.h (6 lines)
6. Edit memGet.cpp/h (~31 lines)
7. Edit NodeDB.cpp (~14 lines: extern + replacements)
8. Edit mesh-pb-constants.h (1 line)

**Total:** ~987 lines across 12 files

### Architecture-Specific Behavior (automatic):
- NRF52 devices → 80 nodes default
- ESP32 devices → 100 nodes default
- ESP32S3 (16MB) → 250 nodes default
- STM32WL → 10 nodes default

### PIN Code (hardcoded):
- `123456` (defined in DeviceStatsModule.cpp)

### No variant configuration needed!

---

## PART 14: Key Design Decisions

### Why weak linkage?
- Allows variants to override defaults without modifying core files
- Makes DeviceStatsModule portable across all devices
- No `#ifdef` spaghetti code

### Why hardcoded PIN?
- Simplifies migration - no variant configuration needed
- Can be changed in single location (DeviceStatsModule.cpp)
- Security by obscurity (change PIN in source if needed)

### Why DynamicNodes.cpp?
- Centralizes dynamic_max_nodes definition
- Provides architecture-specific defaults automatically
- Avoids duplicate symbol errors
- Makes weak linkage work correctly

### Why mesh-pb-constants.h increase (200→250)?
- Support larger NodeInfoLite structures
- Enable 256+ nodes for high-capacity variants
- Minimal RAM impact (~50 bytes per node)

---

## End of Migration Guide

All changes documented. Follow phases 1-6 for complete migration.
For minimal setup on any device, follow PART 13.

