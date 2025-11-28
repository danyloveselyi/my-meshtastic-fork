# RAK4631 Lite - Optimized Firmware Variant

Custom Meshtastic firmware variant for RAK4631 with aggressive memory optimization and DeviceStatsModule integration.

---

## Overview

**Purpose:** Maximize available RAM for network operations while providing comprehensive monitoring capabilities.

**Key Features (Base Variant):**
- ✅ **256 nodes capacity** (vs standard 80 for NRF52)
- ✅ **1000-1500 nodes capacity** (with virtual backend - see below)
- ✅ **Aggressive module exclusion** - ~40% firmware size reduction
- ✅ **Memory optimizations** - minimal RAM usage for core functions

**Optional Monitoring Add-On:**
- ➕ **DeviceStatsModule** (second commit) for real-time device monitoring via text commands
- ➕ Dynamic node scaling controls when monitoring patch is applied

**Hardware:** WisCore RAK4631 (nRF52840 + SX1262)
- **CPU:** nRF52840 @ 64MHz
- **RAM:** 256KB SRAM
- **Flash:** 1MB internal flash
- **Radio:** Semtech SX1262 LoRa

---

## Build Configuration

### Memory Limits

| Parameter | Value | Standard | Change |
|-----------|-------|----------|--------|
| `MAX_NUM_NODES` | 256 | 80 | +176 nodes |
| `MAX_RX_TOPHONE` | 4 | 32 | -28 packets |

**RAM Usage (estimated):**
- NodeDB: ~64KB (256 nodes × 250 bytes)
- PacketHistory: ~4KB (duplicate detection cache)
- Message queues: ~2KB (reduced from ~16KB)
- **Total:** ~70KB / 256KB RAM (~27%)

---

## Modules Status

### ✅ ENABLED Modules (Base Variant + Optional Monitoring)

| Module | Purpose | RAM | Flash |
|--------|---------|-----|-------|
| **TextMessageModule** | Basic text messaging | Low | ~2KB |
| **DeviceStatsModule** | Device monitoring & statistics (optional add-on) | Low | ~15KB |
| **AdminModule** | Device administration | Low | ~5KB |
| **RoutingModule** | Mesh routing | Low | ~3KB |
| **NeighborInfoModule** | Neighbor tracking | Medium | ~4KB |
| **NodeInfoModule** | Node information | Low | ~2KB |
| **PositionModule** | Position broadcast | Low | ~3KB |
| **ReplyModule** | Auto-reply to messages | Low | ~2KB |
| **StoreForwardModule** | Store & forward (router only) | High | ~8KB |
| **TelemetryModule** | Device/environment telemetry | Low | ~6KB |
| **TracerouteModule** | Network path tracing | Low | ~3KB |
| **DetectionSensorModule** | Motion/presence detection | Low | ~4KB |
| **PaxcounterModule** | WiFi/BT device counting | Low | ~4KB |

**Total Enabled:** 13 modules (~61KB Flash)

---

### ❌ DISABLED Modules (Excluded for Size/RAM)

#### Hardware Interfaces (not present on RAK4631)
| Module | Reason | Flash Saved |
|--------|--------|-------------|
| **GPS** | No GPS hardware | ~15KB |
| **Screen** | No display hardware | ~25KB |
| **I2C** | No I2C sensors | ~8KB |
| **EnvironmentalSensor** | No sensors | ~12KB |
| **ExternalNotification** | No external LEDs/buzzers | ~6KB |

#### Network Services (not needed for mesh node)
| Module | Reason | Flash Saved |
|--------|--------|-------------|
| **WiFi** | No WiFi hardware (NRF52) | ~30KB |
| **Ethernet** | No Ethernet hardware | ~20KB |
| **MQTT** | Requires WiFi/Ethernet | ~18KB |
| **Webserver** | Requires WiFi | ~15KB |
| **UPNP** | Requires WiFi | ~5KB |

#### User Interface (phone app provides UI)
| Module | Reason | Flash Saved |
|--------|--------|-------------|
| **CannedMessages** | Phone app provides typing | ~8KB |
| **RangeTest** | Repurposed for DeviceStats enable/disable | ~6KB |
| **RotaryEncoder** | No hardware | ~5KB |
| **Buzzer/RTTTL** | No audio hardware | ~12KB |
| **CardKB** | No keyboard | ~4KB |
| **TrackballInput** | No trackball | ~3KB |

#### Advanced Features (rarely used)
| Module | Reason | Flash Saved |
|--------|--------|-------------|
| **RemoteHardware** | Not needed | ~10KB |
| **Serial** | Debug only | ~8KB |
| **AudioModule** | No audio codec | ~15KB |
| **Sensor** | Generic sensor (covered by specific) | ~6KB |
| **PowerMon** | INA219/260 power monitoring | ~8KB |

