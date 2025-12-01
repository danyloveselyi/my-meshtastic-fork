/**
 * @file NodeDBUnified.h
 * @brief Unified NodeDB implementation for RAK4631 Lite
 * 
 * This file contains all NodeDB components in a single header for easier
 * LLM analysis and code review. All namespaces are preserved for clarity.
 * 
 * Components:
 * - MemoryHelpers: Memory allocation helpers
 * - NodeIndex: RAM index for tracking all nodes
 * - NodeStorage: Flash storage for individual node slots
 * - NodeCache: LRU/LFU cache for active nodes
 * - NodeDBBackgroundTask: Background task and write queue
 * - NodeDBPersistentBackend: Main persistent backend implementation
 * 
 * Previously split across multiple files:
 * - MemoryHelpers.h/cpp
 * - NodeIndexStorage.h/cpp (NodeIndex + NodeStorage)
 * - NodeCache.h/cpp
 * - NodeDBBackgroundTask.h/cpp
 * - NodeDBPersistentBackend (in this file)
 */

#pragma once

#include "configuration.h"
#include "mesh/NodeDB.h"
#include "../../../src/memGet.h"
#include <cstdint>
#include <cstddef>

#ifdef USE_EXTENDED_FS_FOR_NODEDB

// Forward declaration
typedef uint32_t NodeNum;

// ============================================================================
// MemoryHelpers namespace
// ============================================================================

namespace MemoryHelpers {

/**
 * @brief Memory check result structure
 */
struct MemoryCheckResult {
    bool sufficient;           // Whether there's enough memory
    uint32_t freeHeap;          // Free heap before check
    uint32_t heapTotal;         // Total heap size
    uint32_t usedHeap;          // Used heap
    uint32_t requiredMemory;    // Required memory for allocation
    uint32_t safetyMargin;      // Safety margin used
    uint32_t totalNeeded;       // Total needed (required + margin)
    uint32_t shortage;          // Shortage if insufficient (0 if sufficient)
};

/**
 * @brief Check if there's enough memory for allocation
 * @param requiredMemory Memory required for allocation (in bytes)
 * @param safetyMargin Safety margin to add (in bytes)
 * @param componentName Name of component for logging (e.g., "NodeIndex", "NodeCache")
 * @return MemoryCheckResult with check results
 */
MemoryCheckResult checkMemory(uint32_t requiredMemory, uint32_t safetyMargin, const char* componentName);

/**
 * @brief Log memory check failure
 * @param result Memory check result
 * @param componentName Name of component for logging
 */
void logMemoryCheckFailure(const MemoryCheckResult& result, const char* componentName);

/**
 * @brief Log allocation failure
 * @param componentName Name of component for logging
 * @param requestedBytes Bytes requested
 * @param elementSize Size of each element
 * @param elementCount Number of elements
 * @param freeHeapBefore Free heap before allocation
 * @param freeHeapAfter Free heap after allocation (should be same if failed)
 */
void logAllocationFailure(const char* componentName, uint32_t requestedBytes, 
                          uint32_t elementSize, uint32_t elementCount,
                          uint32_t freeHeapBefore, uint32_t freeHeapAfter);

/**
 * @brief Default safety margins for different components
 */
namespace SafetyMargins {
    constexpr uint32_t INDEX = 30720;      // 30 KB for NodeIndex
    constexpr uint32_t CACHE = 40960;      // 40 KB for NodeCache
    constexpr uint32_t WRITE_QUEUE = 20480; // 20 KB for WriteQueue
}

/**
 * @brief Memory threshold constants (in bytes)
 */
namespace Thresholds {
    constexpr uint32_t NODE_READ = 5120;        // 5 KB minimum for reading a node
    constexpr uint32_t NODE_CREATION = 8192;   // 8 KB minimum for creating a node
    constexpr uint32_t EVICTION = 10240;        // 10 KB minimum for eviction operations
    constexpr uint32_t COLLECTION = 10240;     // 10 KB minimum for collection phase
    constexpr uint32_t SORT = 8192;            // 8 KB minimum for sorting
    constexpr uint32_t SAFE_FREE_HEAP = 16384; // 16 KB minimum safe free heap
}

/**
 * @brief Get MAX_NODES_INDEX constant value
 * @return Maximum number of index entries
 */
uint32_t getMaxNodesIndex();

/**
 * @brief Get MAX_NODES_SLOTS constant value
 * @return Maximum number of slots
 */
uint32_t getMaxNodesSlots();

/**
 * @brief Get MAX_NODES_CACHE constant value
 * @return Maximum number of cache entries
 */
uint32_t getMaxNodesCache();

} // namespace MemoryHelpers

/**
 * @brief Macro to feed watchdog and yield (for NRF52 platform)
 * 
 * This macro calls nrf52Loop() to process SoftDevice events and feed watchdog,
 * then calls yield() to allow other tasks to run.
 * 
 * Usage: FEED_WATCHDOG_AND_YIELD();
 */
#ifdef ARCH_NRF52
extern void nrf52Loop();
#include <Arduino.h>  // For yield()
#define FEED_WATCHDOG_AND_YIELD() do { nrf52Loop(); yield(); } while(0)
#else
#include <Arduino.h>  // For yield()
#define FEED_WATCHDOG_AND_YIELD() do { yield(); } while(0)
#endif

// ============================================================================
// NodeIndex namespace (from NodeIndexStorage)
// ============================================================================

/**
 * @brief Index entry structure (~16 bytes per node)
 */
struct NodeIndexEntry {
    NodeNum num;              // 4 bytes - Node number
    uint16_t slot_id;         // 2 bytes - Flash slot location
    uint16_t flags;           // 2 bytes - Flags (cached, protected, etc.)
    uint32_t last_heard;      // 4 bytes - Last heard timestamp (for LRU eviction)
    uint32_t access_count;    // 4 bytes - Access count (for LFU eviction)
    
    // Flags bitfield (stored in flags field)
    static constexpr uint16_t FLAG_CACHED = 0x0001;           // Node is in RAM cache
    static constexpr uint16_t FLAG_PROTECTED = 0x0002;         // Never evict (local, router, favorite)
    static constexpr uint16_t FLAG_DIRTY = 0x0004;            // Cache entry modified, needs flash write
    static constexpr uint16_t FLAG_HAS_USER = 0x0008;         // Node has user info
    static constexpr uint16_t FLAG_HAS_POSITION = 0x0010;     // Node has position
};

