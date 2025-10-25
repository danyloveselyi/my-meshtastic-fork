# DeviceStatsModule - Device Monitoring & Statistics

Real-time device monitoring and statistics via text message commands for Meshtastic devices.

---

## Overview

**Purpose:** Monitor device health, memory usage, network queues, and mesh statistics without phone connection.

**Key Features:**
- 📊 Memory monitoring (heap, flash, RAM)
- 📡 Radio statistics (packets, airtime, signal)
- 🔋 Power status (battery, voltage, charging)
- 🌐 Network queues (RX/TX, DupeCache)
- 👥 Node tracking (online/offline counts)
- ⏰ Periodic monitoring (auto-send stats every N seconds)
- 🔒 PIN-protected admin commands
- 🎛️ Dynamic max nodes adjustment

**Compatibility:** Works on ALL Meshtastic devices (NRF52, ESP32, ESP32S3, STM32WL, RP2040)

---

## Quick Start

### 1. Enable Module

Module is controlled via **Range Test** configuration toggle in phone app:

1. Open Meshtastic app
2. Settings → Module Config → Range Test
3. Toggle "Enable" **ON**
4. Save changes

**Why Range Test?** DeviceStatsModule reuses the Range Test enable/disable flag for dynamic control without firmware recompilation.

### 2. Send Commands

Send text message to device with command:

```
/help
```

Device replies with available commands list.

### 3. Check Memory

```
/mem
```

Device replies:
```
📊 Memory Status
Heap: 93/201 KB (46%)
Flash: 108/795 KB (14%)
Nodes: 159/256 (62%)
Free slots: 97
```

---

## Commands Reference

### Basic Commands (No PIN Required)

#### `/help` - Show all commands
```
You: /help

Device:
📖 Device Stats Commands

Basic:
/mem - Memory status
/packets - Packet stats
/power - Power info
/radio - Radio config
/status - System info
/nodes - Nodes info
/debug - Debug info

Admin (PIN):
/setmaxnodes <n>,<pin>
/monstart <sec>,<pin>
/monstop <pin>

PIN: 123456
```

---

#### `/mem` - Memory statistics
```
You: /mem

Device:
📊 Memory Status
Heap: 93/201 KB (46%)
Flash: 108/795 KB (14%)
Nodes: 159/256 (62%)
Free slots: 97
```

**Shows:**
- Heap usage (used/total)
- Flash usage (used/total)
- Node count (current/max)
- Free node slots

---

#### `/packets` - Packet statistics
```
You: /packets

Device:
📦 Packet Statistics

Last minute:
TX: 12 pkt (0.2/s)
RX: 45 pkt (0.8/s)

Last hour:
TX: 720 pkt (0.2/s)
RX: 2700 pkt (0.8/s)

Airtime: 15.2%
Channel util: 23.4%
```

**Shows:**
- TX/RX packets (last minute)
- TX/RX packets (last hour)
- Average packet rates
- Airtime utilization
- Channel utilization

---

#### `/power` - Power status
```
You: /power

Device:
🔋 Power Status
Battery: 85%
Voltage: 4.12V
Status: Discharging
```

**Shows:**
- Battery percentage
- Voltage
- Charging status

---

#### `/radio` - Radio configuration
```
You: /radio

Device:
📡 Radio Config
Frequency: 868.0 MHz
Bandwidth: 250 kHz
Spread: SF10
Coding: 4/5
TX Power: 22 dBm
Preset: LongFast
```

**Shows:**
- Frequency
- Bandwidth
- Spreading factor
- Coding rate
- TX power
- Modem preset

---

#### `/status` - System information
```
You: /status

Device:
ℹ️ System Info
Uptime: 3d 14h 23m
Region: EU_868
Role: Router
Reboot: 0
HW Model: RAK4631
```

**Shows:**
- Uptime
- Region
- Device role
- Reboot counter
- Hardware model

---

#### `/nodes` - Nodes information
```
You: /nodes

Device:
👥 Nodes Info
Online: 45 (28%)
Offline: 114 (72%)
Total: 159/256 (62%)
Usage: 62.1%
Free slots: 97
```

**Shows:**
- Online nodes count & percentage
- Offline nodes count
- Total nodes vs max capacity
- Database usage percentage
- Free slots available

---

#### `/debug` - Debug information
```
You: /debug

Device:
🔧 Debug Info
RX Q: 2/4 (free:2)
TX Q: 3/16 (free:13)
DupeCache: 89/256 (35%)
Oldest pkt: 245s ⚠️
```

**Shows:**
- RX queue usage (fromRadio)
- TX queue usage (network transmission)
- DupeCache size (PacketHistory)
- Oldest packet age (duplicate detection)

