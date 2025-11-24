# Extended Filesystem for NodeDB

## Overview

This variant implements a **dual filesystem** architecture for RAK4631 Lite:

1. **Main filesystem (28 KB, 7 pages)** - used for configuration files:
   - `config.proto` - device settings
   - `channels.proto` - channels configuration
   - `device.proto` - device state
   - `module.proto` - module configuration
   - `uiconfig.proto` - UI settings

2. **Extended filesystem (320 KB, 80 pages)** - used **ONLY** for NodeDB:
   - `nodes.proto` - node database (can store 400+ nodes)

## Benefits

- ✅ Main filesystem remains standard (28 KB) - **no changes to framework**
- ✅ NodeDB gets 320 KB (80 pages) - sufficient for 400+ nodes
- ✅ Safety: main filesystem never overflows
- ✅ Compatibility: standard Meshtastic framework used for main filesystem

## Configuration

### In `platformio.ini`:

```ini
platform_packages =
  ; Standard Meshtastic framework (7 pages, 28 KB) - for main filesystem
  platformio/framework-arduinoadafruitnrf52 @ https://github.com/meshtastic/Adafruit_nRF52_Arduino#e13f5820002a4fb2a5e6754b42ace185277e5adf

build_flags =
  ...
  -DUSE_EXTENDED_FS_FOR_NODEDB  ; Enable extended filesystem for NodeDB
```

### Custom fork NOT required!

Extended filesystem is created **manually** via direct LittleFS API calls, so a custom fork with 80 pages is **not required**. Standard framework is used for main filesystem.

## Architecture

### Main filesystem (28 KB)
- **Address**: `0xED000` (standard Meshtastic address)
- **Size**: 7 pages × 4 KB = 28 KB
- **Usage**: All config files except `nodes.proto`

### Extended filesystem (320 KB)
- **Address**: `0x80000` (after application, limited by linker script)
- **Size**: 80 pages × 4 KB = 320 KB
- **Usage**: Only `nodes.proto`
- **Protection**: 144 KB gap to bootloader (0xF4000)

## Files

- `NodeDB-extended-fs.cpp` - extended filesystem implementation
- `NodeDB-extended-fs.h` - header file with function declarations
- `FSCommon-fallback.cpp` - extended filesystem initialization and diagnostics

## Initialization

Extended filesystem is automatically initialized in `fsInitExtended()` after mounting the main filesystem (called from `FSCommon.cpp`).

## Current Status

✅ **Compilation**: Successful  
✅ **Initialization**: Implemented  
✅ **Mounting**: Implemented  
✅ **Integration with NodeDB**: **FULLY IMPLEMENTED**
  - `NodeDB::loadProto()` automatically detects `nodes.proto` and uses extended FS
  - `NodeDB::saveProto()` uses `FilesystemType::EXTENDED_FS` for `nodes.proto`
  - `NodeDB::saveNodeDatabaseToDisk()` includes throttling and radio state checks

## Implementation Details

### Load Integration
- `loadProto()` checks `isNodeDBFile(filename)` to detect `/prefs/nodes.proto`
- If extended FS is available and file is NodeDB, uses `lfs_file_open()` directly
- Reads entire file into buffer, then decodes protobuf
- Falls back to main FS if extended FS unavailable

### Save Integration
- `saveProto()` uses `FilesystemType::EXTENDED_FS` enum for `nodes.proto`
- Creates directories via `lfs_mkdir()` if needed
- Uses streaming encoding via `lfs_writecb` (no buffer allocation)
- Calls `lfs_file_sync()` before closing for data integrity
- Verifies save by checking file exists in extended FS

### Throttling and Optimization
- `saveNodeDatabaseToDisk()` implements 1-minute throttling (same as `updateUser()`)
- Checks radio state before write to avoid packet loss during reception
- Waits up to 200ms for packet reception to complete
- Immediate saves bypass throttling (for critical operations like reset/remove)

## Safety

- ✅ Bootloader overlap check (144 KB gap maintained)
- ✅ Protection against bootloader page erasure
- ✅ Automatic formatting on first initialization
- ✅ Fallback to main filesystem if extended FS unavailable
- ✅ Radio state checking to prevent packet loss during flash writes
