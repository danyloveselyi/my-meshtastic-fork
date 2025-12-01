# NodeDB Unified Implementation for RAK4631 Lite

This folder contains the unified NodeDB implementation for RAK4631 Lite variant, consolidated into `NodeDBUnified.h` and `NodeDBUnified.cpp`.

## Files

- **`NodeDBUnified.h`** - Unified header with all NodeDB component declarations (1627 lines, 12 includes)
- **`NodeDBUnified.cpp`** - Unified implementation (3674 lines, 19 includes)

## Components

The unified implementation contains the following namespaces:

1. **MemoryHelpers** - Memory allocation helpers and safety checks
2. **NodeIndex** - RAM index for tracking all nodes and their flash locations
3. **NodeStorage** - Flash storage for individual node slots (LittleFS files)
4. **NodeCache** - LRU/LFU cache for active nodes in RAM
5. **NodeDBBackgroundTask** - Background task and write queue for deferred flash writes
6. **NodeDBVirtualBackend** - Main virtual backend implementation (replaces standard vector-based storage)

## Dependencies and External Usage

### Files That Include NodeDBUnified

#### 1. `src/mesh/NodeDB.cpp`
**Include**: `#include "../../variants/rak4631_lite/nodedb/NodeDBUnified.h"` (included twice - lines 24 and 40)

**Used Functions/Methods**:

**Inline Functions (from NodeDB Patches section)**:
- `initializeVirtualBackendForNodeDatabase()` - Initialize virtual backend early to prevent large vector allocation
  - **Usage**: Called in `NodeDB::NodeDB()` constructor (line 548)
  - **Purpose**: Initializes virtual backend before vector expansion
  
- `initializeVirtualBackendAfterLoad()` - Initialize virtual backend after loading nodes.proto
  - **Usage**: Called in `NodeDB::loadFromDisk()` (line 1236)
  - **Purpose**: Ensures backend is ready BEFORE packets arrive, even if `installDefaultNodeDatabase()` was not called

- `calculateSaveWhatFlags()` - Calculate which segments need saving (CRC checking)
  - **Usage**: Called in `NodeDB::NodeDB()` constructor (line 418)
  - **Purpose**: Replaces standard CRC checking with variant-specific logic

- `getOrCreateMeshNodeLazyInit()` - Get or create node with lazy initialization
  - **Usage**: Called in `NodeDB::getOrCreateMeshNode()` (line 1877)
  - **Purpose**: Handles lazy initialization of virtual backend

- `updateFromWithVirtualBackend()` - Update from packet with virtual backend support
  - **Usage**: Called in `NodeDB::updateFrom()` (line 1793)
  - **Purpose**: Updates node from packet using virtual backend if enabled

- `nodeDBBackgroundTaskTick()` - Call background task tick from main loop
  - **Usage**: Should be called from `main.cpp::loop()` (not currently used in NodeDB.cpp)
  - **Purpose**: Ensures background task runs cooperatively

**Namespace Methods**:
- `NodeDBVirtualBackend::isEnabled()` - Check if virtual backend is enabled
  - **Usage**: Called in `NodeDB::updateFrom()` (line 1794)
  - **Purpose**: Check if virtual backend handled the update

### Summary of External API

#### Inline Functions (NodeDB Patches)
```cpp
// Initialization
bool initializeVirtualBackendForNodeDatabase(meshtastic_NodeDatabase& nodeDatabase,
                                             pb_size_t& numMeshNodes,
                                             std::vector<meshtastic_NodeInfoLite>*& meshNodes);
void initializeVirtualBackendAfterLoad(meshtastic_NodeDatabase& nodeDatabase,
                                       pb_size_t& numMeshNodes,
                                       std::vector<meshtastic_NodeInfoLite>*& meshNodes);

// Utilities
int calculateSaveWhatFlags(uint32_t devicestateCRC, uint32_t nodeDatabaseCRC,
                           uint32_t configCRC, uint32_t channelFileCRC, int saveWhat);
meshtastic_NodeInfoLite* getOrCreateMeshNodeLazyInit(NodeNum n,
                                                     std::function<NodeNum()> getNodeNum,
                                                     std::function<meshtastic_NodeInfoLite*(NodeNum)> getMeshNode);
void updateFromWithVirtualBackend(const meshtastic_MeshPacket* mp,
                                 NodeNum (*getFrom)(const meshtastic_MeshPacket*));
void nodeDBBackgroundTaskTick();
```