#### Protocol Optimizations
| Feature | Reason | Flash Saved |
|---------|--------|-------------|
| **RadioLib SX127x** | Only SX1262 used | ~15KB |
| **RadioLib SX128x** | Only SX1262 used | ~12KB |
| **RadioLib SX123x** | Only SX1262 used | ~8KB |
| **RadioLib Si443x** | Only SX1262 used | ~10KB |
| **RadioLib RF69** | Only SX1262 used | ~8KB |
| **RadioLib CC1101** | Only SX1262 used | ~8KB |
| **RadioLib NRF24** | Only SX1262 used | ~6KB |
| **RadioLib HC05** | Only SX1262 used | ~4KB |
| **RadioLib ESP8266** | Only SX1262 used | ~5KB |
| **RadioLib LR11x0** | Only SX1262 used | ~12KB |

**Total Excluded:** ~30 modules + 10 radio drivers (~400KB Flash saved)

---

## Firmware Size Comparison

| Variant | Flash Used | RAM Used | Modules | Build Time |
|---------|-----------|----------|---------|------------|
| **rak4631_eth_gw** (standard) | ~650KB (63%) | ~35KB (13.7%) | 43 modules | ~50s |
| **rak4631_lite** (optimized) | ~390KB (38%) | ~30KB (11.7%) | 13 modules | ~30s |
| **Savings** | **-260KB (-40%)** | **-5KB (-2%)** | **-30 modules** | **-40%** |

---

## DeviceStatsModule Commands (Optional)

Available when the monitoring commit is applied. All commands sent via text message to device (enable RangeTest module in phone app first).

### Basic Commands

| Command | Description | Example |
|---------|-------------|---------|
| `/help` | Show all commands | `/help` |
| `/mem` | Memory statistics | `/mem` |
| `/packets` | Packet statistics | `/packets` |
| `/power` | Power status | `/power` |
| `/radio` | Radio configuration | `/radio` |
| `/status` | System status | `/status` |
| `/nodes` | Nodes information | `/nodes` |
| `/debug` | Debug info (queues, DupeCache) | `/debug` |

### Admin Commands (require PIN: 123456)

| Command | Description | Example |
|---------|-------------|---------|
| `/setmaxnodes <nodes>,<pin>` | Set max nodes | `/setmaxnodes 300,123456` |
| `/monstart <interval>,<pin>` | Start monitoring (30-300s) | `/monstart 60,123456` |
| `/monstop <pin>` | Stop monitoring | `/monstop 123456` |

**Example session:**
```
You: /mem
Device: 
📊 Memory Status
Heap: 93/201 KB (46%)
Flash: 108/795 KB (14%)
Nodes: 159/256 (62%)
Online: 45 (28%)

You: /debug
Device:
🔧 Debug Info
RX Q: 2/4 (free:2)
TX Q: 3/16 (free:13)
DupeCache: 89/256 (35%)
Oldest pkt: 245s ⚠️

You: /monstart 60,123456
Device: ✅ Monitoring started (60s interval)

[After 60 seconds]
Device: 
📊 #1 Memory
Heap: 94/201 KB (47%)
Nodes: 161/256 (63%)
...
```

---

## Build Instructions

### Prerequisites
```bash
# Install PlatformIO
pip install platformio

# Or via Homebrew (macOS)
brew install platformio
```

### Build Commands

```bash
# Build firmware
pio run -e rak4631_lite

# Build and flash
pio run -e rak4631_lite -t upload

# Clean build
pio run -e rak4631_lite -t clean

# Monitor serial output
pio device monitor -b 115200
```

### Build Output
```
RAM:   [==        ]  11.7% (used 30044 bytes from 256000 bytes)
Flash: [====      ]  38.0% (used 395420 bytes from 1040384 bytes)
```

---

## Configuration Files

### Key Files

| File | Purpose |
|------|---------|
| `platformio.ini` | Build configuration, module exclusions |
| `variant.h` | Hardware pins, MAX_NUM_NODES override |
| `variant.cpp` | Hardware initialization and pin setup |
| `README.md` | This file |

### Important Build Flags

```ini
# Disable Ethernet (must be first!)
-DHAS_ETHERNET=0

# Include variant directory
-Ivariants/rak4631_lite

# Module exclusions (see platformio.ini for full list)
-DMESHTASTIC_EXCLUDE_GPS=1
-DMESHTASTIC_EXCLUDE_SCREEN=1
-DMESHTASTIC_EXCLUDE_WIFI=1
... (30+ more exclusions)

# RadioLib optimizations (only SX1262)
-DRADIOLIB_EXCLUDE_SX127X=1
-DRADIOLIB_EXCLUDE_SX128X=1
... (10+ more exclusions)
```

