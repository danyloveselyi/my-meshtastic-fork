/**
 * @file NodeDBUnified.cpp
 * @brief Unified NodeDB implementation for RAK4631 Lite
 * 
 * This file contains all NodeDB components in a single file for easier
 * LLM analysis and code review. All namespaces are preserved for clarity.
 */

#include "NodeDBUnified.h"
// Note: configuration.h, mesh/NodeDB.h, and memGet.h are already included in NodeDBUnified.h
#include "../variant.h"
#include "../filesystem/FilesystemUnified.h"
#include "../../../src/mesh/MeshTypes.h"
#include "../../../src/mesh/TypeConversions.h"
#include "../../../src/gps/RTC.h"
#include "../../../src/platform/stm32wl/littlefs/lfs.h"
#include "../../../src/platform/stm32wl/littlefs/lfs_util.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#ifdef ARCH_NRF52
#include "rtos.h"
extern void nrf52Loop();
#include "NRF52Bluetooth.h"
#include "../../../src/BluetoothStatus.h"
#include "../../../src/mesh/RadioLibInterface.h"
extern NRF52Bluetooth *nrf52Bluetooth;
extern meshtastic::BluetoothStatus *bluetoothStatus;
#endif
#include <Arduino.h>

#ifdef USE_EXTENDED_FS_FOR_NODEDB


// ============================================================================
// From MemoryHelpers.cpp
// ============================================================================

namespace MemoryHelpers {

MemoryCheckResult checkMemory(uint32_t requiredMemory, uint32_t safetyMargin, const char* componentName)
{
    MemoryCheckResult result;
    result.requiredMemory = requiredMemory;
    result.safetyMargin = safetyMargin;
    result.totalNeeded = requiredMemory + safetyMargin;
    
    result.freeHeap = memGet.getFreeHeap();
    result.heapTotal = memGet.getHeapSize();
    result.usedHeap = (result.heapTotal > result.freeHeap) ? (result.heapTotal - result.freeHeap) : 0;
    
    if (result.freeHeap < result.totalNeeded) {
        result.sufficient = false;
        result.shortage = result.totalNeeded - result.freeHeap;
        logMemoryCheckFailure(result, componentName);
    } else {
        result.sufficient = true;
        result.shortage = 0;
    }
    
    return result;
}

void logMemoryCheckFailure(const MemoryCheckResult& result, const char* componentName)
{
    LOG_ERROR("%s: Not enough memory to allocate", componentName);
    LOG_ERROR("%s:   Required: %u bytes + %u bytes (margin) = %u bytes total", 
             componentName, result.requiredMemory, result.safetyMargin, result.totalNeeded);
    LOG_ERROR("%s:   Available: %u bytes free, %u bytes used, %u bytes total", 
             componentName, result.freeHeap, result.usedHeap, result.heapTotal);
    LOG_ERROR("%s:   Shortage: %u bytes", componentName, result.shortage);
}

void logAllocationFailure(const char* componentName, uint32_t requestedBytes, 
                          uint32_t elementSize, uint32_t elementCount,
                          uint32_t freeHeapBefore, uint32_t freeHeapAfter)
{
    LOG_ERROR("%s: Allocation FAILED", componentName);
    LOG_ERROR("%s:   Requested: %u bytes (%u elements * %u bytes)", 
             componentName, requestedBytes, elementCount, elementSize);
    LOG_ERROR("%s:   Free heap before: %u bytes, after: %u bytes", 
             componentName, freeHeapBefore, freeHeapAfter);
}

uint32_t getMaxNodesIndex()
{
    #ifndef MAX_NODES_INDEX
    #ifdef MAX_NODES_SLOTS
    return MAX_NODES_SLOTS;
    #else
    return MAX_NUM_NODES;  // From variant.h
    #endif
    #else
    return MAX_NODES_INDEX;
    #endif
}

uint32_t getMaxNodesSlots()
{
    #ifndef MAX_NODES_SLOTS
    return MAX_NUM_NODES;  // From variant.h
    #else
    return MAX_NODES_SLOTS;
    #endif
}

uint32_t getMaxNodesCache()
{
    #ifndef MAX_NODES_CACHE
    return 300;  // Default cache size
    #else
    return MAX_NODES_CACHE;
    #endif
}

} // namespace MemoryHelpers

// ============================================================================
// Shared helper functions for memory management
// ============================================================================

/**
 * @brief Free memory helper (handles both rtos_free and delete[])
 * @tparam T Type of pointer
 * @param ptr Pointer to free (set to nullptr after freeing)
 */
template<typename T>
static void freeMemory(T*& ptr)
{
    if (ptr) {
        #ifdef ARCH_NRF52
        rtos_free(ptr);
        #else
        delete[] ptr;
        #endif
        ptr = nullptr;
    }
}

/**
 * @brief Aggressive cache eviction helper
 * @param minFreeHeap Minimum free heap required
 * @param maxIterations Maximum number of eviction iterations
 * @return Number of nodes evicted
 */
static int aggressiveCacheEviction(uint32_t minFreeHeap, int maxIterations)
{
    int evicted = 0;
    for (int i = 0; i < maxIterations && memGet.getFreeHeap() < minFreeHeap; i++) {
        NodeNum node_to_evict = NodeIndex::getEvictionCandidate(false);
        if (node_to_evict != 0) {
            NodeCache::evictNode(node_to_evict);
            evicted++;
        } else {
            break;  // No more nodes to evict
        }
    }
    return evicted;
}

// ============================================================================
// Shared helper functions for memory allocation
// ============================================================================

/**
 * @brief Generic memory allocation helper with error checking
 * @tparam T Type to allocate
 * @param ptr Pointer to store allocated memory
 * @param count Number of elements to allocate
 * @param componentName Component name for logging
 * @param freeHeapBefore Free heap before allocation (from MemoryCheckResult)
 * @return true if allocation succeeded, false otherwise
 */
template<typename T>
static bool allocateMemoryWithCheck(T*& ptr, uint32_t count, const char* componentName, uint32_t freeHeapBefore)
{
    uint32_t allocation_size = count * sizeof(T);
    
    #ifdef ARCH_NRF52
    ptr = (T*)rtos_malloc(allocation_size);
    if (!ptr) {
        uint32_t freeHeapAfter = memGet.getFreeHeap();
        MemoryHelpers::logAllocationFailure(componentName, allocation_size, sizeof(T), 
                                           count, freeHeapBefore, freeHeapAfter);
        return false;
    }
    #else
    ptr = new T[count];
    if (!ptr) {
        uint32_t freeHeapAfter = memGet.getFreeHeap();
        MemoryHelpers::logAllocationFailure(componentName, allocation_size, sizeof(T), 
                                           count, freeHeapBefore, freeHeapAfter);
        return false;
    }
    #endif
    
    // CRITICAL: Log successful allocation for debugging
    uint32_t freeHeapAfter = memGet.getFreeHeap();
    uint32_t allocatedMemory = freeHeapBefore - freeHeapAfter;
    LOG_DEBUG("%s: Successfully allocated %u bytes (%u elements of %u bytes each)", 
             componentName, allocation_size, count, sizeof(T));
    LOG_DEBUG("%s: Free heap before: %u bytes, after: %u bytes, allocated: %u bytes", 
             componentName, freeHeapBefore, freeHeapAfter, allocatedMemory);
    
    return true;
}

// ============================================================================
// From NodeIndexStorage.cpp
// ============================================================================

// ============================================================================
// Shared helper functions (used by both NodeIndex and NodeStorage)
// ============================================================================

/**
 * @brief Helper function to get main filesystem with error checking
 * @return Pointer to lfs_t, or nullptr if not available
 */
static lfs_t* getMainFSWithCheck()
{
    FEED_WATCHDOG_AND_YIELD();
    lfs_t* main_lfs = getMainFS();
    FEED_WATCHDOG_AND_YIELD();
    if (!main_lfs) {
        LOG_ERROR("NodeIndexStorage: Main filesystem not available");
    }
    return main_lfs;
}

/**
 * @brief Helper function to safely close file on error (optionally remove file)
 * @param main_lfs Filesystem handle
 * @param file File handle
 * @param filename Optional filename to remove (nullptr to skip removal)
 */
static void closeFileOnError(lfs_t* main_lfs, lfs_file_t* file, const char* filename = nullptr)
{
    if (main_lfs && file) {
        lfs_file_close(main_lfs, file);
    }
    if (main_lfs && filename) {
        lfs_remove(main_lfs, filename);
    }
}

/**
 * @brief Helper function to check and log LittleFS write result
 * @param main_lfs Filesystem handle
 * @param file File handle
 * @param write_result Result from lfs_file_write
 * @param expected_size Expected size
 * @param error_msg Error message format string
 * @param component_name Component name for logging
 * @return true if write was successful, false otherwise
 */
static bool checkWriteResult(lfs_t* main_lfs, lfs_file_t* file, lfs_ssize_t write_result, 
                             size_t expected_size, const char* error_msg, const char* component_name)
{
    if (write_result != (lfs_ssize_t)expected_size) {
        LOG_ERROR(error_msg, component_name, (int)write_result, (unsigned)expected_size);
        closeFileOnError(main_lfs, file);
        return false;
    }
    return true;
}

/**
 * @brief Helper function to check and log LittleFS read result
 * @param main_lfs Filesystem handle
 * @param file File handle
 * @param read_result Result from lfs_file_read
 * @param expected_size Expected size
 * @param error_msg Error message format string
 * @param component_name Component name for logging
 * @return true if read was successful, false otherwise
 */
static bool checkReadResult(lfs_t* main_lfs, lfs_file_t* file, lfs_ssize_t read_result,
                            size_t expected_size, const char* error_msg, const char* component_name)
{
    if (read_result != (lfs_ssize_t)expected_size) {
        LOG_ERROR(error_msg, component_name, (int)read_result, (unsigned)expected_size);
        closeFileOnError(main_lfs, file);
        return false;
    }
    return true;
}

// cleanupFileOnError is now an alias for closeFileOnError with filename parameter
#define cleanupFileOnError(main_lfs, file, filename) closeFileOnError(main_lfs, file, filename)

// ============================================================================
// Index Implementation (previously NodeIndex namespace)
// ============================================================================

