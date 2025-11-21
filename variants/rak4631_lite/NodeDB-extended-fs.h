/**
 * @file NodeDB-extended-fs.h
 * @brief Header for extended filesystem support for NodeDB
 * 
 * This file provides functions to use extended filesystem (80 pages, 320 KB)
 * for NodeDB operations, while main filesystem (7 pages, 28 KB) is used for configs.
 */

#pragma once

#include "configuration.h"

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
 * @brief Get extended filesystem statistics for NodeDB
 * @param total Total size in bytes (output)
 * @param used Used size in bytes (output)
 * @param free Free size in bytes (output)
 * @return true if extended filesystem is available and stats are valid
 */
bool getExtendedFSStats(uint32_t* total, uint32_t* used, uint32_t* free);

/**
 * @brief Get extended filesystem statistics for NodeDB
 * @param total Total size in bytes (output)
 * @param used Used size in bytes (output)
 * @param free Free size in bytes (output)
 * @return true if extended filesystem is available and stats are valid
 */
bool getExtendedFSStats(uint32_t* total, uint32_t* used, uint32_t* free);

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