namespace NodeIndex {

/**
 * @brief Initialize the index
 * @param maxEntries Maximum number of entries (default: MAX_NODES_INDEX or 1234, must be <= MAX_NODES_SLOTS)
 * @return true if initialization was successful
 */
bool initialize(uint32_t maxEntries = 
#ifdef MAX_NODES_INDEX
    MAX_NODES_INDEX
#elif defined(MAX_NODES_SLOTS)
    MAX_NODES_SLOTS  // Use same value as slots (they must be equal)
#else
    MAX_NUM_NODES  // Use MAX_NUM_NODES from variant.h (500 by default for RAK4631)
#endif
);

/**
 * @brief Add a node to the index
 * @param nodeNum Node number
 * @param slotId Flash slot ID
 * @return true if node was added successfully
 */
bool addNode(NodeNum nodeNum, uint16_t slotId);

/**
 * @brief Find a node in the index
 * @param nodeNum Node number to find
 * @return Pointer to index entry, or nullptr if not found
 */
NodeIndexEntry* findNode(NodeNum nodeNum);

/**
 * @brief Remove a node from the index
 * @param nodeNum Node number to remove
 * @return true if node was removed successfully
 */
bool removeNode(NodeNum nodeNum);

/**
 * @brief Update last_heard timestamp for a node
 * @param nodeNum Node number
 * @param timestamp Last heard timestamp
 * @return true if update was successful
 */
bool updateLastHeard(NodeNum nodeNum, uint32_t timestamp);

/**
 * @brief Increment access count for a node
 * @param nodeNum Node number
 * @return true if update was successful
 */
bool incrementAccessCount(NodeNum nodeNum);

/**
 * @brief Mark a node as protected (never evict)
 * @param nodeNum Node number
 * @param isProtected true to protect, false to unprotect
 * @return true if update was successful
 */
bool markProtected(NodeNum nodeNum, bool isProtected);

/**
 * @brief Mark a node as cached in RAM
 * @param nodeNum Node number
 * @param cached true if cached, false if not
 * @return true if update was successful
 */
bool markCached(NodeNum nodeNum, bool cached);

/**
 * @brief Mark a node as dirty (needs flash write)
 * @param nodeNum Node number
 * @param dirty true if dirty, false if clean
 * @return true if update was successful
 */
bool markDirty(NodeNum nodeNum, bool dirty);

/**
 * @brief Get eviction candidate (LRU/LFU policy)
 * @param allowCached If true, can evict cached nodes (for flash full scenario)
 * @return Node number of candidate, or 0 if none found
 */
NodeNum getEvictionCandidate(bool allowCached = false);

/**
 * @brief Get total number of nodes in index
 * @return Number of entries
 */
uint32_t getNodeCount();

/**
 * @brief Get number of cached nodes
 * @return Number of nodes marked as cached
 */
uint32_t getCachedNodeCount();

/**
 * @brief Clear all entries from index
 */
void clear();

/**
 * @brief Get all node numbers (for iteration)
 * @param buffer Output buffer for node numbers
 * @param bufferSize Size of buffer (in entries)
 * @return Number of node numbers written to buffer
 */
uint32_t getAllNodeNums(NodeNum* buffer, uint32_t bufferSize);

/**
 * @brief Get node number by index (for iteration)
 * @param index Index (0 to getNodeCount()-1)
 * @return Node number, or 0 if index out of range
 */
NodeNum getNodeNumByIndex(uint32_t index);

/**
 * @brief Save index to flash
 * @return true if save was successful
 */
bool saveToFlash();

/**
 * @brief Load index from flash
 * @return true if load was successful, false if index file not found or corrupted
 */
bool loadFromFlash();

} // namespace NodeIndex

// ============================================================================
// NodeStorage namespace (from NodeIndexStorage)
// ============================================================================

namespace NodeStorage {

/**
 * @brief Allocate a new slot for a node
 * @param nodeNum Node number to allocate slot for
 * @return Slot ID (0-1233) or UINT16_MAX on failure
 */
uint16_t allocateSlot(uint32_t nodeNum);

/**
 * @brief Write node data to a slot
 * @param slotId Slot ID to write to
 * @param node Pointer to NodeInfoLite structure
 * @return true if write was successful
 */
bool writeNodeToSlot(uint16_t slotId, const meshtastic_NodeInfoLite* node);

/**
 * @brief Read node data from a slot
 * @param slotId Slot ID to read from
 * @param node Pointer to destination NodeInfoLite structure
 * @return true if read was successful
 */
bool readNodeFromSlot(uint16_t slotId, meshtastic_NodeInfoLite* node);

/**
 * @brief Delete a slot (mark as free)
 * @param slotId Slot ID to delete
 * @return true if deletion was successful
 */
bool deleteSlot(uint16_t slotId);

/**
 * @brief Get total number of allocated slots
 * @return Number of slot files that exist
 */
uint32_t getSlotCount();

/**
 * @brief Get number of free slots
 * @return Estimated number of free slots (total capacity - used)
 */
uint32_t getFreeSlotCount();

/**
 * @brief Get maximum slot capacity
 * @return Maximum number of slots (approximately 1234)
 */
uint32_t getMaxSlotCount();

/**
 * @brief Check if a slot exists
 * @param slotId Slot ID to check
 * @return true if slot file exists
 */
bool slotExists(uint16_t slotId);

/**
 * @brief Initialize slot storage (create /prefs/nodes directory if needed)
 * @return true if initialization was successful
 */
bool initialize();

/**
 * @brief Reset directory flag (called after Main FS reformat)
 * 
 * After Main FS reformat, /prefs/nodes directory doesn't exist anymore,
 * but directory_created flag may still be true. This function resets the flag
 * so that ensureDirectoryExists() will recreate the directory.
 */
void resetDirectoryFlag();

/**
 * @brief Get slot filename for a given slot ID
 * @param slotId Slot ID
 * @param buffer Output buffer (must be at least 32 bytes)
 * @param bufferSize Size of buffer
 * @return Pointer to buffer, or nullptr on error
 */
const char* getSlotFilename(uint16_t slotId, char* buffer, size_t bufferSize);

} // namespace NodeStorage

// ============================================================================
// NodeCache namespace
// ============================================================================

namespace NodeCache {

/**
 * @brief Initialize the cache
 * @param maxSize Maximum number of nodes in cache (default: MAX_NODES_CACHE or 300)
 * @return true if initialization was successful
 */
bool initialize(uint32_t maxSize = 
#ifdef MAX_NODES_CACHE
    MAX_NODES_CACHE
#else
    300
#endif
);

/**
 * @brief Get a node from cache (load from flash if not cached)
 * @param nodeNum Node number
 * @return Pointer to NodeInfoLite, or nullptr if not found
 * 
 * If node is not in cache, it will be loaded from flash and added to cache.
 * If cache is full, least recently used non-protected node will be evicted.
 */
meshtastic_NodeInfoLite* getNode(NodeNum nodeNum);

/**
 * @brief Put a node into cache
 * @param nodeNum Node number
 * @param node Pointer to NodeInfoLite structure
 * @return true if node was added successfully
 * 
 * If cache is full, least recently used non-protected node will be evicted.
 */
bool putNode(NodeNum nodeNum, const meshtastic_NodeInfoLite* node);

/**
 * @brief Evict a node from cache (write to flash if dirty, remove from cache)
 * @param nodeNum Node number
 * @return true if eviction was successful
 */
bool evictNode(NodeNum nodeNum);

/**
 * @brief Check if a node is cached
 * @param nodeNum Node number
 * @return true if node is in cache
 */
bool isCached(NodeNum nodeNum);

/**
 * @brief Get current cache size
 * @return Number of nodes currently in cache
 */
size_t getCacheSize();

/**
 * @brief Get maximum cache size
 * @return Maximum number of nodes that can be cached
 */
size_t getMaxCacheSize();

/**
 * @brief Clear all entries from cache
 * @param flushDirty If true, write dirty entries to flash before clearing (default: true)
 * 
 * During factory reset, set flushDirty=false to skip flash writes since Extended FS will be reformatted.
 */
void clear(bool flushDirty = true);

/**
 * @brief Mark a node as dirty (needs flash write)
 * @param nodeNum Node number
 * 
 * Marks node as dirty and enqueues it in the write queue for background processing.
 */
void markDirty(NodeNum nodeNum);

/**
 * @brief Clear dirty flag for a node
 * @param nodeNum Node number
 */
void clearDirty(NodeNum nodeNum);

/**
 * @brief Write all dirty nodes to flash
 * @return Number of nodes written
 * 
 * NOTE: This is synchronous and may block. For async writes, use markDirty()
 * which enqueues to the write queue processed by NodeDBBackgroundTask.
 */
uint32_t flushDirtyNodes();

} // namespace NodeCache

