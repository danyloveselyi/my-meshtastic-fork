/**
 * @file NodeIndex.cpp
 * @brief Compact RAM index implementation
 */

#include "NodeIndex.h"
// Include LittleFS headers BEFORE NodeDBExtendedFSImpl.h to ensure types are defined
#include "../../../src/platform/stm32wl/littlefs/lfs.h"
#include "../../../src/platform/stm32wl/littlefs/lfs_util.h"
#include "NodeDBExtendedFSImpl.h"
#include "../../../src/memGet.h"
#include "../../../src/gps/RTC.h"
#ifdef ARCH_NRF52
#include "rtos.h"
// Forward declaration for nrf52Loop() (defined in src/platform/nrf52/main-nrf52.cpp)
extern void nrf52Loop();
#endif
#include <algorithm>
#include <cstring>

#ifdef USE_EXTENDED_FS_FOR_NODEDB

namespace NodeIndex {

// Static storage for index entries
static NodeIndexEntry* index_entries = nullptr;
static uint32_t max_entries = 0;
static uint32_t num_entries = 0;
static bool initialized = false;

/**
 * @brief Initialize the index
 */
bool initialize(uint32_t maxEntries)
{
    if (initialized) {
        // Already initialized - just update max if needed
        if (maxEntries > max_entries) {
            // Would need to reallocate - for now, just return false
            return false;
        }
        return true;
    }
    
    // Maximum index entries - use MAX_NUM_NODES from variant.h (already defined in mesh-pb-constants.h)
    // Each index entry points to one flash slot, so index size = slot count = MAX_NUM_NODES
    #ifndef MAX_NODES_INDEX
    #ifdef MAX_NODES_SLOTS
    #define MAX_NODES_INDEX MAX_NODES_SLOTS  // Use same value as slots (they must be equal)
    #else
    #define MAX_NODES_INDEX MAX_NUM_NODES  // Use MAX_NUM_NODES from variant.h (500 by default for RAK4631)
    #endif
    #endif
    if (maxEntries == 0 || maxEntries > MAX_NODES_INDEX) {
        return false;  // Sanity check
    }
    
    // Check available memory before allocation
    // NodeIndexEntry size: ~16 bytes (NodeNum: 4, slot_id: 2, flags: 2, last_heard: 4, access_count: 4)
    // For 1234 entries: 1234 * 16 = 19,744 bytes (~19 KB)
    // Check if we have enough free heap (need at least 50 KB to account for fragmentation)
    uint32_t freeHeap = memGet.getFreeHeap();
    uint32_t heapTotal = memGet.getHeapSize();
    uint32_t usedHeap = (heapTotal > freeHeap) ? (heapTotal - freeHeap) : 0;
    uint32_t requiredMemory = maxEntries * sizeof(NodeIndexEntry);
    // Add large safety margin (30 KB) to account for memory fragmentation
    // Fragmentation can make it impossible to allocate even if freeHeap > requiredMemory
    // Increased from 15 KB to 30 KB for better reliability
    uint32_t safetyMargin = 30720; // 30 KB safety margin
    uint32_t totalNeeded = requiredMemory + safetyMargin;
    
    LOG_DEBUG("NodeIndex: Memory check - required: %u bytes, margin: %u bytes, total needed: %u bytes", 
             requiredMemory, safetyMargin, totalNeeded);
    LOG_DEBUG("NodeIndex: Memory status - free: %u bytes, used: %u bytes, total: %u bytes", 
             freeHeap, usedHeap, heapTotal);
    
    if (freeHeap < totalNeeded) {
        LOG_ERROR("NodeIndex: Not enough memory to allocate index");
        LOG_ERROR("NodeIndex:   Required: %u bytes (index) + %u bytes (margin) = %u bytes total", 
                 requiredMemory, safetyMargin, totalNeeded);
        LOG_ERROR("NodeIndex:   Available: %u bytes free, %u bytes used, %u bytes total", 
                 freeHeap, usedHeap, heapTotal);
        LOG_ERROR("NodeIndex:   Shortage: %u bytes", totalNeeded - freeHeap);
        return false;
    }
    
    LOG_DEBUG("NodeIndex: Memory check PASSED - sufficient memory available");
    
    // Allocate index entries array using rtos_malloc directly to avoid assert in operator new
    // This allows us to check for allocation failure without triggering assert
    uint32_t allocation_size = maxEntries * sizeof(NodeIndexEntry);
    LOG_DEBUG("NodeIndex: Allocating %u bytes for %u index entries", allocation_size, maxEntries);
    
    #ifdef ARCH_NRF52
    index_entries = (NodeIndexEntry*)rtos_malloc(allocation_size);
    if (!index_entries) {
        uint32_t freeHeapAfter = memGet.getFreeHeap();
        LOG_ERROR("NodeIndex: rtos_malloc FAILED");
        LOG_ERROR("NodeIndex:   Requested: %u bytes (%u entries * %u bytes)", 
                 allocation_size, maxEntries, sizeof(NodeIndexEntry));
        LOG_ERROR("NodeIndex:   Free heap before: %u bytes, after: %u bytes", freeHeap, freeHeapAfter);
        return false;
    }
    #else
    index_entries = new NodeIndexEntry[maxEntries];
    if (!index_entries) {
        LOG_ERROR("NodeIndex: new[] FAILED (size: %u entries, %u bytes)", maxEntries, allocation_size);
        return false;
    }
    #endif
    
    uint32_t freeHeapAfter = memGet.getFreeHeap();
    uint32_t allocatedMemory = freeHeap - freeHeapAfter;
    
    max_entries = maxEntries;
    num_entries = 0;
    initialized = true;
    
    memset(index_entries, 0, sizeof(NodeIndexEntry) * max_entries);
    
    LOG_INFO("NodeIndex: Initialized successfully");
    LOG_DEBUG("NodeIndex:   Max entries: %u", max_entries);
    LOG_DEBUG("NodeIndex:   Entry size: %u bytes", sizeof(NodeIndexEntry));
    LOG_DEBUG("NodeIndex:   Total allocated: %u bytes", allocation_size);
    LOG_DEBUG("NodeIndex:   Free heap before: %u bytes, after: %u bytes, allocated: %u bytes", 
             freeHeap, freeHeapAfter, allocatedMemory);
    
    return true;
}

/**
 * @brief Find a node in the index (binary search on sorted array)
 */
NodeIndexEntry* findNode(NodeNum nodeNum)
{
    if (!initialized || num_entries == 0) {
        return nullptr;
    }
    
    // Binary search (array is kept sorted by nodeNum)
    int left = 0;
    int right = num_entries - 1;
    
    while (left <= right) {
        int mid = (left + right) / 2;
        if (index_entries[mid].num == nodeNum) {
            return &index_entries[mid];
        } else if (index_entries[mid].num < nodeNum) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    
    return nullptr;
}

/**
 * @brief Add a node to the index
 */
bool addNode(NodeNum nodeNum, uint16_t slotId)
{
    if (!initialized) {
        #ifndef MAX_NODES_INDEX
        #ifdef MAX_NODES_SLOTS
        #define MAX_NODES_INDEX MAX_NODES_SLOTS  // Use same value as slots
        #else
        #define MAX_NODES_INDEX MAX_NUM_NODES  // Use MAX_NUM_NODES from variant.h
        #endif
        #endif
        if (!initialize(MAX_NODES_INDEX)) {
            return false;
        }
    }
    
    // Check if node already exists
    if (findNode(nodeNum) != nullptr) {
        return false;  // Already exists
    }
    
    // Check if index is full
    if (num_entries >= max_entries) {
        return false;
    }
    
    // Insert in sorted order (by nodeNum)
    int insert_pos = num_entries;
    for (int i = 0; i < num_entries; i++) {
        if (index_entries[i].num > nodeNum) {
            insert_pos = i;
            break;
        }
    }
    
    // Shift entries to make room
    if (insert_pos < num_entries) {
        memmove(&index_entries[insert_pos + 1], &index_entries[insert_pos],
                (num_entries - insert_pos) * sizeof(NodeIndexEntry));
    }
    
    // Insert new entry
    NodeIndexEntry* entry = &index_entries[insert_pos];
    entry->num = nodeNum;
    entry->slot_id = slotId;
    entry->flags = 0;
    entry->last_heard = 0;
    entry->access_count = 0;
    
    num_entries++;
    
    return true;
}

/**
 * @brief Remove a node from the index
 */
bool removeNode(NodeNum nodeNum)
{
    if (!initialized) {
        return false;
    }
    
    NodeIndexEntry* entry = findNode(nodeNum);
    if (!entry) {
        return false;  // Not found
    }
    
    // Calculate position
    int pos = entry - index_entries;
    
    // Shift entries to fill gap
    if (pos < (int)num_entries - 1) {
        memmove(&index_entries[pos], &index_entries[pos + 1],
                (num_entries - pos - 1) * sizeof(NodeIndexEntry));
    }
    
    num_entries--;
    
    return true;
}

/**
 * @brief Update last_heard timestamp for a node
 */
bool updateLastHeard(NodeNum nodeNum, uint32_t timestamp)
{
    NodeIndexEntry* entry = findNode(nodeNum);
    if (!entry) {
        return false;
    }
    
    entry->last_heard = timestamp;
    return true;
}

/**
 * @brief Increment access count for a node
 */
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
 * @brief Mark a node as protected
 */
bool markProtected(NodeNum nodeNum, bool isProtected)
{
    NodeIndexEntry* entry = findNode(nodeNum);
    if (!entry) {
        return false;
    }
    
    if (isProtected) {
        entry->flags |= NodeIndexEntry::FLAG_PROTECTED;
    } else {
        entry->flags &= ~NodeIndexEntry::FLAG_PROTECTED;
    }
    
    return true;
}

/**
 * @brief Mark a node as cached
 */
bool markCached(NodeNum nodeNum, bool cached)
{
    NodeIndexEntry* entry = findNode(nodeNum);
    if (!entry) {
        return false;
    }
    
    if (cached) {
        entry->flags |= NodeIndexEntry::FLAG_CACHED;
    } else {
        entry->flags &= ~NodeIndexEntry::FLAG_CACHED;
    }
    
    return true;
}

/**
 * @brief Mark a node as dirty
 */
bool markDirty(NodeNum nodeNum, bool dirty)
{
    NodeIndexEntry* entry = findNode(nodeNum);
    if (!entry) {
        return false;
    }
    
    if (dirty) {
        entry->flags |= NodeIndexEntry::FLAG_DIRTY;
    } else {
        entry->flags &= ~NodeIndexEntry::FLAG_DIRTY;
    }
    
    return true;
}

/**
 * @brief Get eviction candidate (LRU/LFU policy)
 * 
 * Uses last_heard if time is valid, otherwise uses access_count.
 * If both are 0, uses index order (earlier nodes = lower priority).
 */
NodeNum getEvictionCandidate(bool allowCached)
{
    if (!initialized || num_entries == 0) {
        return 0;
    }
    
    // Check if time is valid (for determining eviction priority)
    RTCQuality time_quality = getRTCQuality();
    uint32_t now = getValidTime(RTCQualityDevice, false);  // Returns 0 if time not valid
    bool time_valid = (time_quality != RTCQualityNone && now > 0);
    
    // Find least recently used/accessed, non-protected node
    NodeIndexEntry* candidate = nullptr;
    uint32_t lowest_priority = UINT32_MAX;
    
    for (uint32_t i = 0; i < num_entries; i++) {
        NodeIndexEntry* entry = &index_entries[i];
        
        // Skip protected nodes
        if (entry->flags & NodeIndexEntry::FLAG_PROTECTED) {
            continue;
        }
        
        // Skip cached nodes only if allowCached is false (for cache eviction)
        // If allowCached is true (flash full), can evict cached nodes too
        if (!allowCached && (entry->flags & NodeIndexEntry::FLAG_CACHED)) {
            continue;
        }
        
        // Calculate priority (lower = better candidate for eviction):
        // - If time is valid: use last_heard (oldest = lowest priority)
        // - If time is not valid: use access_count (least accessed = lowest priority)
        // - If both are 0: use index order (earlier = lower priority)
        uint32_t priority = UINT32_MAX;
        if (time_valid && entry->last_heard > 0) {
            // Time is valid: use last_heard (oldest = lowest priority)
            // But we need to handle case where time was updated after last_heard was saved
            // If last_heard is in the future (time updated), treat it as very old
            if (entry->last_heard > now) {
                priority = 0;  // Future timestamp = very old (time was updated)
            } else {
                priority = entry->last_heard;  // Oldest = lowest
            }
        } else if (entry->access_count > 0) {
            // Time not valid: use access_count (least accessed = lowest priority)
            // Lower access_count = lower priority (better candidate)
            priority = entry->access_count;
        } else {
            // Both are 0: use index order (earlier nodes = lower priority)
            // This ensures we evict first N nodes if no activity data is available
            priority = i;  // Earlier = lower priority
        }
        
        // Find node with lowest priority (best candidate for eviction)
        if (priority < lowest_priority) {
            lowest_priority = priority;
            candidate = entry;
        }
    }
    
    if (candidate) {
        return candidate->num;
    }
    
    return 0;  // No candidate found
}

/**
 * @brief Get total number of nodes
 */
uint32_t getNodeCount()
{
    // CRITICAL: Check if initialized and index_entries is valid before accessing num_entries
    // This prevents crashes if called before initialization or if memory was corrupted
    if (!initialized || index_entries == nullptr) {
        return 0;
    }
    return num_entries;
}

/**
 * @brief Get number of cached nodes
 */
uint32_t getCachedNodeCount()
{
    if (!initialized) {
        return 0;
    }
    
    uint32_t count = 0;
    for (uint32_t i = 0; i < num_entries; i++) {
        if (index_entries[i].flags & NodeIndexEntry::FLAG_CACHED) {
            count++;
        }
    }
    
    return count;
}

/**
 * @brief Get number of protected nodes
 */
uint32_t getProtectedNodeCount()
{
    if (!initialized) {
        return 0;
    }
    
    uint32_t count = 0;
    for (uint32_t i = 0; i < num_entries; i++) {
        if (index_entries[i].flags & NodeIndexEntry::FLAG_PROTECTED) {
            count++;
        }
    }
    
    return count;
}

/**
 * @brief Get number of dirty nodes
 */
uint32_t getDirtyNodeCount()
{
    if (!initialized) {
        return 0;
    }
    
    uint32_t count = 0;
    for (uint32_t i = 0; i < num_entries; i++) {
        if (index_entries[i].flags & NodeIndexEntry::FLAG_DIRTY) {
            count++;
        }
    }
    
    return count;
}

/**
 * @brief Clear all entries
 */
void clear()
{
    if (index_entries) {
        memset(index_entries, 0, sizeof(NodeIndexEntry) * max_entries);
    }
    num_entries = 0;
}

/**
 * @brief Get all node numbers
 */
uint32_t getAllNodeNums(NodeNum* buffer, uint32_t bufferSize)
{
    if (!initialized || !buffer || bufferSize == 0) {
        return 0;
    }
    
    uint32_t count = (num_entries < bufferSize) ? num_entries : bufferSize;
    for (uint32_t i = 0; i < count; i++) {
        buffer[i] = index_entries[i].num;
    }
    
    return count;
}

/**
 * @brief Get node number by index (for iteration)
 * This avoids allocating a large array on the stack
 */
NodeNum getNodeNumByIndex(uint32_t index)
{
    if (!initialized || index >= num_entries) {
        return 0;  // Index out of range
    }
    
    return index_entries[index].num;
}

/**
 * @brief Get last_heard timestamp for a node
 */
uint32_t getLastHeard(NodeNum nodeNum)
{
    NodeIndexEntry* entry = findNode(nodeNum);
    if (entry) {
        return entry->last_heard;
    }
    return 0;
}

/**
 * @brief Get entry by index (for iteration)
 */
NodeIndexEntry* getEntryByIndex(uint32_t index)
{
    if (!initialized || !index_entries || index >= num_entries) {
        return nullptr;
    }
    
    return &index_entries[index];
}

/**
 * @brief Save index to flash
 * Saves index entries to /prefs/nodeindex.bin in extended filesystem
 */
bool saveToFlash()
{
    if (!initialized || !index_entries || num_entries == 0) {
        return false;
    }
    
    // CRITICAL: Feed watchdog before accessing filesystem
    // getExtendedFSForNodeDB() may initialize filesystem, which can take time
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();  // Yield to allow other tasks to run
    #endif
    
    // Use extended filesystem for index
    // CRITICAL: This may trigger filesystem initialization if not already initialized
    lfs_t* extended_lfs = getExtendedFSForNodeDB();
    
    // CRITICAL: Feed watchdog after filesystem access
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();  // Yield to allow other tasks to run
    #endif
    
    if (!extended_lfs) {
        LOG_ERROR("NodeIndex: Extended filesystem not available for saving index");
        return false;
    }
    
    const char* index_filename = "/prefs/nodeindex.bin";
    
    // Open file for writing
    lfs_file_t file;
    int open_result = lfs_file_open(extended_lfs, &file, index_filename, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (open_result != LFS_ERR_OK) {
        LOG_ERROR("NodeIndex: Failed to open index file for writing (error: %d)", open_result);
        return false;
    }
    
    // Write index header: version (4 bytes) + entry count (4 bytes)
    uint32_t index_version = 1;
    uint32_t entry_count = num_entries;
    
    lfs_ssize_t write_result = lfs_file_write(extended_lfs, &file, &index_version, sizeof(index_version));
    if (write_result != (lfs_ssize_t)sizeof(index_version)) {
        LOG_ERROR("NodeIndex: Failed to write index version (wrote %d of %u)", (int)write_result, (unsigned)sizeof(index_version));
        lfs_file_close(extended_lfs, &file);
        return false;
    }
    
    write_result = lfs_file_write(extended_lfs, &file, &entry_count, sizeof(entry_count));
    if (write_result != (lfs_ssize_t)sizeof(entry_count)) {
        LOG_ERROR("NodeIndex: Failed to write entry count (wrote %d of %u)", (int)write_result, (unsigned)sizeof(entry_count));
        lfs_file_close(extended_lfs, &file);
        return false;
    }
    
    // Write all index entries
    size_t entry_size = sizeof(NodeIndexEntry);
    size_t total_size = entry_count * entry_size;
    write_result = lfs_file_write(extended_lfs, &file, index_entries, total_size);
    if (write_result != (lfs_ssize_t)total_size) {
        LOG_ERROR("NodeIndex: Failed to write index entries (wrote %d of %u)", (int)write_result, (unsigned)total_size);
        lfs_file_close(extended_lfs, &file);
        return false;
    }
    
    // Sync and close
    int sync_result = lfs_file_sync(extended_lfs, &file);
    int close_result = lfs_file_close(extended_lfs, &file);
    
    if (sync_result != LFS_ERR_OK || close_result != LFS_ERR_OK) {
        LOG_ERROR("NodeIndex: Failed to sync/close index file (sync: %d, close: %d)", sync_result, close_result);
        return false;
    }
    
    LOG_DEBUG("NodeIndex: Saved %u entries to flash (%u bytes)", entry_count, (unsigned)(sizeof(index_version) + sizeof(entry_count) + total_size));
    return true;
}

/**
 * @brief Load index from flash
 * Loads index entries from /prefs/nodeindex.bin in extended filesystem
 */
bool loadFromFlash()
{
    if (!initialized || !index_entries) {
        return false;
    }
    
    // CRITICAL: Feed watchdog before accessing filesystem
    // getExtendedFSForNodeDB() may initialize filesystem, which can take time
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();  // Yield to allow other tasks to run
    #endif
    
    // Use extended filesystem for index
    // CRITICAL: This may trigger filesystem initialization if not already initialized
    lfs_t* extended_lfs = getExtendedFSForNodeDB();
    
    // CRITICAL: Feed watchdog after filesystem access
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();  // Yield to allow other tasks to run
    #endif
    
    if (!extended_lfs) {
        LOG_DEBUG("NodeIndex: Extended filesystem not available for loading index");
        return false;
    }
    
    const char* index_filename = "/prefs/nodeindex.bin";
    
    // Open file for reading
    lfs_file_t file;
    int open_result = lfs_file_open(extended_lfs, &file, index_filename, LFS_O_RDONLY);
    if (open_result != LFS_ERR_OK) {
        LOG_DEBUG("NodeIndex: Index file not found (error: %d) - will rebuild from slots", open_result);
        return false;  // Index file doesn't exist - this is OK, will rebuild from slots
    }
    
    // Read index header
    uint32_t index_version = 0;
    uint32_t entry_count = 0;
    
    lfs_ssize_t read_result = lfs_file_read(extended_lfs, &file, &index_version, sizeof(index_version));
    if (read_result != (lfs_ssize_t)sizeof(index_version)) {
        LOG_ERROR("NodeIndex: Failed to read index version (read %d of %u)", (int)read_result, (unsigned)sizeof(index_version));
        lfs_file_close(extended_lfs, &file);
        return false;
    }
    
    if (index_version != 1) {
        LOG_WARN("NodeIndex: Unsupported index version %u (expected 1)", index_version);
        lfs_file_close(extended_lfs, &file);
        return false;
    }
    
    read_result = lfs_file_read(extended_lfs, &file, &entry_count, sizeof(entry_count));
    if (read_result != (lfs_ssize_t)sizeof(entry_count)) {
        LOG_ERROR("NodeIndex: Failed to read entry count (read %d of %u)", (int)read_result, (unsigned)sizeof(entry_count));
        lfs_file_close(extended_lfs, &file);
        return false;
    }
    
    // Validate entry count
    if (entry_count == 0 || entry_count > max_entries) {
        LOG_ERROR("NodeIndex: Invalid entry count %u (max: %u)", entry_count, max_entries);
        lfs_file_close(extended_lfs, &file);
        return false;
    }
    
    // Clear existing entries
    clear();
    
    // Read all index entries
    size_t entry_size = sizeof(NodeIndexEntry);
    size_t total_size = entry_count * entry_size;
    read_result = lfs_file_read(extended_lfs, &file, index_entries, total_size);
    lfs_file_close(extended_lfs, &file);
    
    if (read_result != (lfs_ssize_t)total_size) {
        LOG_ERROR("NodeIndex: Failed to read index entries (read %d of %u)", (int)read_result, (unsigned)total_size);
        clear();
        return false;
    }
    
    num_entries = entry_count;
    
    LOG_INFO("NodeIndex: Loaded %u entries from flash (version %u)", entry_count, index_version);
    return true;
}

} // namespace NodeIndex

#endif // USE_EXTENDED_FS_FOR_NODEDB