---

## Memory Management

### Virtual NodeDB Backend (1000-1500 Nodes)

**NEW:** Virtual backend architecture enables scaling to 1000-1500 nodes using RAM cache + flash backend.

**Architecture:**
- **Flash Backend**: Individual slots for 1000-1500 nodes (320 KB extended FS)
- **RAM Cache**: 200-300 active nodes only (~50-75 KB RAM)
- **Node Index**: Compact structure in RAM tracking all nodes (~24 KB for 1500 nodes)
- **Hot/Cold Split**: Critical routing fields in RAM, full data in flash

**Key Features:**
- Hot data (last_heard, snr) **NEVER written to flash** (updates 100-1000x/min, prevents wear)
- Cold data (user, position) written via LittleFS with copy-on-write (automatic wear leveling)
- Batch updates throttled to 1 minute (prevents excessive writes)
- Protected nodes (local, routers, favorites) never evicted from cache

**Memory Usage:**
- Index: ~24 KB (1500 nodes × 16 bytes)
- Cache: ~50-75 KB (200-300 nodes × 250 bytes)
- **Total RAM**: ~75-100 KB (vs 64 KB for 256 nodes in RAM)
- **Flash**: 320 KB extended FS (80 pages) for node storage

**Wear Leveling:**
- Hot data updates don't touch flash (zero wear)
- Cold data updates use LittleFS copy-on-write (automatic block rotation)
- Each update creates new file, deletes old (distributed wear across blocks)

**Status Display:**
- `/mem` command shows: `Nodes: X/1500 (cache: Y/300)`
- `/nodes` command shows: `Total: X/1500 (cache: Y/300), Flash: Z slots free`

### Optional Dynamic Max Nodes Feature

Requires the monitoring commit. When enabled:
1. Default: 256 nodes (base variant configuration)
2. Admin can increase via `/setmaxnodes` command
3. Cannot decrease below the default limit (safety)
4. Persists across reboots (saved to flash)

**Note:** With virtual backend, `dynamic_max_nodes` now means "RAM cache size" (not total limit).

**Memory calculation (without virtual backend):**
- Each node: ~250 bytes RAM
- 256 nodes = 64KB RAM
- 300 nodes = 75KB RAM (safe)
- 350 nodes = 87.5KB RAM (risky)
- 400+ nodes = 100KB+ RAM (dangerous!)

**Safe limits (without virtual backend):**
- Conservative: 256 nodes (25% RAM)
- Aggressive: 300 nodes (29% RAM)
- Maximum safe: 350 nodes (34% RAM)
- **Do not exceed 400 nodes** (RAM overflow risk)

### Packet Queue Optimizations

| Queue | Standard | Optimized | Purpose |
|-------|----------|-----------|---------|
| `MAX_RX_TOPHONE` | 32 | 4 | Phone connection buffer |
| `MAX_RX_FROMRADIO` | 4 | 4 | Radio RX buffer |
| `MAX_TX_QUEUE` | 16 | 16 | Radio TX buffer |

**Optimization rationale:**
- Phone not always connected → reduce `MAX_RX_TOPHONE` to 4
- Saves ~28 packets × ~256 bytes = ~7KB RAM
- Core mesh routing queues unchanged (stability)

---

## Testing Checklist

**Note:** Steps marked as optional apply only when the monitoring add-on is included.

### After Flashing

1. **Boot Test**
   - [ ] Green LED blinks (device alive)
   - [ ] Device appears in Meshtastic app
   - [ ] Can connect via Bluetooth

2. **Module Enable** *(optional monitoring add-on)*
   - [ ] Open Meshtastic app
   - [ ] Settings → Module Config → Range Test
   - [ ] Toggle "Enable" ON
   - [ ] Save changes

3. **Command Test** *(optional monitoring add-on)*
   - [ ] Send `/help` → receives command list
   - [ ] Send `/mem` → receives memory stats
   - [ ] Send `/debug` → receives debug info
   - [ ] Send `/nodes` → receives node count

4. **Network Test**
   - [ ] Device sends/receives messages
   - [ ] Position updates work
   - [ ] Mesh routing functional
   - [ ] No crashes after 24 hours

5. **Memory Test**
   - [ ] `/mem` shows reasonable values (not 0)
   - [ ] Heap usage < 50%
   - [ ] Node count grows as mesh discovered
   - [ ] No memory leaks over time

