/**
 * @file NodeDBWriteQueue.h
 * @brief Write queue for deferred flash writes with prioritization
 * 
 * This module provides a circular buffer queue for nodes that need to be written to flash.
 * Writes are deferred and processed asynchronously by NodeDBBackgroundTask to avoid
 * blocking the main loop.
 * 
 * Key features:
 * - Circular buffer (50-100 nodes)
 * - Priority-based ordering (protected nodes first)
 * - Throttling (max 1-2 writes per background task cycle)
 * - Thread-safe operations (single producer, single consumer)
 */

#pragma once

#include "configuration.h"
#include "mesh/NodeDB.h"
#include <cstdint>
#include <cstddef>

#ifdef USE_EXTENDED_FS_FOR_NODEDB

// meshtastic_NodeInfoLite is defined in mesh/NodeDB.h
typedef uint32_t NodeNum;

namespace NodeDBWriteQueue {

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
bool initialize(uint32_t maxSize = 100);

/**
 * @brief Check if write queue is initialized
 * @return true if initialized
 */
bool isInitialized();

/**
 * @brief Add a node to the write queue
 * @param nodeNum Node number
 * @param slotId Flash slot ID
 * @param isProtected true if node is protected (will have higher priority)
 * @return true if node was added to queue, false if queue is full
 * 
 * If node already exists in queue, it will be updated with new priority.
 * Protected nodes are added with higher priority and processed first.
 */
bool enqueue(NodeNum nodeNum, uint16_t slotId, bool isProtected = false);

/**
 * @brief Get next node from queue (highest priority first)
 * @param entry Output entry structure
 * @return true if entry was retrieved, false if queue is empty
 * 
 * Removes entry from queue. Protected nodes are returned first.
 */
bool dequeue(WriteQueueEntry& entry);

/**
 * @brief Check if queue is empty
 * @return true if queue is empty
 */
bool isEmpty();

/**
 * @brief Check if queue is full
 * @return true if queue is full
 */
bool isFull();

/**
 * @brief Get current queue size
 * @return Number of entries in queue
 */
size_t getSize();

/**
 * @brief Get maximum queue size
 * @return Maximum number of entries
 */
size_t getMaxSize();

/**
 * @brief Clear all entries from queue
 */
void clear();

/**
 * @brief Check if a node is already in queue
 * @param nodeNum Node number to check
 * @return true if node is in queue
 */
bool contains(NodeNum nodeNum);

} // namespace NodeDBWriteQueue

#endif // USE_EXTENDED_FS_FOR_NODEDB



