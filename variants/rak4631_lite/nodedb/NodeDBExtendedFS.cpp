/**
 * @file NodeDBExtendedFS.cpp
 * @brief Extended filesystem support implementation for NodeDB
 * 
 * This file implements the extended filesystem operations for NodeDB,
 * extracted from the main NodeDB.cpp to keep variant-specific logic isolated.
 */

#include "NodeDBExtendedFS.h"
#include "NodeDBExtendedFSImpl.h"  // Use existing implementation
#include "../../../src/FSCommon.h"
#include "../../../src/mesh/NodeDB.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <cstring>

#ifdef USE_EXTENDED_FS_FOR_NODEDB

// Structure to hold LittleFS file context for streaming protobuf encoding
struct LfsFileContext {
    lfs_t* lfs;
    lfs_file_t* file;
};

// Streaming callback for protobuf encoding to LittleFS file (no buffer allocation needed)
static bool lfs_writecb(pb_ostream_t *stream, const pb_byte_t *buf, size_t count)
{
    LfsFileContext* ctx = (LfsFileContext*)stream->state;
    if (!ctx || !ctx->lfs || !ctx->file) {
        return false;
    }
    
    lfs_ssize_t written = lfs_file_write(ctx->lfs, ctx->file, buf, count);
    return (written == (lfs_ssize_t)count);
}

