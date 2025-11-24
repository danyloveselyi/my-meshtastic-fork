/**
 * @file NodeDBFilesystemAdapter.h
 * @brief Adapter for integrating extended filesystem with NodeDB
 * 
 * This adapter provides a clean interface for NodeDB to use extended filesystem
 * when available, with fallback to standard filesystem.
 */

#pragma once

#include "configuration.h"
#include "mesh/NodeDB.h"

#ifdef USE_EXTENDED_FS_FOR_NODEDB

namespace NodeDBFilesystemAdapter {

/**
 * @brief Load protobuf file (with extended FS support if available)
 * @param filename File path to load
 * @param protoSize Expected size of protobuf
 * @param objSize Size of destination object
 * @param fields Protobuf field descriptors
 * @param dest_struct Destination structure pointer
 * @return LoadFileResult indicating success or failure
 */
LoadFileResult loadProto(const char *filename, size_t protoSize, size_t objSize, 
                         const pb_msgdesc_t *fields, void *dest_struct);

/**
 * @brief Save protobuf file (with extended FS support if available)
 * @param filename File path to save
 * @param protoSize Expected size of protobuf
 * @param fields Protobuf field descriptors
 * @param dest_struct Source structure pointer
 * @param fullAtomic Whether to use atomic write
 * @return true if save was successful
 */
bool saveProto(const char *filename, size_t protoSize, const pb_msgdesc_t *fields, 
               const void *dest_struct, bool fullAtomic);

/**
 * @brief Initialize extended filesystem (called from FSCommon)
 */
void initExtendedFilesystem();

} // namespace NodeDBFilesystemAdapter

// C wrapper for initialization (must be outside namespace for extern "C")
extern "C" void initExtendedFilesystemForNodeDB();

#endif // USE_EXTENDED_FS_FOR_NODEDB

