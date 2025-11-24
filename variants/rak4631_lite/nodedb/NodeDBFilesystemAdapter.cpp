/**
 * @file NodeDBFilesystemAdapter.cpp
 * @brief Adapter implementation for integrating extended filesystem with NodeDB
 * 
 * This adapter provides a clean interface for NodeDB to use extended filesystem
 * when available, with fallback to standard filesystem.
 */

#include "NodeDBFilesystemAdapter.h"
#include "NodeDBExtendedFS.h"
#include "../../../src/FSCommon.h"
#include "../../../src/mesh/NodeDB.h"
#include "../../../src/mesh/mesh-pb-constants.h"
#include "../../../src/SafeFile.h"
#include <pb_decode.h>
#include <pb_encode.h>

#ifdef USE_EXTENDED_FS_FOR_NODEDB

namespace NodeDBFilesystemAdapter {

LoadFileResult loadProto(const char *filename, size_t protoSize, size_t objSize, 
                         const pb_msgdesc_t *fields, void *dest_struct)
{
    // Check if this is nodes.proto and if extended filesystem should be used
    if (NodeDBExtendedFS::useExtendedFSForNodeDB() && NodeDBExtendedFS::isNodeDBFile(filename)) {
        // Use extended filesystem for nodes.proto
        return NodeDBExtendedFS::loadFromExtendedFS(filename, protoSize, objSize, fields, dest_struct);
    }
    
    // Use main filesystem (standard behavior)
    auto f = FSCom.open(filename, FILE_O_READ);
    
    if (f) {
        LOG_INFO("Load %s", filename);
        pb_istream_t stream = {&readcb, &f, protoSize};
        
        memset(dest_struct, 0, objSize);
        if (!pb_decode(&stream, fields, dest_struct)) {
            LOG_ERROR("Error: can't decode protobuf %s", PB_GET_ERROR(&stream));
            f.close();
            return LoadFileResult::DECODE_FAILED;
        } else {
            LOG_INFO("Loaded %s successfully", filename);
            f.close();
            return LoadFileResult::LOAD_SUCCESS;
        }
    } else {
        LOG_ERROR("Could not open / read %s", filename);
        return LoadFileResult::NOT_FOUND;
    }
}

bool saveProto(const char *filename, size_t protoSize, const pb_msgdesc_t *fields, 
               const void *dest_struct, bool fullAtomic)
{
    // Check if this is nodes.proto and if extended filesystem should be used
    // IMPORTANT: Only use extended filesystem for nodes.proto (other nodes from network)
    // All other files (config.proto, device.proto, module.proto, channels.proto, uiconfig.proto)
    // MUST go to main filesystem to ensure current node configuration is always saved
    // NO FALLBACK: If extended filesystem fails, nodes.proto simply won't be saved (but system continues)
    if (NodeDBExtendedFS::useExtendedFSForNodeDB() && NodeDBExtendedFS::isNodeDBFile(filename)) {
        // Use extended filesystem for nodes.proto (other nodes from network only)
        return NodeDBExtendedFS::saveToExtendedFS(filename, protoSize, fields, dest_struct, fullAtomic);
    }
    
    // Use main filesystem for all other files
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
    } else {
        // Verify file was saved to correct filesystem
        if (NodeDBExtendedFS::useExtendedFSForNodeDB() && NodeDBExtendedFS::isNodeDBFile(filename)) {
            // Verify nodes.proto is NOT in main filesystem (should be in extended)
            if (FSCom.exists(filename)) {
                LOG_ERROR("VERIFY FAILED: %s was found in MAIN filesystem - this should not happen!", filename);
                LOG_ERROR("nodes.proto should ONLY be in extended filesystem!");
            }
        }
        return true;
    }
}

void initExtendedFilesystem()
{
    // Initialize extended filesystem (called from FSCommon)
    // This is already handled by the existing NodeDB-extended-fs.cpp implementation
    // Just ensure it's initialized
    NodeDBExtendedFS::useExtendedFSForNodeDB();
}

} // namespace NodeDBFilesystemAdapter

#endif // USE_EXTENDED_FS_FOR_NODEDB