namespace NodeDBExtendedFS {

LoadFileResult loadFromExtendedFS(const char *filename, size_t protoSize, size_t objSize, 
                                  const pb_msgdesc_t *fields, void *dest_struct)
{
    LoadFileResult state = LoadFileResult::OTHER_FAILURE;
    
    // Use extended filesystem for nodes.proto
    LOG_INFO("Load %s from EXTENDED filesystem (80 pages, 320 KB)", filename);
    
    // Cast to lfs_t* (headers already included at top of file)
    lfs_t* extended_lfs = ::getExtendedFSForNodeDB();
    if (extended_lfs) {
        // Read file into buffer first, then decode (more reliable than using readcb wrapper)
        // This avoids compatibility issues with File* vs LittleFSFileWrapper*
        LOG_DEBUG("Load: Step 1: Opening file '%s' for reading from extended filesystem...", filename);
        uint32_t load_start_time = millis();
        lfs_file_t file;
        uint32_t open_start = millis();
        int open_result = lfs_file_open(extended_lfs, &file, filename, LFS_O_RDONLY);
        uint32_t open_time = millis() - open_start;
        
        if (open_result == LFS_ERR_OK) {
            LOG_DEBUG("Load: Step 1 SUCCESS: File '%s' opened (took %u ms)", filename, open_time);
            
            // Get file size
            LOG_DEBUG("Load: Step 2: Getting file size for '%s'...", filename);
            uint32_t size_start = millis();
            lfs_soff_t file_size = lfs_file_size(extended_lfs, &file);
            uint32_t size_time = millis() - size_start;
            
            if (file_size >= 0 && file_size <= 512 * 1024) {  // Max 512 KB (safety limit)
                LOG_DEBUG("Load: Step 2 SUCCESS: File '%s' size: %d bytes (took %u ms)", filename, (int)file_size, size_time);
                
                // Allocate buffer for file content
                LOG_DEBUG("Load: Step 3: Allocating buffer for '%s' (%d bytes)...", filename, (int)file_size);
                uint32_t alloc_start = millis();
                uint8_t* file_buffer = (uint8_t*)malloc(file_size);
                uint32_t alloc_time = millis() - alloc_start;
                
                if (file_buffer) {
                    LOG_DEBUG("Load: Step 3 SUCCESS: Buffer allocated (took %u ms)", alloc_time);
                    
                    // Read entire file
                    LOG_DEBUG("Load: Step 4: Reading %d bytes from '%s'...", (int)file_size, filename);
                    uint32_t read_start = millis();
                    lfs_ssize_t read_result = lfs_file_read(extended_lfs, &file, file_buffer, file_size);
                    uint32_t read_time = millis() - read_start;
                    lfs_file_close(extended_lfs, &file);
                    
                    if (read_result == file_size) {
                        LOG_DEBUG("Load: Step 4 SUCCESS: Read %d bytes (took %u ms)", (int)read_result, read_time);
                        
                        // Decode from buffer
                        LOG_DEBUG("Load: Step 5: Decoding protobuf from '%s'...", filename);
                        uint32_t decode_start = millis();
                        pb_istream_t stream = pb_istream_from_buffer(file_buffer, file_size);
                        memset(dest_struct, 0, objSize);
                        if (!pb_decode(&stream, fields, dest_struct)) {
                            LOG_ERROR("Load: Step 5 FAILED: Can't decode protobuf %s: %s", filename, PB_GET_ERROR(&stream));
                            state = LoadFileResult::DECODE_FAILED;
                        } else {
                            uint32_t decode_time = millis() - decode_start;
                            uint32_t total_time = millis() - load_start_time;
                            LOG_INFO("Load: Step 5 SUCCESS: Loaded %s successfully from EXTENDED filesystem (%d bytes, decode: %u ms, total: %u ms)", 
                                    filename, (int)file_size, decode_time, total_time);
                            state = LoadFileResult::LOAD_SUCCESS;
                        }
                    } else {
                        LOG_ERROR("Load: Step 4 FAILED: Read %d of %d bytes from '%s' (took %u ms)", 
                                 (int)read_result, (int)file_size, filename, read_time);
                        state = LoadFileResult::OTHER_FAILURE;
                    }
                    free(file_buffer);
                } else {
                    LOG_ERROR("Load: Step 3 FAILED: Failed to allocate buffer for '%s' (size: %d bytes, took %u ms)", 
                             filename, (int)file_size, alloc_time);
                    lfs_file_close(extended_lfs, &file);
                    state = LoadFileResult::OTHER_FAILURE;
                }
            } else {
                LOG_ERROR("Load: Step 2 FAILED: File '%s' size invalid or too large: %d bytes (took %u ms)", 
                         filename, (int)file_size, size_time);
                lfs_file_close(extended_lfs, &file);
                state = LoadFileResult::OTHER_FAILURE;
            }
        } else {
            // File doesn't exist - normal for first boot, will be created on first save
            LOG_DEBUG("Load: Step 1: File '%s' not found in extended filesystem (error: %d, took %u ms, will be created on first save)", 
                     filename, open_result, open_time);
            state = LoadFileResult::OTHER_FAILURE;  // Return failure so standard code can handle first boot
        }
    } else {
        LOG_ERROR("Extended filesystem not available for %s", filename);
    }
    
    return state;
}

bool saveToExtendedFS(const char *filename, size_t protoSize, const pb_msgdesc_t *fields, 
                      const void *dest_struct, bool fullAtomic)
{
    (void)fullAtomic;  // Atomic writes handled by LittleFS
    
    bool okay = false;
    
    // Use extended filesystem for nodes.proto (other nodes from network only)
    LOG_INFO("Save %s to EXTENDED filesystem (80 pages, 320 KB) - other nodes only", filename);
    
    // Cast to lfs_t* (headers already included at top of file)
    // Get fresh pointer each time in case filesystem was reformatted
    lfs_t* extended_lfs = ::getExtendedFSForNodeDB();
    if (!extended_lfs) {
        LOG_WARN("Extended filesystem pointer is null - extended filesystem unavailable");
        // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
        return false;
    }
    
    // CRITICAL: Extended filesystem uses SoftDevice API directly, NOT SPI!
    // Do NOT use writecb here - it uses spiLock for SPI bus (LoRa radio + main filesystem).
    // Use streaming encoding instead: encode directly to file, no buffer allocation needed!
    // This avoids memory fragmentation issues when free heap is fragmented.
    
    LOG_DEBUG("Opening %s for writing in extended filesystem (streaming, no buffer)...", filename);
    
    // Ensure directory exists (e.g., /prefs/ for /prefs/nodes.proto)
    const char* slash = filename;
    if (slash[0] == '/') {
        slash++; // skip root '/'
    }
    while (NULL != (slash = strchr(slash, '/'))) {
        size_t dir_len = slash - filename;
        char dir_path[64];
        if (dir_len < sizeof(dir_path)) {
            memcpy(dir_path, filename, dir_len);
            dir_path[dir_len] = '\0';
            int mkdir_result = lfs_mkdir(extended_lfs, dir_path);
            if (mkdir_result != LFS_ERR_OK && mkdir_result != LFS_ERR_EXIST) {
                LOG_DEBUG("Failed to create directory '%s' in extended FS (error: %d), continuing...", dir_path, mkdir_result);
            }
        }
        slash++; // move past '/'
    }
    
    // Remove old file first (for atomic write)
    // This is safer than truncate - avoids issues with block allocation
    int remove_result = lfs_remove(extended_lfs, filename);
    if (remove_result == LFS_ERR_OK) {
        LOG_DEBUG("Removed old file '%s' before write", filename);
    } else if (remove_result != LFS_ERR_NOENT) {
        LOG_DEBUG("Failed to remove old file '%s' (error: %d), continuing...", filename, remove_result);
    }
    
    // Open file for writing (create new file)
    lfs_file_t file;
    int flags = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
    int open_result = lfs_file_open(extended_lfs, &file, filename, flags);
    
    if (open_result != LFS_ERR_OK) {
        LOG_ERROR("Failed to open '%s' for writing in extended FS (error: %d)", filename, open_result);
        LOG_ERROR("Filesystem may be full or corrupted - max blocks: 80");
        // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
        return false;
    }
    
    LOG_DEBUG("File opened successfully, encoding protobuf with streaming (expected size: %u)...", (unsigned)protoSize);
    
    // Use streaming encoding - no buffer allocation needed!
    // This avoids memory fragmentation issues when free heap is fragmented
    LfsFileContext ctx;
    ctx.lfs = extended_lfs;
    ctx.file = &file;
    
    pb_ostream_t stream = {&lfs_writecb, &ctx, SIZE_MAX, 0};
    
    LOG_DEBUG("Step 3: Encoding and writing protobuf to file '%s' (streaming, no buffer)...", filename);
    uint32_t encode_start = millis();
    bool encode_success = pb_encode(&stream, fields, dest_struct);
    uint32_t encode_time = millis() - encode_start;
    
    if (!encode_success) {
        const char* error = PB_GET_ERROR(&stream);
        LOG_ERROR("Step 3 FAILED: Protobuf encoding failed: %s (took %u ms)", error ? error : "unknown", encode_time);
        lfs_file_close(extended_lfs, &file);
        // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
        return false;
    }
    
    size_t encoded_size = stream.bytes_written;
    LOG_DEBUG("Step 3 SUCCESS: Encoded and wrote %u bytes to file '%s' (streaming, took %u ms)", 
             (unsigned)encoded_size, filename, encode_time);
    
    // Check if write was successful by checking file size
    lfs_soff_t file_size = lfs_file_size(extended_lfs, &file);
    if (file_size < 0 || (size_t)file_size != encoded_size) {
        LOG_ERROR("Step 3 VERIFICATION FAILED: File size mismatch! Expected %u bytes, got %d", 
                 (unsigned)encoded_size, (int)file_size);
        lfs_file_close(extended_lfs, &file);
        // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
        return false;
    }
    
    // Sync file before closing (CRITICAL for LittleFS)
    LOG_DEBUG("Step 4: Syncing file '%s'...", filename);
    uint32_t sync_start = millis();
    int sync_result = lfs_file_sync(extended_lfs, &file);
    uint32_t sync_time = millis() - sync_start;
    if (sync_result != LFS_ERR_OK) {
        LOG_ERROR("Step 4 FAILED: Sync of '%s' failed (error: %d, took %u ms)", filename, sync_result, sync_time);
        lfs_file_close(extended_lfs, &file);
        // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
        return false;
    }
    LOG_DEBUG("Step 4 SUCCESS: File synced (took %u ms)", sync_time);
    
    // Close file
    LOG_DEBUG("Step 5: Closing file '%s'...", filename);
    uint32_t close_start = millis();
    int close_result = lfs_file_close(extended_lfs, &file);
    uint32_t close_time = millis() - close_start;
    if (close_result != LFS_ERR_OK) {
        LOG_WARN("Step 5 FAILED: Close of '%s' failed (error: %d, took %u ms)", filename, close_result, close_time);
        okay = false;  // Mark as failed if close fails
    } else {
        LOG_DEBUG("Step 5 SUCCESS: File '%s' closed (took %u ms)", filename, close_time);
        okay = true;  // Mark as successful after all steps completed
    }
    
    if (okay) {
        LOG_INFO("Saved %s successfully to EXTENDED filesystem (%u bytes)", filename, (unsigned)encoded_size);
        
        // Verify file was saved to extended filesystem and NOT to main filesystem
        LOG_DEBUG("VERIFY: Step 1: Checking if '%s' exists in extended filesystem...", filename);
        uint32_t verify_start = millis();
        bool found_in_extended = false;
        lfs_file_t verify_file;
        int verify_result = lfs_file_open(extended_lfs, &verify_file, filename, LFS_O_RDONLY);
        if (verify_result == LFS_ERR_OK) {
            found_in_extended = true;
            lfs_soff_t file_size = lfs_file_size(extended_lfs, &verify_file);
            lfs_file_close(extended_lfs, &verify_file);
            LOG_DEBUG("VERIFY: Step 1 SUCCESS: File '%s' found in extended FS (size: %d bytes)", filename, (int)file_size);
        } else {
            LOG_DEBUG("VERIFY: Step 1 FAILED: File '%s' not found in extended FS (error: %d)", filename, verify_result);
        }
        
        LOG_DEBUG("VERIFY: Step 2: Checking if '%s' exists in main filesystem...", filename);
        bool found_in_main = FSCom.exists(filename);
        if (found_in_main) {
            LOG_DEBUG("VERIFY: Step 2: File '%s' found in main FS (this is WRONG for nodes.proto!)", filename);
        } else {
            LOG_DEBUG("VERIFY: Step 2 SUCCESS: File '%s' NOT in main FS (correct)", filename);
        }
        
        uint32_t verify_time = millis() - verify_start;
        
        if (found_in_extended && !found_in_main) {
            LOG_INFO("VERIFIED: %s correctly saved to EXTENDED filesystem (80 pages, 320 KB, verification took %u ms)", 
                    filename, verify_time);
        } else if (found_in_main) {
            LOG_ERROR("VERIFY FAILED: %s was also found in MAIN filesystem - this should not happen!", filename);
            LOG_ERROR("nodes.proto should ONLY be in extended filesystem!");
        } else if (!found_in_extended) {
            LOG_ERROR("VERIFY FAILED: %s not found in EXTENDED filesystem after save (verification took %u ms)!", 
                     filename, verify_time);
        }
        
        return true;
    } else {
        LOG_ERROR("Can't write %s to extended filesystem!", filename);
        // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
        return false;
    }
}

bool useExtendedFSForNodeDB()
{
    return ::useExtendedFSForNodeDB();
}

lfs_t* getExtendedFSForNodeDB()
{
    return ::getExtendedFSForNodeDB();
}

bool isNodeDBFile(const char* filename)
{
    return ::isNodeDBFile(filename);
}

} // namespace NodeDBExtendedFS

#endif // USE_EXTENDED_FS_FOR_NODEDB

