# Migration Guide: DeviceStatsModule to New Firmware

Complete guide for migrating custom DeviceStatsModule functionality to a new Meshtastic firmware version.

---

## Overview

This guide describes ALL modifications needed to add DeviceStatsModule to a fresh Meshtastic firmware.

**Goals achieved:**
1. ✅ **Reduce firmware size** - exclude unused modules (~10-15KB Flash saved)
2. ✅ **Monitor device behavior** - real-time statistics via text commands

**Total changes:** 27 files modified, 3 files added

---

## PART 1: DeviceStatsModule Files (Copy & Paste)

### Step 1.1: Add New Module Files

Copy these 3 files directly to new firmware:

```
src/modules/DeviceStatsModule.cpp    (802 lines)
src/modules/DeviceStatsModule.h      (67 lines)
src/modules/DeviceStatsModule.md     (242 lines - documentation)
```

**Action:** Direct copy - NO modifications needed

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
uint32_t getNewestPacketAge() const;  // Age of newest packet in seconds
uint32_t getAveragePacketAge() const; // Average age of all packets in seconds
```

**File:** `src/mesh/PacketHistory.cpp`

**Add at end of file** (after existing methods):
```cpp
// Statistics methods for monitoring packet history
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

uint32_t PacketHistory::getNewestPacketAge() const
{
    if (recentPackets.empty()) {
        return 0;
    }
    
    uint32_t currentTime = millis();
    uint32_t newestTime = 0;
    
    for (const auto& packet : recentPackets) {
        if (packet.rxTimeMsec > newestTime) {
            newestTime = packet.rxTimeMsec;
        }
    }
    
    return (currentTime - newestTime) / 1000; // Convert to seconds
}

uint32_t PacketHistory::getAveragePacketAge() const
{
    if (recentPackets.empty()) {
        return 0;
    }
    
    uint32_t currentTime = millis();
    uint32_t totalAge = 0;
    
    for (const auto& packet : recentPackets) {
        totalAge += (currentTime - packet.rxTimeMsec);
    }
    
    return (totalAge / recentPackets.size()) / 1000; // Convert to seconds
}
```

**Lines changed:** 4 methods in .h (4 lines), 4 implementations in .cpp (58 lines)

**Why needed:** `/debug` command monitors DupeCache packet ages

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
uint32_t getNewestPacketAge() { return PacketHistory::getNewestPacketAge(); }
uint32_t getAveragePacketAge() { return PacketHistory::getAveragePacketAge(); }
```

**Lines changed:** 8 lines

**Why needed:** `/debug` command displays RX/TX queue status and DupeCache stats

---

### Step 3.3: Fix nRF52 Memory Monitoring

**File:** `src/memGet.cpp`

**Add FreeRTOS heap functions for nRF52:**

Find the `#ifdef ARCH_NRF52` sections and update:

```cpp
#ifdef ARCH_NRF52
// For nRF52, use FreeRTOS heap functions
#include "freertosinc.h"

uint32_t getFreeHeap() {
#ifdef HAS_FREE_RTOS
    return xPortGetFreeHeapSize();
#else
    // Fallback estimate if FreeRTOS not available
    return 150 * 1024; // 150KB estimate
#endif
}

uint32_t getHeapSize() {
    return 200 * 1024; // nRF52840 has ~200KB RAM for heap
}

uint32_t getFlashTotal() {
    return 815104; // nRF52840 flash size
}

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

**Lines changed:** ~78 lines in .cpp, 3 lines in .h

---

## PART 4: Variant Configuration (RAK4631 Specific)

### Step 4.1: Configure RAK4631_ETH_GW Variant

**File:** `variants/rak4631_eth_gw/variant.h`

**Add these definitions:**
```cpp
// PIN code for monitoring/security commands
#define MONITORING_PIN_CODE "123456"

// Override default MAX_NUM_NODES for RAK4631 - make it dynamic
extern const uint32_t DEFAULT_MAX_NODES;  // Safe limit for RAK4631 (256)
extern uint32_t dynamic_max_nodes;        // Current limit (can only increase from default)
#define MAX_NUM_NODES dynamic_max_nodes

// Reduce messages stored for phone connection to save RAM
#define MAX_RX_TOPHONE 4     // Reduce from default 32 to 4 (save ~28*MeshPacket_size RAM)

