/**
 * @file NodeDBVirtualBackend.h
 * @brief Virtual NodeDB backend for RAK4631 Lite (1000-1500 nodes)
 * 
 * This module provides a virtual backend for NodeDB that enables scaling
 * to 1000-1500 nodes by using a RAM cache + flash backend architecture.
 * 
 * Architecture:
 * - Flash Backend: Individual slots for 1000-1500 nodes
 * - RAM Cache: 200-300 active nodes only
 * - Node Index: Compact structure in RAM tracking all nodes
 * - Hot/Cold Split: Critical routing fields in RAM, full data in flash
 */

#pragma once

#include "configuration.h"
#include "mesh/NodeDB.h"

#ifdef USE_EXTENDED_FS_FOR_NODEDB

#include <cstdint>
#include <cstddef>

// meshtastic_NodeInfoLite is defined in mesh/NodeDB.h (already included via configuration.h)
typedef uint32_t NodeNum;

namespace NodeDBVirtualBackend {

/**
 * @brief Initialize the virtual backend
 * @return true if initialization was successful
 */
bool initialize();

/**
 * @brief Check if virtual backend is enabled and initialized
 * @return true if virtual backend is active
 */
bool isEnabled();

/**
 * @brief Check if virtual backend is available (compiled in and can be initialized)
 * @return true if virtual backend is available (even if not yet initialized)
 * 
 * This allows NodeDB to attempt initialization on first use (lazy initialization).
 * Returns true even if backend is not yet initialized, allowing getOrCreateNode()
 * to initialize it on first access.
 */
bool isAvailable();

/**
 * @brief Get a node by node number
 * @param nodeNum Node number
 * @return Pointer to NodeInfoLite, or nullptr if not found
 * 
 * Returns node from cache if available, otherwise loads from flash.
 */
meshtastic_NodeInfoLite* getNode(NodeNum nodeNum);

/**
 * @brief Get or create a node
 * @param nodeNum Node number
 * @return Pointer to NodeInfoLite, or nullptr on failure
 * 
 * Creates new node if it doesn't exist. May evict nodes if cache/flash is full.
 */
meshtastic_NodeInfoLite* getOrCreateNode(NodeNum nodeNum);

/**
 * @brief Update a node from a mesh packet
 * @param nodeNum Node number
 * @param packet Mesh packet containing update data
 * @return true if update was successful
 * 
 * Updates hot data immediately in RAM. Cold data updates are queued.
 */
bool updateFromPacket(NodeNum nodeNum, const meshtastic_MeshPacket* packet);

/**
 * @brief Update user information for a node
 * @param nodeNum Node number
 * @param user User information
 * @return true if update was successful
 * 
 * Updates cold data (user info) and marks node as dirty for flash write.
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
 * 
 * Loads index from flash and populates cache with recent/protected nodes.
 */
bool loadFromDisk();

/**
 * @brief Save dirty nodes to flash
 * @return true if save was successful
 * 
 * Writes all dirty cache entries to flash (throttled to 1 minute).
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
 * 
 * Checks that index entries match actual slot files in flash.
 */
bool validateIndexIntegrity();

/**
 * @brief Rebuild index from flash slots if needed
 * @return true if rebuild was successful or not needed
 * 
 * Rebuilds index by scanning all slot files in flash.
 * This is called automatically if index validation fails.
 */
bool rebuildIndexIfNeeded();

/**
 * @brief Recover from flash errors
 * @return true if recovery was successful
 * 
 * Attempts to recover from flash I/O errors by:
 * - Validating index integrity
 * - Rebuilding index if corrupted
 * - Marking corrupted slots as free
 */
bool recoverFromError();

} // namespace NodeDBVirtualBackend

#endif // USE_EXTENDED_FS_FOR_NODEDB

