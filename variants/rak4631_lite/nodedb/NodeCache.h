/**
 * @file NodeCache.h
 * @brief LRU/LFU cache for active nodes (200-300 nodes in RAM)
 * 
 * This module provides a RAM cache for frequently accessed nodes.
 * It integrates with NodeIndex to track which nodes are cached.
 * 
 * Key features:
 * - LRU/LFU eviction policy
 * - Protected nodes never evicted (local, router, favorite)
 * - Maximum 200-300 nodes in cache (~50-75 KB RAM)
 * - Automatic loading from flash when cache miss
 */

#pragma once

#include "configuration.h"
#include "mesh/NodeDB.h"
#include "NodeIndex.h"
#include <cstdint>
#include <cstddef>

#ifdef USE_EXTENDED_FS_FOR_NODEDB

// meshtastic_NodeInfoLite is defined in mesh/NodeDB.h (already included via configuration.h)
typedef uint32_t NodeNum;

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
 * @brief Clear all entries from cache (write dirty entries to flash first)
 */
void clear();

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

#endif // USE_EXTENDED_FS_FOR_NODEDB