// ============================================================================
// NodeDBBackgroundTask namespace
// ============================================================================

// Forward declaration (defined in NodeDBPersistentBackend namespace)
struct NodeDBBackendContext;

namespace NodeDBBackgroundTask {

/**
 * @brief Write queue entry structure
 */
struct WriteQueueEntry {
    NodeNum nodeNum;
    uint16_t slotId;
    uint32_t priority;  // Higher = more important (protected nodes have higher priority)
    bool isProtected;
    
    WriteQueueEntry() : nodeNum(0), slotId(0), priority(0), isProtected(false) {}
    WriteQueueEntry(NodeNum num, uint16_t slot, uint32_t prio, bool prot)
        : nodeNum(num), slotId(slot), priority(prio), isProtected(prot) {}
};

/**
 * @brief Initialize the write queue
 * @param maxSize Maximum queue size (default: 100)
 * @return true if initialization was successful
 */
bool initializeWriteQueue(uint32_t maxSize = 100);

/**
 * @brief Check if write queue is initialized
 * @return true if initialized
 */
bool isWriteQueueInitialized();

/**
 * @brief Add a node to the write queue
 * @param nodeNum Node number
 * @param slotId Flash slot ID
 * @param isProtected true if node is protected (will have higher priority)
 * @return true if node was added to queue, false if queue is full
 */
bool enqueueWrite(NodeNum nodeNum, uint16_t slotId, bool isProtected = false);

/**
 * @brief Get next node from queue (highest priority first)
 * @param entry Output entry structure
 * @return true if entry was retrieved, false if queue is empty
 */
bool dequeueWrite(WriteQueueEntry& entry);

/**
 * @brief Check if write queue is empty
 * @return true if queue is empty
 */
bool isWriteQueueEmpty();

/**
 * @brief Check if write queue is full
 * @return true if queue is full
 */
bool isWriteQueueFull();

/**
 * @brief Get current write queue size
 * @return Number of entries in queue
 */
size_t getWriteQueueSize();

/**
 * @brief Get maximum write queue size
 * @return Maximum number of entries
 */
size_t getWriteQueueMaxSize();

/**
 * @brief Process write queue (process up to maxWrites entries)
 * @param maxWrites Maximum number of writes to process
 * @param force If true, ignore radio state and write anyway (for critical situations like queue overflow)
 * @return Number of writes actually processed
 */
uint32_t processWriteQueue(uint32_t maxWrites, bool force = false);

/**
 * @brief Clear all entries from write queue
 */
void clearWriteQueue();

/**
 * @brief Check if a node is already in write queue
 * @param nodeNum Node number to check
 * @return true if node is in queue
 */
bool writeQueueContains(NodeNum nodeNum);

/**
 * @brief Initialize the background task
 * @param context Backend context (for future use)
 */
void init(NodeDBBackendContext& context);

/**
 * @brief Cooperative tick function - call from main loop
 * 
 * This function should be called regularly from the main loop.
 * It processes pending operations in small chunks to avoid blocking.
 */
void tick();

/**
 * @brief Check if background task is initialized
 * @return true if initialized
 */
bool isInitialized();

} // namespace NodeDBBackgroundTask

// ============================================================================
// NodeDBPersistentBackend namespace
// ============================================================================

// Hot/Cold Data Structures
struct NodeHotData {
    uint32_t num;              // Node number
    uint32_t last_heard;       // Last heard timestamp (NEVER written to flash - updates every packet)
    uint8_t channel;           // Channel index (may change when node switches channels)
    float snr;                 // Signal-to-noise ratio (NEVER written to flash - updates every packet)
    uint8_t hops_away;         // Number of hops away (NEVER written to flash - updates frequently)
    bool has_hops_away;         // Whether hops_away is valid
    bool via_mqtt;             // Received via MQTT (updates every packet)
    uint8_t next_hop;          // Next hop node for routing (NEVER written to flash - updates during routing)
    bool is_favorite;          // Favorite flag (rarely changes)
    bool is_ignored;           // Ignored flag (rarely changes)
    bool is_key_manually_verified; // Key verification flag (from bitfield LSB 0, rarely changes)
    
    static NodeHotData fromNodeInfoLite(const meshtastic_NodeInfoLite* node)
    {
        NodeHotData hot;
        hot.num = node->num;
        hot.last_heard = node->last_heard;
        hot.channel = node->channel;
        hot.snr = node->snr;
        hot.hops_away = node->has_hops_away ? node->hops_away : 0;
        hot.has_hops_away = node->has_hops_away;
        hot.via_mqtt = node->via_mqtt;
        hot.next_hop = node->next_hop;
        hot.is_favorite = node->is_favorite;
        hot.is_ignored = node->is_ignored;
        hot.is_key_manually_verified = (node->bitfield & 0x01) != 0;
        return hot;
    }
    
    void updateNodeInfoLite(meshtastic_NodeInfoLite* node) const
    {
        node->num = num;
        node->last_heard = last_heard;
        node->channel = channel;
        node->snr = snr;
        if (has_hops_away) {
            node->has_hops_away = true;
            node->hops_away = hops_away;
        }
        node->via_mqtt = via_mqtt;
        node->next_hop = next_hop;
        node->is_favorite = is_favorite;
        node->is_ignored = is_ignored;
        if (is_key_manually_verified) {
            node->bitfield |= 0x01;
        } else {
            node->bitfield &= ~0x01;
        }
    }
};

struct NodeColdData {
    bool has_user;
    meshtastic_UserLite user;
    bool has_position;
    meshtastic_PositionLite position;
    bool has_device_metrics;
    meshtastic_DeviceMetrics device_metrics;
    
    static NodeColdData fromNodeInfoLite(const meshtastic_NodeInfoLite* node)
    {
        NodeColdData cold;
        cold.has_user = node->has_user;
        if (cold.has_user) {
            cold.user = node->user;
        }
        cold.has_position = node->has_position;
        if (cold.has_position) {
            cold.position = node->position;
        }
        cold.has_device_metrics = node->has_device_metrics;
        if (cold.has_device_metrics) {
            cold.device_metrics = node->device_metrics;
        }
        return cold;
    }
    
    void updateNodeInfoLite(meshtastic_NodeInfoLite* node) const
    {
        node->has_user = has_user;
        if (has_user) {
            node->user = user;
        }
        node->has_position = has_position;
        if (has_position) {
            node->position = position;
        }
        node->has_device_metrics = has_device_metrics;
        if (has_device_metrics) {
            node->device_metrics = device_metrics;
        }
    }
};

inline void mergeHotColdData(meshtastic_NodeInfoLite* node, 
                             const NodeHotData* hot, 
                             const NodeColdData* cold)
{
    if (hot) {
        hot->updateNodeInfoLite(node);
    }
    if (cold) {
        cold->updateNodeInfoLite(node);
    }
}

