# DeviceStatsModule

Custom module for mesh network device monitoring and diagnostics via text messages.

## Overview

DeviceStatsModule is a **completely isolated custom module** that provides interactive device monitoring commands through text messages. All custom functionality is contained within this module, with zero modifications to the core Meshtastic firmware.

## Features

### Interactive Commands
Send these commands as direct text messages to the node:

- `/mem`, `/memory` - Memory usage (heap, flash, queues)
- `/packets`, `/packet` - Packet statistics (TX/RX rates, duplicates, airtime)
- `/power`, `/battery` - Battery level, voltage, charging status
- `/radio`, `/rf` - Radio configuration and traffic
- `/status`, `/info` - Device uptime, name, role, reboot count
- `/nodes`, `/network` - Network node statistics (online/offline/total)
- `/debug`, `/dbg` - **Critical debug info with DupeCache monitoring**
- `/help`, `/?`, `/commands` - List all available commands
- `/maxnodes` - Show current max nodes setting
- `/setmaxnodes` - Increase max nodes (requires PIN)
- `/monstart` - Start periodic monitoring (requires PIN and interval)
- `/monstop` - Stop periodic monitoring (requires PIN)

### Periodic Monitoring
- Automatic memory stats reports at configurable intervals (default: 30 seconds)
- PIN-protected activation (`/monstart interval,PIN`)
- Sequential message counter for tracking reports
- Low-memory detection and automatic monitoring disable

### DupeCache Monitoring (Critical Feature)
The `/debug` command provides critical insights into packet deduplication:
- **Oldest packet age** - most important metric for network health
- Automatic warnings when packets are evicted prematurely
- Memory usage tracking for NodeDB and DupeCache
- Real-time network queue status (RX/TX)

**Warning levels:**
- 🔴 **CRITICAL**: Oldest packet < 90s (high retransmission risk)
- 🟡 **WARNING**: Oldest packet < 120s (DupeCache filling fast)
- 🟠 **ALERT**: Cache fullness > 85%

## Architecture

### Complete Isolation
```
TextMessageModule (30 lines)
  └── Original Meshtastic code
  └── Saves messages to devicestate
  └── Returns CONTINUE (allows other modules)

DeviceStatsModule (793 lines)
  └── All custom monitoring functionality
  └── Independent message processing
  └── No dependency on TextMessageModule
  └── Returns CONTINUE (doesn't block other modules)
```

### How It Works
1. Text message arrives at `TEXT_MESSAGE_APP` port
2. `TextMessageModule` processes it first (saves to display)
3. `DeviceStatsModule` receives same message (chain of responsibility pattern)
4. If message is a command (`/mem`, etc), `DeviceStatsModule` sends reply
5. Both modules return `CONTINUE`, allowing others to see the message

### Memory Footprint
- **Code**: ~793 lines (~25KB compiled)
- **RAM**: Minimal static variables (~200 bytes)
- **Flash**: Included in module build (~20KB)

## Integration Points

### 1. Module Initialization (`src/modules/Modules.cpp`)
```cpp
#if !MESHTASTIC_EXCLUDE_DEVICESTATS
#include "modules/DeviceStatsModule.h"
#endif

// In setupModules():
#if !MESHTASTIC_EXCLUDE_DEVICESTATS
    deviceStatsModule = new DeviceStatsModule();
#endif
```

### 2. Periodic Work (`src/main.cpp`)
```cpp
#if !MESHTASTIC_EXCLUDE_DEVICESTATS
#include "modules/DeviceStatsModule.h"
#endif

// In loop():
#if !MESHTASTIC_EXCLUDE_DEVICESTATS
    if (deviceStatsModule) {
        deviceStatsModule->doPeriodicWork();
    }
#endif
```

## Enable/Disable

### To Enable (default)
Module is enabled by default. No configuration needed.

### To Disable
Add to `platformio.ini` in your variant's `build_flags`:
```ini
build_flags =
    ${nrf52840_base.build_flags}
    -DMESHTASTIC_EXCLUDE_DEVICESTATS=1
```

This will:
- Skip module compilation
- Remove all code from firmware
- Save ~20KB flash memory
- No runtime overhead

### Verify Module Status
Check at compile time:
```bash
pio run -e your_variant | grep "DeviceStatsModule"
```

If module is enabled, you'll see:
```
Compiling .pio/build/your_variant/src/modules/DeviceStatsModule.cpp.o
```

## Customization

### Change PIN Code
Edit `variants/your_variant/variant.h`:
```cpp
#define MONITORING_PIN_CODE "1234"  // Change to your PIN
```

### Change Default Max Nodes
Edit `variants/your_variant/variant.cpp`:
```cpp
const uint32_t DEFAULT_MAX_NODES = 256;  // Adjust for your needs
```

### Add New Commands
1. Add command handler in `DeviceStatsModule::sendAutoReply()`
2. Create formatting function (e.g., `formatMyNewStats()`)
3. Update `/help` command list

## Safety Features

### Memory Safety
- All formatting functions validate buffer sizes
- Safe `snprintf` with overflow protection
- Null termination guarantees
- Stack overflow prevention (static buffers)

### Long-term Operation
- Counter overflow handling (wraps safely after ~136 years)
- millis() overflow handling
- Automatic monitoring disable on memory exhaustion
- Low-memory detection and warnings

### PIN Protection
- Interactive commands require PIN verification
- Prevents unauthorized max nodes changes
- Protects monitoring activation

## Technical Details

### Port Number
Uses `meshtastic_PortNum_TEXT_MESSAGE_APP` (same as TextMessageModule)
- Both modules receive all text messages
- Chain of responsibility pattern
- No conflicts (both return CONTINUE)

### Message Processing
```
User sends: "/mem"
  ↓
TextMessageModule::handleReceived()
  └── Save to devicestate
  └── Return CONTINUE
  ↓
DeviceStatsModule::handleReceived()
  └── Detect command "/mem"
  └── Call formatMemoryStats()
  └── Send reply to user
  └── Return CONTINUE
```

### Inheritance
```cpp
class DeviceStatsModule : public SinglePortModule, 
                          public Observable<const meshtastic_MeshPacket *>
```
- Inherits from `SinglePortModule` (automatic registration)
- Observable pattern for event notifications
- Standard Meshtastic module lifecycle

## Troubleshooting

### Commands Not Working
1. Check module is enabled: `#if !MESHTASTIC_EXCLUDE_DEVICESTATS`
2. Verify compilation: Look for `DeviceStatsModule.cpp.o` in build log
3. Check message is direct (not broadcast)
4. Verify command syntax: `/mem` not `mem`

### Memory Monitoring Not Starting
1. Verify PIN code is correct
2. Check format: `/monstart interval,PIN` (e.g., `30,1234`)
3. Interval range: 10-86400 seconds
4. Ensure enough free memory (>4KB)

### Module Conflicts
If you see duplicate replies:
1. Check only one instance of DeviceStatsModule exists
2. Verify TextMessageModule is not modified to send auto-replies
3. Ensure CONTINUE is returned (not STOP)

## Future Enhancements

Potential improvements:
- JSON output format option
- Statistics history/trending
- Configurable warning thresholds
- Web dashboard integration
- MQTT statistics publishing
- Bluetooth LE statistics API

## Author

Custom extension for Meshtastic firmware
- Module isolation architecture
- Zero core modifications
- Production-ready code quality

## License

Same as Meshtastic firmware (GPL v3)