---

## Troubleshooting

### Build Issues

**Error: "RAK13800_W5100S.h: No such file"**
```
Fix: Ensure -DHAS_ETHERNET=0 is FIRST in build_flags
```

**Optional build error: "dynamic_max_nodes not defined"**
```
Fix: Include DynamicNodes sources from the monitoring commit or disable the optional feature
```

**Optional build error: "deviceStatsModule not declared"**
```
Fix: Ensure DeviceStatsModule sources are present and headers included
```

### Runtime Issues

**Commands not working** *(optional monitoring add-on)*
```
Solution: Enable Range Test module in phone app
Settings → Module Config → Range Test → Enable
```

**Memory shows 0**
```
Solution: nRF52 memory monitoring requires FreeRTOS functions
Check src/memGet.cpp includes proper nRF52 implementation
```

**Device freezes after flash** *(optional monitoring add-on)*
```
Solution: Stack overflow likely - check for large stack allocations
Optional DeviceStatsModule uses static buffers to avoid this
```

**Heap memory decreasing over time**
```
Solution: Memory leak - check /debug for growing queue sizes
Monitor PacketHistory (DupeCache) size - should be < 256
```

---

## Performance Metrics *(optional monitoring add-on)*

### Typical Runtime Stats

**After 24 hours of operation:**
```
Heap usage: 45-55% (90-110KB used)
Node count: 150-250 nodes
Packet rate: 20-50 packets/min
DupeCache: 80-150 packets
Uptime: stable (no crashes)
```

**Peak load (high traffic area):**
```
Heap usage: 60-70% (120-140KB used)
Node count: 256 nodes (full)
Packet rate: 100-200 packets/min
DupeCache: 200-256 packets
TX queue: 10-16 (near full)
```

**Critical thresholds:**
```
⚠️ Heap > 70% - reduce max nodes
⚠️ DupeCache > 90% - high traffic
⚠️ TX queue > 80% - network congestion
🚨 Heap > 85% - imminent crash risk
```

---

## Upgrade Path

### From Standard RAK4631_ETH_GW

1. Build rak4631_lite firmware
2. Backup current device configuration
3. Flash rak4631_lite firmware
4. Enable Range Test module
5. Test commands via `/help`
6. Restore configuration if needed

**Data preserved:**
- Node database (if within 256 limit)
- Channel configuration
- User preferences
- Position/telemetry settings

**Data lost:**
- Nodes > 256 (truncated)
- WiFi/Ethernet settings (not applicable)
- GPS data (no GPS)
- Screen settings (no screen)

### Future Updates

To update to new Meshtastic version:
1. Follow MIGRATION_GUIDE.md in repository root
2. Copy optional monitoring files (DeviceStatsModule.*, DynamicNodes.cpp) if using the add-on
3. Apply core enhancements (~12 files)
4. Copy variant directory
5. Test build

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2024-10 | Base variant with 256 nodes and aggressive module exclusion |
| 1.1 | 2024-10 | Optional monitoring add-on (DeviceStatsModule + dynamic scaling) |
| 2.0 | 2024-12 | Virtual NodeDB backend: 1000-1500 nodes with RAM cache + flash backend |

---

## Support & Contributions

**Maintainer:** Custom firmware fork  
**Base:** Meshtastic v2.6.x  
**License:** GPL-3.0 (same as Meshtastic)

**Known Issues:**
- None currently

**Roadmap:**
- [ ] Add more monitoring commands
- [ ] Optimize RAM usage further
- [ ] Support other nRF52 devices

---

## ⚠️ Critical Coding Guidelines - Preventing System Hangs

**IMPORTANT:** When modifying code that interacts with flash memory or filesystem operations, follow these rules to prevent system hangs and log corruption:

### 1. **NEVER Log During Flash Operations**

**Problem:** Logging during flash read/write/erase operations corrupts the static `printBuf[160]` buffer in `RedirectablePrint::vprintf`, causing garbled output (`` instead of numbers).

