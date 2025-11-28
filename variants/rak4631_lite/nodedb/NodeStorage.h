/**
 * @file NodeStorage.h
 * @brief Slot-based flash storage for NodeDB nodes
 * 
 * This module provides slot-based storage for individual nodes using LittleFS.
 * Each node is stored as a separate file to enable wear leveling via copy-on-write.
 * 
 * Key features:
 * - Individual slots stored as files: /nodes/slot_XXXX.bin
 * - Copy-on-write strategy: updates create new file, delete old (wear leveling)
 * - Fixed slot size: 256 bytes per node
 * - Maximum ~1234 slots (limited by FS overhead, not fixed allocation)
 */

#pragma once

#include "configuration.h"
#include "mesh/NodeDB.h"

#ifdef USE_EXTENDED_FS_FOR_NODEDB

#include <cstdint>
#include <cstddef>

namespace NodeStorage {

/**
 * @brief Allocate a new slot for a node
 * @param nodeNum Node number to allocate slot for
 * @return Slot ID (0-1233) or UINT16_MAX on failure
 * 
 * Finds the next available slot and creates the slot file.
 */
uint16_t allocateSlot(uint32_t nodeNum);

/**
 * @brief Write node data to a slot
 * @param slotId Slot ID to write to
 * @param node Pointer to NodeInfoLite structure
 * @return true if write was successful
 * 
 * Uses copy-on-write strategy:
 * 1. Creates new file: /nodes/slot_XXXX.bin.new
 * 2. Writes node data
 * 3. Deletes old file: /nodes/slot_XXXX.bin
 * 4. Renames new file: slot_XXXX.bin.new -> slot_XXXX.bin
 * 
 * This ensures LittleFS automatically uses different blocks (wear leveling).
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
 * @brief Initialize slot storage (create /nodes directory if needed)
 * @return true if initialization was successful
 */
bool initialize();

/**
 * @brief Get slot filename for a given slot ID
 * @param slotId Slot ID
 * @param buffer Output buffer (must be at least 32 bytes)
 * @param bufferSize Size of buffer
 * @return Pointer to buffer, or nullptr on error
 */
const char* getSlotFilename(uint16_t slotId, char* buffer, size_t bufferSize);

} // namespace NodeStorage

#endif // USE_EXTENDED_FS_FOR_NODEDB