// RadioLib optimization - exclude unused radio chips to save Flash memory
#define RADIOLIB_EXCLUDE_SX127X 1    // Exclude SX127x family
#define RADIOLIB_EXCLUDE_SX128X 1    // Exclude SX128x family
#define RADIOLIB_EXCLUDE_SX123X 1    // Exclude SX123x family
#define RADIOLIB_EXCLUDE_SI443X 1    // Exclude Si443x family
#define RADIOLIB_EXCLUDE_RF69 1      // Exclude RF69 modules
#define RADIOLIB_EXCLUDE_CC1101 1    // Exclude CC1101
#define RADIOLIB_EXCLUDE_NRF24 1     // Exclude nRF24 modules
#define RADIOLIB_EXCLUDE_HC05 1      // Exclude HC05 Bluetooth
#define RADIOLIB_EXCLUDE_ESP8266 1   // Exclude ESP8266 WiFi
#define RADIOLIB_EXCLUDE_LR11X0 1    // Exclude LR11x0 family
```

**File:** `variants/rak4631_eth_gw/variant.cpp`

**Add these variables:**
```cpp
// Default maximum nodes for RAK4631 - can be changed via admin command
const uint32_t DEFAULT_MAX_NODES = 256;  // Safe limit for RAK4631: 256 nodes (64KB RAM)
uint32_t dynamic_max_nodes = DEFAULT_MAX_NODES;  // Current limit - can be increased but not decreased below default
```

**Why needed:** 
- `MONITORING_PIN_CODE` - required by DeviceStatsModule commands
- `dynamic_max_nodes` - required for `/setmaxnodes` command
- `RADIOLIB_EXCLUDE_*` - firmware size reduction (goal #1)
- `MAX_RX_TOPHONE 4` - RAM optimization

---

### Step 4.2: Configure platformio.ini for Variant

**File:** `variants/rak4631_eth_gw/platformio.ini`

**Add aggressive module exclusions** (see full diff for complete list):

Key exclusions for firmware size reduction:
```ini
-DMESHTASTIC_EXCLUDE_GPS=1
-DMESHTASTIC_EXCLUDE_SCREEN=1
-DMESHTASTIC_EXCLUDE_WIFI=1
-DMESHTASTIC_EXCLUDE_I2C=1
-DMESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR=1
-DMESHTASTIC_EXCLUDE_MQTT=1
-DMESHTASTIC_EXCLUDE_WEBSERVER=1
-DMESHTASTIC_EXCLUDE_EXTERNALNOTIFICATION=1
-DMESHTASTIC_EXCLUDE_CANNEDMESSAGES=1
-DMESHTASTIC_EXCLUDE_RANGETEST=1
-DMESHTASTIC_EXCLUDE_REMOTEHARDWARE=1
... (see full file for complete list)
```

**Why needed:** Firmware size reduction (goal #1)

---

## PART 5: Supporting Changes (NodeDB dynamic_max_nodes)

### Step 5.1: Update NodeDB for Dynamic Max Nodes

**File:** `src/mesh/NodeDB.cpp`

**Find and replace:**
- `MAX_NUM_NODES` → `dynamic_max_nodes` (in multiple places)

**Add NeighborInfo conditional:**
```cpp
#ifndef MESHTASTIC_EXCLUDE_NEIGHBORINFO
#include "modules/NeighborInfoModule.h"
#endif

// In resetNodes():
#ifndef MESHTASTIC_EXCLUDE_NEIGHBORINFO
    if (neighborInfoModule && moduleConfig.neighbor_info.enabled)
        neighborInfoModule->resetNeighbors();