**Rule:** 
- ❌ **DO NOT** use `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, or `LOG_ERROR` inside:
  - `lfs_read()` callback
  - `lfs_prog()` callback  
  - `lfs_erase()` callback
  - Any function that performs direct flash operations
- ✅ **DO** log errors only if absolutely critical (use minimal logging)
- ✅ **DO** log before/after flash operations, not during

**Example (WRONG):**
```cpp
static int lfs_prog(...) {
    LOG_DEBUG("Writing %u bytes...", size);  // ❌ DON'T DO THIS!
    sd_flash_write(...);
    LOG_DEBUG("Write completed");  // ❌ DON'T DO THIS!
}
```

**Example (CORRECT):**
```cpp
static int lfs_prog(...) {
    // CRITICAL: Disable logging during flash operations
    sd_flash_write(...);
    return LFS_ERR_OK;
}
```

### 2. **NEVER Pre-Erase All Pages Before Formatting**

**Problem:** Erasing 80 pages (320 KB) takes 8-10 seconds, causing:
- Watchdog timeouts
- System overload
- Log buffer corruption
- Device hangs on reboot

**Rule:**
- ❌ **DO NOT** call `eraseExtendedFSPages()` or similar functions in `init()`
- ❌ **DO NOT** erase all pages before `lfs_format()`
- ✅ **DO** let LittleFS erase pages automatically via `lfs_erase()` callback
- ✅ **DO** only erase pages when absolutely necessary (e.g., corruption recovery)

**Why:** LittleFS format automatically erases needed pages via the `lfs_erase` callback. Pre-erasing is unnecessary and causes system overload.

### 3. **Always Feed Watchdog During Long Operations**

**Problem:** Long operations (>5 seconds) can trigger watchdog reset.

**Rule:**
- ✅ **DO** call `::nrf52Loop()` and `yield()` frequently during:
  - Filesystem initialization
  - Format operations
  - Large file reads/writes
  - Directory scanning
- ✅ **DO** feed watchdog at least every 2-3 seconds
- ✅ **DO** feed watchdog before/after each flash operation

**Example:**
```cpp
for (uint32_t i = 0; i < count; i++) {
    #ifdef ARCH_NRF52
    ::nrf52Loop();  // Feed watchdog
    yield();        // Allow other tasks
    #endif
    // ... perform operation ...
}
```

### 4. **Avoid Stack Overflow in Flash Callbacks**

**Problem:** Flash callbacks are called from interrupt context or deep call stacks.

**Rule:**
- ✅ **DO** use static/global buffers instead of large stack arrays
- ✅ **DO** keep callback functions small and simple
- ❌ **DO NOT** allocate large arrays on stack in callbacks
- ❌ **DO NOT** use recursion in flash callbacks

**Example (WRONG):**
```cpp
static int lfs_read(...) {
    char buf[1024];  // ❌ Large stack allocation!
    // ...
}
```

**Example (CORRECT):**
```cpp
static char read_buffer[4096];  // ✅ Static buffer
static int lfs_read(...) {
    // Use read_buffer
}
```

### 5. **Minimize Logging Frequency During Heavy Operations**

**Problem:** Too many log messages during heavy operations can:
- Overflow log buffers
- Block execution
- Corrupt output

**Rule:**
- ✅ **DO** log only every N operations (e.g., every 10th or 20th)
- ✅ **DO** use short log messages
- ❌ **DO NOT** log every iteration in tight loops
- ❌ **DO NOT** use complex formatting (float, long strings) during operations

**Example:**
```cpp
// Log every 10 pages, not every page
if (page_number % 10 == 0 || page_number == first_page || page_number == last_page) {
    LOG_INFO("Progress: %u%%", progress);  // ✅ OK
}
```

### 6. **Test After Any Flash-Related Changes**

**Rule:**
- ✅ **DO** test device boot after changes
- ✅ **DO** check logs for corruption (`` symbols)
- ✅ **DO** verify USB connectivity after reboot
- ✅ **DO** test with fresh filesystem (format scenario)

**Red Flags:**
- Logs show `` instead of numbers → logging during flash operations
- Device doesn't boot after flash → watchdog timeout or stack overflow
- USB not visible → system hang during initialization

### Summary Checklist

Before committing code that touches flash/filesystem:

- [ ] No `LOG_*` calls inside `lfs_read()`, `lfs_prog()`, `lfs_erase()`
- [ ] No pre-erasing all pages before format
- [ ] Watchdog fed during long operations
- [ ] No large stack allocations in callbacks
- [ ] Minimal logging during heavy operations
- [ ] Tested device boot and USB connectivity
- [ ] Logs display correctly (no corruption)

**Remember:** Flash operations are critical and can easily cause system hangs if not handled carefully!

---

## References

- [Meshtastic Official Docs](https://meshtastic.org)
- [RAK4631 Hardware Docs](https://docs.rakwireless.com/Product-Categories/WisBlock/RAK4631)
- [DeviceStatsModule Documentation](../../src/modules/DeviceStatsModule.md)
- [Migration Guide](../../MIGRATION_GUIDE.md)

---

## License

GPL-3.0 - Same as Meshtastic project

**Disclaimer:** This is a custom firmware variant. Use at your own risk. Not officially supported by Meshtastic project.


