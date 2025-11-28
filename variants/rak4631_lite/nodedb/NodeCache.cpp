/**
 * @file NodeCache.cpp
 * @brief LRU/LFU cache implementation
 */

#include "NodeCache.h"
#include "NodeStorage.h"
#include "NodeIndex.h"
#include "NodeDBWriteQueue.h"
#include "../../../src/mesh/NodeDB.h"
#include "../../../src/memGet.h"
#ifdef ARCH_NRF52
#include "rtos.h"
#endif
#include <cstring>
#include <algorithm>

#ifdef USE_EXTENDED_FS_FOR_NODEDB

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
    
    #ifndef MAX_NODES_CACHE
    #define MAX_NODES_CACHE 300
    #endif
    if (maxSize == 0 || maxSize > MAX_NODES_CACHE) {
        return false;  // Sanity check
    }
    
    // Check available memory before allocation
    // CacheEntry size: meshtastic_NodeInfoLite (~200 bytes) + metadata (~8 bytes) = ~208 bytes
    // For 300 entries: 300 * 208 = 62,400 bytes (~61 KB)
    // Check if we have enough free heap (need at least 100 KB to account for fragmentation)
    uint32_t freeHeap = memGet.getFreeHeap();
    uint32_t heapTotal = memGet.getHeapSize();
    uint32_t usedHeap = (heapTotal > freeHeap) ? (heapTotal - freeHeap) : 0;
    uint32_t requiredMemory = maxSize * sizeof(CacheEntry);
    // Add large safety margin (40 KB) to account for memory fragmentation
    // Fragmentation can make it impossible to allocate even if freeHeap > requiredMemory
    // Increased from 20 KB to 40 KB for better reliability
    uint32_t safetyMargin = 40960; // 40 KB safety margin
    uint32_t totalNeeded = requiredMemory + safetyMargin;
    
    LOG_DEBUG("NodeCache: Memory check - required: %u bytes, margin: %u bytes, total needed: %u bytes", 
             requiredMemory, safetyMargin, totalNeeded);
    LOG_DEBUG("NodeCache: Memory status - free: %u bytes, used: %u bytes, total: %u bytes", 
             freeHeap, usedHeap, heapTotal);
    
    if (freeHeap < totalNeeded) {
        LOG_ERROR("NodeCache: Not enough memory to allocate cache");
        LOG_ERROR("NodeCache:   Required: %u bytes (cache) + %u bytes (margin) = %u bytes total", 
                 requiredMemory, safetyMargin, totalNeeded);
        LOG_ERROR("NodeCache:   Available: %u bytes free, %u bytes used, %u bytes total", 
                 freeHeap, usedHeap, heapTotal);
        LOG_ERROR("NodeCache:   Shortage: %u bytes", totalNeeded - freeHeap);
        return false;
    }
    
    LOG_DEBUG("NodeCache: Memory check PASSED - sufficient memory available");
    
    // Allocate cache entries using rtos_malloc directly to avoid assert in operator new
    // This allows us to check for allocation failure without triggering assert
    uint32_t allocation_size = maxSize * sizeof(CacheEntry);
    LOG_DEBUG("NodeCache: Allocating %u bytes for %u cache entries", allocation_size, maxSize);
    
    #ifdef ARCH_NRF52
    cache_entries = (CacheEntry*)rtos_malloc(allocation_size);
    if (!cache_entries) {
        uint32_t freeHeapAfter = memGet.getFreeHeap();
        LOG_ERROR("NodeCache: rtos_malloc FAILED");
        LOG_ERROR("NodeCache:   Requested: %u bytes (%u entries * %u bytes)", 
                 allocation_size, maxSize, sizeof(CacheEntry));
        LOG_ERROR("NodeCache:   Free heap before: %u bytes, after: %u bytes", freeHeap, freeHeapAfter);
        return false;
    }
    #else
    cache_entries = new CacheEntry[maxSize];
    if (!cache_entries) {
        LOG_ERROR("NodeCache: new[] FAILED (size: %u entries, %u bytes)", maxSize, allocation_size);
        return false;
    }
    #endif
    
    uint32_t freeHeapAfter = memGet.getFreeHeap();
    uint32_t allocatedMemory = freeHeap - freeHeapAfter;
    
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
             freeHeap, freeHeapAfter, allocatedMemory);
    
    return true;
}

/**
 * @brief Get a node from cache (load from flash if not cached)
 */
meshtastic_NodeInfoLite* getNode(NodeNum nodeNum)
{
    if (!initialized) {
        #ifndef MAX_NODES_CACHE
        #define MAX_NODES_CACHE 300
        #endif
        if (!initialize(MAX_NODES_CACHE)) {
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
        #ifndef MAX_NODES_CACHE
        #define MAX_NODES_CACHE 300
        #endif
        if (!initialize(MAX_NODES_CACHE)) {
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
            // Write to flash if dirty (cold data changed)
            if (evicted_entry->dirty) {
                NodeIndexEntry* index_entry = NodeIndex::findNode(evicted_entry->num);
                if (index_entry) {
                    NodeStorage::writeNodeToSlot(index_entry->slot_id, &evicted_entry->node);
                    NodeIndex::markDirty(evicted_entry->num, false);
                }
            }
            
            // Remove from index cache flag
            NodeIndex::markCached(evicted_entry->num, false);
            
            // Reuse evicted entry for new node
            cache_entry = evicted_entry;
        } else {
            // All nodes are protected - cannot evict
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
    if (!initialized) {
        return false;
    }
    
    CacheEntry* cache_entry = findEntry(nodeNum);
    if (!cache_entry) {
        return false;  // Not in cache
    }
    
    // Write to flash if dirty (cold data changed)
    if (cache_entry->dirty) {
        NodeIndexEntry* index_entry = NodeIndex::findNode(nodeNum);
        if (index_entry) {
            NodeStorage::writeNodeToSlot(index_entry->slot_id, &cache_entry->node);
            NodeIndex::markDirty(nodeNum, false);
        }
    }
    
    // Remove from index cache flag
    NodeIndex::markCached(nodeNum, false);
    
    // Remove from cache (shift entries to fill gap)
    int entry_position = cache_entry - cache_entries;
    if (entry_position < (int)cache_size - 1) {
        memmove(&cache_entries[entry_position], &cache_entries[entry_position + 1],
                (cache_size - entry_position - 1) * sizeof(CacheEntry));
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
 */
void clear()
{
    if (!initialized) {
        return;
    }
    
    // Write all dirty entries to flash
    flushDirtyNodes();
    
    // Clear cache
    for (uint32_t i = 0; i < cache_size; i++) {
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
    if (!initialized) {
        return;
    }
    
    CacheEntry* cache_entry = findEntry(nodeNum);
    if (cache_entry) {
        cache_entry->dirty = true;
        NodeIndex::markDirty(nodeNum, true);
        
        // Enqueue to write queue for background processing
        NodeIndexEntry* index_entry = NodeIndex::findNode(nodeNum);
        if (index_entry && index_entry->slot_id != UINT16_MAX) {
            bool isProtected = (index_entry->flags & NodeIndexEntry::FLAG_PROTECTED) != 0;
            if (NodeDBWriteQueue::isInitialized()) {
                NodeDBWriteQueue::enqueue(nodeNum, index_entry->slot_id, isProtected);
            }
        }
    }
}

/**
 * @brief Clear dirty flag for a node
 */
void clearDirty(NodeNum nodeNum)
{
    if (!initialized) {
        return;
    }
    
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

#endif // USE_EXTENDED_FS_FOR_NODEDB

