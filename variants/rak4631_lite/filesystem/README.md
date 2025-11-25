# Filesystem Structure for RAK4631 Lite

This folder contains all filesystem logic for the RAK4631 Lite variant.

## Folder Structure

```
filesystem/
├── main/              # Main filesystem (7 pages, 28 KB)
│   ├── main-fs-pre-init.cpp      # Pre-mount preparation (preFSBegin)
│   └── main-fs-diagnostics.cpp  # Diagnostics and initialization (fsInitExtended)
├── nodedb/            # Extended filesystem for NodeDB (80 pages, 320 KB)
│   ├── NodeDBExtendedFSImpl.h/cpp      # Extended FS implementation (initialization, load/save)
│   └── NodeDBFilesystemAdapter.h/cpp    # Adapter for NodeDB integration (routing)
└── linker/            # Linker scripts for memory management
    └── nrf52840_s140_v7.ld      # Script to limit application size
```

## Main Filesystem (main/)

**Purpose**: Stores configuration files and general application data.

**Address**: `0xED000` (standard Meshtastic address)  
**Size**: 7 pages (28 KB) - standard Meshtastic configuration.  
**Usage**: `config.proto`, `channels.proto`, `device.proto`, `module.proto`, `uiconfig.proto`

**Files**:
- `main-fs-pre-init.cpp` - `preFSBegin()` function:
  - Called BEFORE filesystem mounting
  - Prepares flash pages
  - Checks and fixes corruption
  - Erases pages if necessary
  
- `main-fs-diagnostics.cpp` - `fsInitExtended()` function:
  - Called AFTER main filesystem mounting
  - Performs detailed diagnostics
  - Logs filesystem status
  - May initialize extended filesystem (if enabled)

## Extended Filesystem for NodeDB (nodedb/)

**Purpose**: Stores node database (nodes.proto) separately from main filesystem.

**Address**: `0x80000` (separate flash region, after application)  
**Size**: 80 pages (320 KB) - sufficient for 400+ nodes.  
**Usage**: Only `nodes.proto` (node database from network)

**Files**:
- `NodeDBExtendedFSImpl.h/cpp` - Extended FS implementation via LittleFS API (initialization, load/save protobuf)
- `NodeDBFilesystemAdapter.h/cpp` - Adapter integrating extended FS with main NodeDB (routing)

**Usage**: Enabled via `USE_EXTENDED_FS_FOR_NODEDB` flag in `platformio.ini`.

## Linker Scripts (linker/)

**Purpose**: Flash memory allocation management.

**Files**:
- `nrf52840_s140_v7.ld` - Limits application size to 0x80000 (356 KB), protecting extended FS from overwrite.

**Status**: Temporarily disabled (commented in `platformio.ini`).  
**Note**: Extended FS is located at 0x80000, so application must not exceed this address.

## How It Works

1. **Application startup**:
   - `preFSBegin()` prepares flash pages
   - `FSBegin()` mounts main filesystem (7 pages)
   - `fsInitExtended()` performs diagnostics and initializes extended FS (if enabled)

2. **Data operations**:
   - Configuration files → main FS (0xED000, 7 pages, 28 KB)
   - Node database (nodes.proto) → extended FS (0x80000, 80 pages, 320 KB, if enabled)

3. **Integration**:
   - `NodeDBFilesystemAdapter` automatically selects which FS to use
   - If extended FS is available → uses it
   - If not → fallback to main FS

## Benefits of This Structure

✅ **Separation of concerns**: Main FS for configs, extended FS for large data  
✅ **Scalability**: Easy to add new filesystem types  
✅ **Clarity**: File and folder names explain their purpose  
✅ **Isolation**: Filesystem logic is separated from main code
