# RAK4631 Lite: Scaling NodeDB to 1000-1500 Nodes

## Architecture Overview

**Current State:**

- All nodes stored in RAM vector (`meshNodes`)
- Extended FS (320 KB) exists but only used for full `nodes.proto` dump
- Limited to ~256-350 nodes due to RAM constraints

**Target Architecture:**

- **Flash Backend**: Individual fixed-size slots for 1000-1500 nodes
- **RAM Cache**: 200-300 active nodes only
- **Node Index**: Compact structure in RAM tracking all nodes (hot/cold status, flash location)
- **Hot/Cold Split**: Critical routing fields in RAM, full data in flash

## Implementation Plan

### Phase 1: Core Storage Infrastructure

#### 1.1 Node Storage Format (variants/rak4631_lite/nodedb/)

**File**: `NodeStorage.h/cpp`

- Define fixed-size slot structure (e.g., 256 bytes per node)
- Implement slot-based read/write operations
- Slot allocation/deallocation tracking
- Atomic write operations (single slot updates)
- Recovery/validation on startup

**Key Functions:**

- `allocateSlot(NodeNum n) -> slot_id`
- `writeNodeToSlot(slot_id, NodeInfoLite*) -> bool`
- `readNodeFromSlot(slot_id, NodeInfoLite*) -> bool`
- `getSlotCount() -> uint32_t`
- `getFreeSlotCount() -> uint32_t`

**Storage Layout:**

```
Extended FS (320 KB):
- Header (4 KB): metadata, slot allocation bitmap
- Node slots (316 KB): 1234 slots × 256 bytes = 315.9 KB
```

#### 1.2 Node Index (variants/rak4631_lite/nodedb/)

**File**: `NodeIndex.h/cpp`

- Compact index entry structure (~16 bytes per node):
  - `NodeNum num` (4 bytes)
  - `uint16_t slot_id` (2 bytes) - flash slot location
  - `uint16_t flags` (2 bytes) - cached, protected, etc.
  - `uint32_t last_heard` (4 bytes) - for LRU eviction
  - `uint32_t access_count` (4 bytes) - for LFU eviction
- Hash map or sorted array for O(log n) lookup
- Maximum 1500 entries = ~24 KB RAM

**Key Functions:**

- `addNode(NodeNum, slot_id) -> bool`
- `findNode(NodeNum) -> index_entry*`
- `updateLastHeard(NodeNum, timestamp)`
- `getEvictionCandidate() -> NodeNum` (LRU/LFU policy)
- `markProtected(NodeNum, bool)` - routers, favorites, local node

#### 1.3 Node Cache (variants/rak4631_lite/nodedb/)

**File**: `NodeCache.h/cpp`

- LRU/LFU cache for 200-300 nodes
- Integration with NodeIndex (knows which nodes are cached)
- Eviction policy:
  - Protected nodes never evicted (local, router, favorite)
  - LRU for non-protected nodes
  - Pre-evict "cold" nodes (not accessed recently)

**Key Functions:**

- `getNode(NodeNum) -> NodeInfoLite*` (load from flash if not cached)
- `putNode(NodeNum, NodeInfoLite*) -> bool`
- `evictNode(NodeNum) -> bool` (write to flash, remove from cache)
- `isCached(NodeNum) -> bool`
- `getCacheSize() -> size_t`
- `getMaxCacheSize() -> size_t`

### Phase 2: Hot/Cold Data Separation

#### 2.1 Hot Data Structure (variants/rak4631_lite/nodedb/)

**File**: `NodeHotData.h`

- Minimal structure for routing (~32 bytes):
  - `NodeNum num`
  - `uint32_t last_heard`
  - `uint8_t channel`
  - `float snr`
  - `uint8_t hops_away`
  - `bool via_mqtt`
  - Flags (is_favorite, is_ignored, etc.)

#### 2.2 Cold Data Storage

- Full `NodeInfoLite` stored in flash slots
- Hot data kept in RAM cache (NodeIndex + NodeCache)
- On-demand loading: when full node info needed, load from flash

**Integration Points:**

- `getMeshNode()` - returns hot data if available, loads cold if needed
- `updateFrom()` - updates hot data immediately, queues cold data write

### Phase 3: NodeDB Integration

#### 3.1 Abstract Storage Interface (src/mesh/NodeDB.h)

**Add hooks/abstractions:**

```cpp
#ifdef USE_EXTENDED_FS_FOR_NODEDB && defined(ARCH_NRF52)
class NodeStorageBackend {
    virtual meshtastic_NodeInfoLite* getNode(NodeNum n) = 0;
    virtual bool addNode(NodeNum n) = 0;
    virtual size_t getTotalNodeCount() = 0;
    virtual size_t getCachedNodeCount() = 0;
};
#endif
```

#### 3.2 NodeDB.cpp Modifications

**Minimal changes with #ifdef guards:**

- `getMeshNode()`: Check if virtual backend exists, delegate if yes
- `getOrCreateMeshNode()`: Use backend for allocation
- `isFull()`: Check cache size + flash capacity
- `getNumMeshNodes()`: Return total from backend (not just cached)
- `loadFromDisk()`: Load index from flash, populate cache with recent nodes
- `saveNodeDatabaseToDisk()`: Save index + dirty cache entries

**Critical**: All changes wrapped in:

```cpp
#if defined(ARCH_NRF52) && defined(USE_EXTENDED_FS_FOR_NODEDB) && defined(RAK_4631)
// Virtual NodeDB implementation
#else
// Original implementation
#endif
```

### Phase 4: Extended FS Enhancements

#### 4.1 NodeDBExtendedFSImpl.cpp Updates

**Add functions for slot-based access:**