// Backend Context
struct NodeDBBackendContext {
    bool indexInitialized;
    bool cacheInitialized;
    bool storageInitialized;
    bool writeQueueInitialized;
    bool backendInitialized;
    bool backendEnabled;
    bool indexLoaded;
    uint32_t lastSaveTime;
    
    NodeDBBackendContext()
        : indexInitialized(false)
        , cacheInitialized(false)
        , storageInitialized(false)
        , writeQueueInitialized(false)
        , backendInitialized(false)
        , backendEnabled(false)
        , indexLoaded(false)
        , lastSaveTime(0)
    {
    }
    
    void reset()
    {
        indexInitialized = false;
        cacheInitialized = false;
        storageInitialized = false;
        writeQueueInitialized = false;
        backendInitialized = false;
        backendEnabled = false;
        indexLoaded = false;
        lastSaveTime = 0;
    }
};

NodeDBBackendContext& getBackendContext();
void initializeBackendContext();

namespace NodeDBPersistentBackend {

/**
 * @brief Initialize the persistent backend
 * @return true if initialization was successful
 */
bool initialize();

/**
 * @brief Check if persistent backend is enabled and initialized
 * @return true if persistent backend is active
 */
bool isEnabled();

/**
 * @brief Check if persistent backend is available (compiled in and can be initialized)
 * @return true if persistent backend is available (even if not yet initialized)
 */
bool isAvailable();

/**
 * @brief Get a node by node number
 * @param nodeNum Node number
 * @return Pointer to NodeInfoLite, or nullptr if not found
 */
meshtastic_NodeInfoLite* getNode(NodeNum nodeNum);

/**
 * @brief Get or create a node
 * @param nodeNum Node number
 * @return Pointer to NodeInfoLite, or nullptr on failure
 */
meshtastic_NodeInfoLite* getOrCreateNode(NodeNum nodeNum);

/**
 * @brief Update a node from a mesh packet
 * @param nodeNum Node number
 * @param packet Mesh packet containing update data
 * @return true if update was successful
 */
bool updateFromPacket(NodeNum nodeNum, const meshtastic_MeshPacket* packet);

/**
 * @brief Update user information for a node
 * @param nodeNum Node number
 * @param user User information
 * @return true if update was successful
 */
bool updateUser(NodeNum nodeNum, const meshtastic_User* user);

/**
 * @brief Remove a node
 * @param nodeNum Node number
 * @return true if removal was successful
 */
bool removeNode(NodeNum nodeNum);

/**
 * @brief Reset all nodes (clear cache, index, and storage)
 * @return true if reset was successful
 */
bool resetNodes();

/**
 * @brief Check if node database is full
 * @return true if cache is full or flash is full
 */
bool isFull();

/**
 * @brief Get total number of nodes (in flash + cache)
 * @return Total number of nodes
 */
size_t getTotalNodeCount();

/**
 * @brief Get number of cached nodes (in RAM)
 * @return Number of nodes in cache
 */
size_t getCachedNodeCount();

/**
 * @brief Get number of free flash slots
 * @return Number of available flash slots
 */
uint32_t getFreeFlashSlots();

/**
 * @brief Load all nodes from flash (on startup)
 * @return true if load was successful
 */
bool loadFromDisk();

/**
 * @brief Save dirty nodes to flash
 * @return true if save was successful
 */
bool saveToDisk();

/**
 * @brief Get node by index (for iteration)
 * @param index Index (0 to getTotalNodeCount()-1)
 * @return Pointer to NodeInfoLite, or nullptr if index out of range
 */
meshtastic_NodeInfoLite* getNodeByIndex(size_t index);

/**
 * @brief Mark a node as protected (never evict)
 * @param nodeNum Node number
 * @param isProtected true to protect, false to unprotect
 */
void markProtected(NodeNum nodeNum, bool isProtected);

/**
 * @brief Validate index integrity
 * @return true if index is valid, false if corrupted
 */
bool validateIndexIntegrity();

/**
 * @brief Rebuild index from flash slots if needed
 * @return true if rebuild was successful or not needed
 */
bool rebuildIndexIfNeeded();

/**
 * @brief Recover from flash errors
 * @return true if recovery was successful
 */
bool recoverFromError();

} // namespace NodeDBPersistentBackend

// ============================================================================
// NodeDB Patches (for NodeDB.cpp integration)
// ============================================================================
// These inline functions are used to patch NodeDB.cpp for persistent backend support
// and memory optimization. They are included in NodeDB.cpp via conditional compilation.

#ifdef ARCH_NRF52

#include <ErriezCRC32.h>  // For crc32Buffer()
#include <vector>
#include <algorithm>
#include <functional>  // For std::function
#include <Arduino.h>  // For yield() and delay()

// Forward declarations for global variables (defined in NodeDB.cpp)
extern meshtastic_NodeDatabase nodeDatabase;
extern meshtastic_DeviceState devicestate;
extern meshtastic_LocalConfig config;
extern meshtastic_ChannelFile channelFile;
extern NodeDB *nodeDB;

/**
 * @brief Common helper for initializing persistent backend and loading from disk
 * 
 * This function encapsulates the common logic for:
 * - Initializing persistent backend
 * - Loading index from flash incrementally
 * - Setting up minimal vector (1 node for local node)
 * 
 * @param nodeDatabase Reference to nodeDatabase structure
 * @param numMeshNodes Reference to numMeshNodes counter
 * @param meshNodes Reference to meshNodes pointer
 * @param contextMessage Context message for logging (e.g., "after loading nodes.proto")
 * @return true if persistent backend was initialized, false otherwise
 */
inline bool initializeAndLoadPersistentBackend(meshtastic_NodeDatabase& nodeDatabase,
                                            pb_size_t& numMeshNodes,
                                            std::vector<meshtastic_NodeInfoLite>*& meshNodes,
                                            const char* contextMessage)
{
    if (!NodeDBPersistentBackend::isAvailable() || NodeDBPersistentBackend::isEnabled()) {
        return false;
    }
    
    // Initialize persistent backend
    if (!NodeDBPersistentBackend::initialize()) {
        LOG_WARN("NodeDBPersistentBackend: Failed to initialize %s", contextMessage);
        return false;
    }
    
    LOG_INFO("NodeDBPersistentBackend: Initialized %s", contextMessage);
    
    // CRITICAL: Load index from flash immediately after initialization
    // This ensures all components (filesystem, index, cache) are ready BEFORE packets arrive
    FEED_WATCHDOG_AND_YIELD();
    
    LOG_INFO("NodeDBPersistentBackend: Loading index from flash (before packet processing starts)...");
    // loadFromDisk() will load index and preload cache incrementally
    // It returns false if still loading (incremental), true when complete
    // We call it in a loop until complete, feeding watchdog each time
    uint32_t load_attempts = 0;
    constexpr uint32_t MAX_LOAD_ATTEMPTS = 100;  // Safety limit
    while (!NodeDBPersistentBackend::loadFromDisk() && load_attempts < MAX_LOAD_ATTEMPTS) {
        load_attempts++;
        FEED_WATCHDOG_AND_YIELD();
        delay(10);  // Small delay to allow other tasks
    }
    
    if (load_attempts >= MAX_LOAD_ATTEMPTS) {
        LOG_WARN("NodeDBPersistentBackend: Load from disk incomplete after %u attempts, will continue loading incrementally", 
                 MAX_LOAD_ATTEMPTS);
    } else {
        LOG_INFO("NodeDBPersistentBackend: Index loaded successfully, ready for packet processing");
    }
    
    // With persistent backend, we only need vector for local node (1 node)
    // Network nodes will be stored in persistent backend (flash + cache)
    nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(1);  // Only local node in vector
    numMeshNodes = 0;
    meshNodes = &nodeDatabase.nodes;
    return true;
}

