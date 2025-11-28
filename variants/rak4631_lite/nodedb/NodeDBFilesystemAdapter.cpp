/**
 * @file NodeDBFilesystemAdapter.cpp
 * @brief Adapter implementation for integrating extended filesystem with NodeDB
 * 
 * This adapter provides a clean interface for NodeDB to use extended filesystem
 * when available, with fallback to standard filesystem.
 */

#include "NodeDBFilesystemAdapter.h"
#include "NodeDBExtendedFSImpl.h"
#include "FSCommon.h"
#include "mesh/NodeDB.h"
#include "mesh/mesh-pb-constants.h"
#include "mesh/Default.h"
#include "SafeFile.h"
#include "SPILock.h"
#ifdef ARCH_NRF52
#include "mesh/RadioLibInterface.h"
#endif
#include <pb_decode.h>
#include <pb_encode.h>
#include <pb.h>
#include <Arduino.h>

#ifdef USE_EXTENDED_FS_FOR_NODEDB

namespace NodeDBFilesystemAdapter {

LoadFileResult loadProto(const char *filename, size_t protoSize, size_t objSize, 
                         const pb_msgdesc_t *fields, void *dest_struct)
{
    // Check if this is nodes.proto and if extended filesystem should be used
    if (useExtendedFSForNodeDB() && isNodeDBFile(filename)) {
        // CRITICAL OPTIMIZATION: For extended filesystem, ignore protoSize parameter
        // and use real file size instead. This prevents excessive memory allocation
        // (getMaxNodesAllocatedSize() can be 227KB, but file might be only 3 bytes).
        // loadFromExtendedFS() will determine actual file size and allocate only what's needed.
        // Pass 0 as protoSize to indicate "use file size" instead of pre-allocated size.
        return loadFromExtendedFS(filename, 0, objSize, fields, dest_struct);
    }
    
    // Use main filesystem (standard behavior) - delegate to standard NodeDB implementation
    // This avoids duplicating the standard logic
    LoadFileResult state = LoadFileResult::OTHER_FAILURE;
#ifdef FSCom
    concurrency::LockGuard g(spiLock);

    auto f = FSCom.open(filename, FILE_O_READ);

    if (f) {
        LOG_INFO("Load %s", filename);
        pb_istream_t stream = {&readcb, &f, protoSize};

        memset(dest_struct, 0, objSize);
        if (!pb_decode(&stream, fields, dest_struct)) {
            LOG_ERROR("Error: can't decode protobuf %s", PB_GET_ERROR(&stream));
            state = LoadFileResult::DECODE_FAILED;
        } else {
            LOG_INFO("Loaded %s successfully", filename);
            state = LoadFileResult::LOAD_SUCCESS;
        }
        f.close();
    } else {
        LOG_ERROR("Could not open / read %s", filename);
    }
#else
    LOG_ERROR("ERROR: Filesystem not implemented");
    state = LoadFileResult::NO_FILESYSTEM;
#endif
    return state;
}

bool saveProto(const char *filename, size_t protoSize, const pb_msgdesc_t *fields, 
               const void *dest_struct, bool fullAtomic)
{
    // Check if this is nodes.proto and if extended filesystem should be used
    // IMPORTANT: Only use extended filesystem for nodes.proto (other nodes from network)
    // All other files (config.proto, device.proto, module.proto, channels.proto, uiconfig.proto)
    // MUST go to main filesystem to ensure current node configuration is always saved
    // NO FALLBACK: If extended filesystem fails, nodes.proto simply won't be saved (but system continues)
    if (useExtendedFSForNodeDB() && isNodeDBFile(filename)) {
        // Use extended filesystem for nodes.proto (other nodes from network only)
        return saveToExtendedFS(filename, protoSize, fields, dest_struct, fullAtomic);
    }
    
    // Use main filesystem for all other files - delegate to standard NodeDB implementation
    // This avoids duplicating the standard logic
#ifdef FSCom
    auto f = SafeFile(filename, fullAtomic);

    LOG_INFO("Save %s", filename);
    pb_ostream_t stream = {&writecb, static_cast<Print *>(&f), protoSize};

    bool okay = false;
    if (!pb_encode(&stream, fields, dest_struct)) {
        LOG_ERROR("Error: can't encode protobuf %s", PB_GET_ERROR(&stream));
    } else {
        okay = true;
    }

    bool writeSucceeded = f.close();

    if (!okay || !writeSucceeded) {
        LOG_ERROR("Can't write prefs!");
        return false;
    }
    return true;
#else
    LOG_ERROR("ERROR: Filesystem not implemented");
    return false;
#endif
}

void initExtendedFilesystem()
{
    // Initialize extended filesystem (called from FSCommon)
    // This is already handled by the existing NodeDBExtendedFSImpl.cpp implementation
    // Just ensure it's initialized
    useExtendedFSForNodeDB();
}

bool shouldProceedWithSave(uint32_t lastSaveTime)
{
    // Note: Throttling is already handled in updateUser() and addFromContact()
    // This function only checks radio state for variant-specific optimization
    
    // Always check radio state - avoid writing during active packet reception
    // Writing during transmission is OK (reception is disabled anyway)
    // But writing during reception can cause packet loss
#ifdef ARCH_NRF52
    if (RadioLibInterface::instance != nullptr) {
        // Check if radio is actively receiving a packet
        if (RadioLibInterface::instance->isActivelyReceiving()) {
            // Wait a bit for reception to complete (typical LoRa packet is 10-100ms)
            // But don't wait too long - max 200ms
            uint32_t wait_start = millis();
            uint32_t max_wait = 200; // Maximum wait time in ms
            
            while (RadioLibInterface::instance->isActivelyReceiving() && 
                   (millis() - wait_start) < max_wait) {
                yield(); // Allow other tasks to run
                delay(10); // Small delay to avoid busy-wait
            }
            
            // If still receiving after wait, log warning but proceed with save
            // (better to save than lose data, and reception might be stuck)
            if (RadioLibInterface::instance->isActivelyReceiving()) {
                LOG_WARN("NodeDB save: Radio still receiving after %u ms wait - proceeding with save anyway", 
                        millis() - wait_start);
            } else {
                LOG_DEBUG("NodeDB save: Waited %u ms for reception to complete", 
                         millis() - wait_start);
            }
        }
        // Note: We don't check isSending() - writing during transmission is fine
        // because reception is disabled during transmission anyway
    }
#endif
    
    return true; // Proceed with save
}

bool saveNodeDatabaseToDisk(uint32_t &lastNodeDbSave)
{
    // Check radio state (throttling is already handled in updateUser() and addFromContact())
    if (!shouldProceedWithSave(lastNodeDbSave)) {
        return true; // Return success to avoid error handling, but don't actually save
    }
    
#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();
#endif
    size_t nodeDatabaseSize;
    pb_get_encoded_size(&nodeDatabaseSize, meshtastic_NodeDatabase_fields, &nodeDatabase);
    bool saveResult = saveProto(nodeDatabaseFileName, nodeDatabaseSize, &meshtastic_NodeDatabase_msg, &nodeDatabase, false);

    // IMPORTANT: Update lastNodeDbSave timestamp for throttling in updateUser()
    // This is needed because saveNodeDatabaseToDisk() can be called directly from:
    // - resetNodes() - when resetting all nodes
    // - removeNodeByNum() - when removing a node
    // - verifyNodePubKey() - when verifying a node key
    // Without updating this timestamp, updateUser() throttling won't work correctly
    // (it will think last save was long ago, even if we just saved via resetNodes())
    if (saveResult) {
        lastNodeDbSave = millis();
    }

    return saveResult;
}

} // namespace NodeDBFilesystemAdapter

#endif // USE_EXTENDED_FS_FOR_NODEDB

// Note: shouldProceedWithSave() is only needed for extended FS variant (radio state checking)
// For other variants, throttling is handled in updateUser() and addFromContact()