#endif
```

**Why needed:** 
- Support `/setmaxnodes` command
- Conditional compilation for size reduction

---

### Step 5.2: Update mesh-pb-constants.h

**File:** `src/mesh/mesh-pb-constants.h`

**Change static_assert:**
```cpp
static_assert(sizeof(meshtastic_NodeInfoLite) <= 250, "NodeInfoLite size increased. Reconsider impact on MAX_NUM_NODES.");
```

**Why needed:** Support 256+ nodes (was limited to 200 bytes/node)

---

## PART 6: Optional Enhancements (NOT Required)

### Files that can be SKIPPED:

| File | Change | Required? | Action |
|------|--------|-----------|--------|
| `platformio.ini` | default_envs change | ❌ NO | Skip - only dev convenience |
| `README.md` | Documentation | ❌ NO | Skip - only docs |
| `src/ButtonThread.cpp` | RTTTL conditional | ✅ YES (if excluding RTTTL) | Copy |
| `src/shutdown.h` | RTTTL conditional | ✅ YES (if excluding RTTTL) | Copy |
| `src/gps/GPS.h` | GPS stats conditional | ✅ YES (if excluding GPS stats) | Copy |
| `src/input/ExpressLRSFiveWay.cpp` | RTTTL conditional | ✅ YES (if excluding RTTTL) | Copy |
| `src/mesh/generated/meshtastic/telemetry.pb.h` | Generated file | ❌ NO | Skip - auto-generated |
| `src/modules/Telemetry/EnvironmentTelemetry.cpp` | Reverted | ❌ NO | Skip - was reverted |
| `variants/rak4631/variant.*` | RAK4631 config | ⚠️ OPTIONAL | Only if using RAK4631 |

---

## PART 7: Step-by-Step Migration Process

### Phase 1: Prepare New Firmware
1. Download/clone new Meshtastic firmware version
2. Create branch: `git checkout -b custom-devicestats`

### Phase 2: Add DeviceStatsModule
1. Copy `src/modules/DeviceStatsModule.*` (3 files)
2. Edit `src/modules/Modules.cpp` - add registration (Step 2.1)
3. Edit `src/main.cpp` - add periodic call (Step 2.2)
4. Test build: `pio run -e rak4631_eth_gw`

### Phase 3: Add Core Enhancements
1. Edit `src/mesh/PacketHistory.h` - add methods (Step 3.1)
2. Edit `src/mesh/PacketHistory.cpp` - add implementations (Step 3.1)
3. Edit `src/mesh/Router.h` - add getters (Step 3.2)
4. Edit `src/memGet.cpp` - fix nRF52 functions (Step 3.3)
5. Edit `src/memGet.h` - add declaration (Step 3.3)
6. Test build: `pio run -e rak4631_eth_gw`

### Phase 4: Configure Variant (RAK4631_ETH_GW)
1. Edit `variants/rak4631_eth_gw/variant.h` - add defines (Step 4.1)
2. Edit `variants/rak4631_eth_gw/variant.cpp` - add variables (Step 4.1)
3. Edit `variants/rak4631_eth_gw/platformio.ini` - add exclusions (Step 4.2)
4. Test build: `pio run -e rak4631_eth_gw`

### Phase 5: Update NodeDB for dynamic_max_nodes
1. Edit `src/mesh/NodeDB.cpp` - replace MAX_NUM_NODES (Step 5.1)
2. Edit `src/mesh/mesh-pb-constants.h` - update assert (Step 5.2)
3. Test build: `pio run -e rak4631_eth_gw`

### Phase 6: Optional RTTTL/GPS Conditionals (if excluding modules)
1. Edit `src/ButtonThread.cpp` - add RTTTL check
2. Edit `src/shutdown.h` - add RTTTL check
3. Edit `src/gps/GPS.h` - add GPS stats check
4. Edit `src/input/ExpressLRSFiveWay.cpp` - add RTTTL check
5. Test build: `pio run -e rak4631_eth_gw`

### Phase 7: Final Testing
1. Build: `pio run -e rak4631_eth_gw`
2. Flash device: `pio run -e rak4631_eth_gw -t upload`
3. Enable in phone app: Settings → Module Config → Range Test → Enable
4. Test commands: Send `/mem`, `/debug`, `/help` as text message

---

## PART 8: Quick Checklist

### Minimum Required (DeviceStatsModule only):
- [ ] Copy DeviceStatsModule.* files (3 files)
- [ ] Edit Modules.cpp (add registration)
- [ ] Edit main.cpp (add periodic call)
- [ ] Edit PacketHistory.h/cpp (add statistics)
- [ ] Edit Router.h (add getters)
- [ ] Edit memGet.cpp/h (fix nRF52)
- [ ] Edit variant.h/cpp (add dynamic_max_nodes, PIN)
- [ ] Edit NodeDB.cpp (use dynamic_max_nodes)
- [ ] Edit mesh-pb-constants.h (250 bytes limit)

### For Firmware Size Reduction:
- [ ] Edit platformio.ini variant (add exclusions)
- [ ] Edit ButtonThread.cpp (RTTTL conditional)
- [ ] Edit shutdown.h (RTTTL conditional)
- [ ] Edit GPS.h (GPS stats conditional)
- [ ] Edit ExpressLRSFiveWay.cpp (RTTTL conditional)

---

## PART 9: File Summary

### Files to COPY (3 files):
```
src/modules/DeviceStatsModule.cpp
src/modules/DeviceStatsModule.h
src/modules/DeviceStatsModule.md
```

### Files to EDIT - Core Integration (2 files, ~13 lines):
```
src/modules/Modules.cpp          - Add 5 lines (registration)
src/main.cpp                     - Add 8 lines (periodic work + include)
```

### Files to EDIT - Core Enhancements (5 files, ~70 lines):
```
src/mesh/PacketHistory.h         - Add 4 lines (method declarations)
src/mesh/PacketHistory.cpp       - Add 58 lines (implementations)
src/mesh/Router.h                - Add 8 lines (getters)
src/memGet.cpp                   - Add ~75 lines (nRF52 fixes)
src/memGet.h                     - Add 3 lines (declaration)
```

### Files to EDIT - Variant Config (4 files):
```
variants/rak4631_eth_gw/variant.h          - Add ~20 lines
variants/rak4631_eth_gw/variant.cpp        - Add 4 lines
variants/rak4631_eth_gw/platformio.ini     - Add ~300 lines (module exclusions)
src/mesh/mesh-pb-constants.h               - Change 1 line (200→250)
```

### Files to EDIT - NodeDB Support (1 file, ~10 locations):
```
src/mesh/NodeDB.cpp              - Replace MAX_NUM_NODES with dynamic_max_nodes
```

### Files to EDIT - Optional Size Reduction (4 files, ~8 lines):
```
src/ButtonThread.cpp             - Add 4 lines (RTTTL conditional)
src/shutdown.h                   - Add 4 lines (RTTTL conditional)
src/gps/GPS.h                    - Add 5 lines (GPS stats conditional)
src/input/ExpressLRSFiveWay.cpp  - Add 5 lines (RTTTL conditional)
```

---

## PART 10: Testing Checklist

After migration:

1. **Build Test:**
   ```bash
   pio run -e rak4631_eth_gw
   ```
   Should see: `SUCCESS`, RAM ~11.7%, Flash ~38.7%

2. **Flash Test:**
   ```bash
   pio run -e rak4631_eth_gw -t upload
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
   - Send `/monstart 30,123456` (30s interval, PIN)
   - Should receive memory stats every 30 seconds
   - Send `/monstop 123456` to stop