/**
 * @brief Initialize persistent backend after loading nodes.proto file
 * 
 * This function is called from loadFromDisk() after successfully loading nodes.proto
 * to ensure persistent backend is initialized BEFORE packets arrive, even if
 * installDefaultNodeDatabase() was not called (because file version was valid).
 * 
 * @param nodeDatabase Reference to nodeDatabase structure
 * @param numMeshNodes Reference to numMeshNodes counter
 * @param meshNodes Reference to meshNodes pointer
 */
inline void initializePersistentBackendAfterLoad(meshtastic_NodeDatabase& nodeDatabase,
                                              pb_size_t& numMeshNodes,
                                              std::vector<meshtastic_NodeInfoLite>*& meshNodes)
{
    // CRITICAL: Initialize persistent backend if not already initialized
    // This ensures backend is ready BEFORE packets arrive, even if installDefaultNodeDatabase()
    // was not called (because nodes.proto file existed and version was valid)
    initializeAndLoadPersistentBackend(nodeDatabase, numMeshNodes, meshNodes, 
                                     "after loading nodes.proto (before packet processing starts)");
}

/**
 * @brief Patched version of meshNodes resize logic with memory check
 * 
 * This function checks available memory before resizing to prevent allocation
 * failures. It's used as a replacement for direct resize() calls in NodeDB.cpp.
 * 
 * @param meshNodes Pointer to the vector to resize
 * @param numMeshNodes Current number of nodes
 * @param maxNumNodes Maximum number of nodes to allocate
 */
inline void resizeMeshNodesSafely(std::vector<meshtastic_NodeInfoLite>* meshNodes, 
                                   int numMeshNodes, 
                                   uint32_t maxNumNodes)
{
    if (!meshNodes) return;
    
    // CRITICAL: If persistent backend is enabled, vector should be minimal (only local node)
    // Don't try to allocate MAX_NUM_NODES - persistent backend handles network nodes
    if (NodeDBPersistentBackend::isEnabled()) {
        // Persistent backend is active - vector should only contain local node (size 1)
        // Network nodes are stored in persistent backend (flash + cache), not in vector
        if (meshNodes->size() != 1) {
            meshNodes->resize(1);  // Ensure vector is minimal
        }
        return;  // Early return - no need to allocate large vector
    }
    
    // CRITICAL OPTIMIZATION: Check available memory before resize to prevent allocation failure
    // If vector is empty or small, resize might try to allocate MAX_NUM_NODES * sizeof(NodeInfoLite)
    // which can be 227KB - too much for available memory (181KB free)
    uint32_t freeHeap = memGet.getFreeHeap();
    uint32_t requiredMemory = MAX_NUM_NODES * sizeof(meshtastic_NodeInfoLite);
    
    // Only resize if we have enough memory, otherwise use current size or safe size
    if (freeHeap >= requiredMemory + 40960) {  // 40 KB safety margin
        meshNodes->resize(maxNumNodes);
    } else {
        // Calculate safe size based on available memory
        uint32_t safeMaxNodes = (freeHeap > 40960) ? ((freeHeap - 40960) / sizeof(meshtastic_NodeInfoLite)) : 1;
        if (safeMaxNodes > 256) safeMaxNodes = 256;  // Limit to reasonable maximum
        if (safeMaxNodes < (size_t)numMeshNodes) safeMaxNodes = numMeshNodes;  // At least current size
        
        if (meshNodes->size() < safeMaxNodes) {
            LOG_WARN("NodeDB: Insufficient memory for maxNumNodes (%u), using safe size (%u nodes, %u bytes free)", 
                    maxNumNodes, safeMaxNodes, freeHeap);
            meshNodes->resize(safeMaxNodes);
        } else {
            // Vector already has enough capacity, just ensure it's at least numMeshNodes
            if (meshNodes->size() < (size_t)numMeshNodes) {
                meshNodes->resize(numMeshNodes);
            }
        }
    }
}

/**
 * @brief Patched version of node insertion logic for persistent backend
 * 
 * This function handles node insertion when persistent backend is enabled.
 * It adds nodes to persistent backend instead of expanding the vector, preventing
 * memory allocation failures.
 * 
 * @param node Node data to insert
 * @param vec Pointer to the vector (should only contain local node)
 * @return true if node was successfully handled
 */
inline bool handleNodeInsertionForPersistentBackend(const meshtastic_NodeInfoLite& node, 
                                                  std::vector<meshtastic_NodeInfoLite>* vec)
{
    if (!vec) return false;
    
    // CRITICAL: If persistent backend is enabled, add ALL nodes to persistent backend (Main FS)
    // Vector can contain local node temporarily, but all nodes should be in Main FS
    if (NodeDBPersistentBackend::isEnabled()) {
        // Add ALL nodes (including local node) to persistent backend (Main FS)
        meshtastic_NodeInfoLite* backend_node = NodeDBPersistentBackend::getOrCreateNode(node.num);
        if (backend_node) {
            *backend_node = node;  // Copy node data to persistent backend
        }
        
        // Also add to vector if it's the local node (for compatibility)
        NodeNum localNodeNum = nodeDB ? nodeDB->getNodeNum() : 0;
        bool isLocalNode = (node.num == localNodeNum && localNodeNum != 0);
        if (isLocalNode && vec->size() == 0) {
            vec->push_back(node);
        }
        return true;
    }
    
    // Standard behavior: add to vector
    // WARNING: This can cause memory allocation failure if vector expands too much
    vec->push_back(node);
    return true;
}

/**
 * @brief Calculate saveWhat flags based on CRC changes and file existence
 * 
 * This function checks CRC after all modifications and also checks if files exist.
 * It ensures files are saved on first boot or after factory reset even if CRC didn't change.
 * 
 * @param devicestateCRC Initial CRC of devicestate (before modifications)
 * @param nodeDatabaseCRC Initial CRC of nodeDatabase (before modifications)
 * @param configCRC Initial CRC of config (before modifications)
 * @param channelFileCRC Initial CRC of channelFile (before modifications)
 * @param saveWhat Current saveWhat flags (will be modified)
 * @return Updated saveWhat flags
 */
