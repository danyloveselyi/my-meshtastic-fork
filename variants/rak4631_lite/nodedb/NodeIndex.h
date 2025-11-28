/**
 * @file NodeIndex.h
 * @brief Compact RAM index for tracking all nodes (up to MAX_NODES_SLOTS)
 * 
 * This module provides a compact index structure in RAM that tracks all nodes,
 * their flash locations, and metadata for cache management.
 * 
 * Key features:
 * - Compact entry structure: ~16 bytes per node
 * - Maximum 1234 entries = ~20 KB RAM (must be <= MAX_NODES_SLOTS)
 * - Fast lookup via sorted array (binary search)
 * - Tracks hot/cold status, protected nodes, LRU/LFU metadata
 * 
 * IMPORTANT: MAX_NODES_INDEX must be <= MAX_NODES_SLOTS because each index
 * entry points to a flash slot. If slots=1234, index cannot exceed 1234.
 */

#pragma once

#include "configuration.h"
#include <cstdint>
#include <cstddef>

#ifdef USE_EXTENDED_FS_FOR_NODEDB

// Forward declaration
typedef uint32_t NodeNum;

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
 * 
 * Returns the least recently used, non-protected node.
 * If allowCached is false, skips cached nodes (for cache eviction only).
 * If allowCached is true, can return cached nodes (for flash full scenario).
 * If all nodes are protected, returns 0.
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
 * @brief Get number of protected nodes
 * @return Number of nodes marked as protected
 */
uint32_t getProtectedNodeCount();

/**
 * @brief Get number of dirty nodes (need flash write)
 * @return Number of nodes marked as dirty
 */
uint32_t getDirtyNodeCount();

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
 * 
 * This function avoids allocating a large array on the stack by accessing
 * index entries directly. Use this instead of getAllNodeNums() when you
 * only need one node at a time.
 */
NodeNum getNodeNumByIndex(uint32_t index);

/**
 * @brief Get last_heard timestamp for a node
 * @param nodeNum Node number
 * @return last_heard timestamp, or 0 if node not found
 */
uint32_t getLastHeard(NodeNum nodeNum);

/**
 * @brief Get entry by index (for iteration)
 * @param index Index (0 to getNodeCount()-1)
 * @return Pointer to index entry, or nullptr if index out of range
 */
NodeIndexEntry* getEntryByIndex(uint32_t index);

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

#endif // USE_EXTENDED_FS_FOR_NODEDB

