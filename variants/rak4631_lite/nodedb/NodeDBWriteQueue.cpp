/**
 * @file NodeDBWriteQueue.cpp
 * @brief Write queue implementation
 */

#include "NodeDBWriteQueue.h"
#include "../../../src/memGet.h"
#ifdef ARCH_NRF52
#include "rtos.h"
#endif
#include <cstring>
#include <algorithm>

#ifdef USE_EXTENDED_FS_FOR_NODEDB

namespace NodeDBWriteQueue {

// Circular buffer for write queue
static WriteQueueEntry* queue_buffer = nullptr;
static uint32_t max_queue_size = 0;
static uint32_t queue_head = 0;  // Write position (next to add)
static uint32_t queue_tail = 0;  // Read position (next to remove)
static uint32_t queue_count = 0;  // Current number of entries
static bool initialized = false;

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
    if (!initialized || queue_count == 0) {
        return UINT32_MAX;
    }
    
    for (uint32_t i = 0; i < queue_count; i++) {
        uint32_t idx = (queue_tail + i) % max_queue_size;
        if (queue_buffer[idx].nodeNum == nodeNum) {
            return idx;
        }
    }
    
    return UINT32_MAX;
}

/**
 * @brief Initialize the write queue
 */
bool initialize(uint32_t maxSize)
{
    if (initialized) {
        return true;  // Already initialized
    }
    
    if (maxSize == 0 || maxSize > 200) {
        return false;  // Sanity check (max 200 entries)
    }
    
    // Check available memory
    uint32_t freeHeap = memGet.getFreeHeap();
    uint32_t heapTotal = memGet.getHeapSize();
    uint32_t usedHeap = (heapTotal > freeHeap) ? (heapTotal - freeHeap) : 0;
    uint32_t requiredMemory = maxSize * sizeof(WriteQueueEntry);
    // Increased safety margin from 10 KB to 20 KB for better reliability
    uint32_t safetyMargin = 20480;  // 20 KB safety margin
    uint32_t totalNeeded = requiredMemory + safetyMargin;
    
    LOG_DEBUG("NodeDBWriteQueue: Memory check - required: %u bytes, margin: %u bytes, total needed: %u bytes", 
             requiredMemory, safetyMargin, totalNeeded);
    LOG_DEBUG("NodeDBWriteQueue: Memory status - free: %u bytes, used: %u bytes, total: %u bytes", 
             freeHeap, usedHeap, heapTotal);
    
    if (freeHeap < totalNeeded) {
        LOG_ERROR("NodeDBWriteQueue: Not enough memory to allocate queue");
        LOG_ERROR("NodeDBWriteQueue:   Required: %u bytes (queue) + %u bytes (margin) = %u bytes total", 
                 requiredMemory, safetyMargin, totalNeeded);
        LOG_ERROR("NodeDBWriteQueue:   Available: %u bytes free, %u bytes used, %u bytes total", 
                 freeHeap, usedHeap, heapTotal);
        LOG_ERROR("NodeDBWriteQueue:   Shortage: %u bytes", totalNeeded - freeHeap);
        return false;
    }
    
    LOG_DEBUG("NodeDBWriteQueue: Memory check PASSED - sufficient memory available");
    
    // Allocate queue buffer
    uint32_t allocation_size = maxSize * sizeof(WriteQueueEntry);
    LOG_DEBUG("NodeDBWriteQueue: Allocating %u bytes for %u queue entries", allocation_size, maxSize);
    
    #ifdef ARCH_NRF52
    queue_buffer = (WriteQueueEntry*)rtos_malloc(allocation_size);
    #else
    queue_buffer = new WriteQueueEntry[maxSize];
    #endif
    
    if (!queue_buffer) {
        uint32_t freeHeapAfter = memGet.getFreeHeap();
        LOG_ERROR("NodeDBWriteQueue: Allocation FAILED");
        LOG_ERROR("NodeDBWriteQueue:   Requested: %u bytes (%u entries * %u bytes)", 
                 allocation_size, maxSize, sizeof(WriteQueueEntry));
        LOG_ERROR("NodeDBWriteQueue:   Free heap before: %u bytes, after: %u bytes", freeHeap, freeHeapAfter);
        return false;
    }
    
    uint32_t freeHeapAfter = memGet.getFreeHeap();
    uint32_t allocatedMemory = freeHeap - freeHeapAfter;
    
    max_queue_size = maxSize;
    queue_head = 0;
    queue_tail = 0;
    queue_count = 0;
    
    LOG_INFO("NodeDBWriteQueue: Initialized successfully");
    LOG_DEBUG("NodeDBWriteQueue:   Max size: %u entries", max_queue_size);
    LOG_DEBUG("NodeDBWriteQueue:   Entry size: %u bytes", sizeof(WriteQueueEntry));
    LOG_DEBUG("NodeDBWriteQueue:   Total allocated: %u bytes", allocation_size);
    LOG_DEBUG("NodeDBWriteQueue:   Free heap before: %u bytes, after: %u bytes, allocated: %u bytes", 
             freeHeap, freeHeapAfter, allocatedMemory);
    initialized = true;
    
    memset(queue_buffer, 0, sizeof(WriteQueueEntry) * max_queue_size);
    
    LOG_DEBUG("NodeDBWriteQueue: Initialized with max size %u", maxSize);
    return true;
}