inline int calculateSaveWhatFlags(uint32_t devicestateCRC, 
                                   uint32_t nodeDatabaseCRC, 
                                   uint32_t configCRC, 
                                   uint32_t channelFileCRC,
                                   int saveWhat)
{
    // CRITICAL: Check CRC AFTER all modifications to detect changes
    // If CRC changed, data was modified and needs to be saved
    extern meshtastic_DeviceState devicestate;
    extern meshtastic_NodeDatabase nodeDatabase;
    extern meshtastic_LocalConfig config;
    extern meshtastic_ChannelFile channelFile;
    
    if (devicestateCRC != crc32Buffer(&devicestate, sizeof(devicestate)))
        saveWhat |= SEGMENT_DEVICESTATE;
    if (nodeDatabaseCRC != crc32Buffer(&nodeDatabase, sizeof(nodeDatabase)))
        saveWhat |= SEGMENT_NODEDATABASE;
    if (configCRC != crc32Buffer(&config, sizeof(config)))
        saveWhat |= SEGMENT_CONFIG;
    if (channelFileCRC != crc32Buffer(&channelFile, sizeof(channelFile)))
        saveWhat |= SEGMENT_CHANNELS;
    
    // CRITICAL: Always save config and deviceState on first boot or if files don't exist
    // This ensures files are created even if CRC didn't change (e.g., after factory reset)
    #ifdef FSCom
    // Use string literals directly (same as defined in NodeDB.h as static constexpr)
    const char* configFileName = "/prefs/config.proto";
    const char* deviceStateFileName = "/prefs/device.proto";
    const char* moduleConfigFileName = "/prefs/module.proto";
    const char* channelFileName = "/prefs/channels.proto";
    
    if (!FSCom.exists(configFileName)) {
        saveWhat |= SEGMENT_CONFIG;
        LOG_DEBUG("Config file doesn't exist, will save");
    }
    if (!FSCom.exists(deviceStateFileName)) {
        saveWhat |= SEGMENT_DEVICESTATE;
        LOG_DEBUG("DeviceState file doesn't exist, will save");
    }
    if (!FSCom.exists(moduleConfigFileName)) {
        saveWhat |= SEGMENT_MODULECONFIG;
        LOG_DEBUG("ModuleConfig file doesn't exist, will save");
    }
    if (!FSCom.exists(channelFileName)) {
        saveWhat |= SEGMENT_CHANNELS;
        LOG_DEBUG("Channel file doesn't exist, will save");
    }
    #endif
    
    return saveWhat;
}

/**
 * @brief Initialize virtual backend in installDefaultNodeDatabase()
 * 
 * This function initializes virtual backend before allocating vector to prevent
 * large memory allocation. Returns true if virtual backend was initialized.
 * 
 * @param nodeDatabase Reference to nodeDatabase structure
 * @param numMeshNodes Reference to numMeshNodes counter
 * @param meshNodes Reference to meshNodes pointer
 * @return true if virtual backend was initialized, false otherwise
 */
inline bool initializeVirtualBackendForNodeDatabase(meshtastic_NodeDatabase& nodeDatabase,
                                                     pb_size_t& numMeshNodes,
                                                     std::vector<meshtastic_NodeInfoLite>*& meshNodes)
{
    // CRITICAL: For RAK4631 with persistent backend, initialize it BEFORE allocating vector
    // This prevents allocating 1234 nodes * 250 bytes = ~308 KB which exceeds available RAM
    // Persistent backend uses flash storage instead, so we only need a small vector for local node
    if (NodeDBPersistentBackend::isAvailable() && !NodeDBPersistentBackend::isEnabled()) {
        // Use common helper for initialization and loading
        if (initializeAndLoadPersistentBackend(nodeDatabase, numMeshNodes, meshNodes,
                                            "early to prevent large vector allocation")) {
            return true;  // Persistent backend initialized successfully
        } else {
            LOG_WARN("NodeDBPersistentBackend: Failed to initialize, will use standard allocation with memory checks");
            // Don't do fallback here - let performStandardVectorAllocation() handle it
            return false;  // Persistent backend failed, will use standard allocation
        }
    } else if (NodeDBPersistentBackend::isEnabled()) {
        // Persistent backend already initialized - use minimal vector
        LOG_DEBUG("NodeDB: Persistent backend already enabled, using minimal vector (1 node)");
        nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(1);  // Only local node in vector
        if (nodeDatabase.nodes.size() == 1) {
            numMeshNodes = 0;
            meshNodes = &nodeDatabase.nodes;
            LOG_DEBUG("NodeDB: Minimal vector allocated successfully");
            return true;
        } else {
            LOG_ERROR("NodeDB: Vector allocation failed - size mismatch (expected 1, got %u)", 
                     nodeDatabase.nodes.size());
            return false;
        }
    }
    return false;  // Virtual backend not available
}

/**
 * @brief Standard vector allocation with memory checks and fallbacks
 * 
 * This function performs standard vector allocation with comprehensive memory checks
 * and fallback strategies if allocation fails. Used when virtual backend is not available
 * or failed to initialize.
 * 
 * @param nodeDatabase Reference to nodeDatabase structure
 * @param numMeshNodes Reference to numMeshNodes counter
 * @param meshNodes Reference to meshNodes pointer
 * @param maxNumNodes Maximum number of nodes to allocate (from MAX_NUM_NODES macro)
 */
inline void performStandardVectorAllocation(meshtastic_NodeDatabase& nodeDatabase,
                                             pb_size_t& numMeshNodes,
                                             std::vector<meshtastic_NodeInfoLite>*& meshNodes,
                                             uint32_t maxNumNodes)
{
    // Standard allocation (for platforms without virtual backend)
    // WARNING: For RAK4631 with MAX_NUM_NODES=1234, this will likely fail due to memory constraints
    // Virtual backend should be used instead
    LOG_DEBUG("NodeDB: Attempting standard vector allocation (MAX_NUM_NODES=%u)", maxNumNodes);
    
    uint32_t freeHeapBefore = memGet.getFreeHeap();
    uint32_t heapTotal = memGet.getHeapSize();
    uint32_t requiredMemory = maxNumNodes * sizeof(meshtastic_NodeInfoLite);
    
    LOG_DEBUG("NodeDB: Memory check - required: %u bytes (%u nodes * %u bytes)", 
             requiredMemory, maxNumNodes, sizeof(meshtastic_NodeInfoLite));
    LOG_DEBUG("NodeDB: Memory status - free: %u bytes, total: %u bytes", freeHeapBefore, heapTotal);
    
    if (freeHeapBefore < requiredMemory + 40960) {  // 40 KB safety margin
        LOG_ERROR("NodeDB: Insufficient memory for standard allocation");
        LOG_ERROR("NodeDB:   Required: %u bytes + 40 KB margin", requiredMemory);
        LOG_ERROR("NodeDB:   Available: %u bytes", freeHeapBefore);
        
        // Try reduced size
        uint32_t safeMaxNodes = (freeHeapBefore > 40960) ? ((freeHeapBefore - 40960) / sizeof(meshtastic_NodeInfoLite)) : 1;
        if (safeMaxNodes > 256) safeMaxNodes = 256;
        if (safeMaxNodes < 1) safeMaxNodes = 1;
        
        LOG_WARN("NodeDB: Attempting reduced allocation (%u nodes instead of %u)", safeMaxNodes, maxNumNodes);
        nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(safeMaxNodes);
        if (nodeDatabase.nodes.size() == safeMaxNodes) {
            numMeshNodes = 0;
            meshNodes = &nodeDatabase.nodes;
            LOG_WARN("NodeDB: Reduced allocation successful (%u nodes)", safeMaxNodes);
            return;
        } else {
            LOG_ERROR("NodeDB: Reduced allocation failed - size mismatch (expected %u, got %u)", 
                     safeMaxNodes, nodeDatabase.nodes.size());
        }
        
        // Last resort: minimal allocation
        LOG_WARN("NodeDB: Attempting minimal allocation (1 node)");
        nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(1);
        if (nodeDatabase.nodes.size() == 1) {
            numMeshNodes = 0;
            meshNodes = &nodeDatabase.nodes;
            LOG_WARN("NodeDB: Minimal allocation successful (device will have limited node capacity)");
            return;
        } else {
            LOG_ERROR("NodeDB: ALL allocation attempts failed - device may not function correctly");
            // Set to empty vector as last resort
            nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>();
            numMeshNodes = 0;
            meshNodes = &nodeDatabase.nodes;
            return;
        }
    }
    
    // Try standard allocation
    nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(maxNumNodes);
    uint32_t freeHeapAfter = memGet.getFreeHeap();
    uint32_t allocatedMemory = freeHeapBefore - freeHeapAfter;
    
    if (nodeDatabase.nodes.size() == maxNumNodes) {
        numMeshNodes = 0;
        meshNodes = &nodeDatabase.nodes;
        LOG_INFO("NodeDB: Standard allocation successful (%u nodes, allocated %u bytes)", 
                maxNumNodes, allocatedMemory);
        return;
    } else {
        LOG_ERROR("NodeDB: Vector size mismatch (expected %u, got %u)", 
                 maxNumNodes, nodeDatabase.nodes.size());
    }
    
    // If we get here, standard allocation failed - try fallback
    LOG_WARN("NodeDB: Standard allocation failed, using fallback");
    uint32_t safeMaxNodes = (freeHeapBefore > 40960) ? ((freeHeapBefore - 40960) / sizeof(meshtastic_NodeInfoLite)) : 1;
    if (safeMaxNodes > 256) safeMaxNodes = 256;
    if (safeMaxNodes < 1) safeMaxNodes = 1;
    
    nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(safeMaxNodes);
    if (nodeDatabase.nodes.size() == safeMaxNodes) {
        numMeshNodes = 0;
        meshNodes = &nodeDatabase.nodes;
        LOG_WARN("NodeDB: Fallback allocation successful (%u nodes)", safeMaxNodes);
    } else {
        LOG_ERROR("NodeDB: Fallback allocation also failed - using minimal");
        nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(1);
        numMeshNodes = 0;
        meshNodes = &nodeDatabase.nodes;
    }
}