- `readNodeSlot(uint16_t slot_id, void* buffer, size_t size) -> bool`
- `writeNodeSlot(uint16_t slot_id, const void* buffer, size_t size) -> bool`
- `getSlotAllocationBitmap() -> uint8_t*` (for tracking free slots)
- Keep existing `loadFromExtendedFS`/`saveToExtendedFS` for backward compatibility

### Phase 5: DeviceStatsModule Updates

#### 5.1 Statistics Display (src/modules/devicestats/DeviceStatsModule.cpp)

**Update `/mem` and `/nodes` commands:**

- Show `nodes_ram_cache: X / 300` (cached nodes)
- Show `nodes_total: Y` (total in flash)
- Show `nodes_flash_free: Z slots` (available flash slots)
- Show extended FS usage percentage

**Add new command `/nodesummary`:**

```
Nodes Summary:
RAM Cache: 245/300 (82%)
Total Nodes: 1247/1500
Flash Free: 253 slots
Extended FS: 87% used
```

### Phase 6: DynamicNodes Integration

#### 6.1 Update DynamicNodes.cpp (if exists)

**Change semantics:**

- `dynamic_max_nodes` now means "RAM cache size" (not total limit)
- Total node limit determined by flash capacity (1500)
- Validation: cache size must be ≤ 300 for safety

### Phase 7: Migration and Startup

#### 7.1 Data Migration (variants/rak4631_lite/nodedb/)

**File**: `NodeDBMigration.cpp`

- On first boot with new architecture:
  - Load existing `nodes.proto` from extended FS
  - Convert to slot-based storage
  - Create index
  - Populate cache with most recent nodes
- Preserve all existing node data

#### 7.2 Startup Sequence

**In NodeDB constructor:**

1. Initialize extended FS (if not already)
2. Load index from flash (or create new)
3. Verify slot allocation bitmap
4. Populate cache with protected nodes + recent nodes (up to cache limit)
5. Verify integrity

### Phase 8: Error Handling and Edge Cases

#### 8.1 Flash Full Handling

- Check `getFreeSlotCount()` before adding node
- If full: log warning, return error from `getOrCreateMeshNode()`
- DeviceStats shows "Flash full" status
- Optionally: evict oldest non-protected node (if policy allows)

#### 8.2 RAM Pressure Handling

- Monitor `memGet.getFreeHeap()`
- If < MINIMUM_SAFE_FREE_HEAP: aggressive cache eviction
- Evict non-protected, least-recently-used nodes first
- Log eviction events

#### 8.3 Recovery

- On startup: validate slot allocation bitmap
- Detect corrupted slots: mark as free, log error
- Rebuild index from valid slots if index corrupted

### Phase 9: Testing and Validation

#### 9.1 Synthetic Load Testing

- Script to inject 1000-1500 test nodes
- Verify: no crashes, RAM stable, flash usage correct
- Verify: cache eviction works, protected nodes preserved

#### 9.2 Real-World Testing

- Deploy in high-density mesh (100+ nodes)
- Monitor for 24-48 hours
- Verify: no memory leaks, stable operation
- Verify: routing still works correctly

### Phase 10: Documentation

#### 10.1 Update variants/rak4631_lite/README.md

- Describe virtual NodeDB architecture
- Document cache size limits
- Document flash capacity limits
- Migration notes from old format

#### 10.2 Code Comments

- Document all key data structures
- Explain eviction policies
- Document slot allocation algorithm

## File Structure

```
variants/rak4631_lite/nodedb/
├── NodeStorage.h/cpp          # Slot-based flash storage
├── NodeIndex.h/cpp            # Compact RAM index (1500 entries)
├── NodeCache.h/cpp            # LRU/LFU cache (200-300 nodes)
├── NodeHotData.h              # Hot data structure definition
├── NodeDBMigration.cpp        # Migration from old format
├── NodeDBVirtualBackend.h/cpp # Main integration interface
├── NodeDBExtendedFSImpl.h/cpp # (existing, enhance for slots)
└── NodeDBFilesystemAdapter.h/cpp # (existing, keep)

src/mesh/
├── NodeDB.h                   # (minimal changes, add hooks)
└── NodeDB.cpp                 # (conditional compilation for virtual backend)
```

## Key Design Decisions

1. **Individual Slots via LittleFS**: 256-byte slots stored as files, LittleFS provides automatic wear leveling
2. **Hot/Cold Split with Flash Protection**: 

   - Hot data (last_heard, snr) **NEVER written to flash** (updates 100-1000x/min)
   - Cold data (user, position) written via LittleFS (updates 1-10x/hour)
   - LittleFS copy-on-write ensures even cold updates don't wear same blocks

3. **Index in RAM**: 24 KB for 1500 nodes is acceptable, enables fast lookup
4. **Cache Size**: 200-300 nodes = 50-75 KB RAM (safe margin)
5. **Protected Nodes**: Local node, routers, favorites never evicted from cache
6. **Wear Leveling Strategy**:

   - Hot data: RAM only (zero flash writes)
   - Cold data: LittleFS with copy-on-write (automatic block rotation)
   - Batch updates: Throttled to 1 minute (prevents excessive writes)
   - Slot rotation: Each update creates new file, deletes old (distributed wear)

## Success Criteria

- [ ] Support 1000-1500 nodes without crashes
- [ ] RAM usage stable (< 100 KB for NodeDB components)
- [ ] Flash usage tracked and reported
- [ ] No degradation in routing performance
- [ ] Backward compatible (existing nodes.proto migrates correctly)
- [ ] DeviceStats shows accurate cache/total counts
- [ ] Protected nodes never lost
- [ ] Graceful handling of flash full condition