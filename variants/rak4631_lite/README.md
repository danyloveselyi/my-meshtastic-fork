# RAK4631 Lite - Optimized Firmware Variant

Custom Meshtastic firmware variant for RAK4631 with aggressive memory optimization and DeviceStatsModule integration.

---

## Overview

**Purpose:** Maximize available RAM for network operations while providing comprehensive monitoring capabilities.

**Key Features:**
- ✅ **256 nodes capacity** (vs standard 80 for NRF52)
- ✅ **DeviceStatsModule** - real-time device monitoring via text commands
- ✅ **Aggressive module exclusion** - ~40% firmware size reduction
- ✅ **Memory optimizations** - minimal RAM usage for core functions

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
| `DEFAULT_MAX_NODES` | 256 | 80 | +176 nodes |

**RAM Usage (estimated):**
- NodeDB: ~64KB (256 nodes × 250 bytes)
- PacketHistory: ~4KB (duplicate detection cache)
- Message queues: ~2KB (reduced from ~16KB)
- **Total:** ~70KB / 256KB RAM (~27%)

---

## Modules Status

### ✅ ENABLED Modules (Core + Monitoring)

| Module | Purpose | RAM | Flash |
|--------|---------|-----|-------|
| **TextMessageModule** | Basic text messaging | Low | ~2KB |
| **DeviceStatsModule** | Device monitoring & statistics | Low | ~15KB |
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

## DeviceStatsModule Commands

All commands sent via text message to device (enable RangeTest module in phone app first).

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
| `variant.cpp` | dynamic_max_nodes initialization (256) |
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

### Dynamic Max Nodes Feature

**How it works:**
1. Default: 256 nodes (defined in `variant.cpp`)
2. Admin can increase via `/setmaxnodes` command
3. Cannot decrease below DEFAULT_MAX_NODES (safety)
4. Persists across reboots (saved to flash)

**Memory calculation:**
- Each node: ~250 bytes RAM
- 256 nodes = 64KB RAM
- 300 nodes = 75KB RAM (safe)
- 350 nodes = 87.5KB RAM (risky)
- 400+ nodes = 100KB+ RAM (dangerous!)

**Safe limits:**
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

### After Flashing

1. **Boot Test**
   - [ ] Green LED blinks (device alive)
   - [ ] Device appears in Meshtastic app
   - [ ] Can connect via Bluetooth

2. **Module Enable**
   - [ ] Open Meshtastic app
   - [ ] Settings → Module Config → Range Test
   - [ ] Toggle "Enable" ON
   - [ ] Save changes

3. **Command Test**
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

**Error: "dynamic_max_nodes not defined"**
```
Fix: Ensure variant.cpp includes dynamic_max_nodes definition
```

**Error: "deviceStatsModule not declared"**
```
Fix: Check main.cpp includes DeviceStatsModule.h
```

### Runtime Issues

**Commands not working**
```
Solution: Enable Range Test module in phone app
Settings → Module Config → Range Test → Enable
```

**Memory shows 0**
```
Solution: nRF52 memory monitoring requires FreeRTOS functions
Check src/memGet.cpp includes proper nRF52 implementation
```

**Device freezes after flash**
```
Solution: Stack overflow likely - check for large stack allocations
DeviceStatsModule uses static buffers to avoid this
```

**Heap memory decreasing over time**
```
Solution: Memory leak - check /debug for growing queue sizes
Monitor PacketHistory (DupeCache) size - should be < 256
```

---

## Performance Metrics

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
2. Copy 4 core files (DeviceStatsModule.*, DynamicNodes.cpp)
3. Apply core enhancements (~12 files)
4. Copy variant directory
5. Test build

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2024-10 | Initial release with DeviceStatsModule |
| | | - 256 nodes capacity |
| | | - Aggressive module exclusion |
| | | - Text-based monitoring |
| | | - Weak linkage for cross-platform compatibility |

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

## References

- [Meshtastic Official Docs](https://meshtastic.org)
- [RAK4631 Hardware Docs](https://docs.rakwireless.com/Product-Categories/WisBlock/RAK4631)
- [DeviceStatsModule Documentation](../../src/modules/DeviceStatsModule.md)
- [Migration Guide](../../MIGRATION_GUIDE.md)

---

## License

GPL-3.0 - Same as Meshtastic project

**Disclaimer:** This is a custom firmware variant. Use at your own risk. Not officially supported by Meshtastic project.


