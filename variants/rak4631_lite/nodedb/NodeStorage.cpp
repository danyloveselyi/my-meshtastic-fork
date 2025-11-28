/**
 * @file NodeStorage.cpp
 * @brief Slot-based flash storage implementation for NodeDB nodes
 */

#include "NodeStorage.h"
#include "NodeDBExtendedFSImpl.h"
#include "NodeIndex.h"
#include "mesh/NodeDB.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include <cstring>
#include <cstdio>
#include <cstdint>

// Include LittleFS headers (same as NodeDBExtendedFSImpl)
#include "../../../src/platform/stm32wl/littlefs/lfs.h"
#include "../../../src/platform/stm32wl/littlefs/lfs_util.h"

#ifdef ARCH_NRF52
// Forward declaration for nrf52Loop() (defined in src/platform/nrf52/main-nrf52.cpp)
extern void nrf52Loop();
#endif
#include <Arduino.h>  // For yield()

#ifdef USE_EXTENDED_FS_FOR_NODEDB

namespace NodeStorage {

// Constants
constexpr const char* SLOTS_DIR = "/nodes";
constexpr const char* SLOT_FILE_PREFIX = "/nodes/slot_";
constexpr const char* SLOT_FILE_SUFFIX = ".bin";
// Maximum slots - use MAX_NUM_NODES from variant.h (already defined in mesh-pb-constants.h)
#ifndef MAX_NODES_SLOTS
#define MAX_NODES_SLOTS MAX_NUM_NODES  // Use MAX_NUM_NODES from variant.h (500 by default for RAK4631)
#endif
constexpr uint16_t MAX_SLOT_ID = MAX_NODES_SLOTS;
constexpr size_t SLOT_FILENAME_BUFFER_SIZE = 32;

/**
 * @brief Structure to hold LittleFS file context for streaming protobuf encoding
 */
struct LfsFileContext {
    lfs_t* lfs;              // LittleFS instance
    lfs_file_t file;         // File handle (struct, not pointer)
};

// Streaming callback for protobuf encoding to LittleFS file
static bool lfs_writecb(pb_ostream_t *stream, const pb_byte_t *buf, size_t count)
{
    LfsFileContext* ctx = (LfsFileContext*)stream->state;
    if (!ctx || !ctx->lfs) {
        LOG_ERROR("NodeStorage: lfs_writecb: Invalid context (ctx=%p, lfs=%p)", ctx, ctx ? ctx->lfs : nullptr);
        return false;
    }
    
    LOG_DEBUG("NodeStorage: lfs_writecb: Writing %u bytes to file...", (unsigned)count);
    lfs_ssize_t written = lfs_file_write(ctx->lfs, &ctx->file, buf, count);
    LOG_DEBUG("NodeStorage: lfs_writecb: Wrote %d of %u bytes", (int)written, (unsigned)count);
    
    if (written != (lfs_ssize_t)count) {
        LOG_ERROR("NodeStorage: lfs_writecb: Write failed (written: %d, expected: %u)", (int)written, (unsigned)count);
        return false;
    }
    
    return true;
}

/**
 * @brief Get slot filename for a given slot ID
 */
const char* getSlotFilename(uint16_t slotId, char* buffer, size_t bufferSize)
{
    if (!buffer || bufferSize < SLOT_FILENAME_BUFFER_SIZE) {
        return nullptr;
    }
    
    int result = snprintf(buffer, bufferSize, "%s%04u%s", SLOT_FILE_PREFIX, slotId, SLOT_FILE_SUFFIX);
    if (result < 0 || result >= (int)bufferSize) {
        return nullptr;
    }
    
    return buffer;
}


// Static flag to track if directory was created (lazy initialization)
static bool directory_created = false;

/**
 * @brief Ensure /nodes directory exists (lazy creation - only when needed)
 * This is called from writeNodeToSlot() to avoid blocking device startup
 */
static bool ensureDirectoryExists()
{
    if (directory_created) {
        return true;  // Directory already exists
    }
    
    lfs_t* extended_lfs = getExtendedFSForNodeDB();
    if (!extended_lfs) {
        LOG_ERROR("NodeStorage: Extended filesystem not available");
        return false;
    }
    
    // Create /nodes directory if it doesn't exist
    // Note: lfs_mkdir() may trigger lfs_erase and lfs_prog operations
    #ifdef ARCH_NRF52
    // Feed watchdog before directory creation (may trigger erase/prog)
    ::nrf52Loop();
    delay(10);  // Give system time to process
    #endif
    int mkdir_result = lfs_mkdir(extended_lfs, SLOTS_DIR);
    #ifdef ARCH_NRF52
    // Feed watchdog after directory creation (may have triggered erase/prog)
    ::nrf52Loop();
    delay(10);  // Give system time to process
    #endif
    if (mkdir_result != LFS_ERR_OK && mkdir_result != LFS_ERR_EXIST) {
        LOG_ERROR("NodeStorage: Failed to create directory '%s' (error: %d)", SLOTS_DIR, mkdir_result);
        return false;
    }
    
    directory_created = true;
    LOG_DEBUG("NodeStorage: Directory '%s' ready", SLOTS_DIR);
    return true;
}

/**
 * @brief Initialize slot storage (no-op, directory created lazily on first write)
 */
bool initialize()
{
    // Directory will be created lazily on first write to avoid blocking device startup
    // This prevents watchdog timeout during initialization
    return true;
}

/**
 * @brief Check if a slot exists
 */
bool slotExists(uint16_t slotId)
{
    if (slotId > MAX_SLOT_ID) {
        return false;
    }
    
    lfs_t* extended_lfs = getExtendedFSForNodeDB();
    if (!extended_lfs) {
        return false;
    }
    
    char filename[SLOT_FILENAME_BUFFER_SIZE];
    if (!getSlotFilename(slotId, filename, sizeof(filename))) {
        return false;
    }
    
    lfs_file_t file;
    int open_result = lfs_file_open(extended_lfs, &file, filename, LFS_O_RDONLY);
    if (open_result == LFS_ERR_OK) {
        lfs_file_close(extended_lfs, &file);
        return true;
    }
    
    return false;
}

/**
 * @brief Allocate a new slot for a node
 * 
 * CRITICAL: This function should check NodeIndex first to see which slots are actually used,
 * not just check filesystem (which may have stale files or missing files due to wear leveling).
 */
uint16_t allocateSlot(uint32_t nodeNum)
{
    (void)nodeNum;  // NodeNum not used for slot allocation (sequential allocation)
    
    // CRITICAL: Feed watchdog before accessing filesystem
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    lfs_t* extended_lfs = getExtendedFSForNodeDB();
    
    // CRITICAL: Feed watchdog after filesystem access
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    if (!extended_lfs) {
        LOG_ERROR("NodeStorage: Extended filesystem not available for slot allocation");
        return UINT16_MAX;
    }
    
    // Ensure directory exists
    // CRITICAL: Feed watchdog before directory initialization
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    if (!initialize()) {
        return UINT16_MAX;
    }
    
    // CRITICAL: Feed watchdog after directory initialization
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    // CRITICAL FIX: Check NodeIndex to see which slots are actually allocated
    // This is more reliable than checking filesystem (which may have stale files)
    // Get all allocated slots from index
    #ifdef USE_EXTENDED_FS_FOR_NODEDB
    // Build a set of used slots from NodeIndex
    // This is more reliable than checking filesystem
    bool used_slots[MAX_NODES_SLOTS + 1] = {false};  // Initialize all to false
    
    // Get all nodes from index and mark their slots as used
    uint32_t node_count = NodeIndex::getNodeCount();
    for (uint32_t i = 0; i < node_count && i < MAX_NODES_SLOTS; i++) {
        NodeNum node_num = NodeIndex::getNodeNumByIndex(i);
        if (node_num != 0) {
            NodeIndexEntry* entry = NodeIndex::findNode(node_num);
            if (entry && entry->slot_id <= MAX_SLOT_ID) {
                used_slots[entry->slot_id] = true;
            }
        }
    }
    
    // Find first available slot
    for (uint16_t slotId = 0; slotId <= MAX_SLOT_ID; slotId++) {
        if (!used_slots[slotId]) {
            LOG_DEBUG("NodeStorage: Allocated slot %u for node 0x%x (checked %u nodes in index)", slotId, nodeNum, node_count);
            return slotId;
        }
    }
    #else
    // Fallback: Check filesystem (less reliable, but works if index not available)
    for (uint16_t slotId = 0; slotId <= MAX_SLOT_ID; slotId++) {
        if (!slotExists(slotId)) {
            LOG_DEBUG("NodeStorage: Allocated slot %u for node 0x%x (filesystem check)", slotId, nodeNum);
            return slotId;
        }
    }
    #endif
    
    LOG_ERROR("NodeStorage: No free slots available (max: %u)", MAX_SLOT_ID);
    return UINT16_MAX;
}

/**
 * @brief Write node data to a slot (copy-on-write for wear leveling)
 */
bool writeNodeToSlot(uint16_t slotId, const meshtastic_NodeInfoLite* node)
{
    LOG_DEBUG("NodeStorage: writeNodeToSlot(%u) START", slotId);
    
    // CRITICAL: Feed watchdog at the start of write operation
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    if (slotId > MAX_SLOT_ID || !node) {
        LOG_ERROR("NodeStorage: Invalid parameters (slotId: %u, node: %p)", slotId, node);
        return false;
    }
    
    // CRITICAL: Feed watchdog before accessing filesystem
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    lfs_t* extended_lfs = getExtendedFSForNodeDB();
    
    // CRITICAL: Feed watchdog after filesystem access
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    if (!extended_lfs) {
        LOG_ERROR("NodeStorage: Extended filesystem not available");
        return false;
    }
    
    LOG_DEBUG("NodeStorage: Ensuring directory exists...");
    // Ensure directory exists (lazy creation - only on first write)
    if (!ensureDirectoryExists()) {
        LOG_ERROR("NodeStorage: Failed to create directory");
        return false;
    }
    
    char filename[SLOT_FILENAME_BUFFER_SIZE];
    
    if (!getSlotFilename(slotId, filename, sizeof(filename))) {
        LOG_ERROR("NodeStorage: Failed to generate filename for slot %u", slotId);
        return false;
    }
    
    LOG_DEBUG("NodeStorage: Writing to slot %u, filename: %s", slotId, filename);
    
    // SIMPLIFIED STRATEGY for wear leveling:
    // 1. Delete old file (if exists) - marks old blocks as free
    // 2. Write directly to final file - LittleFS will use different blocks (automatic wear leveling)
    // This reduces operations from 8+ to 5, while still providing wear leveling via LittleFS
    
    uint32_t operation_start = millis();  // Track time for this operation
    
    // Step 1: Delete old file (if exists) - this marks old blocks as free
    LOG_DEBUG("NodeStorage: Removing old file '%s' (if exists)...", filename);
    
    // CRITICAL: Feed watchdog before remove (remove may trigger flash operations)
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    int remove_result = lfs_remove(extended_lfs, filename);
    
    // CRITICAL: Feed watchdog after remove and add delay
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    delay(10);  // Small delay to allow flash operations to complete
    
    if (remove_result == LFS_ERR_OK) {
        LOG_DEBUG("NodeStorage: Removed old slot file '%s'", filename);
    } else if (remove_result != LFS_ERR_NOENT) {
        LOG_WARN("NodeStorage: Failed to remove old file '%s' (error: %d), continuing...", filename, remove_result);
    }
    
    // CRITICAL: Check time limit - don't spend more than 5 seconds
    if (millis() - operation_start > 5000) {
        LOG_WARN("NodeStorage: Time limit reached, aborting write");
        yield();
        #ifdef ARCH_NRF52
        ::nrf52Loop();
        #endif
        delay(50);
        return false;
    }
    
    // Step 2: Open final file for writing
    LOG_DEBUG("NodeStorage: Opening final file '%s' for writing...", filename);
    
    // CRITICAL: Feed watchdog before file operations
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    lfs_file_t file;
    int flags = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
    int open_result = lfs_file_open(extended_lfs, &file, filename, flags);
    
    // CRITICAL: Feed watchdog after file open (may trigger flash operations) and add delay
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    delay(10);
    
    if (open_result != LFS_ERR_OK) {
        LOG_ERROR("NodeStorage: Failed to open final file '%s' for writing (error: %d)", filename, open_result);
        return false;
    }
    LOG_DEBUG("NodeStorage: Final file opened successfully");
    
    // Step 2: Encode node to protobuf into memory buffer first
    // This avoids issues with LittleFS callback-based writing and ensures data integrity
    LOG_DEBUG("NodeStorage: Encoding node to protobuf buffer...");
    uint8_t encode_buffer[512];
    pb_ostream_t stream = pb_ostream_from_buffer(encode_buffer, sizeof(encode_buffer));
    
    LOG_DEBUG("NodeStorage: Starting pb_encode for node 0x%x (num=%u)...", node->num, node->num);
    bool encode_success = pb_encode(&stream, meshtastic_NodeInfoLite_fields, node);
    
    if (!encode_success) {
        LOG_ERROR("NodeStorage: Failed to encode node to protobuf: %s", PB_GET_ERROR(&stream));
        lfs_file_close(extended_lfs, &file);
        lfs_remove(extended_lfs, filename);  // Clean up partial file
        return false;
    }
    
    // Save bytes_written for later verification
    size_t bytes_written = stream.bytes_written;
    LOG_DEBUG("NodeStorage: pb_encode completed successfully, bytes_written=%u", (unsigned)bytes_written);
    
    if (bytes_written == 0) {
        LOG_ERROR("NodeStorage: pb_encode returned success but wrote 0 bytes! Node may be empty or invalid.");
        lfs_file_close(extended_lfs, &file);
        lfs_remove(extended_lfs, filename);
        return false;
    }
    
    if (bytes_written > 512) {
        LOG_ERROR("NodeStorage: Encoded size (%u) exceeds max slot size (512)", (unsigned)bytes_written);
        lfs_file_close(extended_lfs, &file);
        lfs_remove(extended_lfs, filename);
        return false;
    }
    
    // Write encoded buffer directly to final file
    LOG_DEBUG("NodeStorage: Writing %u bytes to final file...", (unsigned)bytes_written);
    
    // CRITICAL: Feed watchdog before write (write may trigger flash operations)
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    lfs_ssize_t write_result = lfs_file_write(extended_lfs, &file, encode_buffer, bytes_written);
    
    // CRITICAL: Feed watchdog after write (write may have triggered flash operations) and add delay
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    delay(10);
    
    if (write_result != (lfs_ssize_t)bytes_written) {
        // Check error code for detailed error message
        if (write_result == LFS_ERR_NOSPC) {
            LOG_ERROR("NodeStorage: No space left on device (LFS_ERR_NOSPC) - filesystem is full!");
            LOG_ERROR("NodeStorage: Cannot write %u bytes to slot %u", (unsigned)bytes_written, slotId);
            
            // Log filesystem statistics for debugging
            uint32_t total_bytes = 0, used_bytes = 0, free_bytes = 0;
            if (getExtendedFSStats(&total_bytes, &used_bytes, &free_bytes)) {
                uint32_t total_kb = total_bytes / 1024;
                uint32_t used_kb = used_bytes / 1024;
                uint32_t free_kb = free_bytes / 1024;
                uint32_t used_pct = (total_kb > 0) ? (used_kb * 100) / total_kb : 0;
                LOG_ERROR("NodeStorage: Extended FS stats - Total: %u KB, Used: %u KB (%u%%), Free: %u KB", 
                         total_kb, used_kb, used_pct, free_kb);
            }
            
            // Log node count
            uint32_t node_count = NodeIndex::getNodeCount();
            LOG_ERROR("NodeStorage: Current node count: %u, slot ID: %u", node_count, slotId);
        } else if (write_result == LFS_ERR_IO) {
            LOG_ERROR("NodeStorage: I/O error during write (LFS_ERR_IO)");
        } else if (write_result == LFS_ERR_CORRUPT) {
            LOG_ERROR("NodeStorage: Filesystem corruption detected (LFS_ERR_CORRUPT)");
        } else {
            LOG_ERROR("NodeStorage: Failed to write to file (error code: %d, wrote %d of %u bytes)", 
                     (int)write_result, (int)write_result, (unsigned)bytes_written);
        }
        lfs_file_close(extended_lfs, &file);
        lfs_remove(extended_lfs, filename);  // Clean up partial file
        return false;
    }
    
    // Sync file to ensure data is written to flash
    LOG_DEBUG("NodeStorage: Syncing final file...");
    
    // CRITICAL: Feed watchdog before sync (sync may trigger flash operations)
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    int sync_result = lfs_file_sync(extended_lfs, &file);
    
    // CRITICAL: Feed watchdog after sync (sync may have triggered flash operations) and add delay
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    delay(10);
    
    if (sync_result != LFS_ERR_OK) {
        LOG_WARN("NodeStorage: File sync failed (error: %d), continuing...", sync_result);
    }
    
    // Close file
    LOG_DEBUG("NodeStorage: Closing final file...");
    
    // CRITICAL: Feed watchdog before close
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    int close_result = lfs_file_close(extended_lfs, &file);
    
    // CRITICAL: Feed watchdog after close and add delay
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    delay(10);
    
    if (close_result != LFS_ERR_OK) {
        LOG_ERROR("NodeStorage: Failed to close file (error: %d)", close_result);
        lfs_remove(extended_lfs, filename);  // Clean up partial file
        return false;
    }
    LOG_DEBUG("NodeStorage: File closed successfully");
    
    // CRITICAL: Final time check
    uint32_t total_time = millis() - operation_start;
    if (total_time > 5000) {
        LOG_WARN("NodeStorage: Write operation took %u ms (exceeded 5s limit)", total_time);
    }
    
    LOG_DEBUG("NodeStorage: writeNodeToSlot(%u) COMPLETED successfully (%u bytes)", slotId, (unsigned)bytes_written);
    return true;
}

/**
 * @brief Read node data from a slot
 */
bool readNodeFromSlot(uint16_t slotId, meshtastic_NodeInfoLite* node)
{
    if (slotId > MAX_SLOT_ID || !node) {
        LOG_ERROR("NodeStorage: Invalid parameters (slotId: %u, node: %p)", slotId, node);
        return false;
    }
    
    lfs_t* extended_lfs = getExtendedFSForNodeDB();
    if (!extended_lfs) {
        LOG_ERROR("NodeStorage: Extended filesystem not available");
        return false;
    }
    
    char filename[SLOT_FILENAME_BUFFER_SIZE];
    if (!getSlotFilename(slotId, filename, sizeof(filename))) {
        LOG_ERROR("NodeStorage: Failed to generate filename for slot %u", slotId);
        return false;
    }
    
    // Open file for reading
    lfs_file_t file;
    int open_result = lfs_file_open(extended_lfs, &file, filename, LFS_O_RDONLY);
    if (open_result != LFS_ERR_OK) {
        LOG_DEBUG("NodeStorage: Slot %u file not found (error: %d)", slotId, open_result);
        return false;
    }
    
    // Get file size
    lfs_soff_t file_size = lfs_file_size(extended_lfs, &file);
    if (file_size < 0 || file_size > 512) {  // Max 512 bytes per slot
        LOG_ERROR("NodeStorage: Invalid file size for slot %u: %d", slotId, (int)file_size);
        lfs_file_close(extended_lfs, &file);
        return false;
    }
    
    // Read file into buffer
    uint8_t buffer[512];
    lfs_ssize_t read_result = lfs_file_read(extended_lfs, &file, buffer, file_size);
    lfs_file_close(extended_lfs, &file);
    
    if (read_result != file_size) {
        LOG_ERROR("NodeStorage: Failed to read slot %u (read %d of %d bytes)", slotId, (int)read_result, (int)file_size);
        return false;
    }
    
    // Decode protobuf from buffer
    pb_istream_t stream = pb_istream_from_buffer(buffer, file_size);
    memset(node, 0, sizeof(meshtastic_NodeInfoLite));
    
    if (!pb_decode(&stream, meshtastic_NodeInfoLite_fields, node)) {
        LOG_ERROR("NodeStorage: Failed to decode node from slot %u: %s", slotId, PB_GET_ERROR(&stream));
        return false;
    }
    
    // Validate node integrity
    if (node->num == 0) {
        LOG_ERROR("NodeStorage: Invalid node number (0) in slot %u", slotId);
        return false;
    }
    
    // Log detailed node information
    // Note: last_heard from NodeInfoLite is always 0 (hot data, not stored in flash)
    // Real last_heard is stored in NodeIndex (persisted on flash)
    uint32_t last_heard_from_index = 0;
    NodeIndexEntry* index_entry = NodeIndex::findNode(node->num);
    if (index_entry) {
        last_heard_from_index = index_entry->last_heard;
    }
    
    LOG_DEBUG("NodeStorage: Successfully read node from slot %u (%d bytes)", slotId, (int)file_size);
    int32_t snr_int = (int32_t)node->snr;
    int32_t snr_frac = (int32_t)((node->snr - snr_int) * 100);
    if (snr_frac < 0) snr_frac = -snr_frac;
    LOG_DEBUG("NodeStorage: Node 0x%x details: num=0x%x, last_heard=%u (from index: %u), snr=%d.%02d, hops_away=%u, channel=%u", 
             node->num, node->num, node->last_heard, last_heard_from_index, snr_int, snr_frac, node->hops_away, node->channel);
    if (node->has_user) {
        // meshtastic_UserLite uses char arrays, not pb_bytes_array_t
        LOG_DEBUG("NodeStorage: Node 0x%x user: long_name='%s', short_name='%s'", 
                  node->num,
                  node->user.long_name,
                  node->user.short_name);
        
        // Log public key (32 bytes in hex format)
        // PB_BYTES_ARRAY_T creates a structure with 'size' and 'bytes' fields
        char pubkey_hex[65] = {0}; // 32 bytes * 2 + 1 null terminator
        size_t pubkey_size = node->user.public_key.size;
        if (pubkey_size > 32) {
            pubkey_size = 32; // Safety limit
        }
        
        if (pubkey_size > 0) {
            for (size_t i = 0; i < pubkey_size; i++) {
                char hex_byte[3];
                snprintf(hex_byte, sizeof(hex_byte), "%02x", node->user.public_key.bytes[i]);
                size_t current_len = strlen(pubkey_hex);
                if (current_len + 2 < sizeof(pubkey_hex)) {
                    strncat(pubkey_hex, hex_byte, sizeof(pubkey_hex) - current_len - 1);
                }
            }
            LOG_DEBUG("NodeStorage: Node 0x%x public_key: %s (size: %u)", 
                      node->num, pubkey_hex, (unsigned)pubkey_size);
        } else {
            LOG_DEBUG("NodeStorage: Node 0x%x public_key: (empty or not set)", node->num);
        }
    }
    if (node->has_position) {
        LOG_DEBUG("NodeStorage: Node 0x%x position: lat=%d, lon=%d, alt=%d, time=%u", 
                  node->num, node->position.latitude_i, node->position.longitude_i, 
                  node->position.altitude, node->position.time);
    }
    
    return true;
}

/**
 * @brief Delete a slot (mark as free)
 */
bool deleteSlot(uint16_t slotId)
{
    if (slotId > MAX_SLOT_ID) {
        return false;
    }
    
    lfs_t* extended_lfs = getExtendedFSForNodeDB();
    if (!extended_lfs) {
        return false;
    }
    
    char filename[SLOT_FILENAME_BUFFER_SIZE];
    if (!getSlotFilename(slotId, filename, sizeof(filename))) {
        return false;
    }
    
    int remove_result = lfs_remove(extended_lfs, filename);
    if (remove_result == LFS_ERR_OK) {
        LOG_DEBUG("NodeStorage: Deleted slot %u", slotId);
        return true;
    } else if (remove_result == LFS_ERR_NOENT) {
        // File doesn't exist - consider it already deleted
        return true;
    } else {
        LOG_ERROR("NodeStorage: Failed to delete slot %u (error: %d)", slotId, remove_result);
        return false;
    }
}

/**
 * @brief Get total number of allocated slots
 * 
 * OPTIMIZATION: Use NodeIndex::getNodeCount() instead of scanning all slots.
 * This avoids scanning 1234 slots which can cause device freeze.
 * The index already contains all nodes, so it's the authoritative source.
 */
uint32_t getSlotCount()
{
    // Use NodeIndex as the authoritative source for node count
    // This avoids scanning 1234 slots which can take several seconds and cause freeze
    // The index is always in sync with actual slots (updated on allocate/delete)
    return NodeIndex::getNodeCount();
}

/**
 * @brief Get number of free slots
 */
uint32_t getFreeSlotCount()
{
    // getSlotCount() now calls NodeIndex::getNodeCount() which checks initialization
    // So it's safe to call even if index is not initialized (returns 0)
    uint32_t used = getSlotCount();
    uint32_t max = getMaxSlotCount();
    return (used < max) ? (max - used) : 0;
}

/**
 * @brief Get maximum slot capacity
 */
uint32_t getMaxSlotCount()
{
    return MAX_SLOT_ID + 1;  // 0 to MAX_SLOT_ID inclusive
}

} // namespace NodeStorage

#endif // USE_EXTENDED_FS_FOR_NODEDB