#### NodeDBVirtualBackend Namespace (Public Methods)
```cpp
// Status
static bool isEnabled();
static bool isAvailable();

// Node Operations
static meshtastic_NodeInfoLite* getNode(NodeNum nodeNum);
static meshtastic_NodeInfoLite* getOrCreateNode(NodeNum nodeNum);
static bool updateUser(NodeNum nodeNum, const meshtastic_User* user);
static bool removeNode(NodeNum nodeNum);
static void updateFromPacket(NodeNum nodeNum, const meshtastic_NodeInfoLite* node);
static void updateFromPacket(NodeNum nodeNum, const meshtastic_MeshPacket* mp);

// Management
static bool loadFromDisk();
static bool saveToDisk();
static bool resetNodes();
static void markProtected(NodeNum nodeNum, bool isProtected);
```

## Dependencies Analysis

### Include Dependencies in NodeDBUnified.h

1. **`configuration.h`** - ✅ Used (for LOG_*, platform defines, etc.)
2. **`mesh/NodeDB.h`** - ✅ Used (for `meshtastic_NodeDatabase`, `LoadFileResult`, forward declarations)
3. **`memGet.h`** - ✅ Used (for `memGet.getFreeHeap()`)
4. **`<cstdint>`, `<cstddef>`** - ✅ Used (for standard types)
5. **`<Arduino.h>`** - ✅ Used (for `yield()`, `delay()`, `millis()`)
6. **`<ErriezCRC32.h>`** - ✅ Used (for `crc32Buffer()` in `calculateSaveWhatFlags()`)
7. **`<vector>`, `<algorithm>`, `<functional>`** - ✅ Used (for containers and algorithms)

### Include Dependencies in NodeDBUnified.cpp

1. **`NodeDBUnified.h`** - ✅ Main header
2. **`configuration.h`** - ✅ Used (duplicate from .h, but needed for implementation)
3. **`variant.h`** - ✅ Used (for `MAX_NUM_NODES` constant)
4. **`FilesystemUnified.h`** - ✅ Used (for `ExtendedFilesystemModule::getExtendedFS()`, `getStats()`, `shouldProceedWithSave()`)
5. **`mesh/NodeDB.h`** - ✅ Used (for `meshtastic_NodeDatabase`, `nodeDatabase` global)
6. **`mesh/MeshTypes.h`** - ✅ Used (for `meshtastic_NodeInfoLite`, `meshtastic_NodeDatabase` types)
7. **`mesh/TypeConversions.h`** - ✅ Used (for `TypeConversions::ConvertToUserLite()`)
8. **`gps/RTC.h`** - ✅ Used (for `getValidTime()`)
9. **`memGet.h`** - ✅ Used (duplicate from .h, but needed for implementation)
10. **LittleFS headers** - ✅ Used (for `lfs_t`, `lfs_file_t`, directory operations)
11. **`<pb_encode.h>`, `<pb_decode.h>`** - ✅ Used (for protobuf encoding/decoding)
12. **`<cstring>`, `<cstdio>`, `<cstdint>`, `<algorithm>`** - ✅ Used (standard library)
13. **`rtos.h`** - ✅ Used (for `rtos_malloc()`, `rtos_free()`)
14. **`NRF52Bluetooth.h`** - ✅ Used (for BLE connection state checking in background task)
15. **`BluetoothStatus.h`** - ✅ Used (for BLE pairing state checking)
16. **`<Arduino.h>`** - ✅ Used (for `millis()`, `delay()`, `yield()`)

### Can Dependencies Be Reduced?

**Optimization Applied**:
- ✅ **Removed duplicate includes** from `NodeDBUnified.cpp`:
  - `configuration.h` - already included in `NodeDBUnified.h`
  - `mesh/NodeDB.h` - already included in `NodeDBUnified.h`
  - `memGet.h` - already included in `NodeDBUnified.h`
  - **Result**: Reduced from 22 includes to 19 includes in `.cpp` file

