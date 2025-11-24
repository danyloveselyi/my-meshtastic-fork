/**
 * @file NodeDBExtendedFS.h
 * @brief Extended filesystem support interface for NodeDB
 * 
 * This header provides the interface for working with extended filesystem
 * (80 pages, 320 KB) for NodeDB operations on RAK4631 Lite variant.
 */

#pragma once

#include "configuration.h"
#include "mesh/NodeDB.h"

#ifdef USE_EXTENDED_FS_FOR_NODEDB

#include "../../../src/platform/stm32wl/littlefs/lfs.h"

// Forward declaration
typedef struct lfs lfs_t;

namespace NodeDBExtendedFS {

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
 * @brief Check if extended filesystem should be used for NodeDB
 * @return true if extended filesystem is available and should be used
 */
bool useExtendedFSForNodeDB();

/**
 * @brief Get LittleFS instance for extended filesystem
 * @return Pointer to lfs_t instance, or nullptr if not available
 */
lfs_t* getExtendedFSForNodeDB();

/**
 * @brief Check if a filename is for NodeDB (should use extended filesystem)
 * @param filename File path to check
 * @return true if this file should use extended filesystem
 */
bool isNodeDBFile(const char* filename);

} // namespace NodeDBExtendedFS

#endif // USE_EXTENDED_FS_FOR_NODEDB