/**
 * @brief Check if write queue is initialized
 */
bool isInitialized()
{
    return initialized;
}

/**
 * @brief Add a node to the write queue
 */
bool enqueue(NodeNum nodeNum, uint16_t slotId, bool isProtected)
{
    if (!initialized) {
        if (!initialize(100)) {
            return false;
        }
    }
    
    if (nodeNum == 0) {
        return false;  // Invalid node number
    }
    
    // Check if node already in queue - update it instead
    uint32_t existing_idx = findEntryIndex(nodeNum);
    if (existing_idx != UINT32_MAX) {
        // Update existing entry with new priority
        queue_buffer[existing_idx].slotId = slotId;
        queue_buffer[existing_idx].priority = calculatePriority(isProtected);
        queue_buffer[existing_idx].isProtected = isProtected;
        LOG_DEBUG("NodeDBWriteQueue: Updated entry for node 0x%x (priority: %u)", 
                 nodeNum, queue_buffer[existing_idx].priority);
        return true;
    }
    
    // Check if queue is full
    if (queue_count >= max_queue_size) {
        LOG_WARN("NodeDBWriteQueue: Queue full (%u/%u), cannot add node 0x%x", 
                queue_count, max_queue_size, nodeNum);
        return false;
    }
    
    // Add new entry at head
    queue_buffer[queue_head].nodeNum = nodeNum;
    queue_buffer[queue_head].slotId = slotId;
    queue_buffer[queue_head].priority = calculatePriority(isProtected);
    queue_buffer[queue_head].isProtected = isProtected;
    
    queue_head = (queue_head + 1) % max_queue_size;
    queue_count++;
    
    LOG_DEBUG("NodeDBWriteQueue: Enqueued node 0x%x (slot: %u, priority: %u, queue: %u/%u)", 
             nodeNum, slotId, queue_buffer[(queue_head - 1 + max_queue_size) % max_queue_size].priority, 
             queue_count, max_queue_size);
    
    return true;
}

/**
 * @brief Get next node from queue (highest priority first)
 */
bool dequeue(WriteQueueEntry& entry)
{
    if (!initialized || queue_count == 0) {
        return false;
    }
    
    // Find entry with highest priority
    uint32_t best_idx = queue_tail;
    uint32_t best_priority = queue_buffer[queue_tail].priority;
    
    for (uint32_t i = 1; i < queue_count; i++) {
        uint32_t idx = (queue_tail + i) % max_queue_size;
        if (queue_buffer[idx].priority > best_priority) {
            best_idx = idx;
            best_priority = queue_buffer[idx].priority;
        }
    }
    
    // Copy entry
    entry = queue_buffer[best_idx];
    
    // Remove entry by shifting others
    if (best_idx != queue_tail) {
        // Shift entries from tail to best_idx
        if (best_idx > queue_tail) {
            // Simple case: shift left
            memmove(&queue_buffer[queue_tail + 1], &queue_buffer[queue_tail],
                    (best_idx - queue_tail) * sizeof(WriteQueueEntry));
        } else {
            // Wrap-around case: shift in two parts
            // Part 1: from tail to end
            uint32_t part1_size = max_queue_size - queue_tail;
            WriteQueueEntry temp;
            memcpy(&temp, &queue_buffer[queue_tail], sizeof(WriteQueueEntry));
            memmove(&queue_buffer[queue_tail], &queue_buffer[queue_tail + 1],
                    (part1_size - 1) * sizeof(WriteQueueEntry));
            // Part 2: from 0 to best_idx
            if (best_idx > 0) {
                memmove(&queue_buffer[best_idx - 1], &queue_buffer[best_idx],
                        best_idx * sizeof(WriteQueueEntry));
                memcpy(&queue_buffer[max_queue_size - 1], &temp, sizeof(WriteQueueEntry));
            }
        }
    }
    
    // Update tail and count
    queue_tail = (queue_tail + 1) % max_queue_size;
    queue_count--;
    
    LOG_DEBUG("NodeDBWriteQueue: Dequeued node 0x%x (slot: %u, priority: %u, remaining: %u)", 
             entry.nodeNum, entry.slotId, entry.priority, queue_count);
    
    return true;
}

/**
 * @brief Check if queue is empty
 */
bool isEmpty()
{
    return !initialized || queue_count == 0;
}

/**
 * @brief Check if queue is full
 */
bool isFull()
{
    return initialized && queue_count >= max_queue_size;
}

/**
 * @brief Get current queue size
 */
size_t getSize()
{
    if (!initialized) {
        return 0;
    }
    return queue_count;
}

/**
 * @brief Get maximum queue size
 */
size_t getMaxSize()
{
    return max_queue_size;
}

/**
 * @brief Clear all entries from queue
 */
void clear()
{
    if (!initialized) {
        return;
    }
    
    queue_head = 0;
    queue_tail = 0;
    queue_count = 0;
    memset(queue_buffer, 0, sizeof(WriteQueueEntry) * max_queue_size);
    
    LOG_DEBUG("NodeDBWriteQueue: Cleared all entries");
}

/**
 * @brief Check if a node is already in queue
 */
bool contains(NodeNum nodeNum)
{
    return findEntryIndex(nodeNum) != UINT32_MAX;
}

} // namespace NodeDBWriteQueue

#endif // USE_EXTENDED_FS_FOR_NODEDB