namespace NodeIndex {

// Static storage for index entries
static NodeIndexEntry* index_entries = nullptr;
static uint32_t max_entries = 0;
static uint32_t num_entries = 0;
static bool initialized = false;

// Macros for initialization checks
#define CHECK_INDEX_INIT() do { if (!initialized) return false; } while(0)
#define CHECK_INDEX_INIT_NULL() do { if (!initialized) return nullptr; } while(0)
#define CHECK_INDEX_INIT_VOID() do { if (!initialized) return; } while(0)
#define CHECK_INDEX_INIT_ZERO() do { if (!initialized) return 0; } while(0)

bool initialize(uint32_t maxEntries)
{
    if (initialized) {
        if (maxEntries > max_entries) {
            return false;
        }
        return true;
    }
    
    uint32_t maxNodesIndex = MemoryHelpers::getMaxNodesIndex();
    if (maxEntries == 0 || maxEntries > maxNodesIndex) {
        return false;
    }
    
    uint32_t requiredMemory = maxEntries * sizeof(NodeIndexEntry);
    uint32_t safetyMargin = MemoryHelpers::SafetyMargins::INDEX;
    auto memCheck = MemoryHelpers::checkMemory(requiredMemory, safetyMargin, "NodeIndex");
    
    if (!memCheck.sufficient) {
        return false;
    }
    
    uint32_t freeHeapBefore = memCheck.freeHeap;
    
    if (!allocateMemoryWithCheck(index_entries, maxEntries, "NodeIndex", freeHeapBefore)) {
        return false;
    }
    
    uint32_t freeHeapAfter = memGet.getFreeHeap();
    uint32_t allocatedMemory = freeHeapBefore - freeHeapAfter;
    uint32_t allocation_size = maxEntries * sizeof(NodeIndexEntry);
    
    max_entries = maxEntries;
    num_entries = 0;
    initialized = true;
    
    memset(index_entries, 0, sizeof(NodeIndexEntry) * max_entries);
    
    LOG_INFO("NodeIndex: Initialized successfully");
    LOG_DEBUG("NodeIndex:   Max entries: %u", max_entries);
    LOG_DEBUG("NodeIndex:   Entry size: %u bytes", sizeof(NodeIndexEntry));
    LOG_DEBUG("NodeIndex:   Total allocated: %u bytes", allocation_size);
    LOG_DEBUG("NodeIndex:   Free heap before: %u bytes, after: %u bytes, allocated: %u bytes", 
             freeHeapBefore, freeHeapAfter, allocatedMemory);
    
    return true;
}

NodeIndexEntry* findNode(NodeNum nodeNum)
{
    if (!initialized || num_entries == 0 || !index_entries) {
        return nullptr;
    }
    
    // CRITICAL: Bounds checking for binary search to prevent buffer overflow
    int left = 0;
    int right = (int)num_entries - 1;
    
    // CRITICAL: Validate initial bounds
    if (right < 0 || right >= (int)max_entries) {
        LOG_ERROR("NodeIndex: Invalid num_entries (%u) or max_entries (%u) in findNode", num_entries, max_entries);
        return nullptr;
    }
    
    while (left <= right) {
        // CRITICAL: Calculate mid safely (prevents integer overflow)
        int mid = left + (right - left) / 2;
        
        // CRITICAL: Bounds check before array access
        if (mid < 0 || mid >= (int)num_entries) {
            LOG_ERROR("NodeIndex: Binary search mid index out of bounds (mid: %d, num_entries: %u)", mid, num_entries);
            return nullptr;
        }
        
        if (index_entries[mid].num == nodeNum) {
            return &index_entries[mid];
        } else if (index_entries[mid].num < nodeNum) {
            left = mid + 1;
            // CRITICAL: Validate left doesn't exceed bounds
            if (left > (int)num_entries) {
                break;  // Node not found, exit safely
            }
        } else {
            right = mid - 1;
            // CRITICAL: Validate right doesn't go negative
            if (right < 0) {
                break;  // Node not found, exit safely
            }
        }
    }
    
    return nullptr;
}

bool addNode(NodeNum nodeNum, uint16_t slotId)
{
    if (!initialized) {
        if (!initialize(MemoryHelpers::getMaxNodesIndex())) {
            return false;
        }
    }
    
    if (findNode(nodeNum) != nullptr) {
        return false;
    }
    
    if (num_entries >= max_entries) {
        return false;
    }
    
    // CRITICAL: Bounds checking for insert position
    int insert_pos = (int)num_entries;
    for (int i = 0; i < (int)num_entries; i++) {
        // CRITICAL: Validate array index before access
        if (i < 0 || i >= (int)max_entries) {
            LOG_ERROR("NodeIndex: Invalid index %d in addNode (max_entries: %u)", i, max_entries);
            return false;
        }
        if (index_entries[i].num > nodeNum) {
            insert_pos = i;
            break;
        }
    }
    
    // CRITICAL: Validate insert_pos before memmove
    if (insert_pos < 0 || insert_pos > (int)num_entries) {
        LOG_ERROR("NodeIndex: Invalid insert_pos %d in addNode (num_entries: %u)", insert_pos, num_entries);
        return false;
    }
    
    if (insert_pos < (int)num_entries) {
        // CRITICAL: Validate memmove bounds
        uint32_t shift_size = num_entries - insert_pos;
        if (insert_pos + 1 + shift_size > max_entries) {
            LOG_ERROR("NodeIndex: memmove would overflow (insert_pos: %d, shift_size: %u, max: %u)", 
                     insert_pos, shift_size, max_entries);
            return false;
        }
        memmove(&index_entries[insert_pos + 1], &index_entries[insert_pos],
                shift_size * sizeof(NodeIndexEntry));
    }
    
    // CRITICAL: Validate slot_id before adding to index
    // This prevents corruption from USB Mass Storage bootloader writes
    // Bootloader may write firmware to Main FS area (0xA8000-0xF4000), corrupting filesystem metadata
    uint32_t max_slots = MemoryHelpers::getMaxNodesSlots();
    if (slotId >= max_slots) {
        LOG_ERROR("NodeIndex: Invalid slot_id %u (max: %u) - cannot add node 0x%x to index!", 
                 slotId, max_slots - 1, nodeNum);
        LOG_ERROR("NodeIndex: This may be caused by USB Mass Storage bootloader overwriting Main FS");
        return false;  // Don't add invalid slot_id to index
    }
    
    // Also check for invalid node numbers (0 is invalid)
    if (nodeNum == 0) {
        LOG_ERROR("NodeIndex: Invalid nodeNum (0) - cannot add to index!");
        return false;
    }
    
    NodeIndexEntry* entry = &index_entries[insert_pos];
    entry->num = nodeNum;
    entry->slot_id = slotId;
    entry->flags = 0;
    entry->last_heard = 0;
    entry->access_count = 0;
    
    num_entries++;
    
    return true;
}

bool removeNode(NodeNum nodeNum)
{
    CHECK_INDEX_INIT();
    
    NodeIndexEntry* entry = findNode(nodeNum);
    if (!entry) {
        return false;
    }
    
    int pos = entry - index_entries;
    
    // CRITICAL: Validate position before memmove
    if (pos < 0 || pos >= (int)num_entries) {
        LOG_ERROR("NodeIndex: Invalid position %d in removeNode (num_entries: %u)", pos, num_entries);
        return false;
    }
    
    if (pos < (int)num_entries - 1) {
        // CRITICAL: Validate memmove bounds
        uint32_t shift_size = num_entries - pos - 1;
        if (pos + shift_size > max_entries) {
            LOG_ERROR("NodeIndex: memmove would overflow in removeNode (pos: %d, shift_size: %u, max: %u)", 
                     pos, shift_size, max_entries);
            return false;
        }
        memmove(&index_entries[pos], &index_entries[pos + 1],
                shift_size * sizeof(NodeIndexEntry));
    }
    
    // CRITICAL: Validate num_entries before decrementing
    if (num_entries == 0) {
        LOG_ERROR("NodeIndex: Attempted to decrement num_entries when already 0");
        return false;
    }
    
    num_entries--;
    
    return true;
}

bool updateLastHeard(NodeNum nodeNum, uint32_t timestamp)
{
    NodeIndexEntry* entry = findNode(nodeNum);
    if (!entry) {
        return false;
    }
    
    entry->last_heard = timestamp;
    return true;
}

bool incrementAccessCount(NodeNum nodeNum)
{
    NodeIndexEntry* entry = findNode(nodeNum);
    if (!entry) {
        return false;
    }
    
    if (entry->access_count < UINT32_MAX) {
        entry->access_count++;
    }
    
    return true;
}

/**
 * @brief Internal helper to set/clear a flag in node index entry
 * @param nodeNum Node number
 * @param flag Flag to set/clear
 * @param set true to set flag, false to clear
 * @return true if successful
 */
static bool setNodeFlag(NodeNum nodeNum, uint8_t flag, bool set)
{
    NodeIndexEntry* entry = findNode(nodeNum);
    if (!entry) {
        return false;
    }
    
    if (set) {
        entry->flags |= flag;
    } else {
        entry->flags &= ~flag;
    }
    
    return true;
}

bool markProtected(NodeNum nodeNum, bool isProtected)
{
    return setNodeFlag(nodeNum, NodeIndexEntry::FLAG_PROTECTED, isProtected);
}

bool markCached(NodeNum nodeNum, bool cached)
{
    return setNodeFlag(nodeNum, NodeIndexEntry::FLAG_CACHED, cached);
}

bool markDirty(NodeNum nodeNum, bool dirty)
{
    return setNodeFlag(nodeNum, NodeIndexEntry::FLAG_DIRTY, dirty);
}

NodeNum getEvictionCandidate(bool allowCached)
{
    if (!initialized || num_entries == 0) {
        return 0;
    }
    
    RTCQuality time_quality = getRTCQuality();
    uint32_t now = getValidTime(RTCQualityDevice, false);
    bool time_valid = (time_quality != RTCQualityNone && now > 0);
    
    NodeIndexEntry* candidate = nullptr;
    uint32_t lowest_priority = UINT32_MAX;
    
    for (uint32_t i = 0; i < num_entries; i++) {
        // CRITICAL: Check bounds before accessing index_entries
        if (i >= max_entries) {
            LOG_ERROR("NodeIndex: Index out of bounds in getEvictionCandidate (i: %u, max_entries: %u)", i, max_entries);
            break;
        }
        NodeIndexEntry* entry = &index_entries[i];
        
        if (entry->flags & NodeIndexEntry::FLAG_PROTECTED) {
            continue;
        }
        
        if (!allowCached && (entry->flags & NodeIndexEntry::FLAG_CACHED)) {
            continue;
        }
        
        uint32_t priority = UINT32_MAX;
        if (time_valid && entry->last_heard > 0) {
            if (entry->last_heard > now) {
                priority = 0;
            } else {
                priority = entry->last_heard;
            }
        } else if (entry->access_count > 0) {
            priority = entry->access_count;
        } else {
            priority = i;
        }
        
        if (priority < lowest_priority) {
            lowest_priority = priority;
            candidate = entry;
        }
    }
    
    if (candidate) {
        return candidate->num;
    }
    
    return 0;
}

uint32_t getNodeCount()
{
    if (!initialized || index_entries == nullptr) {
        return 0;
    }
    return num_entries;
}

uint32_t getCachedNodeCount()
{
    CHECK_INDEX_INIT_ZERO();
    
    uint32_t count = 0;
    for (uint32_t i = 0; i < num_entries; i++) {
        // CRITICAL: Check bounds before accessing index_entries
        if (i >= max_entries) {
            LOG_ERROR("NodeIndex: Index out of bounds in getCachedNodeCount (i: %u, max_entries: %u)", i, max_entries);
            break;
        }
        if (index_entries[i].flags & NodeIndexEntry::FLAG_CACHED) {
            count++;
        }
    }
    
    return count;
}

// Removed unused diagnostic functions:
// - getProtectedNodeCount() - not used, can be calculated by iterating if needed
// - getDirtyNodeCount() - not used, can be calculated by iterating if needed

void clear()
{
    if (index_entries) {
        memset(index_entries, 0, sizeof(NodeIndexEntry) * max_entries);
    }
    num_entries = 0;
}

uint32_t getAllNodeNums(NodeNum* buffer, uint32_t bufferSize)
{
    if (!initialized || !buffer || bufferSize == 0) {
        return 0;
    }
    
    uint32_t count = (num_entries < bufferSize) ? num_entries : bufferSize;
    // CRITICAL: Ensure count doesn't exceed max_entries
    if (count > max_entries) {
        count = max_entries;
    }
    for (uint32_t i = 0; i < count; i++) {
        // CRITICAL: Check bounds before accessing index_entries
        if (i >= num_entries || i >= max_entries) {
            LOG_ERROR("NodeIndex: Index out of bounds in getAllNodeNums (i: %u, num_entries: %u, max_entries: %u)", 
                     i, num_entries, max_entries);
            break;
        }
        buffer[i] = index_entries[i].num;
    }
    
    return count;
}

NodeNum getNodeNumByIndex(uint32_t index)
{
    if (!initialized || index >= num_entries || index >= max_entries) {
        return 0;
    }
    
    return index_entries[index].num;
}

// Removed unused functions:
// - getLastHeard() - not used, can be accessed via findNode() if needed
// - getEntryByIndex() - duplicates getNodeNumByIndex() functionality, use findNode(getNodeNumByIndex(i)) if needed

bool saveToFlash()
{
    if (!initialized || !index_entries || num_entries == 0) {
        return false;
    }
    
    lfs_t* main_lfs = getMainFSWithCheck();
    if (!main_lfs) {
        LOG_ERROR("NodeIndex: Main filesystem not available for saving index");
        return false;
    }
    
    const char* index_filename = "/prefs/nodeindex.bin";
    
    lfs_file_t file;
    // Use Main FS directly - openFileForOverwrite equivalent
    int open_result = lfs_file_open(main_lfs, &file, index_filename, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (open_result != LFS_ERR_OK) {
        LOG_ERROR("NodeIndex: Failed to open index file for writing (error: %d)", open_result);
        return false;
    }
    
    uint32_t index_version = 1;
    uint32_t entry_count = num_entries;
    
    // Write version
    lfs_ssize_t write_result = lfs_file_write(main_lfs, &file, &index_version, sizeof(index_version));
    if (!checkWriteResult(main_lfs, &file, write_result, sizeof(index_version), 
                         "%s: Failed to write index version (wrote %d of %u)", "NodeIndex")) {
        return false;
    }
    
    // Write entry count
    write_result = lfs_file_write(main_lfs, &file, &entry_count, sizeof(entry_count));
    if (!checkWriteResult(main_lfs, &file, write_result, sizeof(entry_count),
                         "%s: Failed to write entry count (wrote %d of %u)", "NodeIndex")) {
        return false;
    }
    
    // Write entries
    size_t entry_size = sizeof(NodeIndexEntry);
    size_t total_size = entry_count * entry_size;
    write_result = lfs_file_write(main_lfs, &file, index_entries, total_size);
    if (!checkWriteResult(main_lfs, &file, write_result, total_size,
                         "%s: Failed to write index entries (wrote %d of %u)", "NodeIndex")) {
        return false;
    }
    
    int sync_result = lfs_file_sync(main_lfs, &file);
    int close_result = lfs_file_close(main_lfs, &file);
    
    if (sync_result != LFS_ERR_OK || close_result != LFS_ERR_OK) {
        LOG_ERROR("NodeIndex: Failed to sync/close index file (sync: %d, close: %d)", sync_result, close_result);
        return false;
    }
    
    LOG_DEBUG("NodeIndex: Saved %u entries to flash (%u bytes)", entry_count, (unsigned)(sizeof(index_version) + sizeof(entry_count) + total_size));
    return true;
}

bool loadFromFlash()
{
    if (!initialized || !index_entries) {
        return false;
    }
    
    lfs_t* main_lfs = getMainFSWithCheck();
    if (!main_lfs) {
        LOG_DEBUG("NodeIndex: Main filesystem not available for loading index");
        return false;
    }
    
    const char* index_filename = "/prefs/nodeindex.bin";
    
    lfs_file_t file;
    int open_result = lfs_file_open(main_lfs, &file, index_filename, LFS_O_RDONLY);
    if (open_result != LFS_ERR_OK) {
        LOG_DEBUG("NodeIndex: Index file not found (error: %d) - will rebuild from slots", open_result);
        return false;
    }
    
    uint32_t index_version = 0;
    uint32_t entry_count = 0;
    
    // Read version
    lfs_ssize_t read_result = lfs_file_read(main_lfs, &file, &index_version, sizeof(index_version));
    if (!checkReadResult(main_lfs, &file, read_result, sizeof(index_version),
                        "%s: Failed to read index version (read %d of %u)", "NodeIndex")) {
        return false;
    }
    
    if (index_version != 1) {
        LOG_WARN("NodeIndex: Unsupported index version %u (expected 1)", index_version);
        closeFileOnError(main_lfs, &file);
        return false;
    }
    
    // Read entry count
    read_result = lfs_file_read(main_lfs, &file, &entry_count, sizeof(entry_count));
    if (!checkReadResult(main_lfs, &file, read_result, sizeof(entry_count),
                        "%s: Failed to read entry count (read %d of %u)", "NodeIndex")) {
        return false;
    }
    
    if (entry_count == 0 || entry_count > max_entries) {
        LOG_ERROR("NodeIndex: Invalid entry count %u (max: %u)", entry_count, max_entries);
        closeFileOnError(main_lfs, &file);
        return false;
    }
    
    clear();
    
    // Read entries
    size_t entry_size = sizeof(NodeIndexEntry);
    size_t total_size = entry_count * entry_size;
    read_result = lfs_file_read(main_lfs, &file, index_entries, total_size);
    lfs_file_close(main_lfs, &file);
    
    if (read_result != (lfs_ssize_t)total_size) {
        LOG_ERROR("NodeIndex: Failed to read index entries (read %d of %u)", (int)read_result, (unsigned)total_size);
        clear();
        return false;
    }
    
    // CRITICAL: Validate all slot IDs in index are within valid range
    // This prevents corruption from USB Mass Storage bootloader writes that might damage Extended FS
    // Bootloader may write firmware to Main FS area (0xA8000-0xF4000), corrupting filesystem metadata
    uint32_t max_slots = MemoryHelpers::getMaxNodesSlots();
    bool index_corrupted = false;
    for (uint32_t i = 0; i < entry_count; i++) {
        if (index_entries[i].slot_id >= max_slots) {
            LOG_ERROR("NodeIndex: Invalid slot_id %u (max: %u) in index entry %u - index corrupted!", 
                     index_entries[i].slot_id, max_slots - 1, i);
            LOG_ERROR("NodeIndex: NodeNum: 0x%x, slot_id: %u", index_entries[i].num, index_entries[i].slot_id);
            index_corrupted = true;
        }
        // Also check for invalid node numbers (0 is invalid)
        if (index_entries[i].num == 0) {
            LOG_ERROR("NodeIndex: Invalid nodeNum (0) in index entry %u - index corrupted!", i);
            index_corrupted = true;
        }
    }
    
    if (index_corrupted) {
        LOG_ERROR("NodeIndex: Index corruption detected - likely caused by USB Mass Storage bootloader");
        LOG_ERROR("NodeIndex: Bootloader may have written firmware to Main FS area (0xA8000-0xF4000)");
        LOG_ERROR("NodeIndex: Clearing corrupted index - will rebuild from slots");
        clear();
        return false;  // Force rebuild from slots
    }
    
    num_entries = entry_count;
    
    LOG_INFO("NodeIndex: Loaded %u entries from flash (version %u)", entry_count, index_version);
    return true;
}

} // namespace NodeIndex

// ============================================================================
// Storage Implementation (previously NodeStorage namespace)
// ============================================================================

namespace NodeStorage {

// Constants
constexpr const char* SLOTS_DIR = "/prefs/nodes";
constexpr const char* SLOT_FILE_PREFIX = "/prefs/nodes/slot_";
constexpr const char* SLOT_FILE_SUFFIX = ".bin";
#ifndef MAX_NODES_SLOTS
#define MAX_NODES_SLOTS MAX_NUM_NODES
#endif
constexpr uint16_t MAX_SLOT_ID = MAX_NODES_SLOTS - 1;
constexpr size_t SLOT_FILENAME_BUFFER_SIZE = 32;

static bool directory_created = false;

/**
 * @brief Helper function to validate protobuf encode result
 * @param main_lfs Filesystem handle
 * @param file File handle
 * @param filename Filename
 * @param encode_success Encode success flag
 * @param stream Protobuf stream
 * @param bytes_written Bytes written
 * @return true if valid, false otherwise
 */
static bool validateProtobufEncode(lfs_t* main_lfs, lfs_file_t* file, const char* filename,
                                   bool encode_success, pb_ostream_t* stream, size_t bytes_written)
{
    if (!encode_success) {
        LOG_ERROR("NodeStorage: Failed to encode node to protobuf: %s", PB_GET_ERROR(stream));
        cleanupFileOnError(main_lfs, file, filename);
        return false;
    }
    
    if (bytes_written == 0) {
        LOG_ERROR("NodeStorage: pb_encode returned success but wrote 0 bytes! Node may be empty or invalid.");
        cleanupFileOnError(main_lfs, file, filename);
        return false;
    }
    
    // CRITICAL: Check buffer size to prevent overflow
    constexpr size_t MAX_SLOT_BUFFER_SIZE = 512;
    if (bytes_written > MAX_SLOT_BUFFER_SIZE) {
        LOG_ERROR("NodeStorage: CRITICAL - Encoded size (%u) exceeds max slot size (%u) - buffer overflow prevented!", 
                 (unsigned)bytes_written, (unsigned)MAX_SLOT_BUFFER_SIZE);
        cleanupFileOnError(main_lfs, file, filename);
        return false;
    }
    
    // CRITICAL: Additional safety check - ensure bytes_written is reasonable
    if (bytes_written > 1024) {  // Sanity check - should never happen
        LOG_ERROR("NodeStorage: CRITICAL - Encoded size (%u) is unreasonably large - possible corruption!", 
                 (unsigned)bytes_written);
        cleanupFileOnError(main_lfs, file, filename);
        return false;
    }
    
    return true;
}

const char* getSlotFilename(uint16_t slotId, char* buffer, size_t bufferSize)
{
    if (!buffer || bufferSize < SLOT_FILENAME_BUFFER_SIZE) {
        return nullptr;
    }
    
    int result = snprintf(buffer, bufferSize, "%s%04u%s", SLOT_FILE_PREFIX, slotId, SLOT_FILE_SUFFIX);
    if (result < 0 || result >= (int)bufferSize) {
        return nullptr;
    }
    
    return buffer;
}

static bool ensureDirectoryExists()
{
    // Use cached flag for optimization (most common case)
    if (directory_created) {
        return true;
    }
    
    lfs_t* main_lfs = getMainFSWithCheck();
    if (!main_lfs) {
        return false;
    }
    
    // CRITICAL: LittleFS doesn't create parent directories automatically
    // First ensure /prefs exists, then create /prefs/nodes
    FEED_WATCHDOG_AND_YIELD();
    int prefs_dir_result = lfs_mkdir(main_lfs, "/prefs");
    FEED_WATCHDOG_AND_YIELD();
    if (prefs_dir_result != LFS_ERR_OK && prefs_dir_result != LFS_ERR_EXIST) {
        LOG_WARN("NodeStorage: Failed to create /prefs directory (error: %d) - will try to create /prefs/nodes anyway", prefs_dir_result);
        // Continue anyway - /prefs might already exist or will be created by framework
    }
    
    FEED_WATCHDOG_AND_YIELD();
    int mkdir_result = lfs_mkdir(main_lfs, SLOTS_DIR);
    FEED_WATCHDOG_AND_YIELD();
    if (mkdir_result != LFS_ERR_OK && mkdir_result != LFS_ERR_EXIST) {
        LOG_ERROR("NodeStorage: Failed to create directory '%s' (error: %d)", SLOTS_DIR, mkdir_result);
        return false;
    }
    
    directory_created = true;
    LOG_DEBUG("NodeStorage: Directory '%s' ready", SLOTS_DIR);
    return true;
}

/**
 * @brief Reset directory flag (called after Main FS reformat)
 * 
 * After Main FS reformat, /prefs/nodes directory doesn't exist anymore,
 * but directory_created flag may still be true. This function resets the flag
 * so that ensureDirectoryExists() will recreate the directory.
 */
void resetDirectoryFlag()
{
    directory_created = false;
    LOG_DEBUG("NodeStorage: Reset directory_created flag (Main FS was reformatted)");
}

bool initialize()
{
    return ensureDirectoryExists();
}

bool slotExists(uint16_t slotId)
{
    if (slotId > MAX_SLOT_ID) {
        return false;
    }
    
    lfs_t* main_lfs = getMainFSWithCheck();
    if (!main_lfs) {
        return false;
    }
    
    char filename[SLOT_FILENAME_BUFFER_SIZE];
    if (!getSlotFilename(slotId, filename, sizeof(filename))) {
        return false;
    }
    
    lfs_file_t file;
    int open_result = lfs_file_open(main_lfs, &file, filename, LFS_O_RDONLY);
    if (open_result == LFS_ERR_OK) {
        lfs_file_close(main_lfs, &file);
        return true;
    }
    
    return false;
}

uint16_t allocateSlot(uint32_t nodeNum)
{
    (void)nodeNum;
    
    lfs_t* main_lfs = getMainFSWithCheck();
    if (!main_lfs) {
        return UINT16_MAX;
    }
    
    FEED_WATCHDOG_AND_YIELD();
    
    if (!initialize()) {
        return UINT16_MAX;
    }
    
    FEED_WATCHDOG_AND_YIELD();
    
    uint32_t maxSlots = MemoryHelpers::getMaxNodesSlots();
    uint16_t MAX_SLOT_ID = maxSlots - 1;
    
    // CRITICAL: Check memory before allocating temporary array
    uint32_t allocation_size = (maxSlots + 1) * sizeof(bool);
    uint32_t requiredMemory = allocation_size;
    uint32_t safetyMargin = 2048;  // 2 KB safety margin
    auto memCheck = MemoryHelpers::checkMemory(requiredMemory, safetyMargin, "NodeStorage::allocateSlot");
    if (!memCheck.sufficient) {
        LOG_ERROR("NodeStorage: Insufficient memory for slot tracking array (need %u bytes, have %u free)", 
                 memCheck.totalNeeded, memCheck.freeHeap);
        return UINT16_MAX;
    }
    
    #ifdef ARCH_NRF52
    bool* used_slots = (bool*)rtos_malloc(allocation_size);
    #else
    bool* used_slots = new bool[maxSlots + 1];
    #endif
    if (!used_slots) {
        uint32_t freeHeapAfter = memGet.getFreeHeap();
        MemoryHelpers::logAllocationFailure("NodeStorage::allocateSlot", allocation_size, sizeof(bool), 
                                           maxSlots + 1, memCheck.freeHeap, freeHeapAfter);
        return UINT16_MAX;
    }
    memset(used_slots, 0, (maxSlots + 1) * sizeof(bool));
    
    uint32_t node_count = NodeIndex::getNodeCount();
    for (uint32_t i = 0; i < node_count && i < maxSlots; i++) {
        NodeNum node_num = NodeIndex::getNodeNumByIndex(i);
        if (node_num != 0) {
            NodeIndexEntry* entry = NodeIndex::findNode(node_num);
            if (entry && entry->slot_id <= MAX_SLOT_ID && entry->slot_id < maxSlots + 1) {
                used_slots[entry->slot_id] = true;
            }
        }
    }
    
    for (uint16_t slotId = 0; slotId < maxSlots; slotId++) {
        if (!used_slots[slotId]) {
            LOG_DEBUG("NodeStorage: Allocated slot %u for node 0x%x (checked %u nodes in index)", slotId, nodeNum, node_count);
            freeMemory(used_slots);
            return slotId;
        }
    }
    freeMemory(used_slots);
    
    LOG_ERROR("NodeStorage: No free slots available (max: %u)", MemoryHelpers::getMaxNodesSlots());
    return UINT16_MAX;
}

bool writeNodeToSlot(uint16_t slotId, const meshtastic_NodeInfoLite* node)
{
    // Static variables for throttling frequent logs
    static unsigned long lastWriteStartLog = 0;
    static unsigned long lastSkipWriteLog = 0;
    static const unsigned long WRITE_LOG_INTERVAL_MS = 2000; // Log at most once per 2 seconds
    
    // Throttle write start log
    unsigned long now = millis();
    if (now - lastWriteStartLog > WRITE_LOG_INTERVAL_MS) {
        LOG_DEBUG("NodeStorage: writeNodeToSlot(%u) START", slotId);
        lastWriteStartLog = now;
    }
    
    FEED_WATCHDOG_AND_YIELD();
    
    if (slotId > MAX_SLOT_ID || !node) {
        LOG_ERROR("NodeStorage: Invalid parameters (slotId: %u, node: %p)", slotId, node);
        return false;
    }
    
    lfs_t* main_lfs = getMainFSWithCheck();
    if (!main_lfs) {
        LOG_ERROR("NodeStorage: Main FS not available - cannot write node to slot %u", slotId);
        return false;
    }
    
    // CRITICAL: Quick radio check before flash write to avoid packet loss
    // Writing during transmission is OK (reception is disabled anyway)
    // But writing during reception can cause packet loss
#ifdef ARCH_NRF52
    if (RadioLibInterface::instance != nullptr) {
        // OPTIMIZATION: Check isSending() FIRST - if transmitting, immediately allow write
        // During transmission, radio CANNOT receive packets, so it's SAFE to write
        // This avoids unnecessary isActivelyReceiving() check (fast path)
        if (RadioLibInterface::instance->isSending()) {
            // Radio is sending - SAFE to write immediately (skip isActivelyReceiving() check)
            // Proceed directly to file write
        } else {
            // Radio is NOT sending - check if receiving
            if (RadioLibInterface::instance->isActivelyReceiving()) {
                // Radio is receiving - skip this write, will be retried by background task
                // Throttle logging to avoid log spam
                if (now - lastSkipWriteLog > WRITE_LOG_INTERVAL_MS) {
                    LOG_DEBUG("NodeStorage: Skipping write to slot %u - radio is receiving", slotId);
                    lastSkipWriteLog = now;
                }
                return false;
            }
        }
    }
#endif
    
    // REMOVED: getStats() check - too slow for every write operation
    // Stats will be checked only if write fails with NOSPC error (see below)
    
    // Directory is guaranteed to exist after NodeStorage::initialize() at startup
    // No need to check on every write operation (best practice for embedded devices)
    
    char filename[SLOT_FILENAME_BUFFER_SIZE];
    
    if (!getSlotFilename(slotId, filename, sizeof(filename))) {
        LOG_ERROR("NodeStorage: Failed to generate filename for slot %u", slotId);
        return false;
    }
    
    LOG_DEBUG("NodeStorage: Writing to slot %u, filename: %s", slotId, filename);
    
    uint32_t operation_start = millis();
    
    // CRITICAL: Instead of removing and recreating file, overwrite existing file
    // This avoids LittleFS metadata fragmentation issues ("No more free space" errors)
    // when filesystem has free space but no free metadata blocks
    // openFileForOverwrite() will truncate existing file to 0 bytes, sync to free old blocks,
    // then we write new data. This reuses existing metadata instead of creating new metadata entry
    
    LOG_DEBUG("NodeStorage: Opening file '%s' for writing (will overwrite if exists)...", filename);
    
    lfs_file_t file;
    // Use Main FS directly - open file for overwrite (truncate if exists)
    int open_result = lfs_file_open(main_lfs, &file, filename, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    
    if (open_result != LFS_ERR_OK) {
        // CRITICAL: Detailed error logging for LittleFS errors
        const char* error_name = "UNKNOWN";
        switch (open_result) {
            case LFS_ERR_IO: error_name = "LFS_ERR_IO (I/O error)"; break;
            case LFS_ERR_CORRUPT: error_name = "LFS_ERR_CORRUPT (filesystem corrupted)"; break;
            case LFS_ERR_NOENT: error_name = "LFS_ERR_NOENT (file/dir not found)"; break;
            case LFS_ERR_EXIST: error_name = "LFS_ERR_EXIST (file/dir exists)"; break;
            case LFS_ERR_NOTDIR: error_name = "LFS_ERR_NOTDIR (not a directory)"; break;
            case LFS_ERR_ISDIR: error_name = "LFS_ERR_ISDIR (is a directory)"; break;
            case LFS_ERR_NOTEMPTY: error_name = "LFS_ERR_NOTEMPTY (directory not empty)"; break;
            case LFS_ERR_BADF: error_name = "LFS_ERR_BADF (bad file number)"; break;
            case LFS_ERR_INVAL: error_name = "LFS_ERR_INVAL (invalid parameter)"; break;
            case LFS_ERR_NOSPC: error_name = "LFS_ERR_NOSPC (no space left)"; break;
            case LFS_ERR_NOMEM: error_name = "LFS_ERR_NOMEM (no memory)"; break;
            default: error_name = "UNKNOWN_ERROR"; break;
        }
        LOG_ERROR("NodeStorage: Failed to open final file '%s' for writing (error: %d = %s)", filename, open_result, error_name);
        
        // CRITICAL: If filesystem is corrupted, log Main FS stats
        if (open_result == LFS_ERR_CORRUPT || open_result == LFS_ERR_IO) {
            LOG_ERROR("NodeStorage: Main FS may be corrupted - checking filesystem state...");
            uint32_t total_bytes = 0, used_bytes = 0, free_bytes = 0;
            if (getMainFSStats(&total_bytes, &used_bytes, &free_bytes)) {
                LOG_ERROR("NodeStorage: Main FS stats - Total: %u KB, Used: %u KB, Free: %u KB", 
                         total_bytes / 1024, used_bytes / 1024, free_bytes / 1024);
            } else {
                LOG_ERROR("NodeStorage: Failed to get Main FS stats - filesystem may be unavailable");
            }
            
            // Check if Main FS is still mounted
            lfs_t* check_lfs = getMainFS();
            if (!check_lfs) {
                LOG_ERROR("NodeStorage: Main FS is not available - filesystem may be unmounted");
            }
        }
        
        return false;
    }
    LOG_DEBUG("NodeStorage: Final file opened successfully");
    
    LOG_DEBUG("NodeStorage: Encoding node to protobuf buffer...");
    uint8_t encode_buffer[512];
    pb_ostream_t stream = pb_ostream_from_buffer(encode_buffer, sizeof(encode_buffer));
    
    LOG_DEBUG("NodeStorage: Starting pb_encode for node 0x%x (num=%u)...", node->num, node->num);
    bool encode_success = pb_encode(&stream, meshtastic_NodeInfoLite_fields, node);
    
    size_t bytes_written = stream.bytes_written;
    LOG_DEBUG("NodeStorage: pb_encode completed successfully, bytes_written=%u", (unsigned)bytes_written);
    
    // CRITICAL: Validate encoding result BEFORE writing to prevent buffer overflow
    if (!validateProtobufEncode(main_lfs, &file, filename, encode_success, &stream, bytes_written)) {
        return false;
    }
    
    // CRITICAL: Double-check buffer size to prevent overflow (validateProtobufEncode already checks, but be extra safe)
    constexpr size_t MAX_SLOT_BUFFER_SIZE = 512;
    if (bytes_written > MAX_SLOT_BUFFER_SIZE) {
        LOG_ERROR("NodeStorage: CRITICAL - Encoded size (%u) exceeds buffer size (%u) - buffer overflow prevented!", 
                 (unsigned)bytes_written, (unsigned)MAX_SLOT_BUFFER_SIZE);
        closeFileOnError(main_lfs, &file, filename);
        return false;
    }
    
    // CRITICAL: Ensure bytes_written doesn't exceed buffer size (safety clamp)
    if (bytes_written > sizeof(encode_buffer)) {
        LOG_ERROR("NodeStorage: CRITICAL - bytes_written (%u) exceeds encode_buffer size (%u) - clamping!", 
                 (unsigned)bytes_written, (unsigned)sizeof(encode_buffer));
        bytes_written = sizeof(encode_buffer);
    }
    
    LOG_DEBUG("NodeStorage: Writing %u bytes to final file...", (unsigned)bytes_written);
    
    FEED_WATCHDOG_AND_YIELD();
    
    lfs_ssize_t write_result = lfs_file_write(main_lfs, &file, encode_buffer, bytes_written);
    FEED_WATCHDOG_AND_YIELD();
    
        if (write_result != (lfs_ssize_t)bytes_written) {
        if (write_result == LFS_ERR_NOSPC) {
            LOG_ERROR("NodeStorage: No space left on device (LFS_ERR_NOSPC) - filesystem is full!");
            LOG_ERROR("⚠️  ERROR IN MAIN FS!");
            LOG_ERROR("⚠️  Main FS address range: 0x%08X - 0x%08X (pages 168-243, 76 blocks)", 
                     0xA8000, 0xF4000 - 1);
            LOG_ERROR("NodeStorage: Cannot write %u bytes to slot %u", (unsigned)bytes_written, slotId);
            
            // CRITICAL: Close file BEFORE freeing space to avoid conflicts
            int close_result = lfs_file_close(main_lfs, &file);
            if (close_result != LFS_ERR_OK) {
                LOG_WARN("NodeStorage: Failed to close file before cleanup (error: %d)", close_result);
            }
            
            // CRITICAL: Check for corrupted dir pair error and call lfs_deorphan()
            // This fixes "Corrupted dir pair" errors that prevent LittleFS from finding free blocks
            LOG_WARN("NodeStorage: Attempting to fix filesystem corruption (orphaned entries)...");
            int deorphan_result = lfs_deorphan(main_lfs);
            if (deorphan_result == LFS_ERR_OK) {
                LOG_INFO("NodeStorage: lfs_deorphan() completed successfully - orphaned entries cleaned");
            } else {
                LOG_WARN("NodeStorage: lfs_deorphan() failed: %d (may be normal if no orphans)", deorphan_result);
            }
            FEED_WATCHDOG_AND_YIELD();
            
            uint32_t total_bytes = 0, used_bytes = 0, free_bytes = 0;
            uint32_t total_kb = 0, used_kb = 0, free_kb = 0, used_pct = 0;
            
            if (getMainFSStats(&total_bytes, &used_bytes, &free_bytes)) {
                total_kb = total_bytes / 1024;
                used_kb = used_bytes / 1024;
                free_kb = free_bytes / 1024;
                used_pct = (total_kb > 0) ? (used_kb * 100) / total_kb : 0;
                LOG_ERROR("NodeStorage: Main FS stats - Total: %u KB, Used: %u KB (%u%%), Free: %u KB", 
                         total_kb, used_kb, used_pct, free_kb);
            }
            
            uint32_t node_count = NodeIndex::getNodeCount();
            LOG_ERROR("NodeStorage: Current node count: %u, slot ID: %u", node_count, slotId);
            
            // CRITICAL: Try to free space by removing old nodes
            // ALWAYS attempt eviction on LFS_ERR_NOSPC, regardless of statistics
            // Statistics may be incorrect due to filesystem corruption
            bool space_freed = false;
            // Changed condition: always try eviction on NOSPC, or if filesystem is >80% full
            if (NodeDBPersistentBackend::isEnabled() && node_count > 0) {
                LOG_WARN("NodeStorage: Filesystem reports no space (stats: %u%% used, %u nodes) - attempting to free space by removing old nodes", 
                        used_pct, node_count);
                
                // Calculate how many nodes to remove (target: free at least 20% of filesystem)
                uint32_t target_free_kb = (total_kb > 0) ? (total_kb / 5) : 80;  // 20% of total, or 80 KB default
                uint32_t current_free_kb = free_kb;
                uint32_t need_to_free_kb = (target_free_kb > current_free_kb) ? (target_free_kb - current_free_kb) : 0;
                
                // Each node file uses ~4KB (1 block), so calculate nodes to remove
                uint32_t nodes_to_remove = (need_to_free_kb + 3) / 4;  // Round up
                if (nodes_to_remove == 0) nodes_to_remove = 10;  // Remove at least 10 nodes on NOSPC
                
                // Don't remove more than 25% of nodes
                uint32_t max_remove = node_count / 4;
                if (max_remove == 0) max_remove = 1;  // At least remove 1 node
                if (nodes_to_remove > max_remove) {
                    nodes_to_remove = max_remove;
                }
                
                // Remove oldest nodes using existing eviction mechanism
                uint32_t removed_count = 0;
                
                // CRITICAL: Feed watchdog before starting cleanup loop (can take 5+ seconds)
                FEED_WATCHDOG_AND_YIELD();
                
                for (uint32_t i = 0; i < nodes_to_remove && removed_count < nodes_to_remove; i++) {
                    // Get eviction candidate (oldest non-protected node)
                    NodeNum node_to_remove = NodeIndex::getEvictionCandidate(false);  // Don't allow cached nodes
                    if (node_to_remove == 0) {
                        // Try with cached nodes allowed
                        node_to_remove = NodeIndex::getEvictionCandidate(true);
                    }
                    
                    if (node_to_remove != 0) {
                        // CRITICAL: Feed watchdog before each removeNode call (can trigger flash operations)
                        FEED_WATCHDOG_AND_YIELD();
                        
                        if (NodeDBPersistentBackend::removeNode(node_to_remove)) {
                            removed_count++;
                            LOG_DEBUG("NodeStorage: Removed old node 0x%x to free space", node_to_remove);
                        }
                    } else {
                        LOG_WARN("NodeStorage: No eviction candidates found - all nodes may be protected");
                        break;
                    }
                    FEED_WATCHDOG_AND_YIELD();
                }
                
                if (removed_count > 0) {
                    space_freed = true;
                    LOG_INFO("NodeStorage: Freed space by removing %u old nodes - retrying write...", removed_count);
                    
                    // CRITICAL: Call lfs_deorphan() again after removing nodes to ensure metadata is clean
                    LOG_DEBUG("NodeStorage: Calling lfs_deorphan() after node removal to clean up metadata...");
                    int deorphan_result2 = lfs_deorphan(main_lfs);
                    if (deorphan_result2 == LFS_ERR_OK) {
                        LOG_DEBUG("NodeStorage: lfs_deorphan() after removal completed successfully");
                    } else {
                        LOG_DEBUG("NodeStorage: lfs_deorphan() after removal failed: %d", deorphan_result2);
                    }
                    
                    // Force lookahead buffer refresh after cleanup
                    lfs_t* lfs = getMainFSWithCheck();
                    if (lfs && lfs->cfg && lfs->cfg->sync) {
                        lfs->cfg->sync(lfs->cfg);
                    }
                    FEED_WATCHDOG_AND_YIELD();
                } else {
                    LOG_ERROR("NodeStorage: Cleanup failed - no space can be freed (all nodes protected?)");
                }
            } else {
                LOG_ERROR("NodeStorage: Cannot free space - NodeDBPersistentBackend disabled or no nodes to remove");
            }
            
            // CRITICAL: If space was freed, retry the write immediately
            if (space_freed) {
                // Reopen file for overwrite
                int reopen_result = lfs_file_open(main_lfs, &file, filename, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
                if (reopen_result != LFS_ERR_OK) {
                    LOG_ERROR("NodeStorage: Failed to reopen file '%s' after cleanup (error: %d)", filename, reopen_result);
                    return false;
                }
                
                // Retry the write
                FEED_WATCHDOG_AND_YIELD();
                write_result = lfs_file_write(main_lfs, &file, encode_buffer, bytes_written);
                FEED_WATCHDOG_AND_YIELD();
                
                if (write_result == (lfs_ssize_t)bytes_written) {
                    // Write succeeded after cleanup - continue with sync
                    LOG_INFO("NodeStorage: Write succeeded after freeing space");
                    // Note: write_result is now successful, so the outer if condition is false
                    // We'll skip the error handling below and continue to sync step
                } else {
                    // Write still failed - close file and return false
                    LOG_ERROR("NodeStorage: Write still failed after cleanup (error: %d)", (int)write_result);
                    closeFileOnError(main_lfs, &file, filename);
                    return false;
                }
            } else {
                // No space was freed - close file and return false to let background task retry
                closeFileOnError(main_lfs, &file, filename);
                return false;
            }
            // CRITICAL: After NOSPC handling with retry, check if write_result is now successful
            // If retry succeeded, we need to skip the error handling below and continue to sync
            if (write_result != (lfs_ssize_t)bytes_written) {
                // Write still failed after all retries - handle as error
                closeFileOnError(main_lfs, &file, filename);
                return false;
            }
            // If we reach here, retry succeeded - continue to sync (skip error handling below)
        } else if (write_result == LFS_ERR_IO) {
            LOG_ERROR("NodeStorage: I/O error during write (LFS_ERR_IO)");
            // Note: Error recovery is handled at higher level (NodeDBPersistentBackend)
            closeFileOnError(main_lfs, &file, filename);
            return false;
        } else if (write_result == LFS_ERR_CORRUPT) {
            LOG_ERROR("NodeStorage: Filesystem corruption detected (LFS_ERR_CORRUPT)");
            // CRITICAL: Try to fix corruption with lfs_deorphan()
            LOG_WARN("NodeStorage: Attempting to fix filesystem corruption (orphaned entries)...");
            int deorphan_result = lfs_deorphan(main_lfs);
            if (deorphan_result == LFS_ERR_OK) {
                LOG_INFO("NodeStorage: lfs_deorphan() completed successfully - corruption may be fixed");
            } else {
                LOG_WARN("NodeStorage: lfs_deorphan() failed: %d - filesystem may need reformatting", deorphan_result);
            }
            closeFileOnError(main_lfs, &file, filename);
            return false;
        } else {
            // CRITICAL: Check if write_result is actually successful (not an error code)
            // After NOSPC retry, write_result might be the number of bytes written (success)
            if (write_result == (lfs_ssize_t)bytes_written) {
                // Write succeeded (likely after NOSPC retry) - continue to sync
                // Skip error handling
            } else {
                LOG_ERROR("NodeStorage: Failed to write to file (error code: %d, wrote %d of %u bytes)", 
                         (int)write_result, (int)write_result, (unsigned)bytes_written);
                closeFileOnError(main_lfs, &file, filename);
                return false;
            }
        }
    }
    
    LOG_DEBUG("NodeStorage: Syncing final file...");
    
    FEED_WATCHDOG_AND_YIELD();
    
    // CRITICAL: Sync file before closing to ensure data is written to flash
    // If sync fails, data may not be persisted - treat this as critical error
    int sync_result = lfs_file_sync(main_lfs, &file);
    FEED_WATCHDOG_AND_YIELD();
    
    if (sync_result != LFS_ERR_OK) {
        LOG_ERROR("NodeStorage: CRITICAL - File sync failed (error: %d) - data may not be written to flash!", sync_result);
        LOG_ERROR("NodeStorage: Closing file and removing incomplete file '%s'", filename);
        
        // CRITICAL: Close file before removing
        FEED_WATCHDOG_AND_YIELD();
        int close_result = lfs_file_close(main_lfs, &file);
        if (close_result != LFS_ERR_OK) {
            LOG_ERROR("NodeStorage: Failed to close file after sync error (error: %d)", close_result);
        }
        
        // CRITICAL: Remove incomplete file to prevent corruption
        FEED_WATCHDOG_AND_YIELD();
        int remove_result = lfs_remove(main_lfs, filename);
        if (remove_result != LFS_ERR_OK && remove_result != LFS_ERR_NOENT) {
            LOG_WARN("NodeStorage: Failed to remove incomplete file '%s' (error: %d)", filename, remove_result);
        }
        
        return false;  // CRITICAL: Return false - data was not written successfully
    }
    
    LOG_DEBUG("NodeStorage: File synced successfully");
    
    LOG_DEBUG("NodeStorage: Closing final file...");
    
    FEED_WATCHDOG_AND_YIELD();
    
    int close_result = lfs_file_close(main_lfs, &file);
    FEED_WATCHDOG_AND_YIELD();
    
    if (close_result != LFS_ERR_OK) {
        LOG_ERROR("NodeStorage: Failed to close file (error: %d)", close_result);
        LOG_ERROR("NodeStorage: Removing file '%s' due to close error", filename);
        lfs_remove(main_lfs, filename);
        return false;
    }
    LOG_DEBUG("NodeStorage: File closed successfully");
    
    uint32_t total_time = millis() - operation_start;
    if (total_time > 5000) {
        LOG_WARN("NodeStorage: Write operation took %u ms (exceeded 5s limit)", total_time);
    }
    
    LOG_DEBUG("NodeStorage: writeNodeToSlot(%u) COMPLETED successfully (%u bytes)", slotId, (unsigned)bytes_written);
    LOG_INFO("NodeStorage: Successfully saved node 0x%08X to slot %u (%u bytes)", node->num, slotId, (unsigned)bytes_written);
    
    // CRITICAL: Log statistics after successful save (with throttling to avoid log spam)
    // This helps detect filesystem issues early (e.g., when free space is low)
    static unsigned long lastStatsLog = 0;
    static uint32_t saveCountSinceLastLog = 0;
    constexpr unsigned long STATS_LOG_INTERVAL_MS = 30000;  // Log every 30 seconds
    constexpr uint32_t STATS_LOG_SAVE_COUNT = 10;  // Or every 10 saves
    
    saveCountSinceLastLog++;
    // Reuse 'now' variable from start of function, but update it for accurate timing
    now = millis();
    bool should_log_stats = (now - lastStatsLog > STATS_LOG_INTERVAL_MS) || (saveCountSinceLastLog >= STATS_LOG_SAVE_COUNT);
    
    if (should_log_stats) {
        // Get node statistics from flash
        uint32_t saved_nodes = NodeIndex::getNodeCount();
        uint32_t free_slots = getFreeSlotCount();
        uint32_t max_slots = getMaxSlotCount();
        
        // Get Main FS statistics from flash
        uint32_t fs_total = 0, fs_used = 0, fs_free = 0;
        bool fs_stats_ok = getMainFSStats(&fs_total, &fs_used, &fs_free);
        
        if (fs_stats_ok) {
            uint32_t fs_total_kb = fs_total / 1024;
            uint32_t fs_used_kb = fs_used / 1024;
            uint32_t fs_free_kb = fs_free / 1024;
            uint32_t fs_used_pct = (fs_total_kb > 0) ? (fs_used_kb * 100) / fs_total_kb : 0;
            
            // Brief log with key statistics
            LOG_INFO("NodeDB stats: nodes=%u/%u (free=%u), FS=%uKB/%uKB (%u%%)", 
                     saved_nodes, max_slots, free_slots, fs_used_kb, fs_total_kb, fs_used_pct);
            
            // WARNING: If filesystem is more than 80% full, log warning
            if (fs_used_pct > 80) {
                LOG_WARN("⚠️  Main FS is %u%% full - may cause issues soon!", fs_used_pct);
            }
            // WARNING: If less than 10% slots free, log warning
            if (max_slots > 0) {
                uint32_t free_slots_pct = (free_slots * 100) / max_slots;
                if (free_slots_pct < 10) {
                    LOG_WARN("⚠️  Only %u%% slots free (%u/%u) - may cause issues soon!", 
                             free_slots_pct, free_slots, max_slots);
                }
            }
        } else {
            // Fallback: log node stats only if FS stats unavailable
            LOG_INFO("NodeDB stats: nodes=%u/%u (free=%u), FS stats unavailable", 
                     saved_nodes, max_slots, free_slots);
        }
        
        lastStatsLog = now;
        saveCountSinceLastLog = 0;
    }
    
    return true;
}

bool readNodeFromSlot(uint16_t slotId, meshtastic_NodeInfoLite* node)
{
    if (slotId > MAX_SLOT_ID || !node) {
        LOG_ERROR("NodeStorage: Invalid parameters (slotId: %u, node: %p)", slotId, node);
        return false;
    }
    
    lfs_t* main_lfs = getMainFSWithCheck();
    if (!main_lfs) {
        return false;
    }
    
    char filename[SLOT_FILENAME_BUFFER_SIZE];
    if (!getSlotFilename(slotId, filename, sizeof(filename))) {
        LOG_ERROR("NodeStorage: Failed to generate filename for slot %u", slotId);
        return false;
    }
    
    FEED_WATCHDOG_AND_YIELD();
    
    lfs_file_t file;
    int open_result = lfs_file_open(main_lfs, &file, filename, LFS_O_RDONLY);
    if (open_result != LFS_ERR_OK) {
        LOG_DEBUG("NodeStorage: Slot %u file not found (error: %d)", slotId, open_result);
        return false;
    }
    
    lfs_soff_t file_size = lfs_file_size(main_lfs, &file);
    FEED_WATCHDOG_AND_YIELD();
    if (file_size < 0 || file_size > 512) {
        LOG_ERROR("NodeStorage: Invalid file size for slot %u: %d", slotId, (int)file_size);
        lfs_file_close(main_lfs, &file);
        return false;
    }
    
    uint8_t buffer[512];
    lfs_ssize_t read_result = lfs_file_read(main_lfs, &file, buffer, file_size);
    FEED_WATCHDOG_AND_YIELD();
    
    // CRITICAL: Check result of file close operation before checking read result
    // If close fails, file may be in inconsistent state
    int close_result = lfs_file_close(main_lfs, &file);
    if (close_result != LFS_ERR_OK) {
        LOG_ERROR("NodeStorage: Failed to close file for slot %u (error: %d) - file may be corrupted", slotId, close_result);
        // Continue to check read result anyway
    }
    
    // CRITICAL: Check if all data was read successfully
    if (read_result != file_size) {
        LOG_ERROR("NodeStorage: Failed to read slot %u (read %d of %d bytes)", slotId, (int)read_result, (int)file_size);
        return false;
    }
    
    // CRITICAL: Validate that we actually read some data
    if (read_result <= 0) {
        LOG_ERROR("NodeStorage: No data read from slot %u (read_result: %d)", slotId, (int)read_result);
        return false;
    }
    
    pb_istream_t stream = pb_istream_from_buffer(buffer, file_size);
    memset(node, 0, sizeof(meshtastic_NodeInfoLite));
    
    if (!pb_decode(&stream, meshtastic_NodeInfoLite_fields, node)) {
        LOG_ERROR("NodeStorage: Failed to decode node from slot %u: %s", slotId, PB_GET_ERROR(&stream));
        return false;
    }
    
    if (node->num == 0) {
        LOG_ERROR("NodeStorage: Invalid node number (0) in slot %u", slotId);
        return false;
    }
    
    uint32_t last_heard_from_index = 0;
    NodeIndexEntry* index_entry = NodeIndex::findNode(node->num);
    if (index_entry) {
        last_heard_from_index = index_entry->last_heard;
    }
    
    LOG_DEBUG("NodeStorage: Successfully read node from slot %u (%d bytes)", slotId, (int)file_size);
    int32_t snr_int = (int32_t)node->snr;
    int32_t snr_frac = (int32_t)((node->snr - snr_int) * 100);
    if (snr_frac < 0) snr_frac = -snr_frac;
    LOG_DEBUG("NodeStorage: Node 0x%x details: num=0x%x, last_heard=%u (from index: %u), snr=%d.%02d, hops_away=%u, channel=%u", 
             node->num, node->num, node->last_heard, last_heard_from_index, snr_int, snr_frac, node->hops_away, node->channel);
    if (node->has_user) {
        LOG_DEBUG("NodeStorage: Node 0x%x user: long_name='%s', short_name='%s'", 
                  node->num, node->user.long_name, node->user.short_name);
        
        char pubkey_hex[65] = {0};
        size_t pubkey_size = node->user.public_key.size;
        if (pubkey_size > 32) {
            pubkey_size = 32;
        }
        
        if (pubkey_size > 0) {
            for (size_t i = 0; i < pubkey_size; i++) {
                char hex_byte[3];
                snprintf(hex_byte, sizeof(hex_byte), "%02x", node->user.public_key.bytes[i]);
                size_t current_len = strlen(pubkey_hex);
                if (current_len + 2 < sizeof(pubkey_hex)) {
                    strncat(pubkey_hex, hex_byte, sizeof(pubkey_hex) - current_len - 1);
                }
            }
            LOG_DEBUG("NodeStorage: Node 0x%x public_key: %s (size: %u)", 
                      node->num, pubkey_hex, (unsigned)pubkey_size);
        } else {
            LOG_DEBUG("NodeStorage: Node 0x%x public_key: (empty or not set)", node->num);
        }
    }
    if (node->has_position) {
        LOG_DEBUG("NodeStorage: Node 0x%x position: lat=%d, lon=%d, alt=%d, time=%u", 
                  node->num, node->position.latitude_i, node->position.longitude_i, 
                  node->position.altitude, node->position.time);
    }
    
    return true;
}

bool deleteSlot(uint16_t slotId)
{
    if (slotId > MAX_SLOT_ID) {
        return false;
    }
    
    lfs_t* main_lfs = getMainFSWithCheck();
    if (!main_lfs) {
        return false;
    }
    
    char filename[SLOT_FILENAME_BUFFER_SIZE];
    if (!getSlotFilename(slotId, filename, sizeof(filename))) {
        return false;
    }
    
    // CRITICAL: Feed watchdog before flash operation (lfs_remove can take 200-300ms)
    FEED_WATCHDOG_AND_YIELD();
    
    int remove_result = lfs_remove(main_lfs, filename);
    
    // CRITICAL: Feed watchdog after flash operation
    FEED_WATCHDOG_AND_YIELD();
    
    if (remove_result == LFS_ERR_OK) {
        LOG_DEBUG("NodeStorage: Deleted slot %u", slotId);
        return true;
    } else if (remove_result == LFS_ERR_NOENT) {
        return true;
    } else {
        LOG_ERROR("NodeStorage: Failed to delete slot %u (error: %d)", slotId, remove_result);
        return false;
    }
}

uint32_t getSlotCount()
{
    return NodeIndex::getNodeCount();
}

uint32_t getFreeSlotCount()
{
    uint32_t used = getSlotCount();
    uint32_t max = getMaxSlotCount();
    return (used < max) ? (max - used) : 0;
}

uint32_t getMaxSlotCount()
{
    return MemoryHelpers::getMaxNodesSlots();
}

} // namespace NodeStorage



// ============================================================================
// From NodeCache.cpp
// ============================================================================



#ifdef ARCH_NRF52
#endif
namespace NodeCache {

// Cache entry structure
struct CacheEntry {
    NodeNum num;
    meshtastic_NodeInfoLite node;
    uint32_t last_access;  // For LRU
    uint32_t access_count; // For LFU
    bool dirty;            // Needs flash write
};

// Static storage for cache entries
static CacheEntry* cache_entries = nullptr;
static uint32_t max_cache_size = 0;
static uint32_t cache_size = 0;
static uint32_t access_counter = 0;  // Global access counter for LRU
static bool initialized = false;

// Macros for initialization checks
#define CHECK_CACHE_INIT() do { if (!initialized) return false; } while(0)
#define CHECK_CACHE_INIT_VOID() do { if (!initialized) return; } while(0)

/**
 * @brief Write a dirty node to flash (synchronous write during eviction)
 * @param nodeNum Node number
 * @param node Pointer to node data
 * @return true if write was successful
 * 
 * This is used during eviction when we need to write synchronously
 * to free up memory immediately.
 */
static bool writeDirtyNodeToFlash(NodeNum nodeNum, const meshtastic_NodeInfoLite* node)
{
    // CRITICAL: Check radio state before synchronous write during eviction
    // Skip this write if radio is receiving - will be retried by background task
#ifdef ARCH_NRF52
    if (RadioLibInterface::instance != nullptr) {
        if (RadioLibInterface::instance->isActivelyReceiving()) {
            LOG_DEBUG("NodeCache: Skipping eviction write for node 0x%x - radio is receiving", nodeNum);
            return false;  // Will be retried by background task
        }
    }
#endif
    
    NodeIndexEntry* index_entry = NodeIndex::findNode(nodeNum);
    if (!index_entry) {
        return false;
    }
    if (NodeStorage::writeNodeToSlot(index_entry->slot_id, node)) {
        NodeIndex::markDirty(nodeNum, false);
        return true;
    }
    return false;
}

/**
 * @brief Find cache entry by node number
 * @param nodeNum Node number to find
 * @return Pointer to cache entry, or nullptr if not found
 */
static CacheEntry* findEntry(NodeNum nodeNum)
{
    if (!initialized || cache_size == 0) {
        return nullptr;
    }
    
    for (uint32_t i = 0; i < cache_size; i++) {
        // CRITICAL: Check bounds before accessing cache_entries
        if (i >= max_cache_size) {
            LOG_ERROR("NodeCache: Index out of bounds in findNode (i: %u, max: %u)", i, max_cache_size);
            break;
        }
        if (cache_entries[i].num == nodeNum) {
            return &cache_entries[i];
        }
    }
    
    return nullptr;
}

/**
 * @brief Find least recently used, non-protected entry for eviction
 * @return Pointer to cache entry to evict, or nullptr if all nodes are protected
 */
static CacheEntry* findEvictionCandidate()
{
    if (!initialized || cache_size == 0) {
        return nullptr;
    }
    
    CacheEntry* eviction_candidate = nullptr;
    uint32_t oldest_access_time = UINT32_MAX;
    
    for (uint32_t i = 0; i < cache_size; i++) {
        // CRITICAL: Check bounds before accessing cache_entries
        if (i >= max_cache_size) {
            LOG_ERROR("NodeCache: Index out of bounds in findEvictionCandidate (i: %u, max: %u)", i, max_cache_size);
            break;
        }
        CacheEntry* cache_entry = &cache_entries[i];
        
        // Check if node is protected (local node, router, favorite)
        NodeIndexEntry* index_entry = NodeIndex::findNode(cache_entry->num);
        if (index_entry && (index_entry->flags & NodeIndexEntry::FLAG_PROTECTED)) {
            continue;  // Skip protected nodes
        }
        
        // Find least recently accessed node
        if (cache_entry->last_access < oldest_access_time) {
            oldest_access_time = cache_entry->last_access;
            eviction_candidate = cache_entry;
        }
    }
    
    return eviction_candidate;
}

/**
 * @brief Initialize the cache
 */
bool initialize(uint32_t maxSize)
{
    if (initialized) {
        // Already initialized
        return true;
    }
    
    uint32_t maxCache = MemoryHelpers::getMaxNodesCache();
    if (maxSize == 0 || maxSize > maxCache) {
        return false;  // Sanity check
    }
    
    // Check available memory before allocation
    // CacheEntry size: meshtastic_NodeInfoLite (~200 bytes) + metadata (~8 bytes) = ~208 bytes
    // For 300 entries: 300 * 208 = 62,400 bytes (~61 KB)
    // Check if we have enough free heap (need at least 100 KB to account for fragmentation)
    uint32_t requiredMemory = maxSize * sizeof(CacheEntry);
    uint32_t safetyMargin = MemoryHelpers::SafetyMargins::CACHE;
    auto memCheck = MemoryHelpers::checkMemory(requiredMemory, safetyMargin, "NodeCache");
    
    if (!memCheck.sufficient) {
        return false;
    }
    
    // Allocate cache entries using common helper to avoid assert in operator new
    // This allows us to check for allocation failure without triggering assert
    uint32_t freeHeapBefore = memCheck.freeHeap;
    
    if (!allocateMemoryWithCheck(cache_entries, maxSize, "NodeCache", freeHeapBefore)) {
        return false;
    }
    
    uint32_t freeHeapAfter = memGet.getFreeHeap();
    uint32_t allocatedMemory = freeHeapBefore - freeHeapAfter;
    uint32_t allocation_size = maxSize * sizeof(CacheEntry);
    
    max_cache_size = maxSize;
    cache_size = 0;
    access_counter = 0;
    initialized = true;
    
    memset(cache_entries, 0, sizeof(CacheEntry) * max_cache_size);
    
    LOG_INFO("NodeCache: Initialized successfully");
    LOG_DEBUG("NodeCache:   Max size: %u entries", max_cache_size);
    LOG_DEBUG("NodeCache:   Entry size: %u bytes", sizeof(CacheEntry));
    LOG_DEBUG("NodeCache:   Total allocated: %u bytes", allocation_size);
    LOG_DEBUG("NodeCache:   Free heap before: %u bytes, after: %u bytes, allocated: %u bytes", 
             freeHeapBefore, freeHeapAfter, allocatedMemory);
    
    return true;
}

/**
 * @brief Get a node from cache (load from flash if not cached)
 */
meshtastic_NodeInfoLite* getNode(NodeNum nodeNum)
{
    if (!initialized) {
        if (!initialize(MemoryHelpers::getMaxNodesCache())) {
            return nullptr;
        }
    }
    
    // Check if node is in cache
    CacheEntry* cache_entry = findEntry(nodeNum);
    if (cache_entry) {
        // Update access metadata
        cache_entry->last_access = access_counter++;
        cache_entry->access_count++;
        
        // Update index
        NodeIndex::incrementAccessCount(nodeNum);
        NodeIndex::updateLastHeard(nodeNum, cache_entry->node.last_heard);
        
        return &cache_entry->node;
    }
    
    // Cache miss - load from flash
    NodeIndexEntry* index_entry = NodeIndex::findNode(nodeNum);
    if (!index_entry) {
        return nullptr;  // Node doesn't exist
    }
    
    // Load from flash
    meshtastic_NodeInfoLite node;
    if (!NodeStorage::readNodeFromSlot(index_entry->slot_id, &node)) {
        return nullptr;  // Failed to load from flash
    }
    
    // Add to cache (may evict another node)
    if (putNode(nodeNum, &node)) {
        return getNode(nodeNum);  // Get from cache (now it's there)
    }
    
    return nullptr;
}

/**
 * @brief Put a node into cache
 */
bool putNode(NodeNum nodeNum, const meshtastic_NodeInfoLite* node)
{
    if (!initialized) {
        if (!initialize(MemoryHelpers::getMaxNodesCache())) {
            return false;
        }
    }
    
    if (!node) {
        return false;
    }
    
    // Check if already in cache
    CacheEntry* existing_entry = findEntry(nodeNum);
    if (existing_entry) {
        // Update existing cache entry
        existing_entry->node = *node;
        existing_entry->last_access = access_counter++;
        existing_entry->access_count++;
        return true;
    }
    
    // Find or allocate cache entry
    CacheEntry* cache_entry = nullptr;
    
    // Check if cache is full
    if (cache_size >= max_cache_size) {
        // Evict least recently used, non-protected node
        CacheEntry* evicted_entry = findEvictionCandidate();
        if (evicted_entry) {
            // Write to flash if dirty (cold data changed) - synchronous write during eviction
            if (evicted_entry->dirty) {
                writeDirtyNodeToFlash(evicted_entry->num, &evicted_entry->node);
            }
            
            // Remove from index cache flag
            NodeIndex::markCached(evicted_entry->num, false);
            
            // Reuse evicted entry for new node
            cache_entry = evicted_entry;
        } else {
            // All nodes are protected - cannot evict
            LOG_WARN("NodeCache: Cache full (size: %u, max: %u) and all nodes are protected - cannot add node 0x%x", 
                     cache_size, max_cache_size, nodeNum);
            LOG_WARN("NodeCache: Consider increasing cache size or reducing number of protected nodes");
            return false;
        }
    } else {
        // Use next available entry
        cache_entry = &cache_entries[cache_size];
        cache_size++;
    }
    
    // Fill cache entry with node data
    cache_entry->num = nodeNum;
    cache_entry->node = *node;
    cache_entry->last_access = access_counter++;
    cache_entry->access_count = 1;
    cache_entry->dirty = false;
    
    // Update index
    NodeIndex::markCached(nodeNum, true);
    
    return true;
}

/**
 * @brief Evict a node from cache
 */
bool evictNode(NodeNum nodeNum)
{
    CHECK_CACHE_INIT();
    
    CacheEntry* cache_entry = findEntry(nodeNum);
    if (!cache_entry) {
        return false;  // Not in cache
    }
    
    // Write to flash if dirty (cold data changed) - synchronous write during eviction
    if (cache_entry->dirty) {
        writeDirtyNodeToFlash(nodeNum, &cache_entry->node);
    }
    
    // Remove from index cache flag
    NodeIndex::markCached(nodeNum, false);
    
    // Remove from cache (shift entries to fill gap)
    // CRITICAL: Bounds checking to prevent buffer overflow in memmove
    int entry_position = cache_entry - cache_entries;
    
    // CRITICAL: Validate entry_position is within valid range
    if (entry_position < 0 || entry_position >= (int)cache_size) {
        LOG_ERROR("NodeCache: Invalid entry_position (%d) in evictNode (cache_size: %u)", entry_position, cache_size);
        return false;
    }
    
    // CRITICAL: Validate entry_position points to valid cache entry
    if (entry_position >= (int)max_cache_size) {
        LOG_ERROR("NodeCache: entry_position (%d) exceeds max_cache_size (%u)", entry_position, max_cache_size);
        return false;
    }
    
    // CRITICAL: Only shift if there are entries after this one
    if (entry_position < (int)cache_size - 1) {
        // CRITICAL: Validate shift size and destination
        uint32_t shift_size = cache_size - entry_position - 1;
        if (entry_position + 1 + shift_size > max_cache_size) {
            LOG_ERROR("NodeCache: memmove would overflow (entry_pos: %d, shift_size: %u, max: %u)", 
                     entry_position, shift_size, max_cache_size);
            return false;
        }
        memmove(&cache_entries[entry_position], &cache_entries[entry_position + 1],
                shift_size * sizeof(CacheEntry));
    }
    
    // CRITICAL: Validate cache_size before decrementing
    if (cache_size == 0) {
        LOG_ERROR("NodeCache: Attempted to decrement cache_size when already 0");
        return false;
    }
    
    cache_size--;
    
    return true;
}

/**
 * @brief Check if a node is cached
 */
bool isCached(NodeNum nodeNum)
{
    return findEntry(nodeNum) != nullptr;
}

/**
 * @brief Get current cache size
 */
size_t getCacheSize()
{
    // CRITICAL: Check if initialized and cache_entries is valid before accessing cache_size
    // This prevents crashes if called before initialization or if memory was corrupted
    if (!initialized || cache_entries == nullptr) {
        return 0;
    }
    return cache_size;
}

/**
 * @brief Get maximum cache size
 */
size_t getMaxCacheSize()
{
    return max_cache_size;
}

/**
 * @brief Clear all entries from cache
 * @param flushDirty If true, write dirty entries to flash before clearing (default: true)
 * 
 * During factory reset, set flushDirty=false to skip flash writes since Main FS will be reformatted.
 */
void clear(bool flushDirty)
{
    CHECK_CACHE_INIT_VOID();
    
    // Write all dirty entries to flash (unless during factory reset)
    if (flushDirty) {
    flushDirtyNodes();
    } else {
        LOG_INFO("NodeCache: Skipping flushDirtyNodes() during factory reset (Main FS will be reformatted)");
    }
    
    // Clear cache
    for (uint32_t i = 0; i < cache_size; i++) {
        // CRITICAL: Check bounds before accessing cache_entries
        if (i >= max_cache_size) {
            LOG_ERROR("NodeCache: Index out of bounds in clear (i: %u, max: %u)", i, max_cache_size);
            break;
        }
        NodeIndex::markCached(cache_entries[i].num, false);
    }
    
    cache_size = 0;
    memset(cache_entries, 0, sizeof(CacheEntry) * max_cache_size);
}

/**
 * @brief Mark a node as dirty
 */
void markDirty(NodeNum nodeNum)
{
    CHECK_CACHE_INIT_VOID();
    
    CacheEntry* cache_entry = findEntry(nodeNum);
    if (cache_entry) {
        cache_entry->dirty = true;
        NodeIndex::markDirty(nodeNum, true);
        
        // Enqueue to write queue for background processing
        NodeIndexEntry* index_entry = NodeIndex::findNode(nodeNum);
        if (index_entry && index_entry->slot_id != UINT16_MAX) {
            bool isProtected = (index_entry->flags & NodeIndexEntry::FLAG_PROTECTED) != 0;
            if (NodeDBBackgroundTask::isWriteQueueInitialized()) {
                if (!NodeDBBackgroundTask::enqueueWrite(nodeNum, index_entry->slot_id, isProtected)) {
                    // CRITICAL: Queue is full - data integrity is more important than packet reception
                    // Force write even if radio is receiving - better to lose one packet than lose data
                    LOG_WARN("NodeCache: Write queue full for node 0x%x, FORCING flush immediately (ignoring radio state)...", nodeNum);
                    // Process up to 5 writes to free space - FORCE mode ignores radio state
                    NodeDBBackgroundTask::processWriteQueue(5, true);  // true = force write
                    // Retry enqueue
                    if (!NodeDBBackgroundTask::enqueueWrite(nodeNum, index_entry->slot_id, isProtected)) {
                        LOG_ERROR("NodeCache: CRITICAL - Write queue still full after forced flush, node 0x%x may lose data!", nodeNum);
                        // Last resort: mark as dirty anyway, will be written on next flushDirtyNodes()
                        // Data is still in cache, so it's not completely lost
                    }
                }
            }
        }
    }
}

/**
 * @brief Clear dirty flag for a node
 */
void clearDirty(NodeNum nodeNum)
{
    CHECK_CACHE_INIT_VOID();
    
    CacheEntry* cache_entry = findEntry(nodeNum);
    if (cache_entry) {
        cache_entry->dirty = false;
        NodeIndex::markDirty(nodeNum, false);
    }
}

/**
 * @brief Write all dirty nodes to flash
 */
uint32_t flushDirtyNodes()
{
    if (!initialized) {
        LOG_DEBUG("NodeCache: flushDirtyNodes() called but cache not initialized");
        return 0;
    }
    
    LOG_DEBUG("NodeCache: flushDirtyNodes() - checking %u cache entries", cache_size);
    uint32_t written = 0;
    
    for (uint32_t i = 0; i < cache_size; i++) {
        CacheEntry* cache_entry = &cache_entries[i];
        if (cache_entry->dirty) {
            // CRITICAL: Check radio state before each write
            // Skip this write if radio is receiving - will be retried later
#ifdef ARCH_NRF52
            if (RadioLibInterface::instance != nullptr) {
                if (RadioLibInterface::instance->isActivelyReceiving()) {
                    LOG_DEBUG("NodeCache: Skipping flush of node 0x%x - radio is receiving", cache_entry->num);
                    continue;  // Skip this node, will be retried by background task
                }
            }
#endif
            
            LOG_DEBUG("NodeCache: Found dirty node 0x%x, writing to flash...", cache_entry->num);
            NodeIndexEntry* index_entry = NodeIndex::findNode(cache_entry->num);
            if (index_entry && index_entry->slot_id != UINT16_MAX) {
                if (NodeStorage::writeNodeToSlot(index_entry->slot_id, &cache_entry->node)) {
                    cache_entry->dirty = false;
                    NodeIndex::markDirty(cache_entry->num, false);
                    written++;
                    LOG_DEBUG("NodeCache: Successfully wrote node 0x%x to slot %u", cache_entry->num, index_entry->slot_id);
                } else {
                    LOG_ERROR("NodeCache: Failed to write node 0x%x to slot %u", cache_entry->num, index_entry->slot_id);
                }
            } else {
                LOG_WARN("NodeCache: Dirty node 0x%x not found in index or has invalid slot_id", cache_entry->num);
            }
        }
    }
    
    LOG_DEBUG("NodeCache: flushDirtyNodes() completed - wrote %u nodes", written);
    return written;
}

} // namespace NodeCache



// ============================================================================
// From NodeDBBackgroundTask.cpp
// ============================================================================



#ifdef ARCH_NRF52
extern void nrf52Loop();
extern NRF52Bluetooth *nrf52Bluetooth;
extern meshtastic::BluetoothStatus *bluetoothStatus;
#endif
namespace NodeDBBackgroundTask {

// ============================================================================
// Write Queue Implementation (previously NodeDBWriteQueue namespace)
// ============================================================================

// Circular buffer for write queue
static WriteQueueEntry* queue_buffer = nullptr;
static uint32_t max_queue_size = 0;
static uint32_t queue_head = 0;  // Write position (next to add)
static uint32_t queue_tail = 0;  // Read position (next to remove)
static uint32_t queue_count = 0;  // Current number of entries
static bool write_queue_initialized = false;

/**
 * @brief Calculate priority for a node
 * @param isProtected true if node is protected
 * @return Priority value (higher = more important)
 */
static uint32_t calculatePriority(bool isProtected)
{
    // Protected nodes have priority 0xFFFFFFFF (highest)
    // Non-protected nodes have priority based on access count (from NodeIndex)
    // For now, use simple priority: protected = 0xFFFFFFFF, others = 0
    return isProtected ? 0xFFFFFFFF : 0;
}

/**
 * @brief Find entry in queue by node number
 * @param nodeNum Node number to find
 * @return Index in queue buffer, or UINT32_MAX if not found
 */
static uint32_t findEntryIndex(NodeNum nodeNum)
{
    if (!write_queue_initialized || queue_count == 0 || !queue_buffer) {
        return UINT32_MAX;
    }
    
    // CRITICAL: Validate queue_tail before loop
    if (queue_tail >= max_queue_size) {
        LOG_ERROR("NodeDBBackgroundTask: Invalid queue_tail (%u) >= max_queue_size (%u) in findEntryIndex", 
                 queue_tail, max_queue_size);
        return UINT32_MAX;
    }
    
    // CRITICAL: Limit iterations to prevent infinite loops
    uint32_t max_iterations = (queue_count < max_queue_size) ? queue_count : max_queue_size;
    for (uint32_t i = 0; i < max_iterations; i++) {
        uint32_t idx = (queue_tail + i) % max_queue_size;
        
        // CRITICAL: Bounds check before array access
        if (idx >= max_queue_size) {
            LOG_ERROR("NodeDBBackgroundTask: Buffer overflow in findEntryIndex (idx: %u, max: %u)", 
                     idx, max_queue_size);
            break;
        }
        
        if (queue_buffer[idx].nodeNum == nodeNum) {
            return idx;
        }
    }
    
    return UINT32_MAX;
}

bool initializeWriteQueue(uint32_t maxSize)
{
    if (write_queue_initialized) {
        return true;  // Already initialized
    }
    
    if (maxSize == 0 || maxSize > 200) {
        return false;  // Sanity check (max 200 entries)
    }
    
    // Check available memory
    uint32_t requiredMemory = maxSize * sizeof(WriteQueueEntry);
    uint32_t safetyMargin = MemoryHelpers::SafetyMargins::WRITE_QUEUE;
    auto memCheck = MemoryHelpers::checkMemory(requiredMemory, safetyMargin, "NodeDBBackgroundTask");
    
    LOG_DEBUG("NodeDBBackgroundTask: Write queue memory check - required: %u bytes, margin: %u bytes, total needed: %u bytes", 
             requiredMemory, safetyMargin, memCheck.totalNeeded);
    LOG_DEBUG("NodeDBBackgroundTask: Write queue memory status - free: %u bytes, used: %u bytes, total: %u bytes", 
             memCheck.freeHeap, memCheck.usedHeap, memCheck.heapTotal);
    
    if (!memCheck.sufficient) {
        return false;
    }
    
    LOG_DEBUG("NodeDBBackgroundTask: Write queue memory check PASSED - sufficient memory available");
    
    // Allocate queue buffer
    uint32_t allocation_size = maxSize * sizeof(WriteQueueEntry);
    LOG_DEBUG("NodeDBBackgroundTask: Allocating %u bytes for %u write queue entries", allocation_size, maxSize);
    uint32_t freeHeapBefore = memCheck.freeHeap;
    
    #ifdef ARCH_NRF52
    queue_buffer = (WriteQueueEntry*)rtos_malloc(allocation_size);
    #else
    queue_buffer = new WriteQueueEntry[maxSize];
    #endif
    
    if (!queue_buffer) {
        uint32_t freeHeapAfter = memGet.getFreeHeap();
        MemoryHelpers::logAllocationFailure("NodeDBBackgroundTask", allocation_size, sizeof(WriteQueueEntry), 
                                           maxSize, freeHeapBefore, freeHeapAfter);
        LOG_ERROR("NodeDBBackgroundTask: Write queue allocation FAILED - memory may be fragmented");
        return false;
    }
    
    // CRITICAL: Log successful allocation for debugging
    uint32_t freeHeapAfter = memGet.getFreeHeap();
    uint32_t allocatedMemory = freeHeapBefore - freeHeapAfter;
    LOG_DEBUG("NodeDBBackgroundTask: Write queue allocation SUCCESS");
    LOG_DEBUG("NodeDBBackgroundTask: Allocated %u bytes (%u entries of %u bytes each)", 
             allocation_size, maxSize, sizeof(WriteQueueEntry));
    LOG_DEBUG("NodeDBBackgroundTask: Free heap before: %u bytes, after: %u bytes, allocated: %u bytes", 
             freeHeapBefore, freeHeapAfter, allocatedMemory);
    
    max_queue_size = maxSize;
    queue_head = 0;
    queue_tail = 0;
    queue_count = 0;
    
    LOG_INFO("NodeDBBackgroundTask: Write queue initialized successfully");
    LOG_DEBUG("NodeDBBackgroundTask:   Max size: %u entries", max_queue_size);
    LOG_DEBUG("NodeDBBackgroundTask:   Entry size: %u bytes", sizeof(WriteQueueEntry));
    LOG_DEBUG("NodeDBBackgroundTask:   Total allocated: %u bytes", allocation_size);
    LOG_DEBUG("NodeDBBackgroundTask:   Free heap before: %u bytes, after: %u bytes, allocated: %u bytes", 
             freeHeapBefore, freeHeapAfter, allocatedMemory);
    write_queue_initialized = true;
    
    memset(queue_buffer, 0, sizeof(WriteQueueEntry) * max_queue_size);
    
    LOG_DEBUG("NodeDBBackgroundTask: Write queue initialized with max size %u", maxSize);
    return true;
}

bool isWriteQueueInitialized()
{
    return write_queue_initialized;
}

bool enqueueWrite(NodeNum nodeNum, uint16_t slotId, bool isProtected)
{
    if (!write_queue_initialized) {
        if (!initializeWriteQueue(100)) {
            return false;
        }
    }
    
    if (nodeNum == 0) {
        return false;  // Invalid node number
    }
    
    // Check if node already in queue - update it instead
    uint32_t existing_idx = findEntryIndex(nodeNum);
    if (existing_idx != UINT32_MAX) {
        // CRITICAL: Validate existing_idx before accessing buffer
        if (existing_idx >= max_queue_size) {
            LOG_ERROR("NodeDBBackgroundTask: Invalid existing_idx (%u) >= max_queue_size (%u)", 
                     existing_idx, max_queue_size);
            return false;
        }
        // Update existing entry with new priority
        queue_buffer[existing_idx].slotId = slotId;
        queue_buffer[existing_idx].priority = calculatePriority(isProtected);
        queue_buffer[existing_idx].isProtected = isProtected;
        LOG_DEBUG("NodeDBBackgroundTask: Updated write queue entry for node 0x%x (priority: %u, slot: %u)", 
                 nodeNum, queue_buffer[existing_idx].priority, slotId);
        return true;
    }
    
    // Check if queue is full
    if (queue_count >= max_queue_size) {
        LOG_WARN("NodeDBBackgroundTask: Write queue full (%u/%u), cannot add node 0x%x", 
                queue_count, max_queue_size, nodeNum);
        return false;
    }
    
    // Add new entry at head
    // CRITICAL: Validate queue_head before accessing buffer
    if (queue_head >= max_queue_size) {
        LOG_ERROR("NodeDBBackgroundTask: Invalid queue_head (%u) >= max_queue_size (%u)", 
                 queue_head, max_queue_size);
        return false;
    }
    
    // CRITICAL: Check bounds before writing to queue_buffer
    if (queue_head >= max_queue_size) {
        LOG_ERROR("NodeDBBackgroundTask: queue_head out of bounds (%u >= %u) in enqueueWrite", 
                 queue_head, max_queue_size);
        return false;
    }
    
    queue_buffer[queue_head].nodeNum = nodeNum;
    queue_buffer[queue_head].slotId = slotId;
    queue_buffer[queue_head].priority = calculatePriority(isProtected);
    queue_buffer[queue_head].isProtected = isProtected;
    
    queue_head = (queue_head + 1) % max_queue_size;
    queue_count++;
    
    // CRITICAL: Validate queue_count doesn't exceed max_queue_size
    if (queue_count > max_queue_size) {
        LOG_ERROR("NodeDBBackgroundTask: Queue count overflow (%u > %u)", queue_count, max_queue_size);
        queue_count = max_queue_size;  // Clamp to prevent further issues
        return false;
    }
    
    // Throttle enqueue log to reduce log volume
    static unsigned long lastEnqueueLog = 0;
    static const unsigned long ENQUEUE_LOG_INTERVAL_MS = 2000; // Log at most once per 2 seconds
    unsigned long now = millis();
    if (now - lastEnqueueLog > ENQUEUE_LOG_INTERVAL_MS) {
        LOG_DEBUG("NodeDBBackgroundTask: Enqueued node 0x%x to write queue (slot: %u, priority: %u, queue: %u/%u)", 
                 nodeNum, slotId, queue_buffer[(queue_head - 1 + max_queue_size) % max_queue_size].priority, 
                 queue_count, max_queue_size);
        lastEnqueueLog = now;
    }
    
    return true;
}

bool dequeueWrite(WriteQueueEntry& entry)
{
    if (!write_queue_initialized || queue_count == 0) {
        return false;
    }
    
    // Find entry with highest priority
    // CRITICAL: Bounds checking to prevent buffer overflow
    if (queue_tail >= max_queue_size) {
        LOG_ERROR("NodeDBBackgroundTask: Invalid queue_tail %u (max: %u)", queue_tail, max_queue_size);
        return false;
    }
    
    // CRITICAL: Check bounds before accessing queue_buffer[queue_tail]
    if (queue_tail >= max_queue_size) {
        LOG_ERROR("NodeDBBackgroundTask: queue_tail out of bounds (%u >= %u) in dequeueWrite", 
                 queue_tail, max_queue_size);
        return false;
    }
    
    uint32_t best_idx = queue_tail;
    uint32_t best_priority = queue_buffer[queue_tail].priority;
    
    // CRITICAL: Limit loop to queue_count to prevent buffer overflow
    uint32_t max_iterations = (queue_count < max_queue_size) ? queue_count : max_queue_size;
    for (uint32_t i = 1; i < max_iterations; i++) {
        uint32_t idx = (queue_tail + i) % max_queue_size;
        if (idx >= max_queue_size) {
            LOG_ERROR("NodeDBBackgroundTask: Buffer overflow in dequeue loop (idx: %u, max: %u)", 
                     idx, max_queue_size);
            break;
        }
        if (queue_buffer[idx].priority > best_priority) {
            best_idx = idx;
            best_priority = queue_buffer[idx].priority;
        }
    }
    
    // CRITICAL: Check bounds before accessing queue_buffer[best_idx]
    if (best_idx >= max_queue_size) {
        LOG_ERROR("NodeDBBackgroundTask: best_idx out of bounds (%u >= %u) in dequeueWrite", 
                 best_idx, max_queue_size);
        return false;
    }
    
    // Copy entry
    entry = queue_buffer[best_idx];
    
    // Remove entry by shifting others
    // CRITICAL: Bounds checking to prevent buffer overflow
    if (best_idx >= max_queue_size) {
        LOG_ERROR("NodeDBBackgroundTask: Invalid best_idx %u (max: %u)", best_idx, max_queue_size);
        return false;
    }
    
    if (best_idx != queue_tail) {
        // Shift entries from tail to best_idx
        if (best_idx > queue_tail) {
            // Simple case: shift left
            // CRITICAL: Bounds check before memmove
            uint32_t shift_size = best_idx - queue_tail;
            if (queue_tail + shift_size > max_queue_size) {
                LOG_ERROR("NodeDBBackgroundTask: Buffer overflow in dequeue (tail: %u, best: %u, max: %u)", 
                         queue_tail, best_idx, max_queue_size);
                return false;
            }
            memmove(&queue_buffer[queue_tail + 1], &queue_buffer[queue_tail],
                    shift_size * sizeof(WriteQueueEntry));
        } else {
            // Wrap-around case: shift in two parts
            // CRITICAL: Bounds checking for wrap-around case
            uint32_t part1_size = max_queue_size - queue_tail;
            if (part1_size > max_queue_size || queue_tail >= max_queue_size) {
                LOG_ERROR("NodeDBBackgroundTask: Buffer overflow in dequeue wrap-around (tail: %u, max: %u)", 
                         queue_tail, max_queue_size);
                return false;
            }
            
            // CRITICAL: Validate best_idx for wrap-around case
            if (best_idx >= max_queue_size) {
                LOG_ERROR("NodeDBBackgroundTask: Invalid best_idx (%u) in wrap-around case (max: %u)", 
                         best_idx, max_queue_size);
                return false;
            }
            
            WriteQueueEntry temp;
            memcpy(&temp, &queue_buffer[queue_tail], sizeof(WriteQueueEntry));
            
            // Part 1: Shift from tail to end of buffer
            if (part1_size > 1 && queue_tail + 1 < max_queue_size) {
                uint32_t shift_size = part1_size - 1;
                // CRITICAL: Validate shift doesn't exceed buffer
                if (queue_tail + shift_size >= max_queue_size) {
                    LOG_ERROR("NodeDBBackgroundTask: Wrap-around part1 shift overflow (tail: %u, shift: %u, max: %u)", 
                             queue_tail, shift_size, max_queue_size);
                    return false;
                }
                memmove(&queue_buffer[queue_tail], &queue_buffer[queue_tail + 1],
                        shift_size * sizeof(WriteQueueEntry));
            }
            
            // Part 2: Shift from 0 to best_idx
            if (best_idx > 0 && best_idx < max_queue_size) {
                // CRITICAL: Validate shift size
                if (best_idx > max_queue_size) {
                    LOG_ERROR("NodeDBBackgroundTask: Wrap-around part2 shift overflow (best_idx: %u, max: %u)", 
                             best_idx, max_queue_size);
                    return false;
                }
                memmove(&queue_buffer[best_idx - 1], &queue_buffer[best_idx],
                        best_idx * sizeof(WriteQueueEntry));
                // CRITICAL: Validate destination for temp copy
                if (max_queue_size > 0) {
                    memcpy(&queue_buffer[max_queue_size - 1], &temp, sizeof(WriteQueueEntry));
                }
            }
        }
    }
    
    // Update tail and count
    // CRITICAL: Validate queue_count before decrementing
    if (queue_count == 0) {
        LOG_ERROR("NodeDBBackgroundTask: Attempted to dequeue from empty queue");
        return false;
    }
    
    queue_tail = (queue_tail + 1) % max_queue_size;
    queue_count--;
    
    // CRITICAL: Validate queue_tail after update
    if (queue_tail >= max_queue_size) {
        LOG_ERROR("NodeDBBackgroundTask: Invalid queue_tail (%u) after update (max: %u)", 
                 queue_tail, max_queue_size);
        queue_tail = 0;  // Reset to prevent further issues
    }
    
    // Throttle dequeue log to reduce log volume
    static unsigned long lastDequeueLog = 0;
    static const unsigned long DEQUEUE_LOG_INTERVAL_MS = 2000; // Log at most once per 2 seconds
    unsigned long now = millis();
    if (now - lastDequeueLog > DEQUEUE_LOG_INTERVAL_MS) {
        LOG_DEBUG("NodeDBBackgroundTask: Dequeued node 0x%x from write queue (slot: %u, priority: %u, remaining: %u)", 
                 entry.nodeNum, entry.slotId, entry.priority, queue_count);
        lastDequeueLog = now;
    }
    
    return true;
}

bool isWriteQueueEmpty()
{
    return !write_queue_initialized || queue_count == 0;
}

bool isWriteQueueFull()
{
    return write_queue_initialized && queue_count >= max_queue_size;
}

size_t getWriteQueueSize()
{
    if (!write_queue_initialized) {
        return 0;
    }
    return queue_count;
}

size_t getWriteQueueMaxSize()
{
    return max_queue_size;
}

void clearWriteQueue()
{
    if (!write_queue_initialized) {
        return;
    }
    
    queue_head = 0;
    queue_tail = 0;
    queue_count = 0;
    memset(queue_buffer, 0, sizeof(WriteQueueEntry) * max_queue_size);
    
    LOG_DEBUG("NodeDBBackgroundTask: Cleared all write queue entries");
}

bool writeQueueContains(NodeNum nodeNum)
{
    return findEntryIndex(nodeNum) != UINT32_MAX;
}

// ============================================================================
// Background Task Implementation
// ============================================================================

static bool background_task_initialized = false;
static uint32_t last_tick_time = 0;
static constexpr uint32_t TICK_INTERVAL_MS = 200;

uint32_t processWriteQueue(uint32_t maxWrites, bool force)
{
    if (!isWriteQueueInitialized() || isWriteQueueEmpty()) {
        return 0;
    }
    
    // CRITICAL: Limit maxWrites to prevent buffer overflow and ensure bounded execution time
    constexpr uint32_t MAX_WRITES_PER_TICK = 10;  // Maximum writes per tick to prevent blocking
    if (maxWrites > MAX_WRITES_PER_TICK) {
        maxWrites = MAX_WRITES_PER_TICK;
    }
    
    // CRITICAL: Ensure we don't process more than queue size (safety check)
    uint32_t queue_size = getWriteQueueSize();
    if (maxWrites > queue_size) {
        maxWrites = queue_size;
    }
    
    uint32_t writes_processed = 0;
    uint32_t start_time = millis();
    
    // CRITICAL: Check radio state ONCE at the start, not for each entry
    // This prevents infinite loop when radio is constantly showing false detections
    // If we check for each entry and radio is always "receiving", we get:
    // dequeue -> check radio -> re-enqueue -> dequeue -> ... (infinite loop)
    bool radio_is_receiving = false;
    bool radio_is_sending = false;
    #ifdef ARCH_NRF52
    if (!force && RadioLibInterface::instance != nullptr) {
        // OPTIMIZATION: Check isSending() FIRST - if transmitting, immediately allow writes
        // During transmission, radio CANNOT receive packets, so it's SAFE to write
        // This avoids unnecessary isActivelyReceiving() check and delays
        radio_is_sending = RadioLibInterface::instance->isSending();
        
        if (radio_is_sending) {
            // Radio is sending - SAFE to write immediately (reception is blocked)
            // Skip all other checks - no need to check isActivelyReceiving()
            // Proceed directly to write loop (fast path)
        } else {
            // Radio is NOT sending - check if receiving
            radio_is_receiving = RadioLibInterface::instance->isActivelyReceiving();
            
            // If radio is receiving, wait a short time and check again
            // This handles transient radio activity (real packets) vs false detections
            // False detections are usually very brief, real packets take longer
            if (radio_is_receiving) {
                // Wait 50 ms for real packet reception to complete
                // This prevents losing data when radio is actually receiving
                delay(50);
                FEED_WATCHDOG_AND_YIELD();
                
                // Check again after waiting
                radio_is_receiving = RadioLibInterface::instance->isActivelyReceiving();
                radio_is_sending = RadioLibInterface::instance->isSending();
                
                // If still receiving after wait (and not sending), it might be false detection spam
                // But we'll still skip to avoid packet loss (better safe than sorry)
                // The next tick (200ms later) will try again
            }
        }
    }
    #endif
    
    // If radio is receiving (and NOT sending), skip entire batch
    // This prevents infinite loop: dequeue -> check radio -> re-enqueue -> repeat
    // But if radio is sending, we can write (transmission blocks reception, so no packet loss)
    if (radio_is_receiving && !radio_is_sending) {
        // Radio is receiving (but not sending) - skip this batch, will retry on next tick
        // This is much better than infinite loop of dequeue/re-enqueue
        return 0;
    }
    // If radio is sending OR not receiving - proceed with writes (safe - transmission blocks reception)
    
    while (writes_processed < maxWrites && !isWriteQueueEmpty()) {
        if (millis() - start_time > 5000) {
            FEED_WATCHDOG_AND_YIELD();
            delay(50);
            start_time = millis();
        }
        
        WriteQueueEntry entry;
        if (!dequeueWrite(entry)) {
            break;
        }
        
        meshtastic_NodeInfoLite* node = NodeCache::getNode(entry.nodeNum);
        if (!node) {
            continue;
        }
        
        FEED_WATCHDOG_AND_YIELD();
        
        // REMOVED: Radio check here - we already checked at the start
        // This prevents infinite loop when radio shows constant false detections
        // If force=true, we already skipped the radio check above, so proceed with write
        
        if (NodeStorage::writeNodeToSlot(entry.slotId, node)) {
            NodeCache::clearDirty(entry.nodeNum);
            writes_processed++;
            LOG_INFO("NodeDBBackgroundTask: Processed write queue entry - node 0x%08X saved to slot %u", entry.nodeNum, entry.slotId);
        } else {
            // CRITICAL: Check if write was skipped due to radio receiving (not a real error)
            // This can happen if radio started receiving between our check and writeNodeToSlot() call
            bool was_skipped_due_to_radio = false;
            #ifdef ARCH_NRF52
            // Static variable for throttling write skipped logs
            static unsigned long lastWriteSkippedLog = 0;
            static const unsigned long WRITE_SKIPPED_LOG_INTERVAL_MS = 2000; // Log at most once per 2 seconds
            
            if (!force && RadioLibInterface::instance != nullptr) {
                if (RadioLibInterface::instance->isActivelyReceiving()) {
                    was_skipped_due_to_radio = true;
                    // Re-enqueue for later - this is normal behavior, not an error
                    // Throttle logging to avoid log spam
                    unsigned long now = millis();
                    if (now - lastWriteSkippedLog > WRITE_SKIPPED_LOG_INTERVAL_MS) {
                        LOG_DEBUG("NodeDBBackgroundTask: Write skipped for node 0x%08X (slot %u) - radio is receiving, re-enqueuing", 
                                 entry.nodeNum, entry.slotId);
                        lastWriteSkippedLog = now;
                    }
                    enqueueWrite(entry.nodeNum, entry.slotId, entry.isProtected);
                }
            }
            #endif
            
            if (!was_skipped_due_to_radio) {
                // This is a real error (not radio-related) - log as warning
            LOG_WARN("NodeDBBackgroundTask: Failed to write node 0x%08X to slot %u", entry.nodeNum, entry.slotId);
            }
        }
        
        FEED_WATCHDOG_AND_YIELD();
        
        if (writes_processed < maxWrites && !isWriteQueueEmpty()) {
            delay(10);
        }
    }
    
    return writes_processed;
}

static bool shouldProceedWithFlashOps()
{
    if (memGet.getFreeHeap() < 20480) {
        return false;
    }
    
    #ifdef ARCH_NRF52
    static uint32_t last_ble_connect_time = 0;
    static bool ble_was_connected = false;
    static bool ble_was_pairing = false;
    
    bool ble_connected = (nrf52Bluetooth && nrf52Bluetooth->isConnected());
    bool ble_pairing = false;
    
    if (bluetoothStatus) {
        auto state = bluetoothStatus->getConnectionState();
        ble_pairing = (state == meshtastic::BluetoothStatus::ConnectionState::PAIRING);
    }
    
    uint32_t now = millis();
    
    if (ble_pairing) {
        if (!ble_was_pairing) {
            ble_was_pairing = true;
        }
        return false;
    }
    
    if (ble_was_pairing && !ble_pairing) {
        ble_was_pairing = false;
        if (ble_connected) {
            last_ble_connect_time = now;
            ble_was_connected = true;
        }
    }
    
    if (ble_connected) {
        if (!ble_was_connected) {
            last_ble_connect_time = now;
            ble_was_connected = true;
            return false;
        }
        
        if ((now - last_ble_connect_time) < 30000) {
            return false;
        }
    } else {
        if (ble_was_connected) {
            ble_was_connected = false;
            last_ble_connect_time = 0;
        }
    }
    #endif
    
    return true;
}

void init(NodeDBBackendContext& context)
{
    (void)context;  // For future use
    
    if (background_task_initialized) {
        return;
    }
    
    if (!initializeWriteQueue(100)) {
        return;
    }
    
    background_task_initialized = true;
}

void tick()
{
    if (!background_task_initialized) {
        return;
    }
    
    uint32_t now = millis();
    if (now - last_tick_time < TICK_INTERVAL_MS) {
        return;
    }
    last_tick_time = now;
    
    FEED_WATCHDOG_AND_YIELD();
    
    if (!shouldProceedWithFlashOps()) {
        return;
    }
    
    // CRITICAL: Check radio state before processing write queue
    // This prevents flash writes during active packet reception
#ifdef ARCH_NRF52
    if (RadioLibInterface::instance != nullptr) {
        if (RadioLibInterface::instance->isActivelyReceiving()) {
            // Radio is receiving - skip this tick, will retry next time
            return;
        }
    }
#endif
    
    // CRITICAL: If queue is getting full, process more writes per tick
    uint32_t queue_size = NodeDBBackgroundTask::getWriteQueueSize();
    uint32_t max_queue = NodeDBBackgroundTask::getWriteQueueMaxSize();
    uint32_t writes_to_process = 2;  // Default
    
    if (max_queue > 0) {
        uint32_t queue_usage_percent = (queue_size * 100) / max_queue;
        if (queue_usage_percent > 80) {
            // Queue is >80% full - process more writes to catch up
            writes_to_process = 5;
            LOG_DEBUG("NodeDBBackgroundTask: Queue usage high (%u%%), processing %u writes", 
                     queue_usage_percent, writes_to_process);
        } else if (queue_usage_percent > 60) {
            // Queue is >60% full - process more writes
            writes_to_process = 3;
        }
    }
    
    uint32_t operations_done = processWriteQueue(writes_to_process, false);  // false = respect radio state (normal operation)
    
    if (NodeDBPersistentBackend::loadFromDisk()) {
        // Loading complete
    }
    
    FEED_WATCHDOG_AND_YIELD();
}

bool isInitialized()
{
    return background_task_initialized;
}

} // namespace NodeDBBackgroundTask



// ============================================================================
// From NodeDBPersistentBackend (formerly NodeDBPersistentBackend)
// ============================================================================



// Include LittleFS headers for directory operations
#include "../variant.h"  // For MAX_NUM_NODES
#ifdef ARCH_NRF52
#endif

// nrf52Loop() and yield() are now handled by FEED_WATCHDOG_AND_YIELD() macro from MemoryHelpers.h

namespace NodeDBPersistentBackend {

static bool backend_enabled = false;
static bool backend_initialized = false;
static bool index_loaded = false;  // Track if index was loaded from disk (lazy loading)
static uint32_t last_save_time = 0;
static constexpr uint32_t SAVE_THROTTLE_MS = 60 * 1000;  // 1 minute

// Macros for backend enabled checks
#define CHECK_BACKEND_ENABLED() do { if (!isEnabled()) return false; } while(0)
#define CHECK_BACKEND_ENABLED_NULL() do { if (!isEnabled()) return nullptr; } while(0)
#define CHECK_BACKEND_ENABLED_ZERO() do { if (!isEnabled()) return 0; } while(0)
#define CHECK_BACKEND_ENABLED_TRUE() do { if (!isEnabled()) return true; } while(0)
#define CHECK_BACKEND_ENABLED_VOID() do { if (!isEnabled()) return; } while(0)

// Incremental load state
struct IncrementalLoadState {
    enum State {
        IDLE,           // Not loading
        SCANNING_DIR,   // Scanning directory for slot files
        REBUILDING,     // Rebuilding index from slots
        PRELOADING      // Preloading cache
    };
    
    State state;
    uint32_t current_index;      // Current position in index
    uint32_t total_nodes;        // Total nodes to process
    uint32_t nodes_processed;    // Nodes processed so far
    uint32_t preload_target;      // Target number of nodes to preload
    uint32_t preload_count;      // Nodes preloaded so far
    bool time_valid;             // Whether time is valid for priority calculation
    uint32_t now;                 // Current time (if valid)
    
    // Static buffer for sorting (replaces stack allocation)
    struct NodeWithPriority {
        NodeNum num;
        uint32_t priority;
        uint32_t last_heard;
        uint16_t slot_id;
    };
    static constexpr uint32_t MAX_SORT_NODES = 50;  // Reduced to save memory and processing time
    // NOTE: sort_buffer is in static storage (not on stack), so no stack overflow risk
    NodeWithPriority sort_buffer[MAX_SORT_NODES];
    uint32_t sort_count;
    uint32_t sort_processed;      // How many sorted nodes we've processed
    
    IncrementalLoadState() : state(IDLE), current_index(0), total_nodes(0), 
                             nodes_processed(0), preload_target(0), preload_count(0),
                             time_valid(false), now(0), sort_count(0), sort_processed(0) {}
};

static IncrementalLoadState incremental_load_state;

/**
 * @brief Initialize the virtual backend
 */
/**
 * @brief Initialize with specific sizes (internal helper)
 */
static bool initializeWithSizes(uint32_t cacheSize, uint32_t indexSize)
{
    if (backend_initialized) {
        return true;
    }
    
    LOG_DEBUG("NodeDBPersistentBackend: Attempting initialization with cache=%u, index=%u", cacheSize, indexSize);
    
    // CRITICAL: Check total memory requirements before initializing any components
    // This prevents partial initialization that could leave system in inconsistent state
    uint32_t indexMemory = indexSize * sizeof(NodeIndexEntry);
    uint32_t cacheMemory = cacheSize * sizeof(NodeCache::CacheEntry);
    uint32_t totalRequiredMemory = indexMemory + cacheMemory;
    uint32_t totalSafetyMargin = MemoryHelpers::SafetyMargins::INDEX + MemoryHelpers::SafetyMargins::CACHE;
    
    auto totalMemCheck = MemoryHelpers::checkMemory(totalRequiredMemory, totalSafetyMargin, "NodeDBPersistentBackend");
    if (!totalMemCheck.sufficient) {
        LOG_ERROR("NodeDBPersistentBackend: Insufficient memory for initialization (need %u bytes + %u margin = %u total, have %u free)", 
                 totalRequiredMemory, totalSafetyMargin, totalMemCheck.totalNeeded, totalMemCheck.freeHeap);
        return false;
    }
    
    LOG_DEBUG("NodeDBPersistentBackend: Memory check passed - have %u bytes free (need %u bytes)", 
             totalMemCheck.freeHeap, totalMemCheck.totalNeeded);
    
    // Step 1: Initialize storage (filesystem) - must be first
    // CRITICAL: Filesystem must be initialized BEFORE any other components
    // This happens early in fsInit_patched() before BLE setup
    // Note: Storage initialization doesn't require memory allocation, so it's safe to call before memory check
    if (!NodeStorage::initialize()) {
        LOG_ERROR("NodeDBPersistentBackend: Failed to initialize storage - filesystem may not be available");
        LOG_ERROR("NodeDBPersistentBackend: Check if main filesystem is properly initialized");
        return false;
    }
    
    // Step 2: Initialize index (depends on filesystem)
    // CRITICAL: Index must be initialized before cache because NodeCache uses NodeIndex functions
    // (e.g., NodeIndex::findNode(), NodeIndex::incrementAccessCount(), etc.)
    // CRITICAL: Memory check was done above, but if allocation fails, we need to handle it gracefully
    if (!NodeIndex::initialize(indexSize)) {
        LOG_ERROR("NodeDBPersistentBackend: Failed to initialize index (size: %u)", indexSize);
        LOG_ERROR("NodeDBPersistentBackend: Memory check passed but index allocation failed - possible memory fragmentation");
        LOG_ERROR("NodeDBPersistentBackend: Free heap: %u bytes, required: %u bytes", 
                 memGet.getFreeHeap(), indexMemory);
        return false;
    }
    
    // Step 3: Initialize cache (depends on both filesystem and index)
    // CRITICAL: Cache uses NodeIndex, so it must be initialized after index
    // CRITICAL: If cache initialization fails, index is already initialized but we can't use it
    // This is acceptable - index will be retried on next initialization attempt
    if (!NodeCache::initialize(cacheSize)) {
        LOG_ERROR("NodeDBPersistentBackend: Failed to initialize cache (size: %u)", cacheSize);
        LOG_ERROR("NodeDBPersistentBackend: Memory check passed but cache allocation failed - possible memory fragmentation");
        LOG_ERROR("NodeDBPersistentBackend: Free heap: %u bytes, required: %u bytes", 
                 memGet.getFreeHeap(), cacheMemory);
        LOG_ERROR("NodeDBPersistentBackend: Index initialized but cache failed - partial initialization state");
        // Cleanup index if cache failed
        // Note: NodeIndex doesn't have cleanup, but that's okay - it will be retried
        return false;
    }
    
    // Initialize background task with context
    NodeDBBackendContext& context = getBackendContext();
    NodeDBBackgroundTask::init(context);
    
    // Note: tick() should be called from main loop()
    
    backend_initialized = true;
    backend_enabled = true;
    
    // CRITICAL: Reset index_loaded flag on initialization to ensure
    // loadFromDisk() will load index from flash on first call
    index_loaded = false;
    
    LOG_INFO("NodeDBPersistentBackend: Initialized successfully (cache: %u, index: %u)", cacheSize, indexSize);
    
    return true;
}

/**
 * @brief Initialize with reduced sizes (graceful degradation)
 * Tries multiple size configurations, automatically reducing if memory is insufficient
 */
bool initializeWithReducedSizes()
{
    if (backend_initialized) {
        return true;
    }
    
    // Maximum slots and index - use helper functions from MemoryHelpers
    
    // Try multiple size configurations, from largest to smallest
    struct SizeConfig {
        uint32_t cache;
        uint32_t index;
        const char* name;
    };
    
    uint32_t maxCache = MemoryHelpers::getMaxNodesCache();
    uint32_t maxIndex = MemoryHelpers::getMaxNodesIndex();
    
    SizeConfig configs[] = {
        {maxCache, maxIndex, "FULL"},           // Attempt 1: Full sizes
        {200, 500, "REDUCED"},                   // Attempt 2: Reduced sizes
        {100, 200, "MINIMAL"},                   // Attempt 3: Minimal sizes
        {50, 100, "BASIC"}                       // Attempt 4: Basic functionality only
    };
    
    for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
        LOG_INFO("NodeDBPersistentBackend: Attempt %u/%u - %s (cache: %u, index: %u)", 
                 (unsigned)(i + 1), (unsigned)(sizeof(configs) / sizeof(configs[0])), configs[i].name, 
                 configs[i].cache, configs[i].index);
        
        if (initializeWithSizes(configs[i].cache, configs[i].index)) {
            if (i > 0) {
                LOG_WARN("NodeDBPersistentBackend: Initialized with REDUCED sizes (%s) due to memory constraints", 
                         configs[i].name);
                LOG_WARN("NodeDBPersistentBackend: Cache: %u nodes, Index: %u nodes (requested: cache=%u, index=%u)", 
                         configs[i].cache, configs[i].index, maxCache, maxIndex);
            }
            return true;
        }
        
        LOG_WARN("NodeDBPersistentBackend: Attempt %u/%u (%s) failed, trying next size...", 
                 (unsigned)(i + 1), (unsigned)(sizeof(configs) / sizeof(configs[0])), configs[i].name);
    }
    
    LOG_ERROR("NodeDBPersistentBackend: All initialization attempts failed - persistent backend disabled");
    return false;
}

bool initialize()
{
    if (backend_initialized) {
        return true;
    }
    
    // Use graceful degradation by default
    return initializeWithReducedSizes();
}

/**
 * @brief Check if virtual backend is enabled (initialized and ready)
 */
bool isEnabled()
{
    return backend_enabled && backend_initialized;
}

/**
 * @brief Check if persistent backend is available (compiled in and can be initialized)
 * This returns true even if backend is not yet initialized (lazy initialization)
 * This allows NodeDB to attempt initialization on first use
 */
bool isAvailable()
{
    // Persistent backend is always available if compiled in
    // It will be initialized on first use (lazy initialization)
    return true;
}

/**
 * @brief Get a node by node number
 * 
 * CRITICAL: This function will attempt to initialize the backend if not already initialized.
 * If initialization fails (e.g., not enough memory), it returns nullptr to prevent
 * falling back to standard implementation which would try to allocate ~247 KB in RAM.
 */
/**
 * @brief Helper function to lazy load index from flash
 * @return true if index was loaded or already loaded, false if load failed
 */
static bool ensureIndexLoaded()
{
    if (index_loaded) {
        return true;
    }
    
    // CRITICAL: Feed watchdog before loading from disk (can take time)
    FEED_WATCHDOG_AND_YIELD();
    
    // Load index from flash (this may take time, but we're already in a node access context)
    if (!loadFromDisk()) {
        LOG_WARN("NodeDBPersistentBackend: Failed to load index from flash, will retry on next access");
        // Don't mark as loaded - allow retry on next access
        return false;
    }
    
    // CRITICAL: Feed watchdog after loading from disk
    FEED_WATCHDOG_AND_YIELD();
    
    index_loaded = true;  // Mark as loaded to prevent repeated loading
    return true;
}

meshtastic_NodeInfoLite* getNode(NodeNum nodeNum)
{
    // CRITICAL: Do NOT initialize here - this can be called from getMeshNode()
    // during device startup. If backend is not initialized, return nullptr
    // to allow standard implementation to be used instead.
    CHECK_BACKEND_ENABLED_NULL();
    
    // CRITICAL: Lazy load index from flash on first node access
    // This prevents blocking device startup or packet processing
    if (!ensureIndexLoaded()) {
        return nullptr;
    }
    
    // Try to get from cache (loads from flash if not cached)
    return NodeCache::getNode(nodeNum);
}

/**
 * @brief Helper function to evict nodes to free space for new node
 * @param nodeNum Node number to add
 * @return true if space was freed or not needed, false if all nodes protected
 */
static bool evictNodesForNewNode(NodeNum nodeNum)
{
    // ========================================================================
    // EVICTION LOGIC: Free space before adding new node
    // ========================================================================
    // Algorithm:
    // 1. If index is full (MAX_NODES_INDEX) → remove oldest node from flash (frees slot and index entry)
    // 2. If flash is full → remove oldest node from flash (frees slot)
    // 3. If cache is full → evict from cache (putNode() handles this automatically)
    // 4. RAM pressure → aggressive cache eviction
    // ========================================================================
    
    // CRITICAL: Check available memory before eviction operations
    // Eviction may trigger memory allocation in removeNode() or writeNodeToSlot()
    uint32_t free_heap = memGet.getFreeHeap();
    if (free_heap < MemoryHelpers::Thresholds::EVICTION) {
        LOG_WARN("NodeDBPersistentBackend: Low memory (%u bytes free, need %u), cannot evict nodes safely", 
                 free_heap, MemoryHelpers::Thresholds::EVICTION);
        // Try aggressive cache eviction first to free memory
        aggressiveCacheEviction(MemoryHelpers::Thresholds::EVICTION, 5);
        // Re-check after cache eviction
        free_heap = memGet.getFreeHeap();
        if (free_heap < MemoryHelpers::Thresholds::EVICTION) {
            LOG_ERROR("NodeDBPersistentBackend: Insufficient memory (%u bytes free) for eviction, cannot add node 0x%x", 
                     free_heap, nodeNum);
            return false;
        }
    }
    
    uint32_t maxIndex = MemoryHelpers::getMaxNodesIndex();
    
    // Step 1: Check if index is full
    if (NodeIndex::getNodeCount() >= maxIndex) {
        LOG_WARN("NodeDBPersistentBackend: Index full (%u/%u nodes), evicting oldest node to make room for 0x%x", 
                 NodeIndex::getNodeCount(), maxIndex, nodeNum);
        NodeNum node_to_evict = NodeIndex::getEvictionCandidate(true);
        if (node_to_evict != 0) {
            LOG_INFO("NodeDBPersistentBackend: Step 1 - Evicting node 0x%x (oldest) to free index entry for 0x%x", 
                    node_to_evict, nodeNum);
            removeNode(node_to_evict);
        } else {
            LOG_ERROR("NodeDBPersistentBackend: All nodes protected, cannot evict. Cannot add node 0x%x (index full: %u/%u)", 
                     nodeNum, NodeIndex::getNodeCount(), maxIndex);
            return false;
        }
    }
    
    // Step 2: Check if flash is full
    if (getFreeFlashSlots() == 0) {
        LOG_WARN("NodeDBPersistentBackend: Flash full (%u slots used), evicting oldest node to make room for 0x%x", 
                 NodeStorage::getSlotCount(), nodeNum);
        NodeNum node_to_evict = NodeIndex::getEvictionCandidate(true);
        if (node_to_evict != 0) {
            LOG_INFO("NodeDBPersistentBackend: Step 2 - Evicting node 0x%x (oldest) from flash to free slot for 0x%x", 
                    node_to_evict, nodeNum);
            removeNode(node_to_evict);
        } else {
            LOG_ERROR("NodeDBPersistentBackend: All nodes protected, cannot evict. Cannot add node 0x%x", nodeNum);
            return false;
        }
    }
    
    // Step 3: Check if cache is full
    if (NodeCache::getCacheSize() >= NodeCache::getMaxCacheSize()) {
        LOG_DEBUG("NodeDBPersistentBackend: Step 3 - Cache full (%u/%u), pre-evicting to make room for node 0x%x", 
                 NodeCache::getCacheSize(), NodeCache::getMaxCacheSize(), nodeNum);
        NodeNum node_to_evict_from_cache = NodeIndex::getEvictionCandidate(false);
        if (node_to_evict_from_cache != 0) {
            NodeCache::evictNode(node_to_evict_from_cache);
        }
    }
    
    // Step 4: RAM pressure handling
    if (memGet.getFreeHeap() < MemoryHelpers::Thresholds::SAFE_FREE_HEAP) {
        LOG_WARN("NodeDBPersistentBackend: Low memory (%u bytes free), aggressive cache eviction", 
                 memGet.getFreeHeap());
        aggressiveCacheEviction(MemoryHelpers::Thresholds::SAFE_FREE_HEAP, 10);
    }
    
    return true;
}

/**
 * @brief Helper function to rollback node creation on error
 * @param nodeNum Node number
 * @param slotId Slot ID (UINT16_MAX if not allocated)
 * @param addedToIndex true if added to index
 */
static void rollbackNodeCreation(NodeNum nodeNum, uint16_t slotId, bool addedToIndex)
{
    if (addedToIndex) {
        NodeIndex::removeNode(nodeNum);
    }
    if (slotId != UINT16_MAX) {
        NodeStorage::deleteSlot(slotId);
    }
}

/**
 * @brief Get or create a node
 */
meshtastic_NodeInfoLite* getOrCreateNode(NodeNum nodeNum)
{
    // CRITICAL: Do NOT initialize here - initialization must happen explicitly
    // when first network node is accessed, not during device startup.
    // This prevents blocking device startup with filesystem operations.
    CHECK_BACKEND_ENABLED_NULL();
    
    // CRITICAL: Lazy load index from flash on first node access
    // This prevents blocking device startup or packet processing
    if (!ensureIndexLoaded()) {
        // But still try to create node (may work if index is not critical)
    }
    
    // Check if node exists
    meshtastic_NodeInfoLite* node = getNode(nodeNum);
    if (node) {
        return node;
    }
    
    // ========================================================================
    // EVICTION LOGIC: Free space before adding new node
    // ========================================================================
    if (!evictNodesForNewNode(nodeNum)) {
        return nullptr;
    }
    
    // ========================================================================
    // ADD NEW NODE: Allocate resources and add to all structures
    // ========================================================================
    
    // Step 4: Allocate flash slot for new node
    uint16_t slotId = NodeStorage::allocateSlot(nodeNum);
    if (slotId == UINT16_MAX) {
        LOG_ERROR("NodeDBPersistentBackend: Failed to allocate slot for node 0x%x", nodeNum);
        return nullptr;
    }
    
    // Step 5: Add to index
    bool addedToIndex = NodeIndex::addNode(nodeNum, slotId);
    if (!addedToIndex) {
        LOG_ERROR("NodeDBPersistentBackend: Failed to add node 0x%x to index", nodeNum);
        rollbackNodeCreation(nodeNum, slotId, false);
        return nullptr;
    }
    
    // Step 6: Create new node structure
    meshtastic_NodeInfoLite newNode = {};
    newNode.num = nodeNum;
    
    // Step 7: Add to cache (putNode() will evict from cache if needed)
    if (!NodeCache::putNode(nodeNum, &newNode)) {
        LOG_ERROR("NodeDBPersistentBackend: Failed to add node 0x%x to cache (cache may be full with protected nodes)", nodeNum);
        rollbackNodeCreation(nodeNum, slotId, true);
        return nullptr;
    }
    
    // CRITICAL: Check write queue capacity before marking dirty
    // If queue is almost full, process some writes first
    if (NodeDBBackgroundTask::isWriteQueueInitialized()) {
        uint32_t queue_size = NodeDBBackgroundTask::getWriteQueueSize();
        uint32_t max_queue = NodeDBBackgroundTask::getWriteQueueMaxSize();
        if (max_queue > 0 && queue_size * 100 / max_queue > 90) {
            // Queue is >90% full - process writes to free space
            // Use force=false here (normal operation, not critical overflow)
            LOG_DEBUG("NodeDBPersistentBackend: Write queue almost full (%u/%u), processing writes...", 
                     queue_size, max_queue);
            NodeDBBackgroundTask::processWriteQueue(5, false);  // false = respect radio state
        }
    }
    
    // Mark as dirty (needs initial flash write)
    NodeCache::markDirty(nodeNum);
    
    // CRITICAL: Feed watchdog after creating new node (may trigger writeNodeToSlot later)
    FEED_WATCHDOG_AND_YIELD();
    
    // Get from cache
    return getNode(nodeNum);
}

/**
 * @brief Update a node from a mesh packet
 */
bool updateFromPacket(NodeNum nodeNum, const meshtastic_MeshPacket* packet)
{
    // CRITICAL: Do NOT initialize here - backend must be initialized before use
    if (!isEnabled() || !packet) {
        return false;
    }
    
    // CRITICAL: Feed watchdog before getOrCreateNode (may trigger filesystem operations)
    FEED_WATCHDOG_AND_YIELD();
    
    // Get or create node
    // CRITICAL: This may trigger loadFromDisk() if index not loaded, or writeNodeToSlot() if node is new
    meshtastic_NodeInfoLite* node = getOrCreateNode(nodeNum);
    
    // CRITICAL: Feed watchdog after getOrCreateNode (may have triggered filesystem operations)
    FEED_WATCHDOG_AND_YIELD();
    
    if (!node) {
        return false;
    }
    
    // Update hot data (in RAM only, never written to flash)
    // These fields update frequently (every packet or during routing) and are NEVER written to flash
    if (packet->rx_time) {
        node->last_heard = packet->rx_time;
        NodeIndex::updateLastHeard(nodeNum, packet->rx_time);
    }
    
    if (packet->rx_snr) {
        node->snr = packet->rx_snr;  // Hot data - never written to flash
    }
    
    node->via_mqtt = packet->via_mqtt;  // Hot data - never written to flash
    
    // Update hops_away (hot data)
    if (packet->hop_start != 0 && packet->hop_limit <= packet->hop_start) {
        node->has_hops_away = true;
        node->hops_away = packet->hop_start - packet->hop_limit;
    }
    
    // Note: next_hop is updated in NextHopRouter::sniffReceived() and is also hot data
    // It's stored in RAM cache and never written to flash
    
    // Note: Hot data updates (last_heard, snr, hops_away, via_mqtt, next_hop) are NOT marked as dirty
    // They are only in RAM and never written to flash to prevent wear
    
    return true;
}

/**
 * @brief Update user information for a node
 */
bool updateUser(NodeNum nodeNum, const meshtastic_User* user)
{
    // CRITICAL: Do NOT initialize here - backend must be initialized before use
    if (!isEnabled() || !user) {
        return false;
    }
    
    // Get or create node
    meshtastic_NodeInfoLite* node = getOrCreateNode(nodeNum);
    if (!node) {
        return false;
    }
    
    // Update cold data (user info - stored in flash)
    // Note: NodeInfoLite uses UserLite, but updateUser receives User
    // Use TypeConversions helper to convert User to UserLite
    node->has_user = true;
    node->user = TypeConversions::ConvertToUserLite(*user);
    
    // Mark as dirty (needs flash write)
    NodeCache::markDirty(nodeNum);
    
    // Update index flags
    NodeIndexEntry* index_entry = NodeIndex::findNode(nodeNum);
    if (index_entry) {
        index_entry->flags |= NodeIndexEntry::FLAG_HAS_USER;
    }
    
    return true;
}

/**
 * @brief Remove a node
 */
bool removeNode(NodeNum nodeNum)
{
    // CRITICAL: Do NOT initialize here - backend must be initialized before use
    CHECK_BACKEND_ENABLED();
    
    // Get index entry
    NodeIndexEntry* index_entry = NodeIndex::findNode(nodeNum);
    if (!index_entry) {
        return false;  // Node doesn't exist
    }
    
    // Evict from cache
    NodeCache::evictNode(nodeNum);
    
    // Delete from flash
    NodeStorage::deleteSlot(index_entry->slot_id);
    
    // Remove from index
    NodeIndex::removeNode(nodeNum);
    
    return true;
}

/**
 * @brief Reset all nodes (clear cache, index, and storage)
 */
bool resetNodes()
{
    // CRITICAL: Do NOT initialize here - backend must be initialized before use
    CHECK_BACKEND_ENABLED();
    
    LOG_INFO("NodeDBPersistentBackend: Resetting all nodes...");
    
    // Get all node numbers from index before clearing
    uint32_t maxIndex = MemoryHelpers::getMaxNodesIndex();
    uint32_t allocation_size = maxIndex * sizeof(NodeNum);
    
    // CRITICAL: Check memory before allocation
    uint32_t requiredMemory = allocation_size;
    uint32_t safetyMargin = 4096;  // 4 KB safety margin
    auto memCheck = MemoryHelpers::checkMemory(requiredMemory, safetyMargin, "NodeDBPersistentBackend::resetNodes");
    if (!memCheck.sufficient) {
        LOG_ERROR("NodeDBPersistentBackend: Insufficient memory for reset (need %u bytes, have %u free)", 
                 memCheck.totalNeeded, memCheck.freeHeap);
        return false;
    }
    
    #ifdef ARCH_NRF52
    NodeNum* node_nums = (NodeNum*)rtos_malloc(allocation_size);
    #else
    NodeNum* node_nums = new NodeNum[maxIndex];
    #endif
    if (!node_nums) {
        uint32_t freeHeapAfter = memGet.getFreeHeap();
        MemoryHelpers::logAllocationFailure("NodeDBPersistentBackend::resetNodes", allocation_size, sizeof(NodeNum), 
                                           maxIndex, memCheck.freeHeap, freeHeapAfter);
        return false;
    }
    uint32_t node_count = NodeIndex::getAllNodeNums(node_nums, maxIndex);
    
    // CRITICAL: Validate node_count doesn't exceed maxIndex
    if (node_count > maxIndex) {
        LOG_ERROR("NodeDBPersistentBackend: node_count (%u) exceeds maxIndex (%u) in resetNodes", node_count, maxIndex);
        node_count = maxIndex;  // Clamp to prevent buffer overflow
    }
    
    // Delete all slots from storage
    uint32_t deleted_count = 0;
    for (uint32_t i = 0; i < node_count; i++) {
        // CRITICAL: Check bounds before accessing node_nums
        if (i >= maxIndex) {
            LOG_ERROR("NodeDBPersistentBackend: node_nums index out of bounds (i: %u, max: %u)", i, maxIndex);
            break;
        }
        
        NodeIndexEntry* index_entry = NodeIndex::findNode(node_nums[i]);
        if (index_entry) {
            NodeStorage::deleteSlot(index_entry->slot_id);
            deleted_count++;
        }
    }
    
    // Clear cache (writes dirty entries to flash first)
    NodeCache::clear();
    
    // Clear index
    NodeIndex::clear();
    
    freeMemory(node_nums);
    
    LOG_INFO("NodeDBPersistentBackend: All nodes reset (deleted %u nodes)", deleted_count);
    
    return true;
}

/**
 * @brief Check if node database is full
 */
bool isFull()
{
    CHECK_BACKEND_ENABLED();
    
    // Check cache size
    if (NodeCache::getCacheSize() >= NodeCache::getMaxCacheSize()) {
        return true;
    }
    
    // Check flash capacity
    if (NodeStorage::getFreeSlotCount() == 0) {
        return true;
    }
    
    // Check index capacity
    uint32_t maxIndex = MemoryHelpers::getMaxNodesIndex();
    if (NodeIndex::getNodeCount() >= maxIndex) {
        return true;
    }
    
    return false;
}

/**
 * @brief Get total number of nodes
 */
size_t getTotalNodeCount()
{
    // CRITICAL: Do NOT initialize here - this can be called from /mem command
    // which should be fast. If backend is not initialized, return 0.
    // Initialization should happen on first actual node access, not on stats query.
    CHECK_BACKEND_ENABLED_ZERO();
    
    // CRITICAL: Safe call - NodeIndex::getNodeCount() checks initialization internally
    return NodeIndex::getNodeCount();
}

/**
 * @brief Get number of cached nodes
 */
size_t getCachedNodeCount()
{
    // CRITICAL: Check backend state before accessing cache
    if (!backend_initialized || !backend_enabled) {
        return 0;
    }
    
    // CRITICAL: Safe call - NodeCache::getCacheSize() checks initialization internally
    return NodeCache::getCacheSize();
}

/**
 * @brief Get number of free flash slots
 */
uint32_t getFreeFlashSlots()
{
    // CRITICAL: Check backend state before accessing storage
    if (!backend_initialized || !backend_enabled) {
        return 0;
    }
    
    // CRITICAL: Safe call - NodeStorage::getFreeSlotCount() uses NodeIndex::getNodeCount()
    // which checks initialization internally
    return NodeStorage::getFreeSlotCount();
}

/**
 * @brief Helper function to scan directory and rebuild index from slot files
 * @return Number of slots found and added to index
 */
static uint32_t scanDirectoryAndRebuildIndex()
{
    uint32_t slots_found = 0;
    
    LOG_INFO("NodeDBPersistentBackend: Scanning %s directory to rebuild index...", NodeStorage::SLOTS_DIR);
    
    FEED_WATCHDOG_AND_YIELD();
    lfs_t* main_lfs = getMainFS();
    FEED_WATCHDOG_AND_YIELD();
    
    if (!main_lfs) {
        LOG_ERROR("NodeDBPersistentBackend: Main filesystem not available for directory scan");
        return 0;
    }
    
    // Open /prefs/nodes directory
    lfs_dir_t dir;
    int dir_result = lfs_dir_open(main_lfs, &dir, NodeStorage::SLOTS_DIR);
    if (dir_result != LFS_ERR_OK) {
        LOG_WARN("NodeDBPersistentBackend: Failed to open %s directory (error: %d), falling back to slot-by-slot scan", NodeStorage::SLOTS_DIR, dir_result);
        // Fallback to slow method
        uint32_t maxSlots = MemoryHelpers::getMaxNodesSlots();
        uint32_t MAX_SLOT_ID = (maxSlots > 0) ? (maxSlots - 1) : 0;
        constexpr uint32_t YIELD_INTERVAL = 50;
        for (uint16_t slot_id = 0; slot_id <= MAX_SLOT_ID; slot_id++) {
            if (slot_id % YIELD_INTERVAL == 0) {
                FEED_WATCHDOG_AND_YIELD();
            }
            
            if (NodeStorage::slotExists(slot_id)) {
                meshtastic_NodeInfoLite node;
                // CRITICAL: Check result of readNodeFromSlot() - if it fails, log error but continue
                if (NodeStorage::readNodeFromSlot(slot_id, &node)) {
                    // CRITICAL: Check result of addNode() - if it fails, log error but continue
                    if (NodeIndex::addNode(node.num, slot_id)) {
                        slots_found++;
                    } else {
                        LOG_WARN("NodeDBPersistentBackend: Failed to add node from slot %u to index (index may be full)", slot_id);
                    }
                } else {
                    LOG_WARN("NodeDBPersistentBackend: Failed to read node from slot %u (file may be corrupted)", slot_id);
                }
            }
        }
        return slots_found;
    }
    
    // Read directory entries
    struct lfs_info info;
    uint32_t entries_processed = 0;
    constexpr uint32_t YIELD_INTERVAL = 10;
    
    while (true) {
        int read_result = lfs_dir_read(main_lfs, &dir, &info);
        if (read_result <= 0) {
            break;
        }
        
        entries_processed++;
        
        if (entries_processed % YIELD_INTERVAL == 0) {
            FEED_WATCHDOG_AND_YIELD();
        }
        
        // Check if this is a slot file (format: "slot_XXXX.bin")
        if (info.type == LFS_TYPE_REG && strncmp(info.name, "slot_", 5) == 0) {
            uint16_t slot_id = 0;
            if (sscanf(info.name, "slot_%hu.bin", &slot_id) == 1) {
                FEED_WATCHDOG_AND_YIELD();
                
                meshtastic_NodeInfoLite node;
                // CRITICAL: Check result of readNodeFromSlot() - if it fails, log error but continue
                if (NodeStorage::readNodeFromSlot(slot_id, &node)) {
                    // CRITICAL: Check result of addNode() - if it fails, log error but continue
                    if (NodeIndex::addNode(node.num, slot_id)) {
                        slots_found++;
                    } else {
                        LOG_WARN("NodeDBPersistentBackend: Failed to add node 0x%x (slot %u) to index (index may be full)", 
                                 node.num, slot_id);
                    }
                } else {
                    LOG_WARN("NodeDBPersistentBackend: Failed to read node from slot %u (file may be corrupted)", slot_id);
                }
                
                FEED_WATCHDOG_AND_YIELD();
            }
        }
    }
    
    // CRITICAL: Check result of directory close operation
    int close_result = lfs_dir_close(main_lfs, &dir);
    if (close_result != LFS_ERR_OK) {
        LOG_WARN("NodeDBPersistentBackend: Failed to close %s directory (error: %d)", NodeStorage::SLOTS_DIR, close_result);
        // Continue anyway - directory scan is complete
    }
    
    LOG_DEBUG("NodeDBPersistentBackend: Directory scan complete - found %u slot files", slots_found);
    return slots_found;
}

/**
 * @brief Load all nodes from flash (on startup)
 */
bool loadFromDisk()
{
    // CRITICAL: Do NOT initialize here - initialization must happen explicitly
    // before calling this function. This prevents blocking device startup.
    CHECK_BACKEND_ENABLED();
    
    // If already loaded, return success immediately
    if (index_loaded) {
        return true;
    }
    
    // If incremental load is in progress, continue from where we left off
    // The incremental loading logic is handled below via incremental_load_state
    // Background task will call loadFromDisk() repeatedly until it returns true
    
    // CRITICAL: Feed watchdog at the start of loadFromDisk()
    FEED_WATCHDOG_AND_YIELD();
    
    // CRITICAL: If incremental load is already in progress, continue from where we left off
    // Don't reset state if we're already loading (prevents infinite loop)
    if (incremental_load_state.state == IncrementalLoadState::IDLE) {
        LOG_INFO("NodeDBPersistentBackend: Starting incremental load from flash...");
        // Initialize incremental load state only if starting fresh
        incremental_load_state = IncrementalLoadState();
        incremental_load_state.state = IncrementalLoadState::SCANNING_DIR;
    } else {
        LOG_DEBUG("NodeDBPersistentBackend: Continuing incremental load from flash (state: %d)...", 
                 incremental_load_state.state);
    }
    
    // CRITICAL: Try to load index from flash first (preserves last_heard timestamps)
    // This is essential for:
    // 1. Preloading cache with most active nodes (based on last_heard)
    // 2. Evicting oldest nodes when flash is full (based on last_heard)
    LOG_DEBUG("NodeDBPersistentBackend: Attempting to load index from flash...");
    bool index_loaded_from_flash = NodeIndex::loadFromFlash();
    
    // CRITICAL: Feed watchdog after index load attempt
    FEED_WATCHDOG_AND_YIELD();
    
    // CRITICAL: Check if index load failed - if so, we need to rebuild from slots
    // Don't set index_loaded = true until we successfully rebuild or validate index
    if (index_loaded_from_flash) {
        uint32_t loaded_count = NodeIndex::getNodeCount();
        LOG_INFO("NodeDBPersistentBackend: Successfully loaded index from flash (%u nodes)", loaded_count);
    } else {
        LOG_WARN("NodeDBPersistentBackend: Index file not found or load failed - will rebuild from slots");
        LOG_DEBUG("NodeDBPersistentBackend: This is normal on first boot or after filesystem format");
    }
    
    // Check if index is empty (first boot or after reset, or load failed)
    uint32_t index_count = NodeIndex::getNodeCount();
    
    if (index_count == 0) {
        // Index is empty - rebuild from slot files
        // Note: last_heard will be 0 for all nodes (lost on reboot)
        // This is acceptable for first boot, but subsequent boots should load from flash
        LOG_INFO("NodeDBPersistentBackend: Index is empty, rebuilding from slot files...");
        
        // OPTIMIZED: Scan directory instead of checking each slot individually
        uint32_t slots_found = scanDirectoryAndRebuildIndex();
        LOG_INFO("NodeDBPersistentBackend: Rebuilt index from %u slot files", slots_found);
        
        // CRITICAL: Verify that index rebuild was successful
        // If no slots were found and index is still empty, this is OK (first boot, no nodes yet)
        // Don't treat this as an error - mark index as loaded so we don't retry infinitely
        uint32_t final_index_count = NodeIndex::getNodeCount();
        if (final_index_count == 0 && slots_found == 0) {
            LOG_INFO("NodeDBPersistentBackend: Index is empty (first boot, no nodes in flash yet) - this is normal");
            // Mark as loaded even though empty - this prevents infinite retry loop
            // Index will be populated as nodes are received from network
            index_loaded = true;
            return true;  // Success - empty index is valid state
        }
    } else {
        // Index already has entries - validate against slot files
        uint32_t slot_count = NodeStorage::getSlotCount();
        
        if (slot_count != index_count) {
            LOG_WARN("NodeDBPersistentBackend: Index mismatch (slots: %u, index: %u), rebuilding index", 
                     slot_count, index_count);
            // Rebuild index from slot files
            NodeIndex::clear();
            
            // OPTIMIZED: Scan directory instead of checking each slot individually
            uint32_t slots_found = scanDirectoryAndRebuildIndex();
            
            LOG_INFO("NodeDBPersistentBackend: Rebuilt index from %u slot files", slots_found);
            
            // CRITICAL: Verify that index rebuild was successful
            // If no slots were found and index is still empty, this is OK (first boot, no nodes yet)
            uint32_t final_index_count = NodeIndex::getNodeCount();
            if (final_index_count == 0 && slots_found == 0) {
                LOG_INFO("NodeDBPersistentBackend: Index is empty during validation (first boot, no nodes in flash yet) - this is normal");
                // Mark as loaded even though empty - this prevents infinite retry loop
                index_loaded = true;
                return true;  // Success - empty index is valid state
            }
        } else {
            LOG_INFO("NodeDBPersistentBackend: Index valid (%u nodes)", index_count);
        }
    }
    
    // ========================================================================
    // PRELOAD CACHE: Load important nodes into cache for fast access
    // ========================================================================
    // Strategy:
    // 1. Load all protected nodes (local node, routers, favorites) - always in cache
    // 2. Load N most recent nodes (by last_heard) until cache is 50-70% full
    // This provides fast access to important and active nodes immediately after reboot
    
    uint32_t total_nodes = NodeIndex::getNodeCount();
    if (total_nodes == 0) {
        LOG_INFO("NodeDBPersistentBackend: No nodes to preload");
        return true;
    }
    
    LOG_INFO("NodeDBPersistentBackend: Preloading cache (%u nodes in index)...", total_nodes);
    
    uint32_t preloaded_count = 0;
    uint32_t protected_count = 0;
    size_t max_cache_size = NodeCache::getMaxCacheSize();
    // CRITICAL: Reduce preload target from 60% to 30% to prevent heap exhaustion
    // 30% of 300 = 90 nodes (safe for memory-constrained systems)
    // Original 60% (180 nodes) was too aggressive and caused heap overflow
    size_t target_preload = (max_cache_size * 30) / 100;  // Preload up to 30% of cache
    
    // Step 1: Load all protected nodes (local, routers, favorites)
    // Read directly from flash and add to cache via putNode() for better control
    for (uint32_t i = 0; i < total_nodes; i++) {
        // CRITICAL: Yield periodically to prevent watchdog timeout
        if (i % 10 == 0) {
            FEED_WATCHDOG_AND_YIELD();
        }
        
        NodeNum nodeNum = NodeIndex::getNodeNumByIndex(i);
        if (nodeNum == 0) {
            continue;  // Invalid node number
        }
        
        NodeIndexEntry* entry = NodeIndex::findNode(nodeNum);
        if (entry && (entry->flags & NodeIndexEntry::FLAG_PROTECTED)) {
            // Protected node - load from flash and add to cache
            meshtastic_NodeInfoLite node;
            if (NodeStorage::readNodeFromSlot(entry->slot_id, &node)) {
                if (NodeCache::putNode(nodeNum, &node)) {
                    // Mark as cached in index
                    NodeIndex::markCached(nodeNum, true);
                    protected_count++;
                    preloaded_count++;
                } else {
                    LOG_DEBUG("NodeDBPersistentBackend: Failed to add protected node 0x%x to cache", nodeNum);
                }
            } else {
                LOG_DEBUG("NodeDBPersistentBackend: Failed to read protected node 0x%x from slot %u", nodeNum, entry->slot_id);
            }
            
            // CRITICAL: Feed watchdog after reading each node (readNodeFromSlot can be slow)
            FEED_WATCHDOG_AND_YIELD();
        }
    }
    
    LOG_INFO("NodeDBPersistentBackend: Preloaded %u protected nodes", protected_count);
    
    // If no protected nodes were found, load first N nodes anyway (they may not be marked as protected yet)
    if (protected_count == 0 && total_nodes > 0) {
        LOG_DEBUG("NodeDBPersistentBackend: No protected nodes found, loading first nodes from index...");
        // Load first nodes to ensure cache has some data (up to 50 or target_preload)
        uint32_t initial_load_count = (target_preload < 50) ? target_preload : 50;
        for (uint32_t i = 0; i < total_nodes && i < initial_load_count && preloaded_count < target_preload; i++) {
            // CRITICAL: Yield periodically to prevent watchdog timeout
            if (i % 5 == 0) {
                FEED_WATCHDOG_AND_YIELD();
            }
            
            NodeNum nodeNum = NodeIndex::getNodeNumByIndex(i);
            if (nodeNum == 0) {
                continue;
            }
            
            NodeIndexEntry* entry = NodeIndex::findNode(nodeNum);
            if (!entry) {
                continue;
            }
            
            // CRITICAL: Check available memory before reading node (prevents heap overflow)
            uint32_t free_heap = memGet.getFreeHeap();
            if (free_heap < MemoryHelpers::Thresholds::NODE_READ) {
                LOG_WARN("NodeDBPersistentBackend: Low memory (%u bytes free), stopping protected nodes preload", free_heap);
                break;  // Stop loading protected nodes if memory is low
            }
            
            meshtastic_NodeInfoLite node;
            if (NodeStorage::readNodeFromSlot(entry->slot_id, &node)) {
                // Validate node integrity before adding to cache
                if (node.num == 0 || node.num != nodeNum) {
                    LOG_WARN("NodeDBPersistentBackend: Node integrity check failed for slot %u (expected 0x%x, got 0x%x)", 
                             entry->slot_id, nodeNum, node.num);
                    continue;
                }
                
                if (NodeCache::putNode(nodeNum, &node)) {
                    NodeIndex::markCached(nodeNum, true);
                    preloaded_count++;
                    protected_count++;  // Count as initial load
                    LOG_DEBUG("NodeDBPersistentBackend: Preloaded node 0x%x from slot %u", nodeNum, entry->slot_id);
                } else {
                    LOG_WARN("NodeDBPersistentBackend: Failed to add node 0x%x to cache (cache may be full)", nodeNum);
                }
            } else {
                LOG_WARN("NodeDBPersistentBackend: Failed to read node from slot %u", entry->slot_id);
            }
            
            // CRITICAL: Feed watchdog after reading each node (readNodeFromSlot can be slow)
            FEED_WATCHDOG_AND_YIELD();
        }
        LOG_INFO("NodeDBPersistentBackend: Loaded %u initial nodes", protected_count);
        
        // CRITICAL: Log memory status after loading initial nodes
        uint32_t free_heap_after_initial = memGet.getFreeHeap();
        uint32_t heap_total = memGet.getHeapSize();
        uint32_t used_heap_after_initial = (heap_total > free_heap_after_initial) ? (heap_total - free_heap_after_initial) : 0;
        // Reduced logging to prevent buffer overflow
        
        // CRITICAL: Feed watchdog and add delay after loading initial nodes
        // This prevents watchdog timeout when transitioning to next phase
        FEED_WATCHDOG_AND_YIELD();
        delay(20);  // Small delay to ensure watchdog is fed
    }
    
    // Step 2: Load most recent nodes (by last_heard from NodeIndex) until target is reached
    // CRITICAL: Use last_heard from NodeIndex (persisted on flash), not from NodeInfoLite (always 0 on flash)
    uint32_t recent_count = 0;
    
    // CRITICAL: Feed watchdog before starting next phase
    FEED_WATCHDOG_AND_YIELD();
    
    // Initialize incremental load state if starting fresh
    if (incremental_load_state.state == IncrementalLoadState::IDLE) {
        incremental_load_state.current_index = 0;
        incremental_load_state.total_nodes = total_nodes;
        incremental_load_state.preload_target = target_preload;
        incremental_load_state.preload_count = preloaded_count;
        incremental_load_state.sort_count = 0;
        incremental_load_state.sort_processed = 0;
        incremental_load_state.state = IncrementalLoadState::PRELOADING;
    } else {
        // CRITICAL: Update preload_count from current state (may have changed)
        preloaded_count = incremental_load_state.preload_count;
        
        // CRITICAL: If target already reached, mark as complete immediately
        if (preloaded_count >= incremental_load_state.preload_target) {
            LOG_INFO("NodeDBPersistentBackend: Preload target already reached (%u/%u nodes), marking as complete", 
                    preloaded_count, incremental_load_state.preload_target);
            index_loaded = true;
            incremental_load_state.state = IncrementalLoadState::IDLE;
            return true;
        }
    }
    
    // CRITICAL: Feed watchdog before time checks (getRTCQuality can be slow)
    FEED_WATCHDOG_AND_YIELD();
    
    // Check if time is valid (not RTCQualityNone)
    // If time is not valid, getTime() returns time since boot, not Unix time
    // In this case, we'll use access_count and relative ordering instead
    RTCQuality time_quality = getRTCQuality();
    uint32_t now = getValidTime(RTCQualityDevice, false);  // Returns 0 if time not valid
    bool time_valid = (time_quality != RTCQualityNone && now > 0);
    
    // Store in state for incremental processing
    incremental_load_state.time_valid = time_valid;
    incremental_load_state.now = now;
    
    // CRITICAL: Feed watchdog after time checks and before logging
    FEED_WATCHDOG_AND_YIELD();
    
    // CRITICAL: Log memory status before continuing
    uint32_t free_heap_before_continue = memGet.getFreeHeap();
    uint32_t heap_total_before_continue = memGet.getHeapSize();
    uint32_t used_heap_before_continue = (heap_total_before_continue > free_heap_before_continue) ? (heap_total_before_continue - free_heap_before_continue) : 0;
    // Reduced logging to prevent buffer overflow
    
    LOG_DEBUG("NodeDBPersistentBackend: Continuing to load nodes until cache target (%u/%u)...", preloaded_count, target_preload);
    LOG_DEBUG("NodeDBPersistentBackend: Time quality: %d, now: %u, time_valid: %s", 
              time_quality, now, time_valid ? "yes" : "no");
    
    // CRITICAL: Define memory thresholds before use
    // Use MemoryHelpers::Thresholds::COLLECTION instead of local constant
    
    // CRITICAL: Feed watchdog immediately after logging (before any checks)
    FEED_WATCHDOG_AND_YIELD();
    delay(20);  // Increased delay to ensure watchdog is fed and system is stable
    
    // CRITICAL: Check memory again after time checks (memory might have changed)
    uint32_t free_heap_after_time = memGet.getFreeHeap();
    uint32_t heap_total_after_time = memGet.getHeapSize();
    uint32_t used_heap_after_time = (heap_total_after_time > free_heap_after_time) ? (heap_total_after_time - free_heap_after_time) : 0;
    // Reduced logging to prevent buffer overflow
    
    if (free_heap_after_time < MemoryHelpers::Thresholds::COLLECTION) {
        LOG_WARN("NodeDBPersistentBackend: Low memory after time check (%u bytes free, need %u), stopping preload", 
                 free_heap_after_time, MemoryHelpers::Thresholds::COLLECTION);
        if (preloaded_count >= 20) {
            index_loaded = true;
            incremental_load_state.state = IncrementalLoadState::IDLE;
            LOG_INFO("NodeDBPersistentBackend: Preload stopped due to low memory (%u nodes loaded)", preloaded_count);
            return true;
        }
        return false;
    }
    
    // CRITICAL: If we've already loaded all available nodes, mark as complete
    if (preloaded_count >= total_nodes) {
        LOG_DEBUG("NodeDBPersistentBackend: All available nodes already loaded (%u/%u)", preloaded_count, total_nodes);
        index_loaded = true;
        incremental_load_state.state = IncrementalLoadState::IDLE;
        return true;
    }
    
    // CRITICAL: If target is already reached, mark as complete
    if (preloaded_count >= target_preload) {
        LOG_DEBUG("NodeDBPersistentBackend: Target preload reached (%u/%u)", preloaded_count, target_preload);
        index_loaded = true;
        incremental_load_state.state = IncrementalLoadState::IDLE;
        return true;
    }
    
    // CRITICAL: Feed watchdog before starting collection
    FEED_WATCHDOG_AND_YIELD();
    delay(10);  // Small delay to ensure watchdog is fed
    
    uint32_t operation_start = millis();  // Track time for this chunk
    
    // Use static buffer from incremental_load_state (replaces stack allocation)
    // This prevents stack overflow and allows incremental processing
    using NodeWithPriority = IncrementalLoadState::NodeWithPriority;
    NodeWithPriority* sort_buffer = incremental_load_state.sort_buffer;
    constexpr uint32_t MAX_SORT_NODES = IncrementalLoadState::MAX_SORT_NODES;
    uint32_t& sort_count = incremental_load_state.sort_count;
    sort_count = 0;
    
    // Collect nodes with their priority from NodeIndex
    // Process in chunks to avoid blocking - start from where we left off
    uint32_t start_index = incremental_load_state.current_index;
    uint32_t chunk_size = 15;  // Process 15 nodes at a time (further reduced to prevent timeout)
    uint32_t end_index = (start_index + chunk_size < total_nodes) ? (start_index + chunk_size) : total_nodes;
    
    // CRITICAL: Check available memory before starting collection loop
    uint32_t free_heap_before = memGet.getFreeHeap();
    uint32_t heap_total_before = memGet.getHeapSize();
    uint32_t used_heap_before = (heap_total_before > free_heap_before) ? (heap_total_before - free_heap_before) : 0;
    // Reduced logging to prevent buffer overflow
    
    if (free_heap_before < MemoryHelpers::Thresholds::COLLECTION) {
        LOG_WARN("NodeDBPersistentBackend: Low memory (%u bytes free, need %u), skipping collection phase to prevent heap overflow", 
                 free_heap_before, MemoryHelpers::Thresholds::COLLECTION);
        // Mark as complete with current count to prevent infinite retry
        if (preloaded_count >= 20) {  // At least 20 nodes loaded
            index_loaded = true;
            incremental_load_state.state = IncrementalLoadState::IDLE;
            LOG_INFO("NodeDBPersistentBackend: Preload stopped due to low memory (%u nodes loaded)", preloaded_count);
            return true;  // Success with partial load
        }
        return false;  // Will retry later when memory is available
    }
    
    // CRITICAL: Feed watchdog before starting loop
    FEED_WATCHDOG_AND_YIELD();
    
    LOG_DEBUG("NodeDBPersistentBackend: Starting collection loop (start_index: %u, end_index: %u, total_nodes: %u, preloaded: %u)", 
             start_index, end_index, total_nodes, preloaded_count);
    
    for (uint32_t i = start_index; i < end_index && sort_count < MAX_SORT_NODES; i++) {
        // CRITICAL: Feed watchdog every node during collection (very frequent to prevent timeout)
        FEED_WATCHDOG_AND_YIELD();
        delay(5); // Small delay for stability
        
        // CRITICAL: Check memory every 5 nodes during collection (reduced logging)
        if ((i - start_index) % 5 == 0) {
            uint32_t free_heap_during = memGet.getFreeHeap();
            
            if (free_heap_during < MemoryHelpers::Thresholds::NODE_READ) {
                LOG_WARN("NodeDBPersistentBackend: Low memory during collection (%u bytes free, need %u), stopping", 
                         free_heap_during, MemoryHelpers::Thresholds::NODE_READ);
                incremental_load_state.current_index = i;  // Save progress
                incremental_load_state.sort_count = sort_count;  // Save collected nodes
                return false;  // Will retry later
            }
        }
        
        NodeNum nodeNum = NodeIndex::getNodeNumByIndex(i);
        if (nodeNum == 0) {
            continue;
        }
        
        // Skip if already cached (but log for debugging)
        if (NodeCache::isCached(nodeNum)) {
            // LOG_DEBUG("NodeDBPersistentBackend: Node 0x%x already in cache, skipping", nodeNum);
            continue;
        }
        
        NodeIndexEntry* entry = NodeIndex::findNode(nodeNum);
        if (!entry) {
            continue;
        }
        
        // Get last_heard and access_count from NodeIndex
        uint32_t last_heard = entry->last_heard;
        uint32_t access_count = entry->access_count;
        
        // Calculate priority:
        // - If time is valid: use last_heard (most recent = highest priority)
        // - If time is not valid: use access_count (most accessed = highest priority)
        // - If both are 0: use index order (earlier = higher priority, so we load first N)
        uint32_t priority = 0;
        if (incremental_load_state.time_valid && last_heard > 0) {
            // Time is valid: use last_heard (most recent = highest)
            // But we need to handle case where time was updated after last_heard was saved
            // If last_heard is in the future (time updated), treat it as very old
            if (last_heard > incremental_load_state.now) {
                priority = 0;  // Future timestamp = very old (time was updated)
            } else {
                priority = last_heard;  // Most recent = highest
            }
        } else if (access_count > 0) {
            // Time not valid: use access_count (most accessed = highest)
            // Scale access_count to be comparable to timestamps (multiply by large number)
            priority = access_count * 1000000;  // Scale to make it comparable
        } else {
            // Both are 0: use index order (earlier nodes = higher priority)
            // This ensures we load first N nodes if no activity data is available
            priority = (total_nodes - i);  // Earlier = higher priority
        }
        
        // CRITICAL: Check buffer bounds before writing to prevent overflow
        if (sort_count >= MAX_SORT_NODES) {
            LOG_WARN("NodeDBPersistentBackend: sort_buffer full (%u nodes), stopping collection", sort_count);
            incremental_load_state.current_index = i;  // Save progress
            incremental_load_state.sort_count = sort_count;  // Save collected nodes
            break;  // Stop collecting, but continue with sorting what we have
        }
        
        sort_buffer[sort_count].num = nodeNum;
        sort_buffer[sort_count].priority = priority;
        sort_buffer[sort_count].last_heard = last_heard;
        sort_buffer[sort_count].slot_id = entry->slot_id;
        sort_count++;
        LOG_DEBUG("NodeDBPersistentBackend: Collected node 0x%x for sorting (sort_count: %u)", nodeNum, sort_count);
    }
    
    LOG_DEBUG("NodeDBPersistentBackend: Collection loop finished (sort_count: %u, processed: %u nodes)", sort_count, end_index - start_index);
    
    // CRITICAL: Check if we collected any nodes to sort
    if (sort_count == 0) {
        LOG_DEBUG("NodeDBPersistentBackend: No nodes collected for sorting (all nodes already in cache or no nodes to process)");
        // Update progress and check if we should continue
        incremental_load_state.current_index = end_index;
        incremental_load_state.sort_processed = 0;
        
        // CRITICAL: Feed watchdog before returning
        FEED_WATCHDOG_AND_YIELD();
        delay(20);  // Delay to ensure stability
        
        // If we've processed all nodes, mark as complete
        if (end_index >= total_nodes) {
            LOG_INFO("NodeDBPersistentBackend: Processed all nodes, preload complete (%u nodes loaded)", preloaded_count);
            index_loaded = true;
            incremental_load_state.state = IncrementalLoadState::IDLE;
            return true;
        }
        
        // CRITICAL: If all nodes are already in cache and we've reached target, mark as complete
        // This prevents infinite loop when all nodes are already cached
        if (preloaded_count >= incremental_load_state.preload_target) {
            LOG_INFO("NodeDBPersistentBackend: Preload target reached (%u/%u nodes), all remaining nodes already in cache", 
                    preloaded_count, incremental_load_state.preload_target);
            index_loaded = true;
            incremental_load_state.state = IncrementalLoadState::IDLE;
            return true;
        }
        
        // Otherwise, will continue in next iteration
        return false;
    }
    
    // CRITICAL: Check memory before sorting (sorting uses stack space)
    uint32_t free_heap_before_sort = memGet.getFreeHeap();
    uint32_t heap_total_before_sort = memGet.getHeapSize();
    uint32_t used_heap_before_sort = (heap_total_before_sort > free_heap_before_sort) ? (heap_total_before_sort - free_heap_before_sort) : 0;
    // Reduced logging to prevent buffer overflow
    
    if (free_heap_before_sort < MemoryHelpers::Thresholds::SORT) {
        LOG_WARN("NodeDBPersistentBackend: Low memory before sort (%u bytes free, need %u), skipping sort", 
                 free_heap_before_sort, MemoryHelpers::Thresholds::SORT);
        // Skip sorting, just load nodes in collected order
        incremental_load_state.sort_processed = 0;
    } else {
        // Sort by priority descending (highest priority first)
        // Simple bubble sort (OK for small arrays)
        // CRITICAL: Feed watchdog during sort to prevent timeout
        for (uint32_t i = 0; i < sort_count - 1; i++) {
            // Feed watchdog every 5 iterations (more frequent)
            if (i % 5 == 0) {
                FEED_WATCHDOG_AND_YIELD();
                delay(5);  // Small delay for stability
            }
            
            for (uint32_t j = 0; j < sort_count - i - 1; j++) {
                // CRITICAL: Check bounds before accessing sort_buffer[j + 1]
                if (j + 1 >= MAX_SORT_NODES || j + 1 >= sort_count) {
                    LOG_ERROR("NodeDBPersistentBackend: sort_buffer bounds check failed (j: %u, sort_count: %u)", j, sort_count);
                    break;
                }
                if (sort_buffer[j].priority < sort_buffer[j + 1].priority) {
                    NodeWithPriority temp = sort_buffer[j];
                    sort_buffer[j] = sort_buffer[j + 1];
                    sort_buffer[j + 1] = temp;
                }
            }
        }
        
        // CRITICAL: Feed watchdog after sorting
        FEED_WATCHDOG_AND_YIELD();
        delay(10);  // Delay after sorting to ensure stability
    }
    
    // Load nodes in order (most recent first)
    // Process incrementally - start from where we left off
    uint32_t start_sort = incremental_load_state.sort_processed;
    
    // CRITICAL: Double-check that we have nodes to load (safety check)
    if (sort_count == 0) {
        LOG_DEBUG("NodeDBPersistentBackend: sort_count is 0, no nodes to load - marking as complete");
        incremental_load_state.current_index = end_index;
        incremental_load_state.sort_processed = 0;
        
        // CRITICAL: Feed watchdog before returning
        FEED_WATCHDOG_AND_YIELD();
        delay(20);  // Delay to ensure stability
        
        if (end_index >= total_nodes) {
            index_loaded = true;
            incremental_load_state.state = IncrementalLoadState::IDLE;
            return true;
        }
        return false;
    }
    
    LOG_DEBUG("NodeDBPersistentBackend: Starting to load nodes from sorted buffer (start_sort: %u, sort_count: %u, target: %u)", 
             start_sort, sort_count, incremental_load_state.preload_target);
    
    for (uint32_t i = start_sort; i < sort_count && incremental_load_state.preload_count < incremental_load_state.preload_target; i++) {
        // CRITICAL: Check bounds before accessing sort_buffer
        if (i >= MAX_SORT_NODES) {
            LOG_ERROR("NodeDBPersistentBackend: sort_buffer index out of bounds (i: %u, MAX: %u)", i, MAX_SORT_NODES);
            break;
        }
        // CRITICAL: Yield and feed watchdog before each node read
        FEED_WATCHDOG_AND_YIELD();
        
        NodeNum nodeNum = sort_buffer[i].num;
        uint16_t slot_id = sort_buffer[i].slot_id;
        uint32_t last_heard = sort_buffer[i].last_heard;
        
        // CRITICAL: Feed watchdog before reading from flash
        FEED_WATCHDOG_AND_YIELD();
        
        // CRITICAL: Check available memory before reading node (prevents heap overflow)
        // Each node read uses ~200 bytes for meshtastic_NodeInfoLite on stack
        // Need at least 5 KB free to safely read and process a node
        uint32_t free_heap = memGet.getFreeHeap();
        uint32_t heap_total_loading = memGet.getHeapSize();
        uint32_t used_heap = (heap_total_loading > free_heap) ? (heap_total_loading - free_heap) : 0;
        
        // Log memory every 5 nodes during loading
        if ((i - start_sort) % 5 == 0) {
            // Reduced logging to prevent buffer overflow
        }
        
        if (free_heap < MemoryHelpers::Thresholds::NODE_READ) {
            LOG_WARN("NodeDBPersistentBackend: Low memory (%u bytes free, need %u), stopping preload to prevent heap overflow", 
                     free_heap, MemoryHelpers::Thresholds::NODE_READ);
            incremental_load_state.sort_processed = i;  // Save progress
            incremental_load_state.current_index = end_index;  // Save progress
            incremental_load_state.state = IncrementalLoadState::PRELOADING;
            // Mark as complete with current count to prevent infinite retry
            if (incremental_load_state.preload_count >= 20) {  // At least 20 nodes loaded
                index_loaded = true;
                incremental_load_state.state = IncrementalLoadState::IDLE;
                LOG_INFO("NodeDBPersistentBackend: Preload stopped due to low memory (%u nodes loaded)", incremental_load_state.preload_count);
                return true;  // Success with partial load
            }
            return false;  // Will retry later when memory is available
        }
        
        // Read directly from flash and add to cache
        meshtastic_NodeInfoLite node;
        if (NodeStorage::readNodeFromSlot(slot_id, &node)) {
            // Validate node integrity
            if (node.num == 0 || node.num != nodeNum) {
                LOG_WARN("NodeDBPersistentBackend: Node integrity check failed for slot %u (expected 0x%x, got 0x%x)", 
                         slot_id, nodeNum, node.num);
                continue;
            }
            
            // Restore last_heard from NodeIndex (it's not in flash, but we have it in index)
            node.last_heard = last_heard;
            
            if (NodeCache::putNode(nodeNum, &node)) {
                // Mark as cached in index
                NodeIndex::markCached(nodeNum, true);
                incremental_load_state.preload_count++;
                recent_count++;
                incremental_load_state.sort_processed = i + 1;  // Update progress
                LOG_DEBUG("NodeDBPersistentBackend: Preloaded node 0x%x from slot %u (last_heard: %u, priority: %u)", 
                         nodeNum, slot_id, last_heard, sort_buffer[i].priority);
            } else {
                LOG_DEBUG("NodeDBPersistentBackend: Cache full, cannot preload more nodes");
                incremental_load_state.sort_processed = i;  // Save progress
                break;  // Cache is full, stop trying
            }
        } else {
            LOG_WARN("NodeDBPersistentBackend: Failed to read node from slot %u", slot_id);
            incremental_load_state.sort_processed = i + 1;  // Continue anyway
        }
        
        // CRITICAL: Feed watchdog after reading each node (readNodeFromSlot can be slow)
        FEED_WATCHDOG_AND_YIELD();
        
        // CRITICAL: Check time limit - don't spend more than 30ms in one chunk (further reduced to prevent watchdog timeout)
        if (millis() - operation_start > 30) {
            LOG_DEBUG("NodeDBPersistentBackend: Time limit reached in preload, yielding");
            incremental_load_state.current_index = end_index;  // Save progress
            incremental_load_state.state = IncrementalLoadState::PRELOADING;
            FEED_WATCHDOG_AND_YIELD();
            delay(20);  // Longer delay to ensure watchdog is fed and system is stable
            return false;  // Indicate loading is in progress
        }
    }
    
    // Update state
    incremental_load_state.current_index = end_index;
    
    LOG_INFO("NodeDBPersistentBackend: Preloaded %u recent nodes (total: %u/%u in cache)", 
             recent_count, incremental_load_state.preload_count, max_cache_size);
    
    // Check if we've finished processing all nodes
    if (incremental_load_state.current_index >= incremental_load_state.total_nodes && 
        incremental_load_state.sort_processed >= incremental_load_state.sort_count) {
        // All done - mark as loaded
        uint32_t final_index_count = NodeIndex::getNodeCount();
        if (final_index_count == 0) {
            LOG_ERROR("NodeDBPersistentBackend: Index is empty after loadFromDisk() - cannot mark as loaded");
            incremental_load_state.state = IncrementalLoadState::IDLE;
            return false;
        }
        
        // Mark index as loaded (prevents repeated loading)
        index_loaded = true;
        incremental_load_state.state = IncrementalLoadState::IDLE;
        
        LOG_INFO("NodeDBPersistentBackend: Successfully loaded index (%u nodes)", final_index_count);
        return true;
    } else {
        // Still in progress
        incremental_load_state.state = IncrementalLoadState::PRELOADING;
        LOG_DEBUG("NodeDBPersistentBackend: Load in progress (%u/%u nodes processed, %u/%u preloaded)", 
                 incremental_load_state.current_index, incremental_load_state.total_nodes, 
                 incremental_load_state.preload_count, incremental_load_state.preload_target);
        return false;  // Indicate loading is still in progress
    }
}

/**
 * @brief Save dirty nodes to flash
 * NOTE: Does NOT initialize backend - must be initialized before calling this
 * This prevents blocking device startup with filesystem operations
 */
bool saveToDisk()
{
    // CRITICAL: Do NOT initialize here - this can be called from NodeDB constructor
    // during device startup. If backend is not initialized, return true (no-op)
    // to allow standard save method to be used instead.
    CHECK_BACKEND_ENABLED_TRUE();
    
    LOG_DEBUG("NodeDBPersistentBackend: saveToDisk() called");
    
    // Throttle saves to 1 minute
    uint32_t now = millis();
    if (now - last_save_time < SAVE_THROTTLE_MS) {
        LOG_DEBUG("NodeDBPersistentBackend: Save throttled (last save %u ms ago)", now - last_save_time);
        return true;  // Too soon, skip save
    }
    
    LOG_DEBUG("NodeDBPersistentBackend: Checking radio state...");
    // Check radio state before saving (for Main FS variants)
    // NOTE: During initialization, RadioLibInterface::instance may be nullptr
    // This is OK - we skip radio check if radio is not initialized yet
    #ifdef ARCH_NRF52
    if (RadioLibInterface::instance != nullptr) {
        // Check if radio is actively receiving - if so, defer save to avoid packet loss
        if (RadioLibInterface::instance->isActivelyReceiving() && !RadioLibInterface::instance->isSending()) {
            LOG_DEBUG("NodeDBPersistentBackend: Radio busy, deferring save");
            return true;  // Radio busy, defer save
        }
    }
    #endif
    
        LOG_DEBUG("NodeDBPersistentBackend: Flushing dirty nodes to flash...");
    // Flush dirty nodes to flash
    uint32_t written = NodeCache::flushDirtyNodes();
    
    if (written > 0) {
        LOG_INFO("NodeDBPersistentBackend: Saved %u dirty nodes to flash", written);
    } else {
        LOG_DEBUG("NodeDBPersistentBackend: No dirty nodes to save");
    }
    
    // CRITICAL: Save NodeIndex to flash (preserves last_heard timestamps)
    // This is essential for:
    // 1. Preloading cache with most active nodes on next boot
    // 2. Evicting oldest nodes when flash is full
    LOG_DEBUG("NodeDBPersistentBackend: Saving NodeIndex to flash...");
    if (NodeIndex::saveToFlash()) {
        LOG_DEBUG("NodeDBPersistentBackend: NodeIndex saved to flash successfully");
    } else {
        // Index save failed - attempt recovery
        LOG_WARN("NodeDBPersistentBackend: Failed to save NodeIndex, attempting recovery...");
        recoverFromError();
        // Don't fail the entire save operation if index save fails
    }
    
    last_save_time = now;
    LOG_DEBUG("NodeDBPersistentBackend: saveToDisk() completed");
    return true;
}

/**
 * @brief Get node by index (for iteration)
 * 
 * CRITICAL: This function avoids stack overflow by using getNodeNumByIndex()
 * instead of allocating a large array (1234 * 4 = 4936 bytes) on the stack.
 */
meshtastic_NodeInfoLite* getNodeByIndex(size_t index)
{
    // CRITICAL: Do NOT initialize here - backend must be initialized before use
    CHECK_BACKEND_ENABLED_NULL();
    
    // Get node number by index directly (avoids large stack allocation)
    NodeNum nodeNum = NodeIndex::getNodeNumByIndex(index);
    if (nodeNum == 0) {
        // Index out of range or node not found
        return nullptr;
    }
    
    // Get node by number (loads from flash if not cached)
    return getNode(nodeNum);
}

/**
 * @brief Mark a node as protected
 */
void markProtected(NodeNum nodeNum, bool isProtected)
{
    CHECK_BACKEND_ENABLED_VOID();
    
    NodeIndex::markProtected(nodeNum, isProtected);
}

/**
 * @brief Validate index integrity
 */
bool validateIndexIntegrity()
{
    CHECK_BACKEND_ENABLED();
    
    LOG_DEBUG("NodeDBPersistentBackend: Validating index integrity...");
    
    uint32_t index_count = NodeIndex::getNodeCount();
    uint32_t slot_count = NodeStorage::getSlotCount();
    
    if (index_count != slot_count) {
        LOG_WARN("NodeDBPersistentBackend: Index mismatch (index: %u, slots: %u)", index_count, slot_count);
        return false;
    }
    
    // Check that all index entries point to valid slots
    for (uint32_t i = 0; i < index_count; i++) {
        NodeNum nodeNum = NodeIndex::getNodeNumByIndex(i);
        if (nodeNum == 0) {
            continue;
        }
        
        NodeIndexEntry* entry = NodeIndex::findNode(nodeNum);
        if (!entry || entry->slot_id == UINT16_MAX) {
            LOG_WARN("NodeDBPersistentBackend: Invalid index entry for node 0x%x", nodeNum);
            return false;
        }
        
        // Check if slot file exists
        if (!NodeStorage::slotExists(entry->slot_id)) {
            LOG_WARN("NodeDBPersistentBackend: Slot %u for node 0x%x does not exist", entry->slot_id, nodeNum);
            return false;
        }
    }
    
    LOG_DEBUG("NodeDBPersistentBackend: Index integrity check passed");
    return true;
}

/**
 * @brief Rebuild index from flash slots if needed
 */
bool rebuildIndexIfNeeded()
{
    CHECK_BACKEND_ENABLED();
    
    if (validateIndexIntegrity()) {
        return true;  // Index is valid, no rebuild needed
    }
    
    LOG_WARN("NodeDBPersistentBackend: Index integrity check failed, rebuilding...");
    
    // Clear current index
    NodeIndex::clear();
    
    // Rebuild by scanning slots (this is done in loadFromDisk() when index is empty)
    // For now, just mark index as not loaded so it will be rebuilt on next loadFromDisk()
    index_loaded = false;
    
    LOG_INFO("NodeDBPersistentBackend: Index marked for rebuild on next load");
    return true;
}

/**
 * @brief Recover from flash errors
 */
bool recoverFromError()
{
    CHECK_BACKEND_ENABLED();
    
    LOG_WARN("NodeDBPersistentBackend: Attempting error recovery...");
    
    // Step 1: Validate index integrity
    if (!validateIndexIntegrity()) {
        // Step 2: Rebuild index if corrupted
        if (!rebuildIndexIfNeeded()) {
            LOG_ERROR("NodeDBPersistentBackend: Failed to rebuild index during recovery");
            return false;
        }
    }
    
    // Step 3: Clear any corrupted cache entries
    // This will be handled automatically when nodes are accessed
    
    LOG_INFO("NodeDBPersistentBackend: Error recovery completed");
    return true;
}

} // namespace NodeDBPersistentBackend

// ========================================================================
// Backend Context Implementation
// ========================================================================

namespace {
    // Global context instance (singleton)
    static NodeDBBackendContext* context_instance = nullptr;
    static bool context_initialized = false;
} // anonymous namespace

NodeDBBackendContext& getBackendContext()
{
    if (!context_initialized) {
        initializeBackendContext();
    }
    return *context_instance;
}

void initializeBackendContext()
{
    if (context_initialized) {
        return;
    }
    
    // Allocate context structure
    // CRITICAL: Check memory before allocation (though context is small, ~100 bytes)
    uint32_t requiredMemory = sizeof(NodeDBBackendContext);
    uint32_t safetyMargin = 1024;  // 1 KB safety margin
    auto memCheck = MemoryHelpers::checkMemory(requiredMemory, safetyMargin, "NodeDBBackendContext");
    if (!memCheck.sufficient) {
        LOG_WARN("NodeDBPersistentBackend: Low memory for context allocation (%u bytes free), using static storage", 
                 memCheck.freeHeap);
        // Fallback: use static storage if allocation fails or memory is low
        static NodeDBBackendContext static_context;
        context_instance = &static_context;
    } else {
        #ifdef ARCH_NRF52
        context_instance = (NodeDBBackendContext*)rtos_malloc(sizeof(NodeDBBackendContext));
        #else
        context_instance = new NodeDBBackendContext();
        #endif
        
        if (!context_instance) {
            // Fallback: use static storage if allocation fails
            static NodeDBBackendContext static_context;
            context_instance = &static_context;
        }
    }
    
    // Initialize with default values
    *context_instance = NodeDBBackendContext();
    
    // Sync with actual backend state (for backward compatibility)
    context_instance->backendInitialized = NodeDBPersistentBackend::isEnabled();
    context_instance->backendEnabled = NodeDBPersistentBackend::isEnabled();
    
    context_initialized = true;
}

#endif // USE_EXTENDED_FS_FOR_NODEDB