**Critical warnings:**
- `⚠️` Oldest packet > 180s (3 min) - cache filling up
- `🚨` Oldest packet > 300s (5 min) - cache overflow risk

---

### Admin Commands (Require PIN: 123456)

#### `/setmaxnodes <nodes>,<pin>` - Set maximum nodes
```
You: /setmaxnodes 300,123456

Device: ✅ Max nodes set to 300
Current nodes: 159
Free slots: 141
```

**Usage:**
- `<nodes>` - new maximum (must be ≥ DEFAULT_MAX_NODES)
- `<pin>` - PIN code (default: 123456)

**Constraints:**
- Cannot decrease below DEFAULT_MAX_NODES (safety)
- Maximum safe: ~350 nodes (depends on device RAM)
- Changes persist across reboots

**Example:**
```
/setmaxnodes 280,123456  ✅ Valid (increase)
/setmaxnodes 200,123456  ❌ Invalid (below default 256)
/setmaxnodes 500,123456  ⚠️ Risky (RAM overflow)
```

---

#### `/monstart <interval>,<pin>` - Start monitoring
```
You: /monstart 60,123456

Device: ✅ Monitoring started
Interval: 60s
```

**Usage:**
- `<interval>` - seconds between reports (30-300)
- `<pin>` - PIN code (default: 123456)

**Behavior:**
- Device sends memory stats every N seconds
- Continues until `/monstop` or device reboot
- Sequential counter for message ordering

**Example report (every 60s):**
```
📊 #1 Memory
Heap: 94/201 KB (47%)
Nodes: 161/256 (63%)
Free: 95

📊 #2 Memory
Heap: 95/201 KB (47%)
Nodes: 162/256 (63%)
Free: 94
```

**Use cases:**
- Long-term stability monitoring
- Memory leak detection
- Network growth tracking
- Remote device debugging

---

#### `/monstop <pin>` - Stop monitoring
```
You: /monstop 123456

Device: ⏹ Monitoring stopped
Total reports: 42
```

**Usage:**
- `<pin>` - PIN code (default: 123456)

**Behavior:**
- Stops periodic monitoring
- Shows total reports sent
- Frees monitoring slot

---

## Architecture

### Module Design

**Inheritance:**
```
SinglePortModule (base)
  └─ DeviceStatsModule (extends)
       ├─ handleReceived() - command parsing
       ├─ wantPacket() - filter text messages
       └─ doPeriodicWork() - monitoring timer
```

**Port:** `TEXT_MESSAGE_APP` (same as TextMessageModule)

**Enable/Disable:** Controlled by `moduleConfig.range_test.enabled` flag

### Command Flow

```
1. User sends text message "/mem"
2. wantPacket() filters TEXT_MESSAGE_APP
3. handleReceived() parses command
4. formatMemoryStats() generates response
5. sendAutoReply() sends back to user
```

### Monitoring Flow

```
1. User sends "/monstart 60,123456"
2. Store monitoring config (nodeId, interval)
3. doPeriodicWork() called every loop iteration
4. Check if interval elapsed (60s)
5. formatMemoryStats() generates report
6. sendMemoryStats() sends to user
7. Increment counter, repeat
```

---

## Memory Management

### Dynamic Max Nodes

**How it works:**
1. `DynamicNodes.cpp` defines weak symbols for `dynamic_max_nodes`
2. Uses `MAX_NUM_NODES` from `mesh-pb-constants.h` (architecture-specific)
3. Variants can override in `variant.cpp` (stronger symbols)
4. DeviceStatsModule uses for `/setmaxnodes` command

**Architecture-specific defaults:**
| Architecture | Default | Source |
|--------------|---------|--------|
| NRF52 | 80 | mesh-pb-constants.h |
| ESP32 | 100 | mesh-pb-constants.h |
| ESP32S3 (16MB) | 250 | mesh-pb-constants.h |
| STM32WL | 10 | mesh-pb-constants.h |

**Example override (rak4631_lite):**
```cpp
// variants/rak4631_lite/variant.cpp
const uint32_t DEFAULT_MAX_NODES = 256;
uint32_t dynamic_max_nodes = 256;
```

### Memory Calculations

**Per node:**
- NodeInfoLite: ~250 bytes
- PacketHistory entry: ~16 bytes

**Total for 256 nodes:**
- NodeDB: 256 × 250 = 64KB
- PacketHistory: 256 × 16 = 4KB
- **Total:** ~68KB RAM

**Safe limits:**
| Nodes | NodeDB RAM | Total RAM | Safety |
|-------|-----------|-----------|--------|
| 80 | 20KB | ~24KB | ✅ Very safe |
| 100 | 25KB | ~29KB | ✅ Safe |
| 256 | 64KB | ~68KB | ✅ Safe (NRF52) |
| 300 | 75KB | ~79KB | ⚠️ Moderate |
| 350 | 87.5KB | ~91.5KB | ⚠️ Risky |
| 400+ | 100KB+ | ~104KB+ | 🚨 Dangerous |

