# Filesystem Structure for RAK4631 Lite

This folder contains all filesystem logic for the RAK4631 Lite variant, unified into `FilesystemUnified.h` and `FilesystemUnified.cpp`.

## Files

- **`FilesystemUnified.h`** - Unified header with all filesystem declarations
- **`FilesystemUnified.cpp`** - Unified implementation (1508 lines)
- **`linker/nrf52840_s140_v7.ld`** - Linker script for memory management

## Main Filesystem (7 pages, 28 KB)

**Purpose**: Stores configuration files and general application data.

**Address**: `0xED000` (standard Meshtastic address)  
**Size**: 7 pages (28 KB)  
**Usage**: `config.proto`, `channels.proto`, `device.proto`, `module.proto`, `uiconfig.proto`

## Extended Filesystem for NodeDB (68 pages, 272 KB)

**Purpose**: Stores node database (`nodes.proto`) separately from main filesystem.

**Address**: `0xA8000` (separate flash region, after application)  
**Size**: 68 pages (272 KB) - sufficient for 2,000-2,400 nodes  
**Usage**: Only `/prefs/nodes.proto` (node database from network)

**Capacity calculation**: Based on actual node slot sizes (69-82 bytes data + ~20-50 bytes LittleFS overhead = ~100-130 bytes per node)

**Enabled via**: `USE_EXTENDED_FS_FOR_NODEDB` flag in `platformio.ini`

## Dependencies and External Usage

### Files That Include FilesystemUnified

#### 1. `src/mesh/NodeDB.cpp`
**Include**: `#include "../../variants/rak4631_lite/filesystem/FilesystemUnified.h"`

**Used Methods**:
- `ExtendedFilesystemModule::loadProto()` - Load protobuf files (uses extended FS for `nodes.proto`, fallback to main FS for others)
- `ExtendedFilesystemModule::saveProto()` - Save protobuf files (uses extended FS for `nodes.proto`, main FS for others)
- `ExtendedFilesystemModule::saveNodeDatabaseToDisk()` - Save node database with radio state checking and throttling
- `ExtendedFilesystemModule::forceReformat()` - Force reformat extended filesystem (factory reset)

**Usage Context**:
- `NodeDB::loadProto()` - Delegates to `ExtendedFilesystemModule::loadProto()` when `USE_EXTENDED_FS_FOR_NODEDB` is enabled
- `NodeDB::saveProto()` - Delegates to `ExtendedFilesystemModule::saveProto()` when `USE_EXTENDED_FS_FOR_NODEDB` is enabled
- `NodeDB::saveNodeDatabaseToDisk()` - Delegates to `ExtendedFilesystemModule::saveNodeDatabaseToDisk()` for variant-specific optimizations
- Factory reset - Calls `ExtendedFilesystemModule::forceReformat()` to clear extended filesystem

#### 2. `src/FSCommon.cpp`
**Include**: `#include "../../variants/rak4631_lite/filesystem/FilesystemUnified.h"`

**Used Functions**:
- `getFiles()` - Patched version with `FEED_WATCHDOG_AND_YIELD()` calls to prevent watchdog timeouts during filesystem scans
- `fsInit_patched()` - Patched version of `fsInit()` that initializes extended filesystem before BLE setup
- `preFSBegin()` - Pre-initialization function called before mounting main filesystem (checks corruption flags, prepares flash)
- `initExtendedFilesystemForNodeDB()` - C wrapper for extended filesystem initialization (called from `fsInit_patched()`)

**Usage Context**:
- `getFiles()` replaces standard implementation when `USE_EXTENDED_FS_FOR_NODEDB` is enabled (via conditional compilation)
- `fsInit_patched()` replaces weak `fsInit()` function when `USE_EXTENDED_FS_FOR_NODEDB` is enabled
- `preFSBegin()` is called before `FSBegin()` to prepare filesystem

#### 3. `src/modules/devicestats/DeviceStatsModule.cpp`
**Include**: `extern` declarations (no direct include)

**Used Functions**:
- `getMainFSStats()` - Get main filesystem statistics (total, used, free bytes)
- `getExtendedFSStats()` - Get extended filesystem statistics (total, used, free bytes)

**Usage Context**:
- `DeviceStatsModule::formatDetailedMemoryStats()` - Displays filesystem statistics in device stats

#### 4. `variants/rak4631_lite/nodedb/NodeDBUnified.cpp`
**Include**: `#include "../filesystem/FilesystemUnified.h"`

**Used Methods**:
- `ExtendedFilesystemModule::getExtendedFS()` - Get LittleFS instance for extended filesystem (for direct file operations)
- `ExtendedFilesystemModule::getStats()` - Get extended filesystem statistics (for diagnostics)
- `ExtendedFilesystemModule::shouldProceedWithSave()` - Check if NodeDB save should proceed (radio state checking)

**Usage Context**:
- `NodeIndex::loadFromFlash()` - Uses `getExtendedFS()` to access extended filesystem for index loading
- `NodeDBVirtualBackend::getDiagnostics()` - Uses `getStats()` to display filesystem statistics
- `NodeDBVirtualBackend::saveToDisk()` - Uses `shouldProceedWithSave()` to check radio state before saving