/**
 * @brief Reset nodes with virtual backend support
 * 
 * This function resets nodes, handling virtual backend if enabled.
 * 
 * @param nodeDatabase Reference to nodeDatabase structure
 * @param numMeshNodes Reference to numMeshNodes counter
 * @param getNodeNum Function to get local node number
 */
inline void resetNodesWithVirtualBackend(meshtastic_NodeDatabase& nodeDatabase,
                                         pb_size_t& numMeshNodes,
                                         std::function<NodeNum()> getNodeNum)
{
    if (NodeDBPersistentBackend::isEnabled()) {
        // Reset persistent backend (clears all network nodes)
        NodeDBPersistentBackend::resetNodes();
        // Vector should only contain local node (1 node)
        numMeshNodes = 1;
        if (nodeDatabase.nodes.size() > 1) {
            nodeDatabase.nodes.resize(1);
        }
        // Clear local node data (keep structure, clear fields)
        if (nodeDatabase.nodes.size() > 0) {
            nodeDatabase.nodes[0] = meshtastic_NodeInfoLite();
            nodeDatabase.nodes[0].num = getNodeNum();
        }
    } else {
        // Standard implementation
        numMeshNodes = 1;
        std::fill(nodeDatabase.nodes.begin() + 1, nodeDatabase.nodes.end(), meshtastic_NodeInfoLite());
    }
}

/**
 * @brief Remove node with virtual backend support
 * 
 * This function removes a node, handling virtual backend if enabled.
 * 
 * @param nodeNum Node number to remove
 * @param getNodeNum Function to get local node number
 * @return true if node was removed, false otherwise
 */
inline bool removeNodeByNumWithVirtualBackend(NodeNum nodeNum, std::function<NodeNum()> getNodeNum)
{
    if (NodeDBPersistentBackend::isEnabled()) {
        // All nodes (including local node) are in virtual backend
        if (NodeDBPersistentBackend::removeNode(nodeNum)) {
            LOG_DEBUG("NodeDB::removeNodeByNum: Removed node 0x%x from virtual backend", nodeNum);
            // Virtual backend handles saving internally
            return true;
        } else {
            LOG_DEBUG("NodeDB::removeNodeByNum: Node 0x%x not found in virtual backend", nodeNum);
            return false;
        }
    }
    return false;  // Not handled by virtual backend, use standard implementation
}

/**
 * @brief Read next mesh node with virtual backend support
 * 
 * This function reads next node, handling virtual backend if enabled.
 * 
 * @param readIndex Reference to read index (will be modified)
 * @param numMeshNodes Number of nodes in vector
 * @param meshNodes Pointer to vector
 * @return Pointer to next node, or NULL if no more nodes
 */
inline const meshtastic_NodeInfoLite* readNextMeshNodeWithVirtualBackend(uint32_t& readIndex,
                                                                          pb_size_t numMeshNodes,
                                                                          std::vector<meshtastic_NodeInfoLite>* meshNodes)
{
    // First, iterate through vector (local node)
    if (readIndex < numMeshNodes) {
        return &meshNodes->at(readIndex++);
    }

    // Then iterate through virtual backend (network nodes)
    if (NodeDBPersistentBackend::isEnabled()) {
        // Adjust index for virtual backend (subtract vector size)
        size_t backend_index = readIndex - numMeshNodes;
        size_t backend_count = NodeDBPersistentBackend::getTotalNodeCount();
        
        if (backend_index < backend_count) {
            meshtastic_NodeInfoLite* node = NodeDBPersistentBackend::getNodeByIndex(backend_index);
            readIndex++;  // Advance index
            return node;
        }
    }

    return NULL;
}

/**
 * @brief Get number of online mesh nodes with virtual backend support
 * 
 * This function counts online nodes, including virtual backend nodes.
 * 
 * @param numMeshNodes Number of nodes in vector
 * @param meshNodes Pointer to vector
 * @param localOnly If true, ignore nodes heard via MQTT
 * @param sinceLastSeen Function to calculate time since last seen
 * @return Number of online nodes
 */
inline size_t getNumOnlineMeshNodesWithVirtualBackend(pb_size_t numMeshNodes,
                                                      std::vector<meshtastic_NodeInfoLite>* meshNodes,
                                                      bool localOnly,
                                                      uint32_t (*sinceLastSeen)(const meshtastic_NodeInfoLite*))
{
    size_t numseen = 0;
    const uint32_t NUM_ONLINE_SECS = 60 * 60 * 2; // 2 hrs

    // First, check vector (local node)
    for (int i = 0; i < numMeshNodes; i++) {
        if (localOnly && meshNodes->at(i).via_mqtt)
            continue;
        if (sinceLastSeen(&meshNodes->at(i)) < NUM_ONLINE_SECS)
            numseen++;
    }

    // Also check virtual backend (network nodes)
    if (NodeDBPersistentBackend::isEnabled()) {
        size_t backend_count = NodeDBPersistentBackend::getTotalNodeCount();
        for (size_t i = 0; i < backend_count; i++) {
            meshtastic_NodeInfoLite* node = NodeDBPersistentBackend::getNodeByIndex(i);
            if (!node) {
                continue;
            }
            if (localOnly && node->via_mqtt) {
                continue;
            }
            if (sinceLastSeen(node) < NUM_ONLINE_SECS) {
                numseen++;
            }
        }
    }

    return numseen;
}