**Cannot Be Removed**:
- ❌ **`variant.h`** - Used for `MAX_NUM_NODES` constant (variant-specific, line 11, 2067)
- ❌ **`FilesystemUnified.h`** - Used for extended filesystem access
- ❌ **`MeshTypes.h`** - Used for `meshtastic_NodeInfoLite` type (line 14)
- ❌ **`TypeConversions.h`** - Used for `ConvertToUserLite()` (line 15, 2549)
- ❌ **`RTC.h`** - Used for `getValidTime()` (line 16, lines 466, 3053)
- ❌ **`NRF52Bluetooth.h`** - Used for BLE state checking (line 29, 1963)
- ❌ **`BluetoothStatus.h`** - Used for BLE pairing state (line 30, 1967)

**Why Dependencies Should Stay in NodeDBUnified.cpp**:
1. They are implementation details, not interface requirements
2. Moving them to `NodeDB.h` would increase coupling in the main codebase
3. The current structure keeps variant-specific code isolated
4. All dependencies are necessary for functionality

**Final Dependency Count**:
- **NodeDBUnified.h**: 7 includes (interface dependencies)
- **NodeDBUnified.cpp**: 19 includes (implementation dependencies, after optimization)

### Dependency Flow

```
NodeDB.cpp
  └─> NodeDBUnified.h (interface)
       └─> NodeDBUnified.cpp (implementation)
            ├─> FilesystemUnified.h (extended FS access)
            ├─> variant.h (MAX_NUM_NODES)
            ├─> mesh/MeshTypes.h (types)
            ├─> mesh/TypeConversions.h (conversions)
            ├─> gps/RTC.h (time)
            └─> NRF52Bluetooth.h (BLE state)
```

## Integration Points

### From NodeDB.cpp

1. **Constructor** (`NodeDB::NodeDB()`):
   - Calls `initializeVirtualBackendForNodeDatabase()` to initialize backend early
   - Calls `calculateSaveWhatFlags()` for CRC checking

2. **Load from disk** (`NodeDB::loadFromDisk()`):
   - Calls `initializeVirtualBackendAfterLoad()` to ensure backend is ready

3. **Get or create node** (`NodeDB::getOrCreateMeshNode()`):
   - Calls `getOrCreateMeshNodeLazyInit()` for lazy initialization

4. **Update from packet** (`NodeDB::updateFrom()`):
   - Calls `updateFromWithVirtualBackend()` to handle packet updates
   - Checks `NodeDBVirtualBackend::isEnabled()` to see if backend handled it

### To FilesystemUnified

- Uses `ExtendedFilesystemModule::getExtendedFS()` for direct LittleFS access
- Uses `ExtendedFilesystemModule::getStats()` for filesystem statistics
- Uses `ExtendedFilesystemModule::shouldProceedWithSave()` for radio state checking

## Code Quality Improvements

### Duplication Elimination

All code duplication has been eliminated:

1. **Memory Threshold Constants**: All `MIN_FREE_HEAP_*` constants consolidated into `MemoryHelpers::Thresholds` namespace
   - `NODE_READ = 5120` (5 KB)
   - `NODE_CREATION = 8192` (8 KB)
   - `EVICTION = 10240` (10 KB)
   - `COLLECTION = 10240` (10 KB)
   - `SORT = 8192` (8 KB)
   - `SAFE_FREE_HEAP = 16384` (16 KB)

2. **Memory Free Helper**: Unified `freeMemory<T>()` function replaces all `rtos_free`/`delete[]` patterns
   - Used in: `allocateSlot()`, `resetNodes()`

3. **Aggressive Cache Eviction**: Unified `aggressiveCacheEviction()` function eliminates duplicate eviction loops
   - Used in: `evictNodesForNewNode()` (2 places → 1 function)

4. **Memory Allocation**: Centralized through `MemoryHelpers::checkMemory()` and `allocateMemoryWithCheck()`

### Memory Safety

- ✅ All memory allocations checked before allocation
- ✅ Consistent error handling for allocation failures
- ✅ Platform-specific memory management (rtos_malloc/rtos_free for NRF52, new/delete for others)
- ✅ Memory thresholds centralized and documented

## Benefits

✅ **Unified codebase**: All NodeDB components in one place for easier maintenance  
✅ **Isolated dependencies**: Variant-specific code doesn't pollute main codebase  
✅ **Clear interface**: Inline functions in header provide clean integration points  
✅ **Preserved namespaces**: Original component structure maintained for clarity  
✅ **No code duplication**: All duplicated patterns refactored into reusable functions  
✅ **Memory safety**: Comprehensive memory checks and consistent error handling