#### 5. `src/mesh/PhoneAPI.cpp`
**Indirect Usage**: Uses `getFiles()` through `FSCommon.cpp` (no direct include)

**Used Functions**:
- `getFiles()` - Get list of files in directory (via `FSCommon.cpp`)

**Usage Context**:
- `PhoneAPI::handleFilesManifest()` - Lists files for phone app

### Summary of External API

#### ExtendedFilesystemModule Class (Public Methods)
```cpp
// Initialization
static bool init();
static bool isAvailable();
static lfs_t* getExtendedFS();

// File Operations
static LoadFileResult loadProto(const char *filename, size_t protoSize, size_t objSize, 
                                const pb_msgdesc_t *fields, void *dest_struct);
static bool saveProto(const char *filename, size_t protoSize, const pb_msgdesc_t *fields,
                      const void *dest_struct, bool fullAtomic);
static bool saveNodeDatabaseToDisk(uint32_t &lastNodeDbSave);

// Utilities
static bool forceReformat();
static bool getStats(uint32_t* total, uint32_t* used, uint32_t* free);
static bool shouldProceedWithSave(uint32_t lastSaveTime);
static bool isNodeDBFile(const char* filename);
```

#### Global Functions (FSCommon Patches)
```cpp
// Filesystem Operations
std::vector<meshtastic_FileInfo> getFiles(const char *dirname, uint8_t levels);
void fsInit_patched();
void preFSBegin();

// Statistics
bool getMainFSStats(uint32_t* total, uint32_t* used, uint32_t* free);
bool getExtendedFSStats(uint32_t* total, uint32_t* used, uint32_t* free);

// C Wrapper
extern "C" void initExtendedFilesystemForNodeDB();
```

## How It Works

1. **Application startup**:
   - `preFSBegin()` prepares flash pages and checks corruption flags
   - `fsInit_patched()` mounts main filesystem (7 pages) and initializes extended filesystem (68 pages)
   - Extended FS is initialized BEFORE BLE setup to avoid conflicts

2. **Data operations**:
   - Configuration files → main FS (0xED000, 7 pages, 28 KB)
   - Node database (`nodes.proto`) → extended FS (0xA8000, 68 pages, 272 KB, if enabled)
   - Other files → main FS (fallback)

3. **Integration**:
   - `ExtendedFilesystemModule::loadProto()` and `saveProto()` automatically detect `nodes.proto` and route to extended FS
   - `isNodeDBFile()` checks if filename is `/prefs/nodes.proto`
   - Fallback to main FS if extended FS is not available

## Benefits

✅ **Separation of concerns**: Main FS for configs, extended FS for large data  
✅ **Scalability**: 272 KB for 2,000-2,400 nodes (based on actual slot sizes)  
✅ **Performance**: Radio state checking prevents packet loss during saves  
✅ **Reliability**: Automatic fallback to main FS if extended FS fails  
✅ **Unified codebase**: All filesystem logic in one place for easier maintenance

## Code Quality and Refactoring

### Duplication Elimination

The codebase has been refactored to eliminate code duplication:

#### 1. **Error Handling Helpers**
- `logSoftDeviceError()` - Centralized logging for SoftDevice errors (FORBIDDEN, INVALID_ADDR, etc.)
- `cleanupAsyncOperation()` - Unified cleanup of async flash operations
- **Usage**: Replaces duplicated error handling in `lfs_prog()` and `lfs_erase()`

#### 2. **Async Operation Helpers**
- `handleAsyncFlashWrite()` - Unified async write operation handling
- `handleAsyncFlashErase()` - Unified async erase operation handling
- **Usage**: Replaces duplicated async operation patterns in `lfs_prog()` and `lfs_erase()`

#### 3. **File Operations Helper**
- `safeCloseFile()` - Safe file closing with error handling and optional file removal
- **Usage**: Replaces all direct `lfs_file_close()` calls with proper error handling

#### 4. **Static Variable Initialization**
- `getFormattingDelay()` - Thread-safe access to formatting delay variable
- **Usage**: Replaces duplicated static variable initialization in both `preFSBegin()` implementations

### Statistics

- **Helper functions created**: 6
- **Duplicated error handling patterns eliminated**: 2 (in `lfs_prog()` and `lfs_erase()`)
- **Duplicated async operation patterns eliminated**: 2 (in `lfs_prog()` and `lfs_erase()`)
- **Direct `lfs_file_close()` calls replaced**: 12 → 0 (all use `safeCloseFile()`)
- **Static variable initializations unified**: 2 → 1 (via `getFormattingDelay()`)

### Code Quality Improvements

✅ **Centralized error handling**: All SoftDevice errors logged consistently  
✅ **Unified async operations**: Consistent async operation handling across all flash operations  
✅ **Safe file operations**: All file closes are error-checked and can remove corrupted files  
✅ **Thread-safe initialization**: Static variables initialized safely via helper functions  
✅ **Reduced code duplication**: ~150 lines of duplicated code eliminated