/**
 * @brief Get mesh node with virtual backend support
 * 
 * This function finds a node, checking virtual backend if enabled.
 * 
 * @param n Node number to find
 * @param numMeshNodes Number of nodes in vector
 * @param meshNodes Pointer to vector
 * @return Pointer to node, or NULL if not found
 */
inline meshtastic_NodeInfoLite* getMeshNodeWithVirtualBackend(NodeNum n,
                                                               pb_size_t numMeshNodes,
                                                               std::vector<meshtastic_NodeInfoLite>* meshNodes)
{
    // First, check vector (for local node)
    for (int i = 0; i < numMeshNodes; i++)
        if (meshNodes->at(i).num == n)
            return &meshNodes->at(i);

    // If not found in vector, check virtual backend (for network nodes)
    if (NodeDBPersistentBackend::isEnabled()) {
        // Network nodes are stored in virtual backend
        return NodeDBPersistentBackend::getNode(n);
    }

    return NULL;
}

/**
 * @brief Get number of mesh nodes with virtual backend support
 * 
 * This function returns total node count, including virtual backend nodes.
 * 
 * @param numMeshNodes Number of nodes in vector
 * @return Total number of nodes
 */
inline size_t getNumMeshNodesWithVirtualBackend(pb_size_t numMeshNodes)
{
    if (NodeDBPersistentBackend::isEnabled()) {
        // Vector contains only local node (1 node)
        // Virtual backend contains network nodes
        // Total = vector nodes + virtual backend nodes
        return numMeshNodes + NodeDBPersistentBackend::getTotalNodeCount();
    }
    // Standard implementation: only vector nodes
    return numMeshNodes;
}

/**
 * @brief Check if node database is full with virtual backend support
 * 
 * This function checks if database is full, considering virtual backend.
 * 
 * @param numMeshNodes Number of nodes in vector
 * @param maxNumNodes Maximum number of nodes
 * @return true if database is full
 */
inline bool isFullWithVirtualBackend(pb_size_t numMeshNodes, uint32_t maxNumNodes)
{
    if (NodeDBPersistentBackend::isEnabled()) {
        // Check virtual backend first (it has the real limit)
        if (NodeDBPersistentBackend::isFull()) {
            return true;
        }
        // Also check memory (standard check)
        return (memGet.getFreeHeap() < MemoryHelpers::Thresholds::SAFE_FREE_HEAP);
    }
    // Standard implementation
    return (numMeshNodes >= maxNumNodes) || (memGet.getFreeHeap() < MemoryHelpers::Thresholds::SAFE_FREE_HEAP);
}

/**
 * @brief Get or create mesh node with virtual backend support (lazy initialization)
 * 
 * This function handles lazy initialization of virtual backend when first network node is accessed.
 * It also saves local node to virtual backend after initialization.
 * 
 * @param n Node number
 * @param getNodeNum Function to get local node number
 * @param getMeshNode Function to get node from vector
 * @return Pointer to node from virtual backend, or NULL if should use standard implementation
 */
inline meshtastic_NodeInfoLite* getOrCreateMeshNodeLazyInit(NodeNum n,
                                                             std::function<NodeNum()> getNodeNum,
                                                             std::function<meshtastic_NodeInfoLite*(NodeNum)> getMeshNode)
{
    if (n != getNodeNum()) {
        // This is a network node (not local) - use virtual backend
        // Initialize virtual backend on first network node access (lazy initialization)
        if (NodeDBPersistentBackend::isAvailable() && !NodeDBPersistentBackend::isEnabled()) {
            // Backend is available but not initialized - initialize it now
            if (!NodeDBPersistentBackend::initialize()) {
                LOG_ERROR("NodeDBPersistentBackend: Failed to initialize, falling back to standard implementation");
                // Fall through to standard implementation
                return NULL;
            } else {
                // CRITICAL: This is lazy initialization path (during packet processing)
                // In this case, we cannot block for long, so loadFromDisk() will be called incrementally
                // The main initialization path (in installDefaultNodeDatabase) loads everything before packets arrive
                LOG_DEBUG("NodeDBPersistentBackend: Initialized lazily during packet processing, will load incrementally");
                
                #ifdef PAUSE_ON_START
                // DEBUG MODE: Pause execution but keep console working
                LOG_INFO("========================================");
                LOG_INFO("PAUSE MODE ENABLED - Execution paused");
                LOG_INFO("Console is active, watchdog is fed");
                LOG_INFO("Press reset to continue or disable PAUSE_ON_START");
                LOG_INFO("========================================");
                #ifdef ARCH_NRF52
                extern void nrf52Loop();
                while (true) {
                    // Feed watchdog to prevent reboot
                    nrf52Loop();
                    yield();  // Allow console and other tasks to run
                    delay(100);  // Small delay to prevent CPU spinning
                }
                #else
                while (true) {
                    yield();  // Allow console and other tasks to run
                    delay(100);  // Small delay to prevent CPU spinning
                }
                #endif
                #endif
                
                // NOTE: Local node can now be saved to Extended FS (virtual backend)
                // All nodes (including local node) are stored in Extended FS
            }
        }
        
        // Try to use virtual backend if initialized
        if (NodeDBPersistentBackend::isEnabled()) {
            meshtastic_NodeInfoLite* result = NodeDBPersistentBackend::getOrCreateNode(n);
            if (result) {
                return result;  // Virtual backend succeeded
            }
            // If virtual backend failed, fall through to standard implementation
        }
    }
    
    return NULL;  // Not handled by virtual backend, use standard implementation
}

/**
 * @brief Update from packet with virtual backend support
 * 
 * This function updates node from packet, using virtual backend if enabled.
 * 
 * @param mp Pointer to mesh packet
 * @param getFrom Function to get source node number
 */
inline void updateFromWithVirtualBackend(const meshtastic_MeshPacket* mp,
                                         NodeNum (*getFrom)(const meshtastic_MeshPacket*))
{
    if (mp->from) {
        // Use virtual backend if enabled (handles hot data updates without flash writes)
        if (NodeDBPersistentBackend::isEnabled()) {
            NodeDBPersistentBackend::updateFromPacket(getFrom(mp), mp);
        }
    }
}

/**
 * @brief Macro to call background task tick from NodeDB operations
 * 
 * This should be called from saveToDisk() and other NodeDB operations
 * to ensure background task runs cooperatively.
 */
#define NODEDB_BACKGROUND_TICK() \
    do { \
        if (NodeDBBackgroundTask::isInitialized()) { \
            NodeDBBackgroundTask::tick(); \
        } \
    } while(0)

/**
 * @brief Inline function to call background task tick from main loop
 * 
 * This function should be called from main loop() to ensure background task
 * runs cooperatively. It's a convenience wrapper around NodeDBBackgroundTask::tick().
 * 
 * Usage in main.cpp:
 *   #include "variants/rak4631_lite/nodedb/NodeDBUnified.h"
 *   void loop() {
 *       nodeDBBackgroundTaskTick();
 *       // ... other loop code
 *   }
 */
inline void nodeDBBackgroundTaskTick()
{
    NodeDBBackgroundTask::tick();
}

#endif // ARCH_NRF52
#endif // USE_EXTENDED_FS_FOR_NODEDB