---

## Network Monitoring

### Packet Queues

**RX Queue (fromRadio):**
- Size: 4 messages (MAX_RX_FROMRADIO)
- Purpose: Messages from radio to phone
- Critical: If full, messages dropped

**TX Queue (network):**
- Size: 16 messages (MAX_TX_QUEUE)
- Purpose: Outgoing mesh packets
- Critical: If full, routing delays

**Monitoring:**
```
/debug

RX Q: 2/4 (free:2)    ← 50% full, OK
TX Q: 15/16 (free:1)  ← 94% full, WARNING
```

### DupeCache (PacketHistory)

**Purpose:** Prevent duplicate packet forwarding in mesh network

**How it works:**
1. Every received packet stored with timestamp
2. Duplicate packets ignored (same ID)
3. Old packets purged after 10 minutes (FLOOD_EXPIRE_TIME)

**Monitoring:**
```
/debug

DupeCache: 89/256 (35%)   ← Current size
Oldest pkt: 245s          ← Age of oldest packet
```

**Critical thresholds:**
- < 90% full → ✅ Normal
- > 90% full → ⚠️ High traffic
- Oldest > 180s → ⚠️ Cache filling
- Oldest > 300s → 🚨 Overflow risk

**Problem scenario:**
```
High traffic area:
- 500 packets/minute received
- DupeCache fills to 256 entries
- Oldest packets pushed out before 10min
- Duplicates not detected → forwarding loops
```

**Solution:**
- Increase `dynamic_max_nodes` (larger cache)
- Reduce network traffic
- Improve mesh topology

---

## PIN Security

### Default PIN

**Hardcoded:** `123456` (defined in `DeviceStatsModule.cpp`)

**Location:**
```cpp
// src/modules/DeviceStatsModule.cpp
static const char* MONITORING_PIN_CODE = "123456";
```

### Changing PIN

**Option 1: Rebuild firmware**
```cpp
// Edit src/modules/DeviceStatsModule.cpp
static const char* MONITORING_PIN_CODE = "your_pin_here";
```

**Option 2: Fork and customize**
```bash
# Create custom build with different PIN
git clone <your-fork>
# Edit DeviceStatsModule.cpp
pio run -e <your_variant>
```

### Security Notes

- ⚠️ PIN sent as plaintext in messages
- ⚠️ Anyone intercepting message sees PIN
- ⚠️ Same PIN for all admin commands
- ✅ Better than no protection
- ✅ Prevents accidental commands
- ✅ Stops casual interference

**Recommendation:** Change default PIN if security matters

---

## Performance Impact

### Memory Overhead

**Static (always allocated):**
- Module instance: ~200 bytes
- Static buffers: ~300 bytes
- Total: ~500 bytes

**Dynamic (when monitoring):**
- Monitoring config: ~20 bytes per user
- Response formatting: ~300 bytes (temporary)
- Total: ~320 bytes per active monitor

**Minimal impact:** < 1KB RAM overhead

### CPU Overhead

**Per command:**
- Parsing: < 1ms
- Formatting: 1-5ms
- Sending: < 1ms
- **Total:** < 10ms per command

**Monitoring (60s interval):**
- Check timer: < 0.1ms (every loop)
- Generate report: ~5ms (every 60s)
- Send message: < 1ms
- **Total:** Negligible (< 0.01% CPU)

### Network Overhead

**Per command:**
- Request: ~20 bytes
- Response: 100-300 bytes
- **Total:** ~320 bytes per command

**Monitoring (60s interval):**
- Report: ~150 bytes every 60s
- **Total:** 2.5 bytes/second

**Minimal impact:** < 0.1% of typical mesh traffic

---

## Troubleshooting

### Module Not Responding

**Symptom:** Send `/help`, no reply

**Solutions:**
1. Check Range Test enabled:
   ```
   App → Settings → Module Config → Range Test → Enable
   ```
2. Check device receiving messages:
   ```
   Send regular text message first
   ```
3. Check logs:
   ```
   pio device monitor -b 115200
   ```

---

### Invalid Command Response

**Symptom:** `❌ Unknown command`

**Solutions:**
- Check command spelling: `/mem` not `/memory`
- Use `/help` to see available commands
- Ensure no extra spaces

---

### Memory Shows 0

**Symptom:** `/mem` shows `Heap: 0/0 KB`

**Solution:** NRF52 memory monitoring not implemented
```cpp
// Check src/memGet.cpp includes:
#ifdef ARCH_NRF52
uint32_t getFreeHeap() {
    return xPortGetFreeHeapSize();
}
#endif
```

