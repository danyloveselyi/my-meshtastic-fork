/**
 * @file NodeDBExtendedFSImpl.h
 * @brief Implementation header for extended filesystem support for NodeDB
 * 
 * This file provides the core implementation functions for extended filesystem (80 pages, 320 KB)
 * for NodeDB operations, while main filesystem (7 pages, 28 KB) is used for configs.
 */

#pragma once

#include "configuration.h"
#include "mesh/NodeDB.h"

#ifdef ARCH_NRF52

#ifdef USE_EXTENDED_FS_FOR_NODEDB

// Forward declaration (lfs_t is a typedef)
typedef struct lfs lfs_t;

/**
 * @brief Check if extended filesystem should be used for NodeDB
 * @return true if extended filesystem is available and should be used
 */
bool useExtendedFSForNodeDB();

/**
 * @brief Get LittleFS instance for extended filesystem
 * @return Pointer to lfs_t instance, or nullptr if not available
 * Note: lfs_t is a typedef, not a struct
 */
lfs_t* getExtendedFSForNodeDB();

/**
 * @brief Check if a filename is for NodeDB (should use extended filesystem)
 * @param filename File path to check
 * @return true if this file should use extended filesystem
 */
bool isNodeDBFile(const char* filename);

/**
 * @brief Load protobuf from extended filesystem
 * @param filename File path to load
 * @param protoSize Expected size of protobuf
 * @param objSize Size of destination object
 * @param fields Protobuf field descriptors
 * @param dest_struct Destination structure pointer
 * @return LoadFileResult indicating success or failure
 */
LoadFileResult loadFromExtendedFS(const char *filename, size_t protoSize, size_t objSize, 
                                  const pb_msgdesc_t *fields, void *dest_struct);

/**
 * @brief Save protobuf to extended filesystem
 * @param filename File path to save
 * @param protoSize Expected size of protobuf
 * @param fields Protobuf field descriptors
 * @param dest_struct Source structure pointer
 * @param fullAtomic Whether to use atomic write
 * @return true if save was successful
 */
bool saveToExtendedFS(const char *filename, size_t protoSize, const pb_msgdesc_t *fields, 
                      const void *dest_struct, bool fullAtomic);

/**
 * @brief Get extended filesystem statistics for NodeDB
 * @param total Total size in bytes (output)
 * @param used Used size in bytes (output)
 * @param free Free size in bytes (output)
 * @return true if extended filesystem is available and stats are valid
 */
bool getExtendedFSStats(uint32_t* total, uint32_t* used, uint32_t* free);

/**
 * @brief Initialize extended filesystem (C wrapper for extern declaration)
 */
extern "C" void initExtendedFilesystemForNodeDB();

/**
 * @brief Read node slot from extended filesystem
 * @param slotId Slot ID (0-1233)
 * @param buffer Output buffer
 * @param size Buffer size
 * @return true if read was successful
 * 
 * Reads a node slot file: /nodes/slot_XXXX.bin
 */
bool readNodeSlot(uint16_t slotId, void* buffer, size_t size);

/**
 * @brief Write node slot to extended filesystem
 * @param slotId Slot ID (0-1233)
 * @param buffer Input buffer
 * @param size Buffer size
 * @return true if write was successful
 * 
 * Writes a node slot file: /nodes/slot_XXXX.bin
 * Uses copy-on-write for wear leveling.
 */
bool writeNodeSlot(uint16_t slotId, const void* buffer, size_t size);

/**
 * @brief Get slot file count (for tracking free slots)
 * @return Number of slot files that exist
 */
uint32_t getSlotFileCount();

#else
// If USE_EXTENDED_FS_FOR_NODEDB is not defined, provide empty functions
inline bool useExtendedFSForNodeDB() { return false; }
inline void* getExtendedFSForNodeDB() { return nullptr; }
inline bool isNodeDBFile(const char* filename) { (void)filename; return false; }
inline bool getExtendedFSStats(uint32_t* total, uint32_t* used, uint32_t* free) {
    (void)total; (void)used; (void)free;
    return false;
}
#endif // USE_EXTENDED_FS_FOR_NODEDB

#else
// If not ARCH_NRF52, provide empty functions
inline bool useExtendedFSForNodeDB() { return false; }
inline void* getExtendedFSForNodeDB() { return nullptr; }
inline bool isNodeDBFile(const char* filename) { (void)filename; return false; }
inline bool getExtendedFSStats(uint32_t* total, uint32_t* used, uint32_t* free) {
    (void)total; (void)used; (void)free;
    return false;
}
#endif // ARCH_NRF52

