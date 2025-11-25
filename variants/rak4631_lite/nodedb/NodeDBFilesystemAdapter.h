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

/**
 * @brief Check if NodeDB save should proceed (radio state checking for variant-specific optimization)
 * @param lastSaveTime Timestamp of last save (millis()) - not used, kept for compatibility
 * @return true if save should proceed, false if it should be deferred
 * 
 * Note: Throttling is already handled in updateUser() and addFromContact()
 * This function only checks radio state to avoid writing during packet reception
 */
bool shouldProceedWithSave(uint32_t lastSaveTime);

/**
 * @brief Save NodeDB to disk with variant-specific optimizations
 * This function handles radio state checking and actual save operation
 * @param lastNodeDbSave Reference to last save timestamp (updated on success)
 *                       This timestamp is used by updateUser() for throttling
 *                       (prevents saving more than once per minute)
 * @return true if save was successful
 * 
 * Note: Throttling is already handled in updateUser() and addFromContact()
 * Always checks radio state before saving to avoid packet loss
 * 
 * IMPORTANT: Updates lastNodeDbSave timestamp so that updateUser() throttling
 * works correctly when saveNodeDatabaseToDisk() is called directly from:
 * - resetNodes() - when resetting all nodes
 * - removeNodeByNum() - when removing a node  
 * - verifyNodePubKey() - when verifying a node key
 * 
 * Uses global nodeDatabase and nodeDatabaseFileName from NodeDB
 */
bool saveNodeDatabaseToDisk(uint32_t &lastNodeDbSave);

} // namespace NodeDBFilesystemAdapter

// C wrapper for initialization (must be outside namespace for extern "C")
extern "C" void initExtendedFilesystemForNodeDB();

#endif // USE_EXTENDED_FS_FOR_NODEDB