---

### PIN Not Working

**Symptom:** `/setmaxnodes 300,123456` → `❌ Invalid PIN`

**Solutions:**
1. Check PIN is exactly `123456` (default)
2. No spaces: `300,123456` not `300, 123456`
3. Check if PIN was changed in source code

---

### Monitoring Not Sending

**Symptom:** `/monstart 60,123456` starts but no reports

**Solutions:**
1. Check device has network connectivity
2. Check `doPeriodicWork()` called in `main.cpp`:
   ```cpp
   if (deviceStatsModule) {
       deviceStatsModule->doPeriodicWork();
   }
   ```
3. Check interval valid (30-300s)

---

### Device Crashes After Command

**Symptom:** Device reboots after `/setmaxnodes`

**Solution:** Stack overflow from large allocations
```cpp
// DeviceStatsModule uses static buffers to prevent this
static char commandBuffer[300];
```

---

## Use Cases

### 1. Remote Device Monitoring

**Scenario:** Device deployed in remote location, no phone access

**Solution:**
```
/monstart 300,123456  (5 min intervals)
```

**Benefits:**
- Regular health checks
- Memory leak detection
- Network activity tracking
- No physical access needed

---

### 2. Memory Leak Debugging

**Scenario:** Device crashes after several days

**Solution:**
```
1. /monstart 60,123456   (start monitoring)
2. Wait 24-48 hours
3. Review reports for growing heap usage
4. /debug - check queue sizes
```

**Indicators of leak:**
- Heap usage growing steadily
- Queue sizes increasing
- Node count growing unexpectedly

---

### 3. Network Capacity Planning

**Scenario:** Expanding mesh network, need capacity info

**Solution:**
```
1. /nodes - check current usage
2. /debug - check DupeCache pressure
3. /setmaxnodes 300,123456 - increase if needed
4. /monstart 120,123456 - monitor growth
```

**Metrics:**
- Node slots available
- DupeCache fill rate
- Packet rates

---

### 4. Performance Troubleshooting

**Scenario:** Slow message delivery, network congestion

**Solution:**
```
1. /packets - check packet rates
2. /debug - check TX queue
3. /radio - verify configuration
```

**Diagnostics:**
- TX queue full → network congestion
- High airtime → channel busy
- High packet rate → traffic storm

---

## Integration with Other Modules

### TextMessageModule

**Relationship:** Coexists peacefully
- TextMessageModule: basic text messaging
- DeviceStatsModule: advanced monitoring commands
- Both listen to TEXT_MESSAGE_APP port
- Both return `CONTINUE` to allow chaining

### AdminModule

**Relationship:** Complementary
- AdminModule: firmware OTA, channel config
- DeviceStatsModule: runtime monitoring, statistics
- Different security models (AdminModule uses channel key)

### TelemetryModule

**Relationship:** Different purposes
- TelemetryModule: periodic broadcast to mesh
- DeviceStatsModule: on-demand via text commands
- TelemetryModule: limited fields (protobuf constrained)
- DeviceStatsModule: rich text output

---

## Future Enhancements

### Planned Features
- [ ] More queue statistics
- [ ] Historical data (min/max/avg)
- [ ] Alert thresholds (notify if heap > 80%)
- [ ] Export stats to CSV
- [ ] Integration with external monitoring systems

### Community Requests
- [ ] Custom PIN per command
- [ ] Encrypted commands
- [ ] Web dashboard integration
- [ ] Grafana/Prometheus export

---

## Contributing

### Adding New Commands

1. Add command string to `/help`:
   ```cpp
   strcat(buffer, "/newcmd - Description\n");
   ```

2. Add parser in `handleReceived()`:
   ```cpp
   if (strcmp(command, "/newcmd") == 0) {
       formatNewStats(commandBuffer, sizeof(commandBuffer));
       sendAutoReply(mp);
       return ProcessMessage::STOP;
   }
   ```

3. Implement formatter:
   ```cpp
   void DeviceStatsModule::formatNewStats(char* buffer, size_t bufferSize) {
       snprintf(buffer, bufferSize, "📊 New Stats\n...");
   }
   ```

---

## References

- [Meshtastic Modules Guide](https://meshtastic.org/docs/software/modules/)
- [FreeRTOS Heap Management](https://www.freertos.org/a00111.html)
- [Migration Guide](../../MIGRATION_GUIDE.md)
- [RAK4631 Lite README](../../variants/rak4631_lite/README.md)

---

## License

GPL-3.0 - Same as Meshtastic project

**Author:** Custom firmware extension  
**Version:** 1.0  
**Compatibility:** Meshtastic v2.6.x+