---

## PART 11: Troubleshooting

### Build fails with "deviceStatsModule not declared"
**Fix:** Add `#include "modules/DeviceStatsModule.h"` in main.cpp

### Build fails with "getPacketCount not found"
**Fix:** Copy PacketHistory methods (Step 3.1)

### Commands not working
**Check:** Range Test module enabled in phone app?

### Memory stats show 0
**Fix:** Copy nRF52 memory fixes (Step 3.3)

### "/setmaxnodes not working"
**Fix:** Add dynamic_max_nodes to variant (Step 4.1)

---

## PART 12: Total Lines Changed

| Category | Files | Lines Added | Lines Removed |
|----------|-------|-------------|---------------|
| New Files | 3 | 1,111 | 0 |
| Integration | 2 | 13 | 0 |
| Core Enhancements | 5 | 144 | 0 |
| Variant Config | 4 | 330 | 10 |
| NodeDB Support | 1 | 10 | 10 |
| Size Reduction | 4 | 18 | 0 |
| **TOTAL** | **19** | **~1,626** | **~20** |

**Net impact:** +1,606 lines of code

---

## PART 13: Minimal Migration (DeviceStatsModule Only)

If you ONLY want DeviceStatsModule without firmware size reduction:

### Required Files (11 files, ~240 lines):
1. Copy DeviceStatsModule.* (3 files)
2. Edit Modules.cpp (5 lines)
3. Edit main.cpp (8 lines)
4. Edit PacketHistory.h/cpp (62 lines)
5. Edit Router.h (8 lines)
6. Edit memGet.cpp/h (78 lines)
7. Edit variant.h/cpp (24 lines)
8. Edit NodeDB.cpp (10 replacements)
9. Edit mesh-pb-constants.h (1 line)

**Total:** ~240 lines to add/modify across 11 files

### Skip (if not reducing firmware size):
- platformio.ini exclusions
- RTTTL conditionals
- GPS conditionals
- README.md changes

---

## End of Migration Guide

All changes documented. Follow phases 1-7 for complete migration.
For minimal setup, follow PART 13.

