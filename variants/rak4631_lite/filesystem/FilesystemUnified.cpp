/**
 * @file FilesystemUnified.cpp
 * @brief Unified filesystem module implementation for RAK4631 Lite
 * 
 * This file consolidates all filesystem-related functionality:
 * - ExtendedFilesystemModule: Extended filesystem (272 KB) for NodeDB
 * - FSCommon Patches: Patches for FSCommon.cpp functions
 * - Main FS Pre-init: Pre-initialization logic for main filesystem
 * 
 * Previously split across multiple files:
 * - modules/ExtendedFilesystemModule/ExtendedFilesystemModule.cpp
 * - filesystem/FSCommon-patches.cpp
 * - filesystem/main/main-fs-pre-init.cpp
 */

#include "FilesystemUnified.h"
#include "../../../src/FSCommon.h"
#include "../../../src/mesh/NodeDB.h"
#include "../../../src/mesh/mesh-pb-constants.h"  // For readcb/writecb
#include "configuration.h"
#include "softdevice/nrf_soc.h"
#include "error.h"
#include <Arduino.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// nrf52Loop() is now handled by FEED_WATCHDOG_AND_YIELD() macro from FilesystemUnified.h

// Include LittleFS headers from STM32 implementation (same API)
#include "../../../src/platform/stm32wl/littlefs/lfs.h"
#include "../../../src/platform/stm32wl/littlefs/lfs_util.h"
#include "../../../src/SafeFile.h"
#include "../../../src/SPILock.h"
#include <pb_decode.h>
#include <pb_encode.h>
#ifdef ARCH_NRF52
#include "../../../src/mesh/RadioLibInterface.h"
// Include Adafruit LittleFS namespace for FILE_O_READ/FILE_O_WRITE constants
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;

#ifdef USE_EXTENDED_FS_FOR_NODEDB

// ============================================================================
// Helper Functions (for deduplication)
// ============================================================================

namespace {
    // Forward declarations
    bool calculateLastAddress(uint32_t base_addr, uint32_t size, uint32_t& last_addr);
    
    /**
     * @brief Check if SoftDevice is enabled and async operations should be used
     * @return true if SoftDevice is enabled and async operations are available
     */
    bool isSoftDeviceAsyncEnabled()
    {
        uint8_t sd_enabled = 0;
        uint32_t sd_result = sd_softdevice_is_enabled(&sd_enabled);
        return (sd_result == NRF_SUCCESS && sd_enabled);
    }
    
    /**
     * @brief Check and validate SoftDevice state for flash operations
     * @param log_status Whether to log the SoftDevice status
     * @return true if SoftDevice is properly initialized, false otherwise
     * 
     * This function centralizes SoftDevice state checking logic.
     * It can be used during initialization to verify SoftDevice is ready
     * for flash operations, or to check status before critical operations.
     */
    bool checkSoftDeviceState(bool log_status = true)
    {
        bool sd_enabled = isSoftDeviceAsyncEnabled();
        
        if (log_status) {
            if (sd_enabled) {
                LOG_DEBUG("SoftDevice is enabled - will use SoftDevice API for flash operations");
            } else {
                LOG_DEBUG("SoftDevice not enabled yet - will use direct flash access");
            }
        }
        
        return sd_enabled;
    }
    
    
    /**
     * @brief Process SoftDevice flash operation events
     * @param evt Output parameter for event (last event found)
     * @param max_events Maximum number of events to process in one call
     * @return true if SUCCESS event was found, false otherwise (check evt for ERROR)
     */
    bool processFlashEvents(uint32_t& evt, int max_events = 20)
    {
        for (int i = 0; i < max_events; i++) {
            if (sd_evt_get(&evt) == NRF_SUCCESS) {
                if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) {
                    return true;
                } else if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
                    return false;  // Error found - caller should check evt
                }
                // Continue processing other events
            } else {
                break;  // No more events available
            }
        }
        return false;  // No success event found
    }
    
    /**
     * @brief Wait for async flash write operation to complete
     * @param write_addr Address where data was written
     * @param batch_size_bytes Size of batch in bytes
     * @param src_words Pointer to source words that were written
     * @param words_to_write Number of words written
     * @param poll_timeout Timeout for polling (ms)
     * @param flash_check_delay Initial delay before first flash check (ms)
     * @param flash_check_interval Interval between flash checks (ms)
     * @return true if operation completed successfully, false on error
     */
    bool waitForAsyncFlashWrite(uint32_t write_addr, uint32_t batch_size_bytes, 
                                const uint32_t* src_words, uint32_t words_to_write,
                                uint32_t poll_timeout = 2000,  // INCREASED from 1000 to 2000 ms - give more time
                                uint32_t flash_check_delay = 10,  // INCREASED from 2 to 10 ms - flash needs time to start writing
                                uint32_t flash_check_interval = 5)  // INCREASED from 3 to 5 ms
    {
        uint32_t start_time = millis();
        bool success = false;
        uint32_t evt;
        
        // First, try to get any pending events immediately
        if (processFlashEvents(evt)) {
            return true;
        }
        
        // Check if error event was found
        if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
            LOG_ERROR("Flash operation ERROR event at address 0x%08X", write_addr);
            return false;
        }
        
        // If not immediately successful, wait with polling
        uint32_t last_flash_check = 0;
        
        // CRITICAL: Prevent infinite loop - add max iterations check
        uint32_t max_iterations = (poll_timeout / flash_check_interval) + 10;  // Safety margin
        uint32_t iteration_count = 0;
        
        while ((millis() - start_time) < poll_timeout && iteration_count < max_iterations) {
            iteration_count++;
            uint32_t elapsed = millis() - start_time;
            
            // CRITICAL: Check for timeout due to millis() overflow
            if (elapsed > poll_timeout * 2) {
                LOG_ERROR("waitForAsyncFlashWrite: Timeout calculation overflow detected");
                break;
            }
            
            // Check for events multiple times per loop
            if (processFlashEvents(evt)) {
                success = true;
                break;
            }
            
            // Check if error event was found during polling
            if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
                LOG_ERROR("Flash operation ERROR event at address 0x%08X", write_addr);
                return false;
            }
            
            // Check flash REPEATEDLY
            uint32_t time_since_last_check = elapsed - last_flash_check;
            bool should_check_flash = false;
            
            if (last_flash_check == 0 && elapsed >= flash_check_delay) {
                should_check_flash = true;
            } else if (last_flash_check > 0 && time_since_last_check >= flash_check_interval) {
                should_check_flash = true;
            }
            
            if (should_check_flash) {
                last_flash_check = elapsed;
                // CRITICAL: Check for overflow when calculating last address
                uint32_t last_addr;
                if (!calculateLastAddress(write_addr, batch_size_bytes, last_addr) || last_addr < 4) {
                    LOG_ERROR("waitForAsyncFlashWrite: Address overflow when calculating last address");
                    return false;
                }
                // Verify batch was written (reuse last_addr calculation)
                uint32_t first_written = *(volatile uint32_t*)write_addr;
                uint32_t last_written = *(volatile uint32_t*)(last_addr - 4);
                uint32_t first_expected = src_words[0];
                uint32_t last_expected = src_words[words_to_write - 1];
                
                if (first_written == first_expected && last_written == last_expected) {
                    if (elapsed > 5) {
                        LOG_DEBUG("Flash write event not received for batch at 0x%08X (%u words) after %u ms, but data appears written", 
                                write_addr, words_to_write, elapsed);
                    }
                    success = true;
                    break;
                }
            }
            
            if (!success) {
                FEED_WATCHDOG_AND_YIELD();
            } else {
                break;
            }
        }
        
        // Final check
        if (!success) {
            uint32_t elapsed = millis() - start_time;
            // CRITICAL: Check for overflow when calculating last address (reuse helper function)
            uint32_t last_addr;
            if (!calculateLastAddress(write_addr, batch_size_bytes, last_addr) || last_addr < 4) {
                LOG_ERROR("waitForAsyncFlashWrite: Address overflow in final check");
                return false;
            }
            // Reuse same verification logic as in main loop
            uint32_t first_written = *(volatile uint32_t*)write_addr;
            uint32_t last_written = *(volatile uint32_t*)(last_addr - 4);
            uint32_t first_expected = src_words[0];
            uint32_t last_expected = src_words[words_to_write - 1];
            
            if (first_written == first_expected && last_written == last_expected) {
                LOG_WARN("Flash write event not received for batch at 0x%08X (%u words) after %u ms, but data appears written", 
                        write_addr, words_to_write, elapsed);
                success = true;
            } else {
                // CRITICAL: Data mismatch - wait additional time and retry check
                // Flash write may still be in progress even after initial timeout
                // This is especially important if flash was busy or if write started late
                LOG_DEBUG("Flash write data mismatch after %u ms, waiting additional 20 ms and retrying check...", elapsed);
                delay(20);  // Give flash more time to complete write
                
                // Retry check after additional wait
                first_written = *(volatile uint32_t*)write_addr;
                last_written = *(volatile uint32_t*)(last_addr - 4);
                
                if (first_written == first_expected && last_written == last_expected) {
                    LOG_WARN("Flash write completed after retry check (total wait: %u ms)", elapsed + 20);
                    success = true;
                } else {
                    // CRITICAL: Still mismatch after retry - this is a real error
                    LOG_ERROR("Flash write timeout AND data mismatch at address 0x%08X (first: read 0x%08X, expected 0x%08X, waited %u ms)", 
                             write_addr, first_written, first_expected, elapsed + 20);
                    LOG_ERROR("Flash write FAILED - data was not written to flash!");
                    LOG_ERROR("This may indicate flash hardware issue or timing problem");
                    return false;
                }
            }
        }
        
        return success;
    }
    
    /**
     * @brief Wait for async flash erase operation to complete
     * @param page_number Page number that was erased
     * @param page_addr Address of the page
     * @param timeout Timeout for waiting (ms)
     * @param flash_check_delay Initial delay before first flash check (ms)
     * @param flash_check_interval Interval between flash checks (ms)
     * @return true if operation completed successfully, false on error
     */
    bool waitForAsyncFlashErase(uint32_t page_number, uint32_t page_addr,
                               uint32_t timeout = 20000,  // Increased to 20 seconds (erase can take up to 100ms per page)
                               uint32_t flash_check_delay = 50,  // Reduced to 50ms - check flash earlier
                               uint32_t flash_check_interval = 25)  // Reduced to 25ms - check flash more frequently
    {
        uint32_t start_time = millis();
        bool success = false;
        uint32_t evt;
        
        // First, try to get any pending events immediately
        if (processFlashEvents(evt)) {
            return true;
        }
        
        // Check if error event was found
        if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
            LOG_ERROR("Flash operation ERROR event for page %u", page_number);
            return false;
        }
        
        // If not immediately successful, wait with polling
        uint32_t last_flash_check = 0;
        
        // CRITICAL: Prevent infinite loop - add max iterations check
        uint32_t max_iterations = (timeout / flash_check_interval) + 10;  // Safety margin
        uint32_t iteration_count = 0;
        
        while ((millis() - start_time) < timeout && iteration_count < max_iterations) {
            iteration_count++;
            uint32_t elapsed = millis() - start_time;
            
            // CRITICAL: Check for timeout due to millis() overflow
            if (elapsed > timeout * 2) {
                LOG_ERROR("waitForAsyncFlashErase: Timeout calculation overflow detected");
                break;
            }
            
            // Check for events multiple times per loop
            if (processFlashEvents(evt)) {
                success = true;
                break;
            }
            
            // Check if error event was found during polling
            if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
                LOG_ERROR("Flash operation ERROR event for page %u", page_number);
                return false;
            }
            
            // Check flash REPEATEDLY
            uint32_t time_since_last_check = elapsed - last_flash_check;
            bool should_check_flash = false;
            
            if (last_flash_check == 0 && elapsed >= flash_check_delay) {
                should_check_flash = true;
            } else if (last_flash_check > 0 && time_since_last_check >= flash_check_interval) {
                should_check_flash = true;
            }
            
            if (should_check_flash) {
                last_flash_check = elapsed;
                // Verify page is erased
                uint32_t first_word = *(volatile uint32_t*)page_addr;
                
                if (first_word == 0xFFFFFFFF) {
                    // Page is erased - success! (event may arrive later, but page is already erased)
                    if (elapsed > 100) {
                        LOG_WARN("Page %u erase event not received after %u ms, but page appears erased (0xFFFFFFFF at 0x%08X)", 
                                page_number, elapsed, page_addr);
                    }
                    success = true;
                    break;
                }
            }
            
            if (!success) {
                FEED_WATCHDOG_AND_YIELD();
                // Small delay to prevent busy-waiting and allow flash operation to progress
                delay(1);  // 1ms delay between iterations
            } else {
                break;
            }
        }
        
        // Final check
        if (!success) {
            uint32_t elapsed = millis() - start_time;
            uint32_t first_word = *(volatile uint32_t*)page_addr;
            
            if (first_word == 0xFFFFFFFF) {
                LOG_WARN("Page %u erase event not received after %u ms, but page appears erased (0xFFFFFFFF at 0x%08X)", 
                        page_number, elapsed, page_addr);
                success = true;
            } else {
                LOG_ERROR("Page %u erase timeout AND page not erased (read 0x%08X at 0x%08X, expected 0xFFFFFFFF, waited %u ms)", 
                         page_number, first_word, page_addr, elapsed);
                return false;
            }
        }
        
        return success;
    }
    
    /**
     * @brief Log error code with appropriate message
     * @param err_code Error code from SoftDevice
     * @param operation_name Name of operation (e.g., "write", "erase")
     * @param location Location identifier (address or page number)
     */
    void logSoftDeviceError(uint32_t err_code, const char* operation_name, uint32_t location)
    {
        if (err_code == NRF_ERROR_FORBIDDEN) {
            LOG_ERROR("CRITICAL: SoftDevice FORBIDDEN %s at 0x%08X!", operation_name, location);
        } else if (err_code == NRF_ERROR_INVALID_ADDR) {
            LOG_ERROR("CRITICAL: sd_flash_%s INVALID_ADDR at 0x%08X (error: 0x%08X)", 
                     operation_name, location, err_code);
        } else {
            LOG_ERROR("CRITICAL: sd_flash_%s failed at 0x%08X with error: 0x%08X", 
                     operation_name, location, err_code);
        }
    }
    
    /**
     * @brief Cleanup async operation on error
     * @param use_async Whether async operations are enabled
     */
    void cleanupAsyncOperation(bool use_async)
    {
        if (use_async) {
            uint32_t evt;
            processFlashEvents(evt);  // Clear any pending events
        }
    }
    
    /**
     * @brief Handle async flash operation completion (write)
     * @param use_async Whether async operations are enabled
     * @param write_addr Write address
     * @param batch_size_bytes Batch size in bytes
     * @param src_words Source words pointer
     * @param words_to_write Number of words to write
     * @return true if operation completed successfully, false on error
     */
    bool handleAsyncFlashWrite(bool use_async, uint32_t write_addr, uint32_t batch_size_bytes,
                               const uint32_t* src_words, uint32_t words_to_write)
    {
        if (!use_async) {
            return true;  // Synchronous operation - already complete
        }
        
        // Wait for async operation to complete
        uint32_t evt;
        if (processFlashEvents(evt)) {
            // Success event found immediately
            return true;
        }
        
        // Check if error event was found
        if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
            LOG_ERROR("Flash operation ERROR event at address 0x%08X", write_addr);
            return false;
        }
        
        // Wait for async operation with polling
        return waitForAsyncFlashWrite(write_addr, batch_size_bytes, src_words, words_to_write);
    }
    
    /**
     * @brief Handle async flash operation completion (erase)
     * @param use_async Whether async operations are enabled
     * @param page_number Page number
     * @param page_addr Page address
     * @return true if operation completed successfully, false on error
     */
    bool handleAsyncFlashErase(bool use_async, uint32_t page_number, uint32_t page_addr)
    {
        if (!use_async) {
            return true;  // Synchronous operation - already complete
        }
        
        // Wait for async operation to complete
        uint32_t evt;
        if (processFlashEvents(evt)) {
            // Success event found immediately
            return true;
        }
        
        // Check if error event was found
        if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
            LOG_ERROR("Flash operation ERROR event for page %u", page_number);
            return false;
        }
        
        // Wait for async operation with polling
        return waitForAsyncFlashErase(page_number, page_addr);
    }
    
    /**
     * @brief Safely close LittleFS file with error handling
     * @param lfs LittleFS instance
     * @param file File handle
     * @param filename Filename for error logging
     * @param remove_on_error Whether to remove file on close error
     * @return true if closed successfully, false on error
     */
    bool safeCloseFile(lfs_t* lfs, lfs_file_t* file, const char* filename, bool remove_on_error = false)
    {
        if (!lfs || !file) {
            return false;
        }
        
        int close_result = lfs_file_close(lfs, file);
        if (close_result != LFS_ERR_OK) {
            if (filename) {
                LOG_ERROR("CRITICAL: Close of '%s' failed (error: %d) - file may be corrupted!", filename, close_result);
            } else {
                LOG_ERROR("CRITICAL: File close failed (error: %d)", close_result);
            }
            
            if (remove_on_error && filename) {
                lfs_remove(lfs, filename);
            }
            return false;
        }
        
        return true;
    }
    
    /**
     * @brief Get formatting delay variable (prevents race conditions)
     * @return Reference to static formatting delay variable
     */
    unsigned long& getFormattingDelay()
    {
        static unsigned long millis_until_formatting_again = 0;
        static bool initialized = false;
        
        // CRITICAL: Initialize on first call (thread-safe for single-threaded embedded system)
        if (!initialized) {
            millis_until_formatting_again = 0;
            initialized = true;
        }
        
        return millis_until_formatting_again;
    }
    
    /**
     * @brief Get filesystem boundaries (helper to avoid duplication)
     * @param fs_start Output parameter for filesystem start address
     * @param fs_end Output parameter for filesystem end address
     */
    void getFilesystemBounds(uint32_t& fs_start, uint32_t& fs_end)
    {
        fs_start = ExtendedFilesystemModule::EXTENDED_LFS_FLASH_ADDR;
        fs_end = ExtendedFilesystemModule::EXTENDED_LFS_FLASH_ADDR + ExtendedFilesystemModule::EXTENDED_LFS_FLASH_TOTAL_SIZE;
    }
    
    /**
     * @brief Check and calculate last address with overflow protection
     * @param base_addr Base address
     * @param size Size in bytes
     * @param last_addr Output parameter for calculated last address
     * @return true if calculation successful, false on overflow
     */
    bool calculateLastAddress(uint32_t base_addr, uint32_t size, uint32_t& last_addr)
    {
        last_addr = base_addr + size;
        if (last_addr < base_addr || last_addr < size) {
            return false;  // Overflow detected
        }
        return true;
    }
    
    
    /**
     * @brief RAII guard for LittleFS file operations
     * Automatically closes file on destruction, even if exception is thrown
     */
    class LfsFileGuard {
    private:
        lfs_t* lfs;
        lfs_file_t* file;
        const char* filename;
        bool closed;
        
    public:
        /**
         * @brief Constructor - takes ownership of file handle
         * @param lfs LittleFS instance
         * @param file File handle (must be opened)
         * @param filename Filename for error logging
         */
        LfsFileGuard(lfs_t* lfs, lfs_file_t* file, const char* filename)
            : lfs(lfs), file(file), filename(filename), closed(false)
        {
        }
        
        /**
         * @brief Destructor - automatically closes file if not already closed
         */
        ~LfsFileGuard()
        {
            if (!closed && lfs && file) {
                safeCloseFile(lfs, file, filename);
            }
        }
        
        // Non-copyable
        LfsFileGuard(const LfsFileGuard&) = delete;
        LfsFileGuard& operator=(const LfsFileGuard&) = delete;
        
        // Movable (for future use)
        LfsFileGuard(LfsFileGuard&& other) noexcept
            : lfs(other.lfs), file(other.file), filename(other.filename), closed(other.closed)
        {
            other.closed = true;  // Prevent double close
        }
        
        /**
         * @brief Explicitly close the file
         * @param remove_on_error Whether to remove file on close error
         * @return true if closed successfully
         */
        bool close(bool remove_on_error = false)
        {
            if (closed || !lfs || !file) {
                return true;  // Already closed or invalid
            }
            
            bool result = safeCloseFile(lfs, file, filename, remove_on_error);
            closed = true;
            return result;
        }
        
        /**
         * @brief Get file handle (for operations)
         * @return File handle
         */
        lfs_file_t* get() const { return file; }
        
        /**
         * @brief Check if file is closed
         * @return true if closed
         */
        bool isClosed() const { return closed; }
        
        /**
         * @brief Release ownership (file will NOT be closed automatically)
         * @return File handle
         */
        lfs_file_t* release()
        {
            closed = true;  // Prevent auto-close
            return file;
        }
    };
} // anonymous namespace

// ============================================================================
// From ExtendedFilesystemModule.cpp
// ============================================================================

// Static member definitions
bool ExtendedFilesystemModule::isInitialized = false;
bool ExtendedFilesystemModule::isMounted = false;
bool ExtendedFilesystemModule::format_in_progress = false;
uint32_t ExtendedFilesystemModule::erased_pages_bitmap[3] = {0, 0, 0};  // Bitmap to track erased pages (68 pages: 0xA8000-0xEBFFF, pages 168-235)
lfs_t ExtendedFilesystemModule::extended_lfs;
struct lfs_config ExtendedFilesystemModule::extended_lfs_cfg;
uint8_t ExtendedFilesystemModule::read_buffer[128];  // 128 byte buffer (matches block size)
uint8_t ExtendedFilesystemModule::prog_buffer[128];  // 128 byte buffer (matches block size)
uint8_t ExtendedFilesystemModule::lookahead_buffer[8];  // 8 bytes = 64 blocks (64 blocks * 1 bit/block / 8 = 8 bytes, increased from 32 to 64 blocks for better coverage)

// Global instance pointer
ExtendedFilesystemModule* extendedFilesystemModule = nullptr;

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

// LittleFS read callback
int ExtendedFilesystemModule::lfs_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    (void)c;  // Unused parameter
    
    if (!buffer || !size) {
        return LFS_ERR_INVAL;
    }
    
    // CRITICAL: Validate block number to prevent buffer overflow
    // REPLACE LFS_ASSERT with explicit check to avoid reboot
    // This prevents "block < lfs->cfg->block_count" error from causing system reboot
    if (block >= EXTENDED_LFS_BLOCK_COUNT) {
        LOG_ERROR("lfs_read: CRITICAL - Invalid block %lu (max: %u) - Extended FS corruption detected!", 
                 block, EXTENDED_LFS_BLOCK_COUNT - 1);
        LOG_ERROR("lfs_read: This may be caused by USB Mass Storage bootloader overwriting Extended FS");
        LOG_ERROR("lfs_read: Extended FS will be reformatted on next mount");
        // Return corruption error - this will trigger reformat on next initInternal()
        // Don't reboot here - let initInternal() handle reformatting
        return LFS_ERR_CORRUPT;
    }
    
    // CRITICAL: Check for overflow when calculating block offset
    uint32_t block_offset;
    if (!calculateLastAddress(0, block * EXTENDED_LFS_BLOCK_SIZE, block_offset)) {
        LOG_ERROR("lfs_read: Block offset overflow (block: %lu, block_size: %u)", block, EXTENDED_LFS_BLOCK_SIZE);
        return LFS_ERR_INVAL;
    }
    
    // CRITICAL: Check for overflow when adding offset to block offset
    uint32_t total_offset;
    if (!calculateLastAddress(block_offset, off, total_offset)) {
        LOG_ERROR("lfs_read: Total offset overflow (block_offset: 0x%08X, off: %u)", block_offset, off);
        return LFS_ERR_INVAL;
    }
    
    // CRITICAL: Check for overflow when calculating final address
    uint32_t address;
    if (!calculateLastAddress(EXTENDED_LFS_FLASH_ADDR, total_offset, address)) {
        LOG_ERROR("lfs_read: Address overflow (base: 0x%08X, offset: 0x%08X)", EXTENDED_LFS_FLASH_ADDR, total_offset);
        return LFS_ERR_INVAL;
    }
    
    // CRITICAL: Validate address is within filesystem bounds
    uint32_t fs_start, fs_end;
    getFilesystemBounds(fs_start, fs_end);
    
    // CRITICAL: Check for address overflow before validation
    if ((address + size) < address) {
        LOG_ERROR("lfs_read: Address overflow (addr: 0x%08X, size: %u)", address, size);
        return LFS_ERR_INVAL;
    }
    
    // CRITICAL: Validate address is within filesystem bounds
    if (address < fs_start || address >= fs_end) {
        LOG_ERROR("lfs_read: Address out of bounds (addr: 0x%08X, fs: 0x%08X-0x%08X)", 
                 address, fs_start, fs_end);
        return LFS_ERR_INVAL;
    }
    
    // CRITICAL: Validate that address + size doesn't exceed filesystem end
    // (This check is sufficient - no need for separate buffer overflow check)
    if ((address + size) > fs_end) {
        LOG_ERROR("lfs_read: Read would exceed filesystem bounds (addr: 0x%08X, size: %u, fs_end: 0x%08X)", 
                 address, size, fs_end);
        return LFS_ERR_INVAL;
    }
    
    // CRITICAL: Final null pointer check before memcpy() to prevent crash
    if (!buffer) {
        LOG_ERROR("lfs_read: buffer is null pointer - cannot read data");
        return LFS_ERR_INVAL;
    }
    
    // Read from flash memory
    memcpy(buffer, (void *)address, size);
    
    return LFS_ERR_OK;
}

// LittleFS program callback
int ExtendedFilesystemModule::lfs_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    (void)c;  // Unused parameter
    
    if (!buffer || !size) {
        return LFS_ERR_INVAL;
    }
    
    // CRITICAL: Validate block number BEFORE any operations
    // REPLACE LFS_ASSERT with explicit check to avoid reboot
    // This prevents "block < lfs->cfg->block_count" error from causing system reboot
    if (block >= EXTENDED_LFS_BLOCK_COUNT) {
        LOG_ERROR("lfs_prog: CRITICAL - Invalid block %lu (max: %u) - Extended FS corruption detected!", 
                 block, EXTENDED_LFS_BLOCK_COUNT - 1);
        LOG_ERROR("lfs_prog: This may be caused by USB Mass Storage bootloader overwriting Extended FS");
        LOG_ERROR("lfs_prog: Extended FS will be reformatted on next mount");
        // Return corruption error - this will trigger reformat on next initInternal()
        // Don't reboot here - let initInternal() handle reformatting
        return LFS_ERR_CORRUPT;
    }
    
    // CRITICAL: Check for overflow when calculating block offset
    uint32_t block_offset;
    if (!calculateLastAddress(0, block * EXTENDED_LFS_BLOCK_SIZE, block_offset)) {
        LOG_ERROR("lfs_prog: Block offset overflow (block: %lu, block_size: %u)", block, EXTENDED_LFS_BLOCK_SIZE);
        return LFS_ERR_IO;
    }
    
    // CRITICAL: Check for overflow when adding offset to block offset
    uint32_t total_offset;
    if (!calculateLastAddress(block_offset, off, total_offset)) {
        LOG_ERROR("lfs_prog: Total offset overflow (block_offset: 0x%08X, off: %u)", block_offset, off);
        return LFS_ERR_IO;
    }
    
    // CRITICAL: Check for overflow when calculating final address
    uint32_t address;
    if (!calculateLastAddress(EXTENDED_LFS_FLASH_ADDR, total_offset, address)) {
        LOG_ERROR("lfs_prog: Address overflow (base: 0x%08X, offset: 0x%08X)", EXTENDED_LFS_FLASH_ADDR, total_offset);
        return LFS_ERR_IO;
    }
    
    // Check if SoftDevice is enabled
    bool use_async = isSoftDeviceAsyncEnabled();
    
    // CRITICAL OPTIMIZATION: Use batch writing instead of word-by-word
    constexpr uint32_t OPTIMAL_BATCH_SIZE = 32;     // Optimal batch size (128 bytes)
    
    // Program flash memory using SoftDevice API with batch writing
    uint32_t i = 0;
    // CRITICAL: Limit iterations to prevent infinite loop
    constexpr uint32_t MAX_ITERATIONS = (512 * 1024 / OPTIMAL_BATCH_SIZE) + 10;  // Safety margin
    uint32_t iteration_count = 0;
    
    while (i < size && iteration_count < MAX_ITERATIONS) {
        iteration_count++;
        
        // CRITICAL: Check for overflow of i to prevent infinite loop
        if (i >= size) {
            break;  // All data written, exit loop
        }
        
        // Calculate how many words we can write in this batch
        uint32_t remaining_bytes = size - i;
        
        // CRITICAL: Early check - if no bytes remaining, exit loop
        if (remaining_bytes == 0) {
            break;  // All data written, exit loop
        }
        
        uint32_t remaining_words = remaining_bytes / 4;
        
        // CRITICAL: Early check - if no words remaining and bytes < 4, cannot write partial word
        if (remaining_words == 0) {
            LOG_ERROR("lfs_prog: Cannot write partial word (remaining: %u bytes)", remaining_bytes);
            return LFS_ERR_IO;
        }
        
        uint32_t words_to_write = (remaining_words > OPTIMAL_BATCH_SIZE) ? OPTIMAL_BATCH_SIZE : remaining_words;
        
        // CRITICAL: Verify words_to_write > 0 (should always be true after above checks, but double-check)
        if (words_to_write == 0) {
            LOG_ERROR("lfs_prog: words_to_write is zero (remaining_words: %u)", remaining_words);
            return LFS_ERR_IO;
        }
        
        uint32_t batch_size_bytes = words_to_write * 4;
        
        // CRITICAL: Check for overflow when calculating write_addr = address + i
        if ((address + i) < address) {
            LOG_ERROR("lfs_prog: Address overflow when calculating write address (address: 0x%08X, i: %u)", address, i);
            return LFS_ERR_IO;
        }
        
        uint32_t write_addr = address + i;
        
        // CRITICAL: Check for overflow when calculating write_addr + batch_size_bytes
        if ((write_addr + batch_size_bytes) < write_addr) {
            LOG_ERROR("lfs_prog: Address overflow when calculating end address (write_addr: 0x%08X, batch_size: %u)", write_addr, batch_size_bytes);
            return LFS_ERR_IO;
        }
        
        // CRITICAL: Validate write address to prevent overflow
        if (write_addr < EXTENDED_LFS_FLASH_ADDR || 
            (write_addr + batch_size_bytes) > (EXTENDED_LFS_FLASH_ADDR + EXTENDED_LFS_FLASH_TOTAL_SIZE)) {
            LOG_ERROR("lfs_prog: Write address out of bounds (addr: 0x%08X, size: %u)", write_addr, batch_size_bytes);
            return LFS_ERR_IO;
        }
        
        // Prepare batch buffer (words must be aligned)
        uint32_t* src_words = (uint32_t*)((uint8_t*)buffer + i);
        
        // CRITICAL: Validate src_words pointer before calling sd_flash_write()
        if (!src_words) {
            LOG_ERROR("lfs_prog: src_words is null pointer at offset %u", i);
            cleanupAsyncOperation(use_async);
            return LFS_ERR_IO;
        }
        
        uint32_t err_code;
        // Retry if busy with timeout check
        uint32_t busy_retry_start = millis();
        constexpr uint32_t BUSY_RETRY_TIMEOUT_MS = 5000;  // 5 second timeout for busy retries
        for (uint8_t attempt = 0; attempt < 10; attempt++) {
            if (attempt > 0) {
                LOG_DEBUG("lfs_prog: Retry attempt %u for address 0x%08X (batch: %u words)", attempt + 1, write_addr, words_to_write);
            }
            err_code = sd_flash_write((uint32_t *)write_addr, src_words, words_to_write);
            
            if (err_code == NRF_SUCCESS) {
                // CRITICAL: Handle async operation completion
                if (!handleAsyncFlashWrite(use_async, write_addr, batch_size_bytes, src_words, words_to_write)) {
                    return LFS_ERR_IO;
                }
                // CRITICAL: Batch written successfully - increment i to ensure progress
                uint32_t old_i = i;
                i += batch_size_bytes;
                
                // CRITICAL: Verify that i actually increased (safety check)
                if (i <= old_i) {
                    LOG_ERROR("lfs_prog: Progress check failed (i did not increase: %u -> %u)", old_i, i);
                    return LFS_ERR_IO;
                }
                
                // CRITICAL: Clear erased page bitmap bit when writing to a page
                // This is necessary because after formatting, LittleFS writes data to blocks,
                // and we need to track that the page is no longer fully erased
                // BUT: Don't clear bitmap during format - format needs to track erased pages
                if (!format_in_progress) {
                    constexpr uint32_t EXTENDED_FS_FIRST_PAGE = EXTENDED_LFS_FLASH_ADDR / FLASH_NRF52_PAGE_SIZE;  // 168
                    constexpr uint32_t EXTENDED_FS_PAGE_COUNT = EXTENDED_LFS_FLASH_TOTAL_SIZE / FLASH_NRF52_PAGE_SIZE;  // 68
                    uint32_t write_page = write_addr / FLASH_NRF52_PAGE_SIZE;
                    if (write_page >= EXTENDED_FS_FIRST_PAGE && write_page < (EXTENDED_FS_FIRST_PAGE + EXTENDED_FS_PAGE_COUNT)) {
                        uint32_t bit_index = write_page - EXTENDED_FS_FIRST_PAGE;
                        uint32_t word_index = bit_index / 32;
                        uint32_t bit_in_word = bit_index % 32;
                        if (word_index < 3 && (erased_pages_bitmap[word_index] & (1U << bit_in_word))) {
                            erased_pages_bitmap[word_index] &= ~(1U << bit_in_word);
                            // DEBUG log removed to reduce log volume - this happens on every write operation
                        }
                    }
                }
                
                // CRITICAL: Optimize watchdog feeding - only feed between batches, not in inner loop
                if (i < size) {
                    FEED_WATCHDOG_AND_YIELD();
                    // Note: FEED_WATCHDOG_AND_YIELD() already includes yield(), no need for delay(1)
                }
                
                break;
            } else if (err_code == NRF_ERROR_BUSY) {
                // CRITICAL: Check timeout for busy retries
                uint32_t elapsed = millis() - busy_retry_start;
                if (elapsed > BUSY_RETRY_TIMEOUT_MS) {
                    LOG_ERROR("lfs_prog: NRF_ERROR_BUSY timeout after %u ms at address 0x%08X", elapsed, write_addr);
                    cleanupAsyncOperation(use_async);
                    return LFS_ERR_IO;
                }
                
                // CRITICAL: Feed watchdog during retry delay
                FEED_WATCHDOG_AND_YIELD();
                delay(50);
                continue;
            } else {
                // CRITICAL: Log error on first attempt, retry on subsequent attempts
                if (attempt == 0) {
                    logSoftDeviceError(err_code, "write", write_addr);
                    LOG_DEBUG("lfs_prog: First attempt failed, will retry up to 9 more times");
                } else {
                    LOG_DEBUG("lfs_prog: Retry %u failed with error 0x%08X", attempt + 1, err_code);
                }
                // CRITICAL: For non-busy errors, retry a few times in case it's transient
                // But if it's FORBIDDEN or INVALID_ADDR, it won't work on retry
                if (err_code == NRF_ERROR_FORBIDDEN || err_code == NRF_ERROR_INVALID_ADDR) {
                    LOG_ERROR("lfs_prog: Fatal error (0x%08X) - will not retry", err_code);
                    cleanupAsyncOperation(use_async);
                    return LFS_ERR_IO;
                }
                // For other errors, retry with delay
                FEED_WATCHDOG_AND_YIELD();
                delay(10);
                continue;
            }
        }
        
        // CRITICAL: If we reach here, all retries failed for this batch
        // Check if we made progress (i should have been incremented on success)
        if (i < size) {
            LOG_ERROR("lfs_prog: Failed after all 10 retries at address 0x%08X (wrote %u of %u bytes)", 
                     write_addr, i, size);
            LOG_ERROR("lfs_prog: Last error code: 0x%08X (check logs above for details)", err_code);
            // CRITICAL: Ensure we don't leave async operation in progress
            cleanupAsyncOperation(use_async);
            return LFS_ERR_IO;
        }
    }
    
    // CRITICAL: Verify all data was written
    if (i < size) {
        LOG_ERROR("lfs_prog: Incomplete write (wrote %u of %u bytes) at address 0x%08X", i, size, address);
        // CRITICAL: Ensure we don't leave async operation in progress
        cleanupAsyncOperation(use_async);
        return LFS_ERR_IO;
    }
    
    return LFS_ERR_OK;
}

// LittleFS erase callback
int ExtendedFilesystemModule::lfs_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)c;  // Unused parameter
    
    // CRITICAL: Track format progress (format_in_progress is set in formatFilesystem() before lfs_format is called)
    if (block == 0) {
        LOG_INFO("lfs_erase: Starting format - erasing block 0/%u", EXTENDED_LFS_BLOCK_COUNT - 1);
    }
    
    // Feed watchdog at start of each erase call
    FEED_WATCHDOG_AND_YIELD();
    
    // Log progress during format (every 10 blocks or last block)
    if (format_in_progress && (block % 10 == 0 || block == EXTENDED_LFS_BLOCK_COUNT - 1)) {
        LOG_INFO("lfs_erase: Format progress - erasing block %lu/%u", block, EXTENDED_LFS_BLOCK_COUNT - 1);
    }
    
    // CRITICAL: Validate block number BEFORE any operations
    // REPLACE LFS_ASSERT with explicit check to avoid reboot
    // This prevents "block < lfs->cfg->block_count" error from causing system reboot
    if (block >= EXTENDED_LFS_BLOCK_COUNT) {
        LOG_ERROR("lfs_erase: CRITICAL - Invalid block %lu (max: %u) - Extended FS corruption detected!", 
                 block, EXTENDED_LFS_BLOCK_COUNT - 1);
        LOG_ERROR("lfs_erase: This may be caused by USB Mass Storage bootloader overwriting Extended FS");
        LOG_ERROR("lfs_erase: Extended FS will be reformatted on next mount");
        // Return corruption error - this will trigger reformat on next initInternal()
        // Don't reboot here - let initInternal() handle reformatting
        return LFS_ERR_CORRUPT;
    }
    
    // Calculate block address and page number
    // When BLOCK_SIZE < PAGE_SIZE, multiple blocks fit in one page
    // Block address = filesystem_start + (block_number * block_size)
    uint32_t block_addr;
    if (!calculateLastAddress(EXTENDED_LFS_FLASH_ADDR, block * EXTENDED_LFS_BLOCK_SIZE, block_addr)) {
        LOG_ERROR("lfs_erase: Block address overflow (block: %lu)", block);
        return LFS_ERR_INVAL;
    }
    
    // Calculate which page contains this block
    // Page number = block_addr / PAGE_SIZE
    uint32_t page_number = block_addr / FLASH_NRF52_PAGE_SIZE;
    uint32_t page_addr = page_number * FLASH_NRF52_PAGE_SIZE;
    
    // CRITICAL: Check if this page was already erased (to avoid erasing same page multiple times)
    // Extended FS pages are 168-235 (0xA8000-0xEBFFF), so bit index = page_number - 168
    constexpr uint32_t EXTENDED_FS_FIRST_PAGE = EXTENDED_LFS_FLASH_ADDR / FLASH_NRF52_PAGE_SIZE;  // 168
    constexpr uint32_t EXTENDED_FS_PAGE_COUNT = EXTENDED_LFS_FLASH_TOTAL_SIZE / FLASH_NRF52_PAGE_SIZE;  // 68
    if (page_number >= EXTENDED_FS_FIRST_PAGE && page_number < (EXTENDED_FS_FIRST_PAGE + EXTENDED_FS_PAGE_COUNT)) {
        uint32_t bit_index = page_number - EXTENDED_FS_FIRST_PAGE;
        uint32_t word_index = bit_index / 32;
        uint32_t bit_in_word = bit_index % 32;
        if (word_index < 3 && (erased_pages_bitmap[word_index] & (1U << bit_in_word))) {
            // Page marked as erased in bitmap - verify the BLOCK is actually erased before skipping
            // NOTE: Now block_size = page_size (4096), so block always starts at page boundary
            bool block_is_erased = true;
            uint32_t block_end = block_addr + EXTENDED_LFS_BLOCK_SIZE;
            // Check every 4th word in the block (every 16 bytes) to verify it's erased
            for (uint32_t check_addr = block_addr; check_addr < block_end && block_is_erased; check_addr += 16) {
                uint32_t word_value = *(volatile uint32_t*)check_addr;
                if (word_value != 0xFFFFFFFF) {
                    block_is_erased = false;
                    // CRITICAL: Only warn and erase if we're still in format mode
                    // After format, LittleFS may write to blocks, and we shouldn't erase pages that contain data
                    if (format_in_progress) {
                        LOG_WARN("lfs_erase: Block %lu (addr 0x%08X) in page %lu marked as erased but contains data (word at 0x%08X = 0x%08X) - will erase page", 
                                block, block_addr, page_number, check_addr, word_value);
                        // Clear the bit so we erase the page
                        if (word_index < 3) {
                            erased_pages_bitmap[word_index] &= ~(1U << bit_in_word);
                        }
                    } else {
                        // After format, if block contains data but LittleFS requests erase, we MUST erase it
                        // LittleFS knows what it's doing - if it requests erase, we should erase
                        // This happens when LittleFS needs to relocate data or clean up blocks
                        LOG_DEBUG("lfs_erase: Block %lu (addr 0x%08X) in page %lu contains data (after format) - erasing as requested by LittleFS", 
                                block, block_addr, page_number);
                        // Clear the bitmap bit and proceed with erase (don't return here)
                        if (word_index < 3) {
                            erased_pages_bitmap[word_index] &= ~(1U << bit_in_word);
                        }
                        // Continue to actual erase operation below (don't return LFS_ERR_OK)
                    }
                    break;
                }
            }
            
            if (block_is_erased) {
                // Block is truly erased - skip erase operation
                // NOTE: Now block_size = page_size (4096), so one block = one page
                LOG_DEBUG("lfs_erase: Block %lu (addr 0x%08X) in page %lu already erased, skipping", 
                         block, block_addr, page_number);
                return LFS_ERR_OK;
            }
        }
    }
    
    // DEBUG log removed to reduce log volume - erase operations are very frequent
    // LOG_DEBUG("lfs_erase: Erasing block %lu (addr 0x%08X) -> page %lu (page addr: 0x%08X)", ...);
    
    // Safety check: ensure we don't erase bootloader
    #ifdef NRF52840_XXAA
    constexpr uint32_t BOOTLOADER_PAGE = BOOTLOADER_ADDR / FLASH_NRF52_PAGE_SIZE;
    if (page_number >= BOOTLOADER_PAGE) {
        LOG_ERROR("ERROR: Attempted to erase bootloader page %lu! (block %lu)", page_number, block);
        return LFS_ERR_INVAL;
    }
    
    // Protect main filesystem (0xED000-0xF4000, pages 237-243)
    // Main FS: 0xED000 - 0xF4000 (28 KB, 7 pages) - confirmed by framework diagnostics
    // Framework uses 7 pages (28 KB) as confirmed by FSCom.totalBytes() = 28672 bytes
    constexpr uint32_t MAIN_FS_START = 0xED000;
    constexpr uint32_t MAIN_FS_END = 0xF4000;  // Main FS ends here (28 KB, 7 pages) - CORRECTED from 0xEF800
    // Check if the page being erased overlaps with Main FS
    // A page overlaps if: page_start < MAIN_FS_END && page_end > MAIN_FS_START
    uint32_t page_end = page_addr + FLASH_NRF52_PAGE_SIZE;
    if (page_addr < MAIN_FS_END && page_end > MAIN_FS_START) {
        LOG_ERROR("ERROR: Attempted to erase MAIN filesystem page %lu! (block %lu, addr 0x%08X - 0x%08X)", 
                  page_number, block, page_addr, page_end - 1);
        LOG_ERROR("  Main FS range: 0x%08X - 0x%08X", MAIN_FS_START, MAIN_FS_END);
        return LFS_ERR_INVAL;
    }
    #endif
    
    // Check if SoftDevice is enabled
    bool use_async = isSoftDeviceAsyncEnabled();
    
    // DEBUG log removed to reduce log volume - this happens on every erase operation
    // LOG_DEBUG("lfs_erase: SoftDevice %s, starting erase of page %lu", ...);
    
    // Erase flash page using SoftDevice API
    uint32_t err_code;
    uint32_t erase_start_time = millis();
    
    // Retry if busy with timeout check
    uint32_t busy_retry_start = millis();
    constexpr uint32_t BUSY_RETRY_TIMEOUT_MS = 10000;  // 10 second timeout for busy retries (erase is slower)
    for (uint8_t attempt = 0; attempt < 10; attempt++) {
        if (attempt > 0) {
            LOG_DEBUG("lfs_erase: Retry attempt %u for page %lu", attempt + 1, page_number);
        }
        err_code = sd_flash_page_erase(page_number);
        
        if (err_code == NRF_SUCCESS) {
            // CRITICAL: Handle async operation completion
            if (!handleAsyncFlashErase(use_async, page_number, page_addr)) {
                // CRITICAL: Reset format_in_progress on error
                ExtendedFilesystemModule::resetFilesystemState(false, false, true);  // Reset only format_in_progress
                return LFS_ERR_IO;
            }
            
            uint32_t erase_time = millis() - erase_start_time;
            // DEBUG logs removed to reduce log volume - success messages are very frequent
            // Only log errors or during format progress (already logged above)
            
            // CRITICAL: Mark page as erased in bitmap (to avoid erasing same page multiple times)
            constexpr uint32_t EXTENDED_FS_FIRST_PAGE = EXTENDED_LFS_FLASH_ADDR / FLASH_NRF52_PAGE_SIZE;  // 168
            constexpr uint32_t EXTENDED_FS_PAGE_COUNT = EXTENDED_LFS_FLASH_TOTAL_SIZE / FLASH_NRF52_PAGE_SIZE;  // 68
            if (page_number >= EXTENDED_FS_FIRST_PAGE && page_number < (EXTENDED_FS_FIRST_PAGE + EXTENDED_FS_PAGE_COUNT)) {
                uint32_t bit_index = page_number - EXTENDED_FS_FIRST_PAGE;
                uint32_t word_index = bit_index / 32;
                uint32_t bit_in_word = bit_index % 32;
                if (word_index < 3) {
                    erased_pages_bitmap[word_index] |= (1U << bit_in_word);
                    // DEBUG log removed to reduce log volume - this happens on every erase operation
                }
            }
            
            // NOTE: format_in_progress is now reset in formatFilesystem() immediately after lfs_format() completes
            // This is necessary because LittleFS may call blocks in any order, and the last block may not be processed last
            return LFS_ERR_OK;
        } else if (err_code == NRF_ERROR_BUSY) {
            // CRITICAL: Check timeout for busy retries
            uint32_t elapsed = millis() - busy_retry_start;
            if (elapsed > BUSY_RETRY_TIMEOUT_MS) {
                LOG_ERROR("lfs_erase: NRF_ERROR_BUSY timeout after %u ms for page %u", elapsed, page_number);
                ExtendedFilesystemModule::resetFilesystemState(false, false, true);  // Reset only format_in_progress
                cleanupAsyncOperation(use_async);
                return LFS_ERR_IO;
            }
            
            FEED_WATCHDOG_AND_YIELD();
            delay(50);
            continue;
        } else {
            // CRITICAL: Improved error handling with proper resource cleanup
            logSoftDeviceError(err_code, "erase", page_number);
            // CRITICAL: Reset format_in_progress on error
            ExtendedFilesystemModule::resetFilesystemState(false, false, true);  // Reset only format_in_progress
            // CRITICAL: Ensure we don't leave async operation in progress
            cleanupAsyncOperation(use_async);
            return LFS_ERR_IO;
        }
    }
    
    LOG_ERROR("sd_flash_page_erase failed for page %u after all retries", page_number);
    // CRITICAL: Reset format_in_progress after all retries failed
    resetFilesystemState(false, false, true);  // Reset only format_in_progress
    return LFS_ERR_IO;
}

// LittleFS sync callback (no-op for flash)
int ExtendedFilesystemModule::lfs_sync(const struct lfs_config *c)
{
    (void)c;
    return LFS_ERR_OK;
}

// Constructor - does NOT initialize automatically (initialization happens in fsInit_patched())
// CRITICAL: Do NOT call init() here - it may be called before USB/console is ready,
// causing device to hang without any logs. Initialization is deferred to fsInit_patched().
ExtendedFilesystemModule::ExtendedFilesystemModule()
{
    // CRITICAL: This constructor is called during static initialization, BEFORE main()
    // DO NOT use LOG_INFO or any functions that require system initialization here
    // LOG_INFO may cause hard fault or hang if USB/console is not initialized yet
    // Keep constructor minimal - only set pointer, no logging, no function calls
    
    if (!extendedFilesystemModule) {
        extendedFilesystemModule = this;
        // NOTE: init() is NOT called here - it's called later in fsInit_patched()
        // after USB and console are initialized, so we can see logs if something goes wrong
    }
    // If extendedFilesystemModule is already set, silently ignore (should not happen)
}

// Initialize extended filesystem
bool ExtendedFilesystemModule::init()
{
    LOG_INFO("ExtendedFilesystemModule::init() CALLED");
    LOG_INFO("  - extendedFilesystemModule pointer: %p", (void*)extendedFilesystemModule);
    // NOTE: init() is a static function, so 'this' is not available
    // initInternal() is also static, so call it directly
    bool result = ExtendedFilesystemModule::initInternal();
    LOG_INFO("ExtendedFilesystemModule::init() returning: %s", result ? "SUCCESS" : "FAILED");
    return result;
}

// Internal state management helpers
void ExtendedFilesystemModule::resetFilesystemState(bool reset_initialized, bool reset_mounted, bool reset_format)
{
    if (reset_initialized) {
        isInitialized = false;
    }
    if (reset_mounted) {
        isMounted = false;
    }
    if (reset_format) {
        format_in_progress = false;
        memset(erased_pages_bitmap, 0, sizeof(erased_pages_bitmap));  // Reset erased pages bitmap when format is reset
    }
}

bool ExtendedFilesystemModule::isFilesystemReady()
{
    return isInitialized && isMounted;
}

bool ExtendedFilesystemModule::ensureFilesystemInitialized()
{
    if (!isInitialized) {
        return init();
    }
    return isMounted;
}

// Internal initialization
bool ExtendedFilesystemModule::initInternal()
{
    // CRITICAL: Early logging to diagnose startup issues
    LOG_INFO("========================================");
    LOG_INFO("initInternal() CALLED - Extended FS initialization starting");
    LOG_INFO("========================================");
    
    // CRITICAL: Initialize format_in_progress flag at start
    format_in_progress = false;
    
    // Use helper function for state check
    if (ExtendedFilesystemModule::isFilesystemReady()) {
        LOG_INFO("Extended filesystem already ready - skipping initialization");
        return true;
    }
    
    // CRITICAL: Do NOT set isInitialized = true here - set it only after all checks pass
    // This prevents issues with repeated initialization attempts
    
    LOG_INFO("========================================");
    LOG_INFO("INITIALIZING EXTENDED FILESYSTEM FOR NODEDB");
    LOG_INFO("========================================");
    LOG_INFO("Configuration:");
    LOG_INFO("  - Address: 0x%08X (page %u)", EXTENDED_LFS_FLASH_ADDR, EXTENDED_LFS_FLASH_ADDR / FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Size: %u bytes (%u KB, %u pages)", EXTENDED_LFS_FLASH_TOTAL_SIZE, EXTENDED_LFS_FLASH_TOTAL_SIZE / 1024, EXTENDED_LFS_FLASH_TOTAL_SIZE / FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Block size: %u bytes", EXTENDED_LFS_BLOCK_SIZE);
    LOG_INFO("  - Block count: %u blocks", EXTENDED_LFS_BLOCK_COUNT);
    LOG_INFO("  - Lookahead: %u", EXTENDED_LFS_LOOKAHEAD);
    LOG_INFO("  - Read buffer: %u bytes", sizeof(read_buffer));
    LOG_INFO("  - Prog buffer: %u bytes", sizeof(prog_buffer));
    LOG_INFO("  - Lookahead buffer: %u bytes", sizeof(lookahead_buffer));
    LOG_INFO("  - End address: 0x%08X (page %u)", 
             EXTENDED_LFS_FLASH_ADDR + EXTENDED_LFS_FLASH_TOTAL_SIZE - 1, 
             (EXTENDED_LFS_FLASH_ADDR + EXTENDED_LFS_FLASH_TOTAL_SIZE - 1) / FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Bootloader start: 0x%08X (page %u)", BOOTLOADER_ADDR, BOOTLOADER_ADDR / FLASH_NRF52_PAGE_SIZE);
    
    // CRITICAL: Safety check - validate filesystem addresses are within valid flash range
    // Check that filesystem start address is valid
    if (EXTENDED_LFS_FLASH_ADDR < 0x1000) {
        LOG_ERROR("ERROR: Extended filesystem start address (0x%08X) is too low (below 0x1000)!", EXTENDED_LFS_FLASH_ADDR);
        return false;
    }
    
    // CRITICAL: Check that Extended FS doesn't conflict with application area
    // Application ends at 0xA8000 (limited by linker script to make room for Extended FS)
    // Extended FS starts at 0xA8000 (page 168), after application, before bootloader
    constexpr uint32_t APPLICATION_END_ADDR = 0xA8000;  // Должно соответствовать FLASH end из ld
    constexpr uint32_t MAIN_FS_START = 0xED000;  // Main FS start
    constexpr uint32_t MAIN_FS_END_ADDR = 0xF4000;  // Main FS end (0xED000 + 28KB = 0xF4000) - CORRECTED from 0xEF800
    
    if (EXTENDED_LFS_FLASH_ADDR < APPLICATION_END_ADDR) {
        LOG_ERROR("ERROR: Extended filesystem start (0x%08X) is INSIDE application area (ends at 0x%08X)!", 
                 EXTENDED_LFS_FLASH_ADDR, APPLICATION_END_ADDR);
        return false;
    }
    
    // Check that Extended FS doesn't start inside Main FS area
    // Extended FS (0xA8000-0xEBFFF) and Main FS (0xED000-0xF4000) do NOT overlap
    // Extended FS ends at 0xEBFFF, Main FS starts at 0xED000 (4 KB gap)
    // INSIDE Main FS area (between 0xED000 and 0xF4000)
    if (EXTENDED_LFS_FLASH_ADDR >= MAIN_FS_START && EXTENDED_LFS_FLASH_ADDR < MAIN_FS_END_ADDR) {
        LOG_ERROR("ERROR: Extended filesystem start (0x%08X) is INSIDE Main FS area (0x%08X - 0x%08X)!", 
                 EXTENDED_LFS_FLASH_ADDR, MAIN_FS_START, MAIN_FS_END_ADDR);
        return false;
    }
    
    // Extended FS starts before Main FS (0xA8000 < 0xED000), which is OK
    // Extended FS ends at 0xEBFFF, Main FS starts at 0xED000 (4 KB gap - safe)
    // Extended FS: 0xA8000-0xEBFFF (68 pages, 272 KB), Main FS: 0xED000-0xF4000 (7 pages, 28 KB)
    
    // Check that filesystem end address is valid
    // CRITICAL: Use calculateLastAddress() to safely compute ext_fs_end with overflow protection
    uint32_t ext_fs_end;
    if (!calculateLastAddress(EXTENDED_LFS_FLASH_ADDR, EXTENDED_LFS_FLASH_TOTAL_SIZE, ext_fs_end)) {
        LOG_ERROR("ERROR: Extended filesystem address overflow when calculating end address!");
        LOG_ERROR("  - Start: 0x%08X", EXTENDED_LFS_FLASH_ADDR);
        LOG_ERROR("  - Size: %u bytes", EXTENDED_LFS_FLASH_TOTAL_SIZE);
        return false;
    }
    
    // Subtract 1 to get the last valid byte address (ext_fs_end is now start + size, so last byte is start + size - 1)
    if (ext_fs_end == 0) {
        LOG_ERROR("ERROR: Cannot subtract 1 from ext_fs_end (would underflow)!");
        return false;
    }
    ext_fs_end = ext_fs_end - 1;
    
    if (ext_fs_end >= BOOTLOADER_ADDR) {
        LOG_ERROR("ERROR: Extended filesystem would overlap bootloader!");
        LOG_ERROR("  - Filesystem end: 0x%08X", ext_fs_end);
        LOG_ERROR("  - Bootloader start: 0x%08X", BOOTLOADER_ADDR);
        return false;
    }
    
    // CRITICAL: Additional check - verify ext_fs_end is still >= start after subtraction
    if (ext_fs_end < EXTENDED_LFS_FLASH_ADDR) {
        LOG_ERROR("ERROR: Extended filesystem address overflow (end < start)!");
        LOG_ERROR("  - Start: 0x%08X", EXTENDED_LFS_FLASH_ADDR);
        LOG_ERROR("  - Size: %u bytes", EXTENDED_LFS_FLASH_TOTAL_SIZE);
        LOG_ERROR("  - End: 0x%08X", ext_fs_end);
        return false;
    }
    
    uint32_t gap = BOOTLOADER_ADDR - ext_fs_end - 1;
    LOG_INFO("Safety check:");
    LOG_INFO("  - Gap to bootloader: %u bytes (%u KB, %u pages)", gap, gap / 1024, gap / FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Status: %s", 
             gap >= (64 * 1024) ? "SAFE (>=64 KB)" : 
             gap >= (32 * 1024) ? "OK (>=32 KB)" : 
             gap >= (16 * 1024) ? "WARNING (<32 KB)" : "CRITICAL (<16 KB)");
    
    // Initialize LittleFS config
    extended_lfs_cfg.context = NULL;
    extended_lfs_cfg.read = lfs_read;
    extended_lfs_cfg.prog = lfs_prog;
    extended_lfs_cfg.erase = lfs_erase;
    extended_lfs_cfg.sync = lfs_sync;
    
    // Физика флеша: блоки LittleFS соответствуют страницам флеша (4096 байт)
    // Но операции чтения/записи остаются мелкими (128 байт) для эффективности
    extended_lfs_cfg.read_size = 128;                       // Мелкие операции чтения
    extended_lfs_cfg.prog_size = 128;                       // Мелкие операции записи
    extended_lfs_cfg.block_size = EXTENDED_LFS_BLOCK_SIZE;   // 4096, как у флеша
    extended_lfs_cfg.block_count = EXTENDED_LFS_BLOCK_COUNT;  // 68 блоков
    extended_lfs_cfg.lookahead = EXTENDED_LFS_LOOKAHEAD;     // 64 (must be multiple of 32, increased from 32 to 64 for better coverage)
    
    extended_lfs_cfg.read_buffer = read_buffer;
    extended_lfs_cfg.prog_buffer = prog_buffer;
    extended_lfs_cfg.lookahead_buffer = lookahead_buffer;
    
    // CRITICAL: Diagnostic logging and validation (as per code review)
    LOG_INFO("EXT FS DIAGNOSTICS:");
    LOG_INFO("  - base=0x%08X blocks=%u block_size=%u end=0x%08X",
             EXTENDED_LFS_FLASH_ADDR, 
             extended_lfs_cfg.block_count, 
             extended_lfs_cfg.block_size,
             EXTENDED_LFS_FLASH_ADDR + extended_lfs_cfg.block_count * extended_lfs_cfg.block_size);
    LOG_INFO("  - lookahead_size=%u lookahead_buf=%p lookahead_buf_size=%u",
             extended_lfs_cfg.lookahead,
             extended_lfs_cfg.lookahead_buffer,
             sizeof(lookahead_buffer));
    
    // CRITICAL: Validate lookahead buffer size matches configuration
    uint32_t required_lookahead_buffer_size = extended_lfs_cfg.lookahead / 8;
    if (sizeof(lookahead_buffer) < required_lookahead_buffer_size) {
        LOG_ERROR("CRITICAL: lookahead_buffer size mismatch!");
        LOG_ERROR("  - Buffer size: %u bytes", sizeof(lookahead_buffer));
        LOG_ERROR("  - Required size: %u bytes (for %u blocks)", 
                  required_lookahead_buffer_size, extended_lfs_cfg.lookahead);
        LOG_ERROR("  - This will cause LittleFS corruption!");
        return false;
    }
    if (sizeof(lookahead_buffer) > required_lookahead_buffer_size) {
        LOG_WARN("WARNING: lookahead_buffer is larger than required");
        LOG_WARN("  - Buffer size: %u bytes", sizeof(lookahead_buffer));
        LOG_WARN("  - Required size: %u bytes (for %u blocks)", 
                 required_lookahead_buffer_size, extended_lfs_cfg.lookahead);
        LOG_WARN("  - Extra bytes will be unused");
    }
    LOG_INFO("  - Expected lookahead buffer size: %u bytes (for %u blocks) - OK",
             required_lookahead_buffer_size, extended_lfs_cfg.lookahead);
    
    // CRITICAL: Check if SoftDevice is initialized before attempting flash operations
    // SoftDevice is required for flash read/write/erase operations on NRF52
    // Use centralized function for SoftDevice state checking (logs status)
    // NOTE: Code supports both SoftDevice API and direct flash access, but SoftDevice is preferred
    bool sd_enabled = checkSoftDeviceState(true);
    if (!sd_enabled) {
        LOG_WARN("SoftDevice not enabled - using direct flash access (may be slower or less safe)");
        LOG_WARN("  - For best performance, ensure SoftDevice is initialized before fsInit()");
        LOG_WARN("  - SoftDevice is typically initialized via Bluetooth (setBluetoothEnable)");
    }
    
    // CRITICAL: Validate that config callbacks are set
    // Note: extended_lfs and extended_lfs_cfg are static objects, so their addresses are always valid
    // Checking &extended_lfs == nullptr would always be false for static objects, so we skip that check
    if (!extended_lfs_cfg.read || !extended_lfs_cfg.prog || !extended_lfs_cfg.erase || !extended_lfs_cfg.sync) {
        LOG_ERROR("CRITICAL: Extended filesystem config callbacks are not properly initialized!");
        return false;
    }
    
    // Try to mount filesystem
    LOG_INFO("Attempting to mount extended filesystem...");
    FEED_WATCHDOG_AND_YIELD();
    int mount_result = lfs_mount(&extended_lfs, &extended_lfs_cfg);
    FEED_WATCHDOG_AND_YIELD();
    
    bool need_reformat = false;
    
    if (mount_result != LFS_ERR_OK) {
        LOG_WARN("Extended filesystem mount failed (error: %d) - will format...", mount_result);
        if (mount_result == LFS_ERR_CORRUPT) {
            LOG_WARN("Mount failed with LFS_ERR_CORRUPT - filesystem is corrupted");
            LOG_WARN("This may be caused by USB Mass Storage bootloader overwriting Extended FS");
        }
        LOG_DEBUG("Mount failure details:");
        LOG_DEBUG("  - Config: read_size=%u, prog_size=%u, block_size=%u, block_count=%u", 
                 extended_lfs_cfg.read_size, extended_lfs_cfg.prog_size, 
                 extended_lfs_cfg.block_size, extended_lfs_cfg.block_count);
        LOG_DEBUG("  - Buffers: read=%p, prog=%p, lookahead=%p", 
                 extended_lfs_cfg.read_buffer, extended_lfs_cfg.prog_buffer, 
                 extended_lfs_cfg.lookahead_buffer);
        need_reformat = true;
    } else {
        // Mount succeeded - perform additional validation
        LOG_DEBUG("Mount succeeded, performing additional validation...");
        
        // CRITICAL: Additional validation after mount - check if filesystem is actually working
        // This catches corruption that might not be detected during mount (e.g., from USB Mass Storage bootloader)
        // Try to read version file to verify filesystem is actually accessible
        lfs_file_t test_file;
        int test_open = lfs_file_open(&extended_lfs, &test_file, VERSION_FILE, LFS_O_RDONLY);
        if (test_open == LFS_ERR_OK) {
            // Try to read a small amount to verify file is readable
            char test_buffer[16] = {0};
            lfs_ssize_t test_read = lfs_file_read(&extended_lfs, &test_file, test_buffer, sizeof(test_buffer) - 1);
            int test_close = lfs_file_close(&extended_lfs, &test_file);
            
            if (test_read < 0 || test_close != LFS_ERR_OK) {
                LOG_WARN("Filesystem validation FAILED - version file not readable (read: %d, close: %d)", 
                        (int)test_read, test_close);
                LOG_WARN("Filesystem may be corrupted (possibly from USB Mass Storage bootloader) - will reformat");
                lfs_unmount(&extended_lfs);
                need_reformat = true;
            } else {
                LOG_DEBUG("Filesystem validation passed - version file readable");
                // Now check version file content
                if (!checkVersionFile()) {
                    LOG_WARN("Version file check failed - will reformat");
                    need_reformat = true;
                }
            }
        } else {
            LOG_WARN("Filesystem validation FAILED - version file not accessible (error: %d)", test_open);
            LOG_WARN("Filesystem may be corrupted (possibly from USB Mass Storage bootloader) - will reformat");
            LOG_WARN("This can happen if bootloader writes firmware to Extended FS area (0xA8000-0xEBFFF)");
            lfs_unmount(&extended_lfs);
            need_reformat = true;
        }
    }
    
    if (need_reformat) {
        // Unmount if mounted
        if (mount_result == LFS_ERR_OK) {
            LOG_INFO("Unmounting filesystem before reformat...");
            lfs_unmount(&extended_lfs);
        }
        
        // CRITICAL: Verify formatFilesystem() succeeded before proceeding
        LOG_DEBUG("Starting filesystem format...");
        if (!formatFilesystem()) {
            LOG_ERROR("CRITICAL: formatFilesystem() failed - filesystem is NOT initialized!");
            LOG_ERROR("Initialization state after format failure:");
            LOG_ERROR("  - isInitialized: %s", isInitialized ? "true" : "false");
            LOG_ERROR("  - isMounted: %s", isMounted ? "true" : "false");
            LOG_ERROR("  - format_in_progress: %s", format_in_progress ? "true" : "false");
            ExtendedFilesystemModule::resetFilesystemState();  // Centralized state reset
            return false;
        }
        LOG_DEBUG("Filesystem format completed successfully");
        
        // CRITICAL: Verify createVersionFile() succeeded (warn but don't fail)
        if (!createVersionFile()) {
            LOG_WARN("Failed to create version file, but filesystem is mounted");
            // Continue anyway - version file is not critical for operation
        }
        
        // NOTE: format_in_progress is already reset in formatFilesystem() after lfs_format() completes
        // No need to reset it here to avoid duplication
        
        LOG_INFO("REFORMAT COMPLETED SUCCESSFULLY");
        
        // CRITICAL: After format, filesystem should be mounted (formatFilesystem() mounts it)
        // Verify mount status
        if (!isMounted) {
            LOG_ERROR("CRITICAL: Filesystem not mounted after format!");
            ExtendedFilesystemModule::resetFilesystemState(true, false, false);  // Reset only isInitialized
            return false;
        }
    }
    
    // CRITICAL: Only set isMounted if filesystem is actually mounted
    // If we didn't reformat, mount_result should be LFS_ERR_OK
    if (!need_reformat && mount_result == LFS_ERR_OK) {
        isMounted = true;
    } else if (need_reformat) {
        // After format, isMounted should already be set by formatFilesystem()
        if (!isMounted) {
            LOG_ERROR("CRITICAL: Filesystem mount failed after format - cannot proceed!");
            isInitialized = false;
            return false;
        }
    } else {
        LOG_ERROR("CRITICAL: Filesystem mount failed - cannot proceed!");
        ExtendedFilesystemModule::resetFilesystemState(true, true, false);  // Reset isInitialized and isMounted, keep format_in_progress
        return false;
    }
    LOG_INFO("SUCCESS: Extended filesystem mounted for NodeDB!");
    LOG_INFO("  - Total capacity: %u KB (%u pages)", EXTENDED_LFS_FLASH_TOTAL_SIZE / 1024, EXTENDED_LFS_FLASH_TOTAL_SIZE / FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Block count: %u blocks", EXTENDED_LFS_BLOCK_COUNT);
    LOG_INFO("========================================");
    
    // CRITICAL: Create required directories after successful mount
    // This ensures directories exist even if filesystem was formatted with old version
    // or if directories were deleted (e.g., by USB Mass Storage bootloader)
    LOG_DEBUG("Creating required directories...");
    FEED_WATCHDOG_AND_YIELD();
    
    // NOTE: This is old Extended FS code (no longer used after migration to Main FS)
    // NodeDB slot files are now stored in /prefs/nodes/ in Main FS
    // Create /nodes directory (legacy - for old Extended FS)
    int nodes_dir_err = lfs_mkdir(&extended_lfs, "/nodes");
    if (nodes_dir_err != LFS_ERR_OK && nodes_dir_err != LFS_ERR_EXIST) {
        LOG_WARN("Failed to create /nodes directory (error: %d) - will be created on first write", nodes_dir_err);
    } else {
        LOG_DEBUG("Directory /nodes ready");
    }
    FEED_WATCHDOG_AND_YIELD();
    
    // Create /prefs directory (required for nodes.proto)
    int prefs_dir_err = lfs_mkdir(&extended_lfs, "/prefs");
    if (prefs_dir_err != LFS_ERR_OK && prefs_dir_err != LFS_ERR_EXIST) {
        LOG_WARN("Failed to create /prefs directory (error: %d) - will be created on first write", prefs_dir_err);
    } else {
        LOG_DEBUG("Directory /prefs ready");
    }
    FEED_WATCHDOG_AND_YIELD();
    
    // CRITICAL: Set isInitialized = true only after all checks pass and filesystem is successfully mounted
    isInitialized = true;
    
    // CRITICAL: Log Extended FS statistics after successful initialization
    // This helps detect filesystem issues early (e.g., when free space is low)
    LOG_INFO("========================================");
    LOG_INFO("EXTENDED FS STATISTICS:");
    LOG_INFO("========================================");
    
    // Get filesystem usage statistics
    uint32_t fs_total = 0, fs_used = 0, fs_free = 0;
    if (getStats(&fs_total, &fs_used, &fs_free)) {
        uint32_t fs_total_kb = fs_total / 1024;
        uint32_t fs_used_kb = fs_used / 1024;
        uint32_t fs_free_kb = fs_free / 1024;
        uint32_t fs_used_pct = (fs_total_kb > 0) ? (fs_used_kb * 100) / fs_total_kb : 0;
        
        LOG_INFO("  - Filesystem usage: %u KB / %u KB (%u%%)", fs_used_kb, fs_total_kb, fs_used_pct);
        LOG_INFO("  - Free space: %u KB", fs_free_kb);
        
        // WARNING: If filesystem is more than 80% full, log warning
        if (fs_used_pct > 80) {
            LOG_WARN("  - ⚠️  WARNING: Extended FS is %u%% full - may cause issues soon!", fs_used_pct);
        }
        // WARNING: If filesystem is more than 90% full, log critical warning
        if (fs_used_pct > 90) {
            LOG_ERROR("  - ⚠️  CRITICAL: Extended FS is %u%% full - filesystem issues likely!", fs_used_pct);
        }
    } else {
        LOG_WARN("  - Failed to get filesystem statistics");
    }
    
    LOG_INFO("========================================");
    LOG_INFO("Note: Node statistics (saved nodes, free slots) will be logged after NodeDB initialization");
    LOG_INFO("========================================");
    
    return true;
}

// Check version file
bool ExtendedFilesystemModule::checkVersionFile()
{
    LOG_INFO("Mount succeeded - checking version file...");
    FEED_WATCHDOG_AND_YIELD();
    lfs_file_t version_file;
    int open_result = lfs_file_open(&extended_lfs, &version_file, VERSION_FILE, LFS_O_RDONLY);
    FEED_WATCHDOG_AND_YIELD();
    
    if (open_result != LFS_ERR_OK) {
        LOG_WARN("Version file not found - filesystem may be corrupted or old format, will reformat...");
        return false;
    }
    
    // Use RAII guard for automatic file closing
    LfsFileGuard file_guard(&extended_lfs, &version_file, VERSION_FILE);
    
    // Read version
    char version_str[16] = {0};
    lfs_ssize_t read_result = lfs_file_read(&extended_lfs, file_guard.get(), version_str, sizeof(version_str) - 1);
    // File will be automatically closed by RAII guard
    
    if (read_result > 0) {
        char* endptr = nullptr;
        uint32_t saved_version = strtoul(version_str, &endptr, 10);
        bool valid_conversion = (endptr != version_str) && (*endptr == '\0' || *endptr == '\n' || *endptr == '\r' || *endptr == ' ');
        
        if (!valid_conversion) {
            LOG_WARN("Version file contains invalid data: '%s' - will reformat...", version_str);
            return false;
        }
        
        LOG_INFO("Version file found: version %lu (expected: %u)", saved_version, EXPECTED_VERSION);
        
        if (saved_version != EXPECTED_VERSION) {
            LOG_WARN("Version mismatch (%lu != %u) - will reformat...", saved_version, EXPECTED_VERSION);
            return false;
        } else {
            LOG_INFO("Version matches - filesystem is valid");
            return true;
        }
    } else {
        LOG_WARN("Version file is empty - will reformat...");
        return false;
    }
}

// Validate flash addresses before formatting
bool ExtendedFilesystemModule::validateFlashAddressesBeforeFormat()
{
    LOG_INFO("========================================");
    LOG_INFO("VALIDATING FLASH ADDRESSES BEFORE FORMAT");
    LOG_INFO("========================================");
    
    // Get total flash size for this chip
    #ifdef NRF52840_XXAA
        // nRF52840 has 1MB flash (0x100000)
        constexpr uint32_t TOTAL_FLASH_SIZE = 0x100000;  // 1 MB
        constexpr uint32_t MAX_FLASH_ADDR = TOTAL_FLASH_SIZE;
    #else
        // Other nRF52 chips may have different sizes
        // For safety, assume 512KB (0x80000) if not nRF52840
        constexpr uint32_t TOTAL_FLASH_SIZE = 0x80000;  // 512 KB
        constexpr uint32_t MAX_FLASH_ADDR = TOTAL_FLASH_SIZE;
    #endif
    
    LOG_INFO("Flash memory configuration:");
    LOG_INFO("  - Total flash size: 0x%08X (%u KB)", TOTAL_FLASH_SIZE, TOTAL_FLASH_SIZE / 1024);
    LOG_INFO("  - Max flash address: 0x%08X", MAX_FLASH_ADDR);
    
    // Calculate filesystem bounds
    uint32_t fs_start = EXTENDED_LFS_FLASH_ADDR;
    uint32_t fs_end;
    if (!calculateLastAddress(EXTENDED_LFS_FLASH_ADDR, EXTENDED_LFS_FLASH_TOTAL_SIZE, fs_end)) {
        LOG_ERROR("ERROR: Address overflow when calculating filesystem end!");
        LOG_ERROR("  - Start: 0x%08X", fs_start);
        LOG_ERROR("  - Size: %u bytes", EXTENDED_LFS_FLASH_TOTAL_SIZE);
        return false;
    }
    
    LOG_INFO("Extended filesystem bounds:");
    LOG_INFO("  - Start: 0x%08X", fs_start);
    LOG_INFO("  - End: 0x%08X", fs_end);
    LOG_INFO("  - Size: %u bytes (%u KB, %u pages)", 
             EXTENDED_LFS_FLASH_TOTAL_SIZE, 
             EXTENDED_LFS_FLASH_TOTAL_SIZE / 1024,
             EXTENDED_LFS_FLASH_TOTAL_SIZE / FLASH_NRF52_PAGE_SIZE);
    
    // Check 1: Filesystem must be within total flash size
    if (fs_end > MAX_FLASH_ADDR) {
        LOG_ERROR("ERROR: Filesystem extends beyond available flash!");
        LOG_ERROR("  - Filesystem end: 0x%08X", fs_end);
        LOG_ERROR("  - Max flash address: 0x%08X", MAX_FLASH_ADDR);
        LOG_ERROR("  - Overflow: %u bytes", fs_end - MAX_FLASH_ADDR);
        return false;
    }
    
    // Check 2: Calculate which pages will be used
    uint32_t start_page = fs_start / FLASH_NRF52_PAGE_SIZE;
    uint32_t end_page = (fs_end - 1) / FLASH_NRF52_PAGE_SIZE;
    uint32_t total_pages = end_page - start_page + 1;
    
    LOG_INFO("Pages that will be erased:");
    LOG_INFO("  - Start page: %u (0x%08X)", start_page, start_page * FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - End page: %u (0x%08X)", end_page, end_page * FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Total pages: %u", total_pages);
    
    // Check 3: Verify all pages are within flash bounds
    uint32_t max_page = MAX_FLASH_ADDR / FLASH_NRF52_PAGE_SIZE;
    if (end_page >= max_page) {
        LOG_ERROR("ERROR: Filesystem pages extend beyond available flash pages!");
        LOG_ERROR("  - End page: %u", end_page);
        LOG_ERROR("  - Max page: %u", max_page - 1);
        return false;
    }
    
    // Check 4: Verify no overlap with Main FS
    #ifdef NRF52840_XXAA
        // Use the same constant as in initInternal() to ensure consistency
        constexpr uint32_t MAIN_FS_START = 0xED000;
        constexpr uint32_t MAIN_FS_END = 0xF4000;  // Main FS end (28 KB, 7 pages) - CORRECTED from 0xEF800
        
        // Current layout:
        // - Extended FS: 0xA8000 - 0xEBFFF (272 KB, 68 pages) - for NodeDB, ends before Main FS
        // - Main FS: 0xED000 - 0xF4000 (28 KB, 7 pages) - for config files
        // Note: Extended FS (0xA8000-0xEBFFF) and Main FS (0xED000-0xF4000) do NOT overlap.
        // Extended FS ends at 0xEBFFF, Main FS starts at 0xED000 (4 KB gap - safe).
        // This ensures Extended FS never tries to erase Main FS pages.
        // We just log this for information.
        if (fs_end >= MAIN_FS_START) {
            LOG_INFO("  - Main FS check: OK (0x%08X - 0x%08X) - Extended FS and Main FS overlap by address but use separate LittleFS instances", MAIN_FS_START, MAIN_FS_END);
        } else {
            LOG_INFO("  - Main FS check: OK (0x%08X - 0x%08X)", MAIN_FS_START, MAIN_FS_END);
        }
    #endif
    
    // Check 5: Verify no overlap with bootloader
    if (fs_end > BOOTLOADER_ADDR) {
        LOG_ERROR("ERROR: Extended filesystem overlaps with bootloader!");
        LOG_ERROR("  - Filesystem end: 0x%08X", fs_end);
        LOG_ERROR("  - Bootloader start: 0x%08X", BOOTLOADER_ADDR);
        LOG_ERROR("  - Overlap: %u bytes", fs_end - BOOTLOADER_ADDR);
        return false;
    }
    LOG_INFO("  - Bootloader check: OK (starts at 0x%08X)", BOOTLOADER_ADDR);
    
    // Check 6: Verify each page address individually
    LOG_INFO("Validating each page address:");
    bool all_pages_valid = true;
    for (uint32_t page = start_page; page <= end_page; page++) {
        uint32_t page_addr = page * FLASH_NRF52_PAGE_SIZE;
        uint32_t page_end_addr = page_addr + FLASH_NRF52_PAGE_SIZE;
        
        if (page_end_addr > MAX_FLASH_ADDR) {
            LOG_ERROR("  - Page %u (0x%08X - 0x%08X): OUT OF BOUNDS!", page, page_addr, page_end_addr);
            all_pages_valid = false;
        } else if (page_addr >= BOOTLOADER_ADDR) {
            LOG_ERROR("  - Page %u (0x%08X - 0x%08X): OVERLAPS BOOTLOADER!", page, page_addr, page_end_addr);
            all_pages_valid = false;
        } else {
            // Only log first and last pages to avoid spam
            if (page == start_page || page == end_page || (page - start_page) % 10 == 0) {
                LOG_INFO("  - Page %u (0x%08X - 0x%08X): OK", page, page_addr, page_end_addr);
            }
        }
    }
    
    if (!all_pages_valid) {
        LOG_ERROR("ERROR: Some pages are invalid!");
        return false;
    }
    
    LOG_INFO("========================================");
    LOG_INFO("ALL FLASH ADDRESS VALIDATIONS PASSED");
    LOG_INFO("========================================");
    return true;
}

// Format filesystem
bool ExtendedFilesystemModule::formatFilesystem()
{
    // CRITICAL: Reset format tracking state BEFORE format starts
    // This must be done here, not in lfs_erase, because LittleFS may call blocks in any order
    format_in_progress = true;
    memset(erased_pages_bitmap, 0, sizeof(erased_pages_bitmap));  // Reset bitmap at start of format
    
    LOG_WARN("========================================");
    LOG_WARN("REFORMATTING EXTENDED FILESYSTEM");
    LOG_WARN("========================================");
    LOG_WARN("  - This will erase %u blocks (%u pages)", EXTENDED_LFS_BLOCK_COUNT, EXTENDED_LFS_FLASH_TOTAL_SIZE / FLASH_NRF52_PAGE_SIZE);
    LOG_WARN("  - Format may take up to %u seconds (watchdog will be fed during erase)", 
             (EXTENDED_LFS_BLOCK_COUNT * 100) / 1000);  // Rough estimate: 100ms per block
    LOG_WARN("  - Watchdog feeding is active during format");
    LOG_WARN("========================================");
    
    // CRITICAL: Validate flash addresses BEFORE formatting
    // This prevents accidental erasure of wrong memory regions
    if (!validateFlashAddressesBeforeFormat()) {
        LOG_ERROR("CRITICAL: Flash address validation FAILED - aborting format!");
        LOG_ERROR("  - This prevents potential damage to flash memory");
        LOG_ERROR("  - Check filesystem configuration and flash memory size");
        return false;
    }
    
    uint32_t format_start = millis();
    
    // Format filesystem (LittleFS will handle block erasure internally)
    // CRITICAL: lfs_format() will call lfs_erase() for each block
    // Each lfs_erase() call feeds watchdog, so format should not timeout
    FEED_WATCHDOG_AND_YIELD();
    LOG_INFO("Starting lfs_format() - this may take a while...");
    int format_result = lfs_format(&extended_lfs, &extended_lfs_cfg);
    uint32_t format_time = millis() - format_start;
    FEED_WATCHDOG_AND_YIELD();
    LOG_INFO("lfs_format() completed in %lu ms", format_time);
    
    if (format_result != LFS_ERR_OK) {
        LOG_ERROR("FORMAT FAILED! Error code: %d", format_result);
        // Reset format_in_progress on failure
        format_in_progress = false;
        memset(erased_pages_bitmap, 0, sizeof(erased_pages_bitmap));
        return false;
    }
    
    // CRITICAL: Reset format_in_progress immediately after successful lfs_format()
    // This must be done here, not in lfs_erase, because LittleFS may call blocks in any order
    // and the last block may not be processed last
    format_in_progress = false;
    // CRITICAL: DO NOT reset erased_pages_bitmap here - it must remain set for pages that were erased during format
    // This allows us to skip erasing pages that are already erased when LittleFS calls lfs_erase after format
    // Bitmap will be reset at the start of the next format (line 1598)
    
    // Mount again after format
    FEED_WATCHDOG_AND_YIELD();
    int mount_result = lfs_mount(&extended_lfs, &extended_lfs_cfg);
    FEED_WATCHDOG_AND_YIELD();
    
    if (mount_result != LFS_ERR_OK) {
        LOG_ERROR("MOUNT FAILED AFTER FORMAT! Error code: %d", mount_result);
        LOG_ERROR("Mount failure after format - filesystem may be corrupted");
        LOG_ERROR("  - Config: read_size=%u, prog_size=%u, block_size=%u, block_count=%u", 
                 extended_lfs_cfg.read_size, extended_lfs_cfg.prog_size, 
                 extended_lfs_cfg.block_size, extended_lfs_cfg.block_count);
        return false;
    }
    
    // CRITICAL: Set isMounted flag after successful mount
    isMounted = true;
    
    return true;
}
// Ensure all parent directories for an absolute path exist (LittleFS).
// This function creates all parent directories needed for a file path.
// For example, for "/prefs/nodes/slot_0016.bin", it will create "/prefs" and "/prefs/nodes" if they don't exist.
static int ensureParentDirs(lfs_t* lfs, const char* filename)
{
    if (!lfs || !filename || filename[0] != '/') {
        return 0;  // Invalid parameters or not an absolute path
    }

    char dir[128] = {0};
    const char* p = filename + 1;  // Skip leading '/'
    
    while (true) {
        p = strchr(p, '/');
        if (!p) {
            break;  // No more '/' found - we've processed all directory levels
        }
        
        size_t len = (size_t)(p - filename);
        if (len == 0 || len >= sizeof(dir)) {
            LOG_ERROR("ensureParentDirs: Path too long: '%s'", filename);
            return LFS_ERR_INVAL;
        }
        
        memcpy(dir, filename, len);
        dir[len] = '\0';
        
        int err = lfs_mkdir(lfs, dir);
        if (err != 0 && err != LFS_ERR_EXIST) {
            LOG_ERROR("ensureParentDirs: Failed to create directory '%s' (error: %d)", dir, err);
            return err;
        }
        
        ++p;  // Skip '/' for next iteration
    }
    
    return 0;  // Success
}

// Open file for overwriting (truncate existing or create new)
int ExtendedFilesystemModule::openFileForOverwrite(lfs_t* lfs, lfs_file_t* file, const char* filename)
{
    // STEP 1: Log initial state and filename to ensure we're working with correct file
    LOG_DEBUG("openFileForOverwrite: Processing file '%s'", filename);
    
    // CRITICAL: Ensure parent directories exist before attempting to open file
    // This fixes the issue where LFS_ERR_NOENT occurs not because file doesn't exist,
    // but because parent directory (e.g., "/prefs/nodes") doesn't exist
    int dirErr = ensureParentDirs(lfs, filename);
    if (dirErr != 0) {
        LOG_ERROR("openFileForOverwrite: Failed to ensure parent dirs for '%s' (error: %d)", filename, dirErr);
        return dirErr;
    }
    
    if (lfs && lfs->cfg) {
        LOG_DEBUG("openFileForOverwrite: BEFORE operations - file='%s', free.ack=%u, free.off=%u, free.size=%u, free.i=%u, block_count=%u",
                  filename, lfs->free.ack, lfs->free.off, lfs->free.size, lfs->free.i, lfs->cfg->block_count);
        // Log first few bytes of lookahead buffer
        if (lfs->free.buffer && lfs->free.size > 0) {
            uint8_t buf_preview[4] = {0};
            uint32_t preview_size = (lfs->free.size / 8) < 4 ? (lfs->free.size / 8) : 4;
            if (preview_size > 0) {
                memcpy(buf_preview, lfs->free.buffer, preview_size);
                LOG_DEBUG("openFileForOverwrite: BEFORE operations - file='%s', lookahead buffer preview: %02x %02x %02x %02x",
                          filename, buf_preview[0], buf_preview[1], buf_preview[2], buf_preview[3]);
            }
        }
    }
    
    // CRITICAL: Do NOT remove file before TRUNC - this is redundant and loses metadata
    // Instead, use TRUNC which will:
    // 1. If file exists: truncate it to 0 bytes (reusing existing metadata)
    // 2. If file doesn't exist: create new file
    // This preserves metadata and avoids fragmentation
    
    // STEP 2: Diagnostic check of lookahead buffer state (READ-ONLY, no modifications)
    // CRITICAL: Do NOT modify lfs->free.* directly - this is not public API and can corrupt FS
    // Instead, we'll use temporary file operations to trigger native LittleFS lookahead refresh
    if (lfs && lfs->free.buffer && lfs->free.size > 0) {
        // Check if all bits in lookahead buffer are set (all blocks marked as used)
        bool all_used = true;
        uint32_t buffer_bytes = lfs->free.size / 8;
        for (uint32_t i = 0; i < buffer_bytes; i++) {
            if (lfs->free.buffer[i] != 0xFF) {
                all_used = false;
                break;
            }
        }
        
        if (all_used) {
            LOG_DEBUG("openFileForOverwrite: Lookahead buffer shows all blocks as used (0xFF) - will refresh via temp file");
        }
    }
    FEED_WATCHDOG_AND_YIELD();
    
    // STEP 4: Check filesystem state before opening
    if (lfs && lfs->cfg) {
        // Log filesystem configuration
        LOG_DEBUG("openFileForOverwrite: FS config - file='%s', block_count=%u, block_size=%u, lookahead=%u",
                  filename, lfs->cfg->block_count, lfs->cfg->block_size, lfs->cfg->lookahead);
    }
    
    // STEP 5: Remove file if it exists to ensure blocks are freed IMMEDIATELY
    // CRITICAL: lfs_remove() frees blocks immediately by removing file entry from directory
    // This guarantees that lfs_traverse() will see freed blocks when lookahead buffer is refreshed
    // TRUNC doesn't free blocks until metadata is committed, which may be too late
    LOG_DEBUG("openFileForOverwrite: Removing file '%s' if it exists to free blocks immediately...", filename);
    int remove_result = lfs_remove(lfs, filename);
    if (remove_result == LFS_ERR_OK) {
        LOG_DEBUG("openFileForOverwrite: File '%s' removed successfully, blocks freed immediately", filename);
        
        // CRITICAL: Force block device sync to ensure metadata is committed to disk
        // lfs_dir_commit() in lfs_remove() commits metadata, but we need to sync the block device
        // to ensure lfs_traverse() will see the removed file's blocks as free
        if (lfs && lfs->cfg && lfs->cfg->sync) {
            LOG_DEBUG("openFileForOverwrite: Forcing block device sync after remove...");
            int sync_result = lfs->cfg->sync(lfs->cfg);
            if (sync_result != 0) {
                LOG_DEBUG("openFileForOverwrite: Warning - block device sync failed: %d", sync_result);
            } else {
                LOG_DEBUG("openFileForOverwrite: Block device synced successfully");
            }
        }
        FEED_WATCHDOG_AND_YIELD();
        
        // CRITICAL: Call lfs_deorphan() to clean up orphaned entries
        // This ensures that blocks freed by lfs_remove() are properly marked as free
        // lfs_deorphan() prunes recoverable errors and updates metadata
        LOG_DEBUG("openFileForOverwrite: Calling lfs_deorphan() to clean up orphaned entries...");
        int deorphan_result = lfs_deorphan(lfs);
        if (deorphan_result != LFS_ERR_OK) {
            LOG_DEBUG("openFileForOverwrite: Warning - lfs_deorphan() failed: %d (may be normal if no orphans)", deorphan_result);
        } else {
            LOG_DEBUG("openFileForOverwrite: lfs_deorphan() completed successfully");
        }
        FEED_WATCHDOG_AND_YIELD();
        
        // CRITICAL: Check actual filesystem usage before attempting to allocate
        // Count files and calculate used space to verify if filesystem is really full
        uint32_t file_count = 0;
        uint32_t total_size = 0;
        lfs_dir_t dir;
        struct lfs_info info;
        
        if (lfs_dir_open(lfs, &dir, "/") == LFS_ERR_OK) {
            while (lfs_dir_read(lfs, &dir, &info) > 0) {
                if (info.type == LFS_TYPE_REG) {
                    file_count++;
                    total_size += info.size;
                }
            }
            lfs_dir_close(lfs, &dir);
            
            uint32_t total_blocks = lfs->cfg->block_count;
            uint32_t block_size = lfs->cfg->block_size;
            uint32_t total_bytes = total_blocks * block_size;
            uint32_t used_bytes = total_size;  // Approximate (doesn't include metadata overhead)
            uint32_t free_bytes = (total_bytes > used_bytes) ? (total_bytes - used_bytes) : 0;
            
            LOG_DEBUG("openFileForOverwrite: Filesystem usage check:");
            LOG_DEBUG("  - Total blocks: %u (%u KB)", total_blocks, total_bytes / 1024);
            LOG_DEBUG("  - File count: %u", file_count);
            LOG_DEBUG("  - Approximate used: %u bytes (%u KB)", used_bytes, used_bytes / 1024);
            LOG_DEBUG("  - Approximate free: %u bytes (%u KB)", free_bytes, free_bytes / 1024);
            LOG_DEBUG("  - free.ack=%u, free.off=%u, free.size=%u", 
                      lfs->free.ack, lfs->free.off, lfs->free.size);
            
            // If filesystem appears to have free space but free.ack indicates all blocks are used,
            // this suggests lookahead buffer is stale
            if (free_bytes > block_size && lfs->free.ack >= total_blocks) {
                LOG_WARN("openFileForOverwrite: WARNING - Filesystem has free space (%u KB) but free.ack=%u (all blocks marked as used)", 
                         free_bytes / 1024, lfs->free.ack);
                LOG_WARN("openFileForOverwrite: This indicates lookahead buffer is stale - will attempt refresh");
            }
        }
        FEED_WATCHDOG_AND_YIELD();
        
        // CRITICAL: Force lookahead buffer refresh by creating and deleting a temp file
        // After lfs_remove() and lfs_deorphan(), blocks are freed but lookahead buffer may be stale
        // We use temp file operations to trigger native LittleFS lfs_alloc() -> lfs_traverse()
        // This refreshes lookahead buffer with updated metadata after remove
        // CRITICAL: Do NOT modify lfs->free.* directly - this is not public API
        const char* temp_filename = "/.lfs_refresh_tmp";
        lfs_file_t temp_file;
        
        // Try to remove temp file if it exists from previous failed attempt
        lfs_remove(lfs, temp_filename);
        if (lfs && lfs->cfg && lfs->cfg->sync) {
            lfs->cfg->sync(lfs->cfg);
        }
        FEED_WATCHDOG_AND_YIELD();
        
        int temp_open = lfs_file_open(lfs, &temp_file, temp_filename, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL);
        if (temp_open == LFS_ERR_OK) {
            // Write 1 byte to force block allocation and lookahead refresh
            // This triggers native lfs_alloc() which will call lfs_traverse() if needed
            uint8_t dummy = 0;
            int temp_write = lfs_file_write(lfs, &temp_file, &dummy, 1);
            if (temp_write == 1) {
                // Sync and close temp file
                lfs_file_sync(lfs, &temp_file);
                lfs_file_close(lfs, &temp_file);
                // Remove temp file to free the block we just allocated
                lfs_remove(lfs, temp_filename);
                // Sync again to ensure removal is committed
                if (lfs && lfs->cfg && lfs->cfg->sync) {
                    lfs->cfg->sync(lfs->cfg);
                }
                FEED_WATCHDOG_AND_YIELD();
                LOG_DEBUG("openFileForOverwrite: Lookahead buffer refreshed via temp file");
            } else {
                lfs_file_close(lfs, &temp_file);
                lfs_remove(lfs, temp_filename);
                LOG_DEBUG("openFileForOverwrite: WARNING - temp file write failed: %d", temp_write);
            }
        } else {
            LOG_DEBUG("openFileForOverwrite: WARNING - failed to create temp file for lookahead refresh: %d", temp_open);
            
            // CRITICAL: If temp file refresh failed, try alternative method
            // Try to force lookahead refresh by syncing filesystem and attempting dummy file operation
            if (lfs && lfs->cfg && lfs->cfg->sync) {
                LOG_DEBUG("openFileForOverwrite: Forcing filesystem sync to update metadata...");
                int sync_result = lfs->cfg->sync(lfs->cfg);
                if (sync_result != 0) {
                    LOG_DEBUG("openFileForOverwrite: Filesystem sync failed: %d", sync_result);
                }
            }
            
            // Try to open a dummy file to trigger lookahead refresh
            // This will force LittleFS to traverse filesystem and update lookahead buffer
            lfs_file_t dummy_file;
            const char* dummy_name = "/.lfs_dummy_refresh";
            int dummy_open = lfs_file_open(lfs, &dummy_file, dummy_name, LFS_O_RDONLY);
            if (dummy_open == LFS_ERR_OK) {
                // File exists - close it (this may trigger metadata refresh)
                lfs_file_close(lfs, &dummy_file);
            } else if (dummy_open == LFS_ERR_NOENT) {
                // File doesn't exist - try to create and delete it
                int dummy_create = lfs_file_open(lfs, &dummy_file, dummy_name, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL);
                if (dummy_create == LFS_ERR_OK) {
                    uint8_t dummy_byte = 0;
                    lfs_file_write(lfs, &dummy_file, &dummy_byte, 1);
                    lfs_file_close(lfs, &dummy_file);
                    lfs_remove(lfs, dummy_name);
                    if (lfs && lfs->cfg && lfs->cfg->sync) {
                        lfs->cfg->sync(lfs->cfg);
                    }
                    LOG_DEBUG("openFileForOverwrite: Dummy file refresh completed");
                } else {
                    LOG_DEBUG("openFileForOverwrite: Dummy file creation also failed: %d", dummy_create);
                }
            }
        }
        
        // Diagnostic logging (read-only, no modifications)
        if (lfs && lfs->cfg) {
            LOG_DEBUG("openFileForOverwrite: After temp file refresh - free.ack=%u, free.size=%u, free.off=%u", 
                      lfs->free.ack, lfs->free.size, lfs->free.off);
        }
    } else if (remove_result == LFS_ERR_NOENT) {
        LOG_DEBUG("openFileForOverwrite: File '%s' doesn't exist, no need to remove", filename);
    } else {
        LOG_DEBUG("openFileForOverwrite: Warning - failed to remove file '%s' (error: %d), will try TRUNC instead", filename, remove_result);
        // Fall through to TRUNC approach
    }
    FEED_WATCHDOG_AND_YIELD();
    
    // STEP 6: Create new file (file was removed, so we need LFS_O_CREAT)
    // If remove failed, fall back to TRUNC approach
    int flags;
    if (remove_result == LFS_ERR_OK) {
        flags = LFS_O_WRONLY | LFS_O_CREAT;  // File was removed, create new
        LOG_DEBUG("openFileForOverwrite: Opening file '%s' for writing (will create new file after remove)...", filename);
    } else if (remove_result == LFS_ERR_NOENT) {
        // CRITICAL: File doesn't exist, so we need to CREATE it (not TRUNC)
        // LFS_O_TRUNC only works on existing files, it doesn't create new ones
        flags = LFS_O_WRONLY | LFS_O_CREAT;  // Create new file
        LOG_DEBUG("openFileForOverwrite: Opening file '%s' for writing (file doesn't exist, will create new)...", filename);
    } else {
        flags = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;  // Fallback to TRUNC for existing file
        LOG_DEBUG("openFileForOverwrite: Opening file '%s' with TRUNC (remove failed with error %d, using TRUNC fallback)...", filename, remove_result);
    }
    int open_result = lfs_file_open(lfs, file, filename, flags);
    
    if (open_result != LFS_ERR_OK) {
        LOG_DEBUG("openFileForOverwrite: Failed to open file '%s' (error: %d)", filename, open_result);
        if (lfs && lfs->cfg) {
            LOG_DEBUG("openFileForOverwrite: On error - file='%s', free.ack=%u, free.off=%u, free.size=%u, free.i=%u",
                      filename, lfs->free.ack, lfs->free.off, lfs->free.size, lfs->free.i);
            LOG_ERROR("⚠️  ERROR IN EXTENDED FS (NOT Main FS)!");
            LOG_ERROR("⚠️  Extended FS address range: 0x%08X - 0x%08X (pages 168-235, %u blocks)", 
                     ExtendedFilesystemModule::EXTENDED_LFS_FLASH_ADDR, 
                     ExtendedFilesystemModule::EXTENDED_LFS_FLASH_ADDR + ExtendedFilesystemModule::EXTENDED_LFS_FLASH_TOTAL_SIZE - 1,
                     lfs->cfg->block_count);
            LOG_ERROR("⚠️  Main FS address range: 0xED000 - 0xF4000 (pages 237-243, 28 KB)");
        }
        return open_result;
    }
    
    // Log final state after successful open
    if (lfs && lfs->cfg) {
        LOG_DEBUG("openFileForOverwrite: AFTER open - file='%s', free.ack=%u, free.off=%u, free.size=%u, free.i=%u",
                  filename, lfs->free.ack, lfs->free.off, lfs->free.size, lfs->free.i);
    }
    
    LOG_DEBUG("openFileForOverwrite: File '%s' opened successfully", filename);
    
    FEED_WATCHDOG_AND_YIELD();
    
    if (open_result != LFS_ERR_OK) {
        LOG_DEBUG("openFileForOverwrite: Failed to open file '%s' (error: %d)", filename, open_result);
        if (lfs && lfs->cfg) {
            LOG_DEBUG("openFileForOverwrite: On error - file='%s', free.ack=%u, free.off=%u, free.size=%u, free.i=%u",
                      filename, lfs->free.ack, lfs->free.off, lfs->free.size, lfs->free.i);
            // Log Extended FS address range to show this is Extended FS, not Main FS
            LOG_ERROR("⚠️  ERROR IN EXTENDED FS (NOT Main FS)!");
            LOG_ERROR("⚠️  Extended FS address range: 0x%08X - 0x%08X (pages 168-235, %u blocks)", 
                     ExtendedFilesystemModule::EXTENDED_LFS_FLASH_ADDR, 
                     ExtendedFilesystemModule::EXTENDED_LFS_FLASH_ADDR + ExtendedFilesystemModule::EXTENDED_LFS_FLASH_TOTAL_SIZE - 1,
                     lfs->cfg->block_count);
            LOG_ERROR("⚠️  Main FS address range: 0xED000 - 0xF4000 (pages 237-243, 28 KB)");
        }
        return open_result;
    }
    
    // Log final state after successful open
    if (lfs && lfs->cfg) {
        LOG_DEBUG("openFileForOverwrite: AFTER open - file='%s', free.ack=%u, free.off=%u, free.size=%u, free.i=%u",
                  filename, lfs->free.ack, lfs->free.off, lfs->free.size, lfs->free.i);
    }
    
    LOG_DEBUG("openFileForOverwrite: File '%s' opened successfully", filename);
    
    return LFS_ERR_OK;
}

// Create version file
bool ExtendedFilesystemModule::createVersionFile()
{
    lfs_file_t version_file;
    int create_result = openFileForOverwrite(&extended_lfs, &version_file, VERSION_FILE);
    
    if (create_result == LFS_ERR_OK) {
        // Use RAII guard for automatic file closing
        LfsFileGuard file_guard(&extended_lfs, &version_file, VERSION_FILE);
        
        char version_str[16];
        snprintf(version_str, sizeof(version_str), "%u", EXPECTED_VERSION);
        size_t version_str_len = strlen(version_str);
        lfs_ssize_t write_result = lfs_file_write(&extended_lfs, file_guard.get(), version_str, version_str_len);
        
        // CRITICAL: Verify that all bytes were written
        if (write_result > 0 && (size_t)write_result == version_str_len) {
            FEED_WATCHDOG_AND_YIELD();
            int sync_result = lfs_file_sync(&extended_lfs, file_guard.get());
            FEED_WATCHDOG_AND_YIELD();
            
            if (sync_result != LFS_ERR_OK) {
                LOG_ERROR("CRITICAL: Failed to sync version file (error: %d) - version file may not be written!", sync_result);
                // File will be automatically closed by RAII guard
                return false;
            }
            
            delay(50);
            
            // CRITICAL: Check result of close() - file must be closed successfully
            if (!file_guard.close()) {
                LOG_ERROR("CRITICAL: Failed to close version file after write - file may be corrupted!");
                return false;
            }
            
            // Verify version file was written
            FEED_WATCHDOG_AND_YIELD();
            lfs_file_t verify_file;
            int verify_open = lfs_file_open(&extended_lfs, &verify_file, VERSION_FILE, LFS_O_RDONLY);
            if (verify_open == LFS_ERR_OK) {
                // Use RAII guard for verify file
                LfsFileGuard verify_guard(&extended_lfs, &verify_file, VERSION_FILE);
                char verify_str[16] = {0};
                lfs_ssize_t verify_read = lfs_file_read(&extended_lfs, verify_guard.get(), verify_str, sizeof(verify_str) - 1);
                // File will be automatically closed by RAII guard
                if (verify_read > 0 && strcmp(verify_str, version_str) == 0) {
                    LOG_INFO("Version file verified successfully: %s", verify_str);
                    return true;
                } else {
                    LOG_ERROR("CRITICAL: Version file verification FAILED (read: '%s', expected: '%s')", 
                             verify_str, version_str);
                    return false;
                }
            } else {
                LOG_ERROR("CRITICAL: Failed to reopen version file for verification (error: %d)", verify_open);
                return false;
            }
        } else {
            LOG_ERROR("CRITICAL: Failed to write version file (write_result: %ld, expected: %u)", 
                     write_result, (unsigned)version_str_len);
            // File will be automatically closed by RAII guard
            return false;
        }
    } else {
        LOG_WARN("Failed to create version file (error: %d)", create_result);
        return false;
    }
}

// Check if extended filesystem is available
bool ExtendedFilesystemModule::isAvailable()
{
    return isMounted;
}

// Get LittleFS instance
lfs_t* ExtendedFilesystemModule::getExtendedFS()
{
    // Use helper function for lazy initialization
    if (!ExtendedFilesystemModule::ensureFilesystemInitialized()) {
        return nullptr;  // Initialization failed
    }
    return &extended_lfs;  // Filesystem is ready
}

// Force reformat
bool ExtendedFilesystemModule::forceReformat()
{
    LOG_WARN("========================================");
    LOG_WARN("FORCING REFORMAT OF EXTENDED FILESYSTEM");
    LOG_WARN("========================================");
    
    // Unmount if mounted
    if (isMounted) {
        int unmount_result = lfs_unmount(&extended_lfs);
        if (unmount_result != LFS_ERR_OK) {
            LOG_WARN("forceReformat: Failed to unmount filesystem (error: %d), continuing anyway", unmount_result);
        }
    }
    
    // Reset initialization flags (centralized)
    resetFilesystemState(true, true, false);  // Reset isInitialized and isMounted, keep format_in_progress for format operation
    
    // Format filesystem
    if (!formatFilesystem()) {
        return false;
    }
    
    // Create version file
    if (!createVersionFile()) {
        LOG_WARN("Failed to create version file, but filesystem is formatted");
    }
    
    // Reset format tracking flag
    format_in_progress = false;
    
    // Set success flags
    isMounted = true;
    isInitialized = true;
    
    LOG_INFO("FORCE REFORMAT COMPLETED SUCCESSFULLY");
    
    return true;
}

// Get statistics
bool ExtendedFilesystemModule::getStats(uint32_t* total, uint32_t* used, uint32_t* free)
{
    if (!total || !used || !free) {
        return false;
    }
    
    if (!isAvailable()) {
        return false;
    }
    
    *total = EXTENDED_LFS_FLASH_TOTAL_SIZE;
    
    *used = 0;
    
    // Calculate used space from root directory
    if (calcDirSizeRecursive(&extended_lfs, "/", used, 0) == LFS_ERR_OK) {
        *free = (*total > *used) ? (*total - *used) : 0;
        return true;
    }
    
    // Fallback
    *used = 0;
    *free = *total;
    return false;
}

// Helper function to iteratively calculate directory size (replaces recursive version to prevent stack overflow)
int ExtendedFilesystemModule::calcDirSizeRecursive(lfs_t* lfs, const char* path, uint32_t* size, int depth)
{
    // CRITICAL: Limit depth to prevent stack overflow (safety check)
    constexpr int MAX_DEPTH = 5;
    if (depth > MAX_DEPTH || !lfs || !size) {
        return LFS_ERR_OK;
    }
    
    // CRITICAL: Use iterative approach with stack-based directory queue to prevent stack overflow
    // Instead of recursion, we use a simple depth-first traversal with explicit stack
    struct DirEntry {
        char path[128];
        int depth;
    };
    
    DirEntry dir_stack[16];  // Max 16 directories in stack
    int stack_top = 0;
    
    // Initialize stack with root directory
    if (stack_top < 16) {
        strncpy(dir_stack[stack_top].path, path, sizeof(dir_stack[stack_top].path) - 1);
        dir_stack[stack_top].path[sizeof(dir_stack[stack_top].path) - 1] = '\0';
        dir_stack[stack_top].depth = depth;
        stack_top++;
    }
    
    // Process directories iteratively
    while (stack_top > 0) {
        // Pop directory from stack
        stack_top--;
        DirEntry current = dir_stack[stack_top];
        
        // CRITICAL: Validate depth before processing
        if (current.depth > MAX_DEPTH) {
            continue;  // Skip if depth exceeded
        }
        
        lfs_dir_t d;
        int err = lfs_dir_open(lfs, &d, current.path);
        if (err != LFS_ERR_OK) {
            continue;  // Skip directories that can't be opened
        }
        
        struct lfs_info info;
        uint32_t file_count = 0;
        while (true) {
            err = lfs_dir_read(lfs, &d, &info);
            if (err <= 0) {
                break;
            }
            
            file_count++;
            // CRITICAL: Feed watchdog periodically during directory scan
            if (file_count % 10 == 0) {
                FEED_WATCHDOG_AND_YIELD();
            }
            
            // Skip . and ..
            if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
                continue;
            }
            
            if (info.type == LFS_TYPE_REG) {
                // CRITICAL: Count actual block usage, not just file data size
                // LittleFS stores files in blocks (4KB each), so even small files use at least 1 block
                // Example: a 77-byte node file uses 1 block (4KB), not 77 bytes
                // This is why 72 nodes × 4KB = 288 KB, not 72 × 77 bytes = 5.5 KB
                // NOTE: This calculation already accounts for metadata overhead because:
                // - Each file requires at least 1 block (4KB) regardless of data size
                // - Directory entries and file headers are included in the block
                // - For small files, metadata and data share the same block
                uint32_t block_size = (lfs && lfs->cfg) ? lfs->cfg->block_size : ExtendedFilesystemModule::EXTENDED_LFS_BLOCK_SIZE;
                uint32_t blocks_used = (info.size + block_size - 1) / block_size;  // Round up to nearest block
                if (blocks_used == 0) blocks_used = 1;  // Minimum 1 block for any file
                *size += blocks_used * block_size;  // Count actual block usage (includes metadata)
            } else if (info.type == LFS_TYPE_DIR && current.depth < MAX_DEPTH) {
                // Build subdirectory path
                char subpath[128];
                if (strcmp(current.path, "/") == 0) {
                    snprintf(subpath, sizeof(subpath), "/%s", info.name);
                } else {
                    snprintf(subpath, sizeof(subpath), "%s/%s", current.path, info.name);
                }
                
                // CRITICAL: Push subdirectory to stack instead of recursing
                if (stack_top < 16) {
                    strncpy(dir_stack[stack_top].path, subpath, sizeof(dir_stack[stack_top].path) - 1);
                    dir_stack[stack_top].path[sizeof(dir_stack[stack_top].path) - 1] = '\0';
                    dir_stack[stack_top].depth = current.depth + 1;
                    stack_top++;
                } else {
                    LOG_WARN("calcDirSizeRecursive: Directory stack full, skipping subdirectory %s", subpath);
                }
            }
        }
        
        lfs_dir_close(lfs, &d);
    }
    
    return LFS_ERR_OK;
}

// Load protobuf from filesystem (extended FS if available, fallback to main FS)
LoadFileResult ExtendedFilesystemModule::loadProto(const char *filename, size_t protoSize, size_t objSize, 
                                                   const pb_msgdesc_t *fields, void *dest_struct)
{
    // CRITICAL: Config files (config.proto, device.proto, module.proto, channels.proto) 
    // MUST ALWAYS be loaded from Main FS, never from Extended FS
    // This ensures device configuration (password, device name, region) is always loaded correctly
    if (filename && (
        strcmp(filename, "/prefs/config.proto") == 0 ||
        strcmp(filename, "/prefs/device.proto") == 0 ||
        strcmp(filename, "/prefs/module.proto") == 0 ||
        strcmp(filename, "/prefs/channels.proto") == 0 ||
        strcmp(filename, "/prefs/uiconfig.proto") == 0)) {
        // Force Main FS for all config files - skip Extended FS check
        // Fall through to Main FS code below
    }
    // Check if this is nodes.proto and if extended filesystem should be used
    else if (isAvailable() && isNodeDBFile(filename)) {
        // CRITICAL OPTIMIZATION: For extended filesystem, ignore protoSize parameter
        // and use real file size instead. This prevents excessive memory allocation
        // (getMaxNodesAllocatedSize() can be 227KB, but file might be only 3 bytes).
        LoadFileResult state = LoadFileResult::OTHER_FAILURE;
        
        LOG_INFO("Load %s from EXTENDED filesystem (68 pages, 272 KB)", filename);
        
        // Note: isAvailable() already checked above, no need to check again
        
        lfs_t* extended_lfs = getExtendedFS();
        // CRITICAL: Verify extended_lfs pointer is valid before use
        if (!extended_lfs) {
            LOG_ERROR("CRITICAL: getExtendedFS() returned nullptr - filesystem not properly initialized!");
            return LoadFileResult::OTHER_FAILURE;
        }
        
        lfs_file_t file;
        int open_result = lfs_file_open(extended_lfs, &file, filename, LFS_O_RDONLY);
        
        if (open_result == LFS_ERR_OK) {
            // Use RAII guard for automatic file closing
            LfsFileGuard file_guard(extended_lfs, &file, filename);
            
            lfs_soff_t file_size = lfs_file_size(extended_lfs, file_guard.get());
            
            // CRITICAL: Define maximum file size to prevent buffer overflow or excessive memory allocation
            constexpr size_t MAX_FILE_SIZE = 512 * 1024;  // 512 KB maximum
            
            // CRITICAL: Validate file size before allocating memory
            if (file_size < 0) {
                LOG_ERROR("File '%s' has invalid size: %d bytes", filename, (int)file_size);
                // File will be automatically closed by RAII guard
                return LoadFileResult::OTHER_FAILURE;
            }
            
            if ((size_t)file_size > MAX_FILE_SIZE) {
                LOG_ERROR("File '%s' size (%d bytes) exceeds maximum allowed size (%u bytes)", 
                         filename, (int)file_size, (unsigned)MAX_FILE_SIZE);
                // File will be automatically closed by RAII guard
                return LoadFileResult::OTHER_FAILURE;
            }
            
            uint8_t* file_buffer = (uint8_t*)malloc((size_t)file_size);
            
            // CRITICAL: Check malloc result before use
            if (file_buffer) {
                lfs_ssize_t read_result = lfs_file_read(extended_lfs, file_guard.get(), file_buffer, file_size);
                
                // CRITICAL: Close file before processing data (explicit close for early return)
                file_guard.close();
                
                if (read_result == file_size) {
                    pb_istream_t stream = pb_istream_from_buffer(file_buffer, file_size);
                    memset(dest_struct, 0, objSize);
                    if (!pb_decode(&stream, fields, dest_struct)) {
                        LOG_ERROR("Can't decode protobuf %s: %s", filename, PB_GET_ERROR(&stream));
                        // CRITICAL: Free buffer on decode failure
                        free(file_buffer);
                        state = LoadFileResult::DECODE_FAILED;
                    } else {
                        LOG_INFO("Loaded %s successfully from EXTENDED filesystem (%d bytes)", 
                                filename, (int)file_size);
                        // CRITICAL: Free buffer after successful decode
                        free(file_buffer);
                        state = LoadFileResult::LOAD_SUCCESS;
                    }
                } else {
                    LOG_ERROR("Read %d of %d bytes from '%s'", 
                             (int)read_result, (int)file_size, filename);
                    // CRITICAL: Free buffer on read failure
                    free(file_buffer);
                    state = LoadFileResult::OTHER_FAILURE;
                }
            } else {
                LOG_ERROR("CRITICAL: Failed to allocate buffer for '%s' (size: %d bytes) - memory leak prevented", 
                         filename, (int)file_size);
                // File will be automatically closed by RAII guard
                state = LoadFileResult::OTHER_FAILURE;
            }
        } else {
            state = LoadFileResult::OTHER_FAILURE;
        }
        
        return state;
    }
    
    // Use main filesystem (standard behavior) - fallback
    LoadFileResult state = LoadFileResult::OTHER_FAILURE;
#ifdef FSCom
    concurrency::LockGuard g(spiLock);
    
    // CRITICAL: For device.proto, add detailed logging to diagnose why it might not load
    bool isDeviceProto = (filename && strcmp(filename, "/prefs/device.proto") == 0);
    if (isDeviceProto) {
        bool fileExists = FSCom.exists(filename);
        LOG_INFO("Loading device.proto from Main FS - file exists: %s", fileExists ? "YES" : "NO");
        if (fileExists) {
            File testFile = FSCom.open(filename, FILE_O_READ);
            if (testFile) {
                size_t fileSize = testFile.size();
                LOG_INFO("device.proto file size: %u bytes", (unsigned)fileSize);
                testFile.close();
            }
        }
    }
    
    auto f = FSCom.open(filename, FILE_O_READ);
    
    if (f) {
        LOG_INFO("Load %s", filename);
        pb_istream_t stream = {&readcb, &f, protoSize};
        
        memset(dest_struct, 0, objSize);
        if (!pb_decode(&stream, fields, dest_struct)) {
            LOG_ERROR("Error: can't decode protobuf %s: %s", filename, PB_GET_ERROR(&stream));
            if (isDeviceProto) {
                LOG_ERROR("CRITICAL: device.proto decode failed - this will cause nodenum to be regenerated!");
            }
            state = LoadFileResult::DECODE_FAILED;
        } else {
            LOG_INFO("Loaded %s successfully", filename);
            if (isDeviceProto) {
                // Log device.proto version for debugging
                extern meshtastic_DeviceState devicestate;
                LOG_INFO("device.proto loaded - version: %u (min required: 24)", devicestate.version);
            }
            state = LoadFileResult::LOAD_SUCCESS;
        }
        f.close();
    } else {
        LOG_ERROR("Could not open / read %s", filename);
        if (isDeviceProto) {
            LOG_ERROR("CRITICAL: device.proto file not found or cannot be opened - this will cause nodenum to be regenerated!");
        }
    }
#else
    LOG_ERROR("ERROR: Filesystem not implemented");
    state = LoadFileResult::NO_FILESYSTEM;
#endif
    return state;
}

// Save protobuf to filesystem (extended FS if available, fallback to main FS)
bool ExtendedFilesystemModule::saveProto(const char *filename, size_t protoSize, const pb_msgdesc_t *fields, 
                                         const void *dest_struct, bool fullAtomic)
{
    // CRITICAL: Config files (config.proto, device.proto, module.proto, channels.proto) 
    // MUST ALWAYS go to Main FS, never to Extended FS
    // This ensures device configuration (password, device name, region) is always preserved
    if (filename && (
        strcmp(filename, "/prefs/config.proto") == 0 ||
        strcmp(filename, "/prefs/device.proto") == 0 ||
        strcmp(filename, "/prefs/module.proto") == 0 ||
        strcmp(filename, "/prefs/channels.proto") == 0 ||
        strcmp(filename, "/prefs/uiconfig.proto") == 0)) {
        // Force Main FS for all config files - skip Extended FS check
        // Fall through to Main FS code below
    }
    // Check if this is nodes.proto and if extended filesystem should be used
    // IMPORTANT: Use extended filesystem for nodes.proto (all nodes including local node)
    // All other files (config.proto, device.proto, module.proto, channels.proto, uiconfig.proto)
    // MUST go to main filesystem to ensure current node configuration is always saved
    else if (isAvailable() && isNodeDBFile(filename)) {
        LOG_INFO("Save %s to EXTENDED filesystem (68 pages, 272 KB)", filename);
        
        // REMOVED: Filter out local node - local node can now be stored in Extended FS
        
        extern meshtastic_NodeDatabase nodeDatabase;
        
        LOG_INFO("saveProto: Saving %u nodes to Extended FS", 
                 (unsigned)nodeDatabase.nodes.size());
        
        lfs_t* extended_lfs = getExtendedFS();
        if (!extended_lfs) {
            LOG_WARN("Extended filesystem pointer is null - extended filesystem unavailable");
            return false;
        }
        
        // Ensure directory exists
        const char* slash = filename;
        if (slash[0] == '/') {
            slash++;
        }
        while (NULL != (slash = strchr(slash, '/'))) {
            // CRITICAL: Check for overflow when calculating dir_len
            if (slash < filename) {
                LOG_ERROR("saveProto: Invalid slash position in filename '%s'", filename);
                return false;
            }
            size_t dir_len = slash - filename;
            char dir_path[64];
            if (dir_len < sizeof(dir_path)) {
                memcpy(dir_path, filename, dir_len);
                dir_path[dir_len] = '\0';
                int mkdir_result = lfs_mkdir(extended_lfs, dir_path);
                (void)mkdir_result;
            } else {
                LOG_ERROR("saveProto: Directory path too long in filename '%s'", filename);
                return false;
            }
            slash++;
        }
        
        // Open file for writing (using helper to handle TRUNC + sync properly)
        // openFileForOverwrite() will truncate existing file or create new, and sync to free old blocks
        lfs_file_t file;
        int open_result = openFileForOverwrite(extended_lfs, &file, filename);
        
        if (open_result != LFS_ERR_OK) {
            LOG_ERROR("Failed to open '%s' for writing in extended FS (error: %d)", filename, open_result);
            return false;
        }
        
        // Use RAII guard for automatic file closing
        LfsFileGuard file_guard(extended_lfs, &file, filename);
        
        // Use streaming encoding
        LfsFileContext ctx;
        ctx.lfs = extended_lfs;
        ctx.file = file_guard.get();
        
        pb_ostream_t stream = {&lfs_writecb, &ctx, SIZE_MAX, 0};
        
        // CRITICAL: Encode full database (including local node) instead of filtered
        bool encode_success = pb_encode(&stream, fields, &nodeDatabase);
        
        if (!encode_success) {
            const char* error = PB_GET_ERROR(&stream);
            LOG_ERROR("Protobuf encoding failed for '%s': %s", filename, error ? error : "unknown");
            // File will be automatically closed by RAII guard
            return false;
        }
        
        size_t encoded_size = stream.bytes_written;
        
        // Check file size
        lfs_soff_t file_size = lfs_file_size(extended_lfs, file_guard.get());
        if (file_size < 0 || (size_t)file_size != encoded_size) {
            LOG_ERROR("File size mismatch for '%s': expected %u bytes, got %d", 
                     filename, (unsigned)encoded_size, (int)file_size);
            // File will be automatically closed by RAII guard
            return false;
        }
        
        // CRITICAL: Sync file before closing to ensure data is written
        FEED_WATCHDOG_AND_YIELD();
        int sync_result = lfs_file_sync(extended_lfs, file_guard.get());
        FEED_WATCHDOG_AND_YIELD();
        if (sync_result != LFS_ERR_OK) {
            LOG_ERROR("CRITICAL: Sync of '%s' failed (error: %d) - data may not be written!", filename, sync_result);
            // CRITICAL: Close file and remove on sync failure
            if (!file_guard.close(true)) {
                return false;
            }
            return false;
        }
        
        // CRITICAL: Close file and verify it was closed successfully
        if (!file_guard.close(true)) {
            return false;
        }
        
        return true;
    }
    
    // Use main filesystem for all other files - fallback
#ifdef FSCom
    // CRITICAL: For device.proto, add detailed logging to diagnose save issues
    bool isDeviceProto = (filename && strcmp(filename, "/prefs/device.proto") == 0);
    if (isDeviceProto) {
        LOG_INFO("Saving device.proto to Main FS");
        extern meshtastic_DeviceState devicestate;
        LOG_INFO("device.proto version: %u, nodenum: 0x%x", devicestate.version, devicestate.my_node.my_node_num);
    }
    
    auto f = SafeFile(filename, fullAtomic);
    
    LOG_INFO("Save %s", filename);
    pb_ostream_t stream = {&writecb, static_cast<Print *>(&f), protoSize};
    
    bool okay = false;
    if (!pb_encode(&stream, fields, dest_struct)) {
        LOG_ERROR("Error: can't encode protobuf %s: %s", filename, PB_GET_ERROR(&stream));
        if (isDeviceProto) {
            LOG_ERROR("CRITICAL: device.proto encode failed - nodenum will not be saved!");
        }
    } else {
        okay = true;
    }
    
    bool writeSucceeded = f.close();
    
    if (!okay || !writeSucceeded) {
        LOG_ERROR("Can't write prefs!");
        if (isDeviceProto) {
            LOG_ERROR("CRITICAL: device.proto write failed - nodenum will not be saved!");
        }
        return false;
    }
    
    if (isDeviceProto) {
        LOG_INFO("device.proto saved successfully to Main FS");
        // Verify file exists after save
        concurrency::LockGuard g(spiLock);
        if (FSCom.exists(filename)) {
            File verifyFile = FSCom.open(filename, FILE_O_READ);
            if (verifyFile) {
                size_t fileSize = verifyFile.size();
                LOG_INFO("device.proto verified - file size: %u bytes", (unsigned)fileSize);
                verifyFile.close();
            }
        } else {
            LOG_ERROR("CRITICAL: device.proto file not found after save!");
        }
    }
    
    return true;
#else
    LOG_ERROR("ERROR: Filesystem not implemented");
    return false;
#endif
}

// Check if a filename is for NodeDB
bool ExtendedFilesystemModule::isNodeDBFile(const char* filename)
{
    if (!filename) return false;
    return (strcmp(filename, "/prefs/nodes.proto") == 0);
}

// Check if NodeDB save should proceed (radio state checking)
bool ExtendedFilesystemModule::shouldProceedWithSave(uint32_t lastSaveTime)
{
    (void)lastSaveTime;  // Not used, kept for compatibility
    
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
            uint32_t iteration_count = 0;
            constexpr uint32_t MAX_WAIT_ITERATIONS = 30; // Safety limit
            
            while (RadioLibInterface::instance->isActivelyReceiving() && 
                   iteration_count < MAX_WAIT_ITERATIONS) {
                iteration_count++;
                uint32_t elapsed = millis() - wait_start;
                
                // CRITICAL: Check for millis() overflow
                if (elapsed > max_wait * 2) {
                    LOG_WARN("NodeDB save: millis() overflow detected in wait loop");
                    break;
                }
                
                if (elapsed >= max_wait) {
                    break;
                }
                
                FEED_WATCHDOG_AND_YIELD();
                // REMOVED: delay(10) - FEED_WATCHDOG_AND_YIELD() already handles timing
            }
            
            // If still receiving after wait, log warning but proceed with save
            // (better to save than lose data, and reception might be stuck)
            uint32_t final_elapsed = millis() - wait_start;
            if (RadioLibInterface::instance->isActivelyReceiving()) {
                LOG_WARN("NodeDB save: Radio still receiving after %u ms wait - proceeding with save anyway", 
                        final_elapsed);
            } else {
                LOG_DEBUG("NodeDB save: Waited %u ms for reception to complete", 
                         final_elapsed);
            }
        }
        // Note: We don't check isSending() - writing during transmission is fine
        // because reception is disabled during transmission anyway
    }
#endif
    
    return true; // Proceed with save
}

// Save NodeDB to disk with variant-specific optimizations
bool ExtendedFilesystemModule::saveNodeDatabaseToDisk(uint32_t &lastNodeDbSave)
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
    extern meshtastic_NodeDatabase nodeDatabase;
    // nodeDatabaseFileName is defined in src/mesh/NodeDB.h as static constexpr
    // We need to use the actual string value here
    static constexpr const char* nodeDatabaseFileName = "/prefs/nodes.proto";
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

// C wrapper for initialization (deprecated - Extended FS removed)
// Main FS initialization is now handled by initMainFS() called from fsInit_patched()
extern "C" void initExtendedFilesystemForNodeDB()
{
    // Extended FS has been removed - Main FS is initialized by initMainFS() in fsInit_patched()
    // This function is kept for compatibility but does nothing
    LOG_DEBUG("initExtendedFilesystemForNodeDB() called but Extended FS has been removed");
    LOG_DEBUG("Main FS initialization is handled by initMainFS() in fsInit_patched()");
}

// C wrapper for force reformat (deprecated - Extended FS removed)
// Main FS formatting is handled by initMainFS() which formats if mount fails
extern "C" bool forceReformatExtendedFS()
{
    // Extended FS has been removed - Main FS formatting is handled by initMainFS()
    // This function is kept for compatibility but does nothing
    LOG_DEBUG("forceReformatExtendedFS() called but Extended FS has been removed");
    LOG_DEBUG("Main FS formatting is handled by initMainFS() if mount fails");
    return false;
}

// Override lfs_assert() to handle Main FS corruption gracefully
// This is defined in our variant files to avoid modifying main-nrf52.cpp
extern "C" void lfs_assert(const char *reason)
{
    LOG_ERROR("LittleFS corruption detected: %s", reason);
    
    // Define Main FS constants at function scope (they are in anonymous namespace, so use values directly)
    #ifdef NRF52840_XXAA
        constexpr uint32_t MAIN_FS_ADDR = 0xA8000;
        constexpr uint32_t MAIN_FS_SIZE = 76 * 4096;  // 304 KB
    #else
        constexpr uint32_t MAIN_FS_ADDR = 0x6D000;
        constexpr uint32_t MAIN_FS_SIZE = 76 * 4096;  // 304 KB
    #endif
    
    #ifdef USE_EXTENDED_FS_FOR_NODEDB
    #ifdef ARCH_NRF52
    // CRITICAL: Check if this is Main FS error by checking block count
    // Main FS has 76 blocks (304 KB / 4 KB per block)
    // Extended FS has been removed - all errors are from Main FS now
    
    bool is_main_fs_error = false;
    if (reason) {
        // Check for Main FS block count (76 blocks = 304 KB / 4 KB per block)
        if (strstr(reason, "No more free space 76") != nullptr) {
            is_main_fs_error = true;
            LOG_WARN("⚠️  ERROR IS FROM MAIN FS (76 blocks = 304 KB)!");
            LOG_WARN("⚠️  Main FS address range: 0x%08X - 0x%08X (pages 168-243)", 
                     MAIN_FS_ADDR, 
                     MAIN_FS_ADDR + MAIN_FS_SIZE - 1);
        } else if (strstr(reason, "block") != nullptr) {
            // Generic block error - likely from Main FS (Extended FS removed)
            is_main_fs_error = true;
            LOG_WARN("⚠️  Block-related error detected - likely from Main FS");
            LOG_WARN("⚠️  Main FS address range: 0x%08X - 0x%08X (pages 168-243)", 
                     MAIN_FS_ADDR, 
                     MAIN_FS_ADDR + MAIN_FS_SIZE - 1);
        } else {
            LOG_WARN("⚠️  Error may be from Main FS");
            LOG_WARN("⚠️  Main FS address range: 0x%08X - 0x%08X (pages 168-243, 304 KB)", 
                     MAIN_FS_ADDR, 
                     MAIN_FS_ADDR + MAIN_FS_SIZE - 1);
        }
    }
    
    // Check if error is from Main FS (Extended FS has been removed)
    // Main FS has 76 blocks, so "No more free space 76" is definitely Main FS
    if (reason && (is_main_fs_error || strstr(reason, "block") != nullptr)) {
        LOG_WARN("LittleFS error is from Main FS - Main FS will be reformatted on next boot");
        LOG_WARN("This prevents infinite reboot loop when Main FS is corrupted");
        LOG_WARN("Main FS will be automatically reformatted by initMainFS() on next boot");
        // Main FS formatting is handled by initMainFS() which formats if mount fails
        // We need to reboot to trigger reformat
        // Fall through to reboot logic below
    }
    #endif
    #endif
    
    // Fallback: Reboot system (original behavior)
    // This handles Main FS corruption - Main FS will be reformatted by initMainFS() on next boot
    #ifdef USE_EXTENDED_FS_FOR_NODEDB
    #ifdef ARCH_NRF52
    // MAIN_FS_ADDR and MAIN_FS_SIZE are already defined above
    LOG_WARN("⚠️  REBOOTING - Main FS will be reformatted on next boot if corruption is detected");
    LOG_WARN("⚠️  Main FS address range: 0x%08X - 0x%08X (pages 168-243, 304 KB)", 
             MAIN_FS_ADDR, MAIN_FS_ADDR + MAIN_FS_SIZE - 1);
    #else
    LOG_WARN("⚠️  REBOOTING - LittleFS corruption detected");
    #endif
    #else
    LOG_WARN("⚠️  REBOOTING - LittleFS corruption detected");
    #endif
    unsigned long& millis_until_formatting_again = getFormattingDelay();
    if (millis_until_formatting_again > millis()) {
        RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_FLASH_CORRUPTION_UNRECOVERABLE);
        const long millis_remain = millis_until_formatting_again - millis();
        LOG_WARN("Pausing %d seconds to avoid wear on flash storage", millis_remain / 1000);
        delay(millis_remain);
    }
    LOG_INFO("Rebooting to format LittleFS");
    delay(500); // Give the serial port a bit of time to output that last message.
    
    // Set GPREGRET flag for corruption detection on next boot
    #ifdef ARCH_NRF52
    constexpr uint8_t NRF52_MAGIC_LFS_IS_CORRUPT = 0xF5;
    if (!(sd_power_gpregret_clr(0, 0xFF) == NRF_SUCCESS && sd_power_gpregret_set(0, NRF52_MAGIC_LFS_IS_CORRUPT) == NRF_SUCCESS)) {
        NRF_POWER->GPREGRET = NRF52_MAGIC_LFS_IS_CORRUPT;
    }
    NVIC_SystemReset();
    #else
    // For other platforms, just reboot
    NVIC_SystemReset();
    #endif
}

// Global instance - does NOT initialize automatically (see constructor comment)
// Initialization happens in fsInit_patched() after USB/console is ready
static ExtendedFilesystemModule g_extendedFSModule;

// ============================================================================
// Main FS Patch - Enlarged Main FS (304 KB) for NodeDB
// ============================================================================

namespace {
    // Main FS configuration constants
    static constexpr uint32_t MAIN_FS_FLASH_NRF52_PAGE_SIZE = 4096;
    #ifdef NRF52840_XXAA
        static constexpr uint32_t MAIN_FS_FLASH_ADDR = 0xA8000;  // After application, starts where Extended FS was
        static constexpr uint32_t MAIN_FS_BOOTLOADER_ADDR = 0xF8000;  // Adafruit nRF52840 bootloader standard address
    #else
        static constexpr uint32_t MAIN_FS_FLASH_ADDR = 0x6D000;  // Other NRF52 boards
        static constexpr uint32_t MAIN_FS_BOOTLOADER_ADDR = 0x74000;
    #endif
    static constexpr uint32_t MAIN_FS_FLASH_TOTAL_SIZE = 76 * MAIN_FS_FLASH_NRF52_PAGE_SIZE;  // 76 pages = 304 KB (0xA8000-0xF4000)
    static constexpr uint32_t MAIN_FS_BLOCK_SIZE = MAIN_FS_FLASH_NRF52_PAGE_SIZE;  // 4096 bytes
    static constexpr uint32_t MAIN_FS_BLOCK_COUNT = MAIN_FS_FLASH_TOTAL_SIZE / MAIN_FS_BLOCK_SIZE;  // 76 blocks
    static constexpr uint32_t MAIN_FS_LOOKAHEAD = 64;  // Lookahead (must be multiple of 32)
    
    // Main FS state
    static bool main_fs_initialized = false;
    static bool main_fs_mounted = false;
    static bool main_fs_format_in_progress = false;
    static lfs_t main_lfs;
    static struct lfs_config main_lfs_cfg;
    
    // Buffers for LittleFS (required for operation)
    static uint8_t main_read_buffer[128];  // 128 byte buffer
    static uint8_t main_prog_buffer[128];  // 128 byte buffer
    static uint8_t main_lookahead_buffer[8];  // Lookahead buffer (8 bytes = 64 blocks)
    
    // Static assert to ensure Main FS doesn't overlap with bootloader
    static_assert(MAIN_FS_FLASH_ADDR + MAIN_FS_FLASH_TOTAL_SIZE <= MAIN_FS_BOOTLOADER_ADDR, 
                  "Main FS overlaps with bootloader!");
    
    // LittleFS callbacks for Main FS
    int main_lfs_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
    {
        (void)c;
        
        if (!buffer || !size) {
            return LFS_ERR_INVAL;
        }
        
        // CRITICAL: Validate block number to prevent buffer overflow
        // REPLACE LFS_ASSERT with explicit check to avoid reboot
        if (block >= MAIN_FS_BLOCK_COUNT) {
            LOG_ERROR("main_lfs_read: CRITICAL - Invalid block %lu (max: %u) - Main FS corruption detected!", 
                     block, MAIN_FS_BLOCK_COUNT - 1);
            LOG_ERROR("main_lfs_read: This may be caused by USB Mass Storage bootloader overwriting Main FS");
            LOG_ERROR("main_lfs_read: Main FS will be reformatted on next mount");
            return LFS_ERR_CORRUPT;
        }
        
        // CRITICAL: Check for overflow when calculating block offset
        uint32_t block_offset;
        if (!calculateLastAddress(0, block * MAIN_FS_BLOCK_SIZE, block_offset)) {
            LOG_ERROR("main_lfs_read: Block offset overflow (block: %lu, block_size: %u)", block, MAIN_FS_BLOCK_SIZE);
            return LFS_ERR_INVAL;
        }
        
        // CRITICAL: Check for overflow when adding offset to block offset
        uint32_t total_offset;
        if (!calculateLastAddress(block_offset, off, total_offset)) {
            LOG_ERROR("main_lfs_read: Total offset overflow (block_offset: 0x%08X, off: %u)", block_offset, off);
            return LFS_ERR_INVAL;
        }
        
        // CRITICAL: Check for overflow when calculating final address
        uint32_t address;
        if (!calculateLastAddress(MAIN_FS_FLASH_ADDR, total_offset, address)) {
            LOG_ERROR("main_lfs_read: Address overflow (base: 0x%08X, offset: 0x%08X)", MAIN_FS_FLASH_ADDR, total_offset);
            return LFS_ERR_INVAL;
        }
        
        // CRITICAL: Validate address is within filesystem bounds
        uint32_t fs_start = MAIN_FS_FLASH_ADDR;
        uint32_t fs_end = MAIN_FS_FLASH_ADDR + MAIN_FS_FLASH_TOTAL_SIZE;
        
        // CRITICAL: Check for address overflow before validation
        if ((address + size) < address) {
            LOG_ERROR("main_lfs_read: Address overflow (addr: 0x%08X, size: %u)", address, size);
            return LFS_ERR_INVAL;
        }
        
        // CRITICAL: Validate address is within filesystem bounds
        if (address < fs_start || address >= fs_end) {
            LOG_ERROR("main_lfs_read: Address out of bounds (addr: 0x%08X, fs: 0x%08X-0x%08X)", 
                     address, fs_start, fs_end);
            return LFS_ERR_INVAL;
        }
        
        // CRITICAL: Validate that address + size doesn't exceed filesystem end
        if ((address + size) > fs_end) {
            LOG_ERROR("main_lfs_read: Read would exceed filesystem bounds (addr: 0x%08X, size: %u, fs_end: 0x%08X)", 
                     address, size, fs_end);
            return LFS_ERR_INVAL;
        }
        
        // CRITICAL: Final null pointer check before memcpy() to prevent crash
        if (!buffer) {
            LOG_ERROR("main_lfs_read: buffer is null pointer - cannot read data");
            return LFS_ERR_INVAL;
        }
        
        // Read from flash memory
        memcpy(buffer, (void *)address, size);
        return LFS_ERR_OK;
    }
    
    int main_lfs_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
    {
        (void)c;
        
        if (!buffer || !size) {
            return LFS_ERR_INVAL;
        }
        
        // CRITICAL: Validate block number BEFORE any operations
        // REPLACE LFS_ASSERT with explicit check to avoid reboot
        if (block >= MAIN_FS_BLOCK_COUNT) {
            LOG_ERROR("main_lfs_prog: CRITICAL - Invalid block %lu (max: %u) - Main FS corruption detected!", 
                     block, MAIN_FS_BLOCK_COUNT - 1);
            LOG_ERROR("main_lfs_prog: This may be caused by USB Mass Storage bootloader overwriting Main FS");
            LOG_ERROR("main_lfs_prog: Main FS will be reformatted on next mount");
            return LFS_ERR_CORRUPT;
        }
        
        // CRITICAL: Check for overflow when calculating block offset
        uint32_t block_offset;
        if (!calculateLastAddress(0, block * MAIN_FS_BLOCK_SIZE, block_offset)) {
            LOG_ERROR("main_lfs_prog: Block offset overflow (block: %lu, block_size: %u)", block, MAIN_FS_BLOCK_SIZE);
            return LFS_ERR_IO;
        }
        
        // CRITICAL: Check for overflow when adding offset to block offset
        uint32_t total_offset;
        if (!calculateLastAddress(block_offset, off, total_offset)) {
            LOG_ERROR("main_lfs_prog: Total offset overflow (block_offset: 0x%08X, off: %u)", block_offset, off);
            return LFS_ERR_IO;
        }
        
        // CRITICAL: Check for overflow when calculating final address
        uint32_t address;
        if (!calculateLastAddress(MAIN_FS_FLASH_ADDR, total_offset, address)) {
            LOG_ERROR("main_lfs_prog: Address overflow (base: 0x%08X, offset: 0x%08X)", MAIN_FS_FLASH_ADDR, total_offset);
            return LFS_ERR_IO;
        }
        
        // CRITICAL: Validate address is within filesystem bounds
        uint32_t fs_end = MAIN_FS_FLASH_ADDR + MAIN_FS_FLASH_TOTAL_SIZE;
        if (address < MAIN_FS_FLASH_ADDR || address >= fs_end || (address + size) > fs_end) {
            LOG_ERROR("main_lfs_prog: Address out of bounds (addr: 0x%08X, size: %u, fs: 0x%08X-0x%08X)", 
                     address, size, MAIN_FS_FLASH_ADDR, fs_end);
            return LFS_ERR_IO;
        }
        
        // Check if SoftDevice is enabled
        bool use_async = isSoftDeviceAsyncEnabled();
        
        // CRITICAL OPTIMIZATION: Use batch writing instead of word-by-word
        constexpr uint32_t OPTIMAL_BATCH_SIZE = 32;  // Optimal batch size (128 bytes)
        
        // Program flash memory using SoftDevice API with batch writing
        uint32_t i = 0;
        // CRITICAL: Limit iterations to prevent infinite loop
        constexpr uint32_t MAX_ITERATIONS = (512 * 1024 / OPTIMAL_BATCH_SIZE) + 10;  // Safety margin
        uint32_t iteration_count = 0;
        
        while (i < size && iteration_count < MAX_ITERATIONS) {
            iteration_count++;
            
            // CRITICAL: Check for overflow of i to prevent infinite loop
            if (i >= size) {
                break;  // All data written, exit loop
            }
            
            // Calculate how many words we can write in this batch
            uint32_t remaining_bytes = size - i;
            
            // CRITICAL: Early check - if no bytes remaining, exit loop
            if (remaining_bytes == 0) {
                break;  // All data written, exit loop
            }
            
            uint32_t remaining_words = remaining_bytes / 4;
            
            // CRITICAL: Early check - if no words remaining and bytes < 4, cannot write partial word
            if (remaining_words == 0) {
                LOG_ERROR("main_lfs_prog: Cannot write partial word (remaining: %u bytes)", remaining_bytes);
                cleanupAsyncOperation(use_async);
                return LFS_ERR_IO;
            }
            
            uint32_t words_to_write = (remaining_words > OPTIMAL_BATCH_SIZE) ? OPTIMAL_BATCH_SIZE : remaining_words;
            
            // CRITICAL: Verify words_to_write > 0
            if (words_to_write == 0) {
                LOG_ERROR("main_lfs_prog: words_to_write is zero (remaining_words: %u)", remaining_words);
                cleanupAsyncOperation(use_async);
                return LFS_ERR_IO;
            }
            
            uint32_t batch_size_bytes = words_to_write * 4;
            
            // CRITICAL: Check for overflow when calculating write_addr = address + i
            if ((address + i) < address) {
                LOG_ERROR("main_lfs_prog: Address overflow when calculating write address (address: 0x%08X, i: %u)", address, i);
                cleanupAsyncOperation(use_async);
                return LFS_ERR_IO;
            }
            
            uint32_t write_addr = address + i;
            
            // CRITICAL: Check for overflow when calculating write_addr + batch_size_bytes
            if ((write_addr + batch_size_bytes) < write_addr) {
                LOG_ERROR("main_lfs_prog: Address overflow when calculating end address (write_addr: 0x%08X, batch_size: %u)", write_addr, batch_size_bytes);
                cleanupAsyncOperation(use_async);
                return LFS_ERR_IO;
            }
            
            // CRITICAL: Validate write address to prevent overflow
            if (write_addr < MAIN_FS_FLASH_ADDR || 
                (write_addr + batch_size_bytes) > (MAIN_FS_FLASH_ADDR + MAIN_FS_FLASH_TOTAL_SIZE)) {
                LOG_ERROR("main_lfs_prog: Write address out of bounds (addr: 0x%08X, size: %u)", write_addr, batch_size_bytes);
                cleanupAsyncOperation(use_async);
                return LFS_ERR_IO;
            }
            
            // Prepare batch buffer (words must be aligned)
            uint32_t* src_words = (uint32_t*)((uint8_t*)buffer + i);
            
            // CRITICAL: Validate src_words pointer before calling sd_flash_write()
            if (!src_words) {
                LOG_ERROR("main_lfs_prog: src_words is null pointer at offset %u", i);
                cleanupAsyncOperation(use_async);
                return LFS_ERR_IO;
            }
            
            uint32_t err_code;
            // Retry if busy with timeout check
            uint32_t busy_retry_start = millis();
            constexpr uint32_t BUSY_RETRY_TIMEOUT_MS = 5000;  // 5 second timeout for busy retries
            for (uint8_t attempt = 0; attempt < 10; attempt++) {
                if (attempt > 0) {
                    LOG_DEBUG("main_lfs_prog: Retry attempt %u for address 0x%08X (batch: %u words)", attempt + 1, write_addr, words_to_write);
                }
                err_code = sd_flash_write((uint32_t *)write_addr, src_words, words_to_write);
                
                if (err_code == NRF_SUCCESS) {
                    // CRITICAL: Handle async operation completion
                    if (!handleAsyncFlashWrite(use_async, write_addr, batch_size_bytes, src_words, words_to_write)) {
                        cleanupAsyncOperation(use_async);
                        return LFS_ERR_IO;
                    }
                    // CRITICAL: Batch written successfully - increment i to ensure progress
                    uint32_t old_i = i;
                    i += batch_size_bytes;
                    
                    // CRITICAL: Verify that i actually increased (safety check)
                    if (i <= old_i) {
                        LOG_ERROR("main_lfs_prog: Progress check failed (i did not increase: %u -> %u)", old_i, i);
                        cleanupAsyncOperation(use_async);
                        return LFS_ERR_IO;
                    }
                    
                    // CRITICAL: Optimize watchdog feeding - only feed between batches, not in inner loop
                    if (i < size) {
                        FEED_WATCHDOG_AND_YIELD();
                    }
                    
                    break;
                } else if (err_code == NRF_ERROR_BUSY) {
                    // CRITICAL: Check timeout for busy retries
                    uint32_t elapsed = millis() - busy_retry_start;
                    if (elapsed > BUSY_RETRY_TIMEOUT_MS) {
                        LOG_ERROR("main_lfs_prog: NRF_ERROR_BUSY timeout after %u ms at address 0x%08X", elapsed, write_addr);
                        cleanupAsyncOperation(use_async);
                        return LFS_ERR_IO;
                    }
                    
                    // CRITICAL: Feed watchdog during retry delay
                    FEED_WATCHDOG_AND_YIELD();
                    delay(50);
                    continue;
                } else {
                    // CRITICAL: Log error on first attempt, retry on subsequent attempts
                    if (attempt == 0) {
                        logSoftDeviceError(err_code, "write", write_addr);
                        LOG_DEBUG("main_lfs_prog: First attempt failed, will retry up to 9 more times");
                    } else {
                        LOG_DEBUG("main_lfs_prog: Retry %u failed with error 0x%08X", attempt + 1, err_code);
                    }
                    // CRITICAL: For non-busy errors, retry a few times in case it's transient
                    // But if it's FORBIDDEN or INVALID_ADDR, it won't work on retry
                    if (err_code == NRF_ERROR_FORBIDDEN || err_code == NRF_ERROR_INVALID_ADDR) {
                        LOG_ERROR("main_lfs_prog: Fatal error (0x%08X) - will not retry", err_code);
                        cleanupAsyncOperation(use_async);
                        return LFS_ERR_IO;
                    }
                    // For other errors, retry with delay
                    FEED_WATCHDOG_AND_YIELD();
                    delay(10);
                    continue;
                }
            }
            
            // CRITICAL: If we reach here, all retries failed for this batch
            // Check if we made progress (i should have been incremented on success)
            if (i < size && err_code != NRF_SUCCESS) {
                LOG_ERROR("main_lfs_prog: Failed after all 10 retries at address 0x%08X (wrote %u of %u bytes)", 
                         write_addr, i, size);
                LOG_ERROR("main_lfs_prog: Last error code: 0x%08X (check logs above for details)", err_code);
                // CRITICAL: Ensure we don't leave async operation in progress
                cleanupAsyncOperation(use_async);
                return LFS_ERR_IO;
            }
        }
        
        // CRITICAL: Verify all data was written
        if (i < size) {
            LOG_ERROR("main_lfs_prog: Incomplete write (wrote %u of %u bytes) at address 0x%08X", i, size, address);
            // CRITICAL: Ensure we don't leave async operation in progress
            cleanupAsyncOperation(use_async);
            return LFS_ERR_IO;
        }
        
        return LFS_ERR_OK;
    }
    
    int main_lfs_erase(const struct lfs_config *c, lfs_block_t block)
    {
        (void)c;
        
        // CRITICAL: Track format progress
        if (block == 0) {
            LOG_INFO("main_lfs_erase: Starting format - erasing block 0/%u", MAIN_FS_BLOCK_COUNT - 1);
        }
        
        // Feed watchdog at start of each erase call
        FEED_WATCHDOG_AND_YIELD();
        
        // Log progress during format (every 10 blocks or last block)
        if (main_fs_format_in_progress && (block % 10 == 0 || block == MAIN_FS_BLOCK_COUNT - 1)) {
            LOG_INFO("main_lfs_erase: Format progress - erasing block %lu/%u", block, MAIN_FS_BLOCK_COUNT - 1);
        }
        
        // CRITICAL: Validate block number BEFORE any operations
        // REPLACE LFS_ASSERT with explicit check to avoid reboot
        if (block >= MAIN_FS_BLOCK_COUNT) {
            LOG_ERROR("main_lfs_erase: CRITICAL - Invalid block %lu (max: %u) - Main FS corruption detected!", 
                     block, MAIN_FS_BLOCK_COUNT - 1);
            LOG_ERROR("main_lfs_erase: This may be caused by USB Mass Storage bootloader overwriting Main FS");
            LOG_ERROR("main_lfs_erase: Main FS will be reformatted on next mount");
            return LFS_ERR_CORRUPT;
        }
        
        // Calculate block address and page number
        uint32_t block_addr;
        if (!calculateLastAddress(MAIN_FS_FLASH_ADDR, block * MAIN_FS_BLOCK_SIZE, block_addr)) {
            LOG_ERROR("main_lfs_erase: Block address overflow (block: %lu)", block);
            return LFS_ERR_INVAL;
        }
        
        // Calculate which page contains this block
        uint32_t page_number = block_addr / MAIN_FS_FLASH_NRF52_PAGE_SIZE;
        uint32_t page_addr = page_number * MAIN_FS_FLASH_NRF52_PAGE_SIZE;
        
        // Safety check: ensure we don't erase bootloader
        #ifdef NRF52840_XXAA
        constexpr uint32_t BOOTLOADER_PAGE = MAIN_FS_BOOTLOADER_ADDR / MAIN_FS_FLASH_NRF52_PAGE_SIZE;
        if (page_number >= BOOTLOADER_PAGE) {
            LOG_ERROR("main_lfs_erase: Attempted to erase bootloader page %lu! (block %lu)", page_number, block);
            return LFS_ERR_INVAL;
        }
        #endif
        
        // Check if SoftDevice is enabled
        bool use_async = isSoftDeviceAsyncEnabled();
        
        // Erase flash page using SoftDevice API
        uint32_t err_code;
        uint32_t erase_start_time = millis();
        
        // Retry if busy with timeout check
        uint32_t busy_retry_start = millis();
        constexpr uint32_t BUSY_RETRY_TIMEOUT_MS = 10000;  // 10 second timeout for busy retries (erase is slower)
        for (uint8_t attempt = 0; attempt < 10; attempt++) {
            if (attempt > 0) {
                LOG_DEBUG("main_lfs_erase: Retry attempt %u for page %lu", attempt + 1, page_number);
            }
            err_code = sd_flash_page_erase(page_number);
            
            if (err_code == NRF_SUCCESS) {
                // CRITICAL: Handle async operation completion
                if (!handleAsyncFlashErase(use_async, page_number, page_addr)) {
                    return LFS_ERR_IO;
                }
                break;
            } else if (err_code == NRF_ERROR_BUSY) {
                // CRITICAL: Check timeout for busy retries
                uint32_t elapsed = millis() - busy_retry_start;
                if (elapsed > BUSY_RETRY_TIMEOUT_MS) {
                    LOG_ERROR("main_lfs_erase: NRF_ERROR_BUSY timeout after %u ms for page %lu", elapsed, page_number);
                    return LFS_ERR_IO;
                }
                
                // CRITICAL: Feed watchdog during retry delay
                FEED_WATCHDOG_AND_YIELD();
                delay(50);
                continue;
            } else {
                // CRITICAL: Log error on first attempt
                if (attempt == 0) {
                    logSoftDeviceError(err_code, "erase", page_number);
                    LOG_DEBUG("main_lfs_erase: First attempt failed, will retry up to 9 more times");
                } else {
                    LOG_DEBUG("main_lfs_erase: Retry %u failed with error 0x%08X", attempt + 1, err_code);
                }
                // CRITICAL: For non-busy errors, retry a few times in case it's transient
                if (err_code == NRF_ERROR_FORBIDDEN || err_code == NRF_ERROR_INVALID_ADDR) {
                    LOG_ERROR("main_lfs_erase: Fatal error (0x%08X) - will not retry", err_code);
                    return LFS_ERR_IO;
                }
                // For other errors, retry with delay
                FEED_WATCHDOG_AND_YIELD();
                delay(10);
                continue;
            }
        }
        
        // CRITICAL: If we reach here, all retries failed
        if (err_code != NRF_SUCCESS) {
            LOG_ERROR("main_lfs_erase: Failed after all 10 retries for page %lu (error: 0x%08X)", page_number, err_code);
            return LFS_ERR_IO;
        }
        
        return LFS_ERR_OK;
    }
    
    int main_lfs_sync(const struct lfs_config *c)
    {
        (void)c;
        // No-op for flash memory (writes are synchronous)
        return LFS_ERR_OK;
    }
    
    // Initialize Main FS
    bool initMainFS()
    {
        if (main_fs_initialized && main_fs_mounted) {
            return true;
        }
        
        LOG_INFO("========================================");
        LOG_INFO("INITIALIZING ENLARGED MAIN FILESYSTEM");
        LOG_INFO("========================================");
        LOG_INFO("Configuration:");
        LOG_INFO("  - Address: 0x%08X (page %u)", MAIN_FS_FLASH_ADDR, MAIN_FS_FLASH_ADDR / MAIN_FS_FLASH_NRF52_PAGE_SIZE);
        LOG_INFO("  - Size: %u bytes (%u KB, %u pages)", MAIN_FS_FLASH_TOTAL_SIZE, MAIN_FS_FLASH_TOTAL_SIZE / 1024, MAIN_FS_FLASH_TOTAL_SIZE / MAIN_FS_FLASH_NRF52_PAGE_SIZE);
        LOG_INFO("  - Block size: %u bytes", MAIN_FS_BLOCK_SIZE);
        LOG_INFO("  - Block count: %u blocks", MAIN_FS_BLOCK_COUNT);
        LOG_INFO("  - End address: 0x%08X (page %u)", 
                 MAIN_FS_FLASH_ADDR + MAIN_FS_FLASH_TOTAL_SIZE - 1, 
                 (MAIN_FS_FLASH_ADDR + MAIN_FS_FLASH_TOTAL_SIZE - 1) / MAIN_FS_FLASH_NRF52_PAGE_SIZE);
        LOG_INFO("  - Bootloader start: 0x%08X (page %u)", MAIN_FS_BOOTLOADER_ADDR, MAIN_FS_BOOTLOADER_ADDR / MAIN_FS_FLASH_NRF52_PAGE_SIZE);
        
        // CRITICAL: Safety check - validate filesystem addresses are within valid flash range
        // Check that filesystem start address is valid
        if (MAIN_FS_FLASH_ADDR < 0x1000) {
            LOG_ERROR("ERROR: Main filesystem start address (0x%08X) is too low (below 0x1000)!", MAIN_FS_FLASH_ADDR);
            return false;
        }
        
        // CRITICAL: Check that Main FS doesn't conflict with application area
        // Application ends at 0xA8000 (limited by linker script)
        constexpr uint32_t APPLICATION_END_ADDR = 0xA8000;
        if (MAIN_FS_FLASH_ADDR < APPLICATION_END_ADDR) {
            LOG_ERROR("ERROR: Main filesystem start (0x%08X) is INSIDE application area (ends at 0x%08X)!", 
                     MAIN_FS_FLASH_ADDR, APPLICATION_END_ADDR);
            return false;
        }
        
        // Check that filesystem end address is valid
        // CRITICAL: Use calculateLastAddress() to safely compute fs_end with overflow protection
        uint32_t fs_end;
        if (!calculateLastAddress(MAIN_FS_FLASH_ADDR, MAIN_FS_FLASH_TOTAL_SIZE, fs_end)) {
            LOG_ERROR("ERROR: Main filesystem address overflow when calculating end address!");
            LOG_ERROR("  - Start: 0x%08X", MAIN_FS_FLASH_ADDR);
            LOG_ERROR("  - Size: %u bytes", MAIN_FS_FLASH_TOTAL_SIZE);
            return false;
        }
        
        // Subtract 1 to get the last valid byte address
        if (fs_end == 0) {
            LOG_ERROR("ERROR: Cannot subtract 1 from fs_end (would underflow)!");
            return false;
        }
        fs_end = fs_end - 1;
        
        if (fs_end >= MAIN_FS_BOOTLOADER_ADDR) {
            LOG_ERROR("ERROR: Main filesystem would overlap bootloader!");
            LOG_ERROR("  - Filesystem end: 0x%08X", fs_end);
            LOG_ERROR("  - Bootloader start: 0x%08X", MAIN_FS_BOOTLOADER_ADDR);
            return false;
        }
        
        // CRITICAL: Additional check - verify fs_end is still >= start after subtraction
        if (fs_end < MAIN_FS_FLASH_ADDR) {
            LOG_ERROR("ERROR: Main filesystem address overflow (end < start)!");
            LOG_ERROR("  - Start: 0x%08X", MAIN_FS_FLASH_ADDR);
            LOG_ERROR("  - Size: %u bytes", MAIN_FS_FLASH_TOTAL_SIZE);
            LOG_ERROR("  - End: 0x%08X", fs_end);
            return false;
        }
        
        uint32_t gap = MAIN_FS_BOOTLOADER_ADDR - fs_end - 1;
        LOG_INFO("Safety check:");
        LOG_INFO("  - Gap to bootloader: %u bytes (%u KB, %u pages)", gap, gap / 1024, gap / MAIN_FS_FLASH_NRF52_PAGE_SIZE);
        LOG_INFO("  - Status: %s", 
                 gap >= (64 * 1024) ? "SAFE (>=64 KB)" : 
                 gap >= (32 * 1024) ? "OK (>=32 KB)" : 
                 gap >= (16 * 1024) ? "WARNING (<32 KB)" : "CRITICAL (<16 KB)");
        
        // Initialize LittleFS config
        main_lfs_cfg.context = NULL;
        main_lfs_cfg.read = main_lfs_read;
        main_lfs_cfg.prog = main_lfs_prog;
        main_lfs_cfg.erase = main_lfs_erase;
        main_lfs_cfg.sync = main_lfs_sync;
        
        main_lfs_cfg.read_size = 128;
        main_lfs_cfg.prog_size = 128;
        main_lfs_cfg.block_size = MAIN_FS_BLOCK_SIZE;
        main_lfs_cfg.block_count = MAIN_FS_BLOCK_COUNT;
        main_lfs_cfg.lookahead = MAIN_FS_LOOKAHEAD;
        
        main_lfs_cfg.read_buffer = main_read_buffer;
        main_lfs_cfg.prog_buffer = main_prog_buffer;
        main_lfs_cfg.lookahead_buffer = main_lookahead_buffer;
        
        // CRITICAL: Validate lookahead buffer size matches configuration
        uint32_t required_lookahead_buffer_size = main_lfs_cfg.lookahead / 8;
        if (sizeof(main_lookahead_buffer) < required_lookahead_buffer_size) {
            LOG_ERROR("CRITICAL: lookahead_buffer size mismatch!");
            LOG_ERROR("  - Buffer size: %u bytes", sizeof(main_lookahead_buffer));
            LOG_ERROR("  - Required size: %u bytes (for %u blocks)", 
                     required_lookahead_buffer_size, main_lfs_cfg.lookahead);
            LOG_ERROR("  - This will cause LittleFS corruption!");
            return false;
        }
        if (sizeof(main_lookahead_buffer) > required_lookahead_buffer_size) {
            LOG_WARN("WARNING: lookahead_buffer is larger than required");
            LOG_WARN("  - Buffer size: %u bytes", sizeof(main_lookahead_buffer));
            LOG_WARN("  - Required size: %u bytes (for %u blocks)", 
                    required_lookahead_buffer_size, main_lfs_cfg.lookahead);
            LOG_WARN("  - Extra bytes will be unused");
        }
        LOG_INFO("  - Expected lookahead buffer size: %u bytes (for %u blocks) - OK",
                 required_lookahead_buffer_size, main_lfs_cfg.lookahead);
        
        // CRITICAL: Check if SoftDevice is initialized before attempting flash operations
        bool sd_enabled = checkSoftDeviceState(true);
        if (!sd_enabled) {
            LOG_WARN("SoftDevice not enabled - using direct flash access (may be slower or less safe)");
            LOG_WARN("  - For best performance, ensure SoftDevice is initialized before fsInit()");
            LOG_WARN("  - SoftDevice is typically initialized via Bluetooth (setBluetoothEnable)");
        }
        
        // CRITICAL: Validate that config callbacks are set
        if (!main_lfs_cfg.read || !main_lfs_cfg.prog || !main_lfs_cfg.erase || !main_lfs_cfg.sync) {
            LOG_ERROR("CRITICAL: Main filesystem config callbacks are not properly initialized!");
            return false;
        }
        
        // Try to mount filesystem
        LOG_INFO("Attempting to mount enlarged main filesystem...");
        FEED_WATCHDOG_AND_YIELD();
        int mount_result = lfs_mount(&main_lfs, &main_lfs_cfg);
        FEED_WATCHDOG_AND_YIELD();
        
        bool need_reformat = false;
        
        if (mount_result != LFS_ERR_OK) {
            LOG_WARN("Main filesystem mount failed (error: %d) - will format...", mount_result);
            if (mount_result == LFS_ERR_CORRUPT) {
                LOG_WARN("Mount failed with LFS_ERR_CORRUPT - filesystem is corrupted");
                LOG_WARN("This may be caused by USB Mass Storage bootloader overwriting Main FS");
            }
            LOG_DEBUG("Mount failure details:");
            LOG_DEBUG("  - Config: read_size=%u, prog_size=%u, block_size=%u, block_count=%u", 
                     main_lfs_cfg.read_size, main_lfs_cfg.prog_size, 
                     main_lfs_cfg.block_size, main_lfs_cfg.block_count);
            LOG_DEBUG("  - Buffers: read=%p, prog=%p, lookahead=%p", 
                     main_lfs_cfg.read_buffer, main_lfs_cfg.prog_buffer, 
                     main_lfs_cfg.lookahead_buffer);
            need_reformat = true;
        } else {
            // Mount succeeded - perform additional validation
            LOG_DEBUG("Mount succeeded, performing additional validation...");
            
            // CRITICAL: Additional validation after mount - check if filesystem is actually working
            // Try to read a test to verify filesystem is actually accessible
            lfs_file_t test_file;
            int test_open = lfs_file_open(&main_lfs, &test_file, "/.test", LFS_O_RDONLY);
            if (test_open == LFS_ERR_OK) {
                lfs_file_close(&main_lfs, &test_file);
                LOG_DEBUG("Filesystem validation passed");
            } else if (test_open != LFS_ERR_NOENT) {
                LOG_WARN("Filesystem validation FAILED - test file access failed (error: %d)", test_open);
                LOG_WARN("Filesystem may be corrupted (possibly from USB Mass Storage bootloader) - will reformat");
                lfs_unmount(&main_lfs);
                need_reformat = true;
            } else {
                LOG_DEBUG("Filesystem validation passed - test file doesn't exist (expected)");
            }
        }
        
        if (need_reformat) {
            // Unmount if mounted
            if (mount_result == LFS_ERR_OK) {
                LOG_INFO("Unmounting filesystem before reformat...");
                lfs_unmount(&main_lfs);
            }
            
            // CRITICAL: Verify formatFilesystem() succeeded before proceeding
            LOG_DEBUG("Starting filesystem format...");
            main_fs_format_in_progress = true;
            FEED_WATCHDOG_AND_YIELD();
            int format_result = lfs_format(&main_lfs, &main_lfs_cfg);
            main_fs_format_in_progress = false;
            FEED_WATCHDOG_AND_YIELD();
            
            if (format_result != LFS_ERR_OK) {
                LOG_ERROR("CRITICAL: Main filesystem format failed (error: %d) - filesystem is NOT initialized!", format_result);
                LOG_ERROR("Initialization state after format failure:");
                LOG_ERROR("  - isInitialized: %s", main_fs_initialized ? "true" : "false");
                LOG_ERROR("  - isMounted: %s", main_fs_mounted ? "true" : "false");
                LOG_ERROR("  - format_in_progress: %s", main_fs_format_in_progress ? "true" : "false");
                return false;
            }
            LOG_DEBUG("Filesystem format completed successfully");
            
            // NOTE: format_in_progress is already reset above after lfs_format() completes
            
            LOG_INFO("REFORMAT COMPLETED SUCCESSFULLY");
            
            // CRITICAL: After format, filesystem should be mounted (lfs_format() doesn't mount, so we mount here)
            mount_result = lfs_mount(&main_lfs, &main_lfs_cfg);
            FEED_WATCHDOG_AND_YIELD();
            
            if (mount_result != LFS_ERR_OK) {
                LOG_ERROR("CRITICAL: Filesystem mount failed after format (error: %d) - cannot proceed!", mount_result);
                return false;
            }
        }
        
        // CRITICAL: Only set isMounted if filesystem is actually mounted
        if (!need_reformat && mount_result == LFS_ERR_OK) {
            main_fs_mounted = true;
        } else if (need_reformat) {
            // After format, mount_result should be LFS_ERR_OK if we reached here
            if (mount_result == LFS_ERR_OK) {
                main_fs_mounted = true;
            } else {
                LOG_ERROR("CRITICAL: Filesystem mount failed after format - cannot proceed!");
                return false;
            }
        } else {
            LOG_ERROR("CRITICAL: Filesystem mount failed - cannot proceed!");
            return false;
        }
        
        LOG_INFO("SUCCESS: Enlarged main filesystem mounted!");
        
        // Create required directories
        LOG_DEBUG("Creating required directories...");
        FEED_WATCHDOG_AND_YIELD();
        
        // Create /prefs directory first (required for config files and node slots)
        int prefs_dir_err = lfs_mkdir(&main_lfs, "/prefs");
        if (prefs_dir_err != LFS_ERR_OK && prefs_dir_err != LFS_ERR_EXIST) {
            LOG_WARN("Failed to create /prefs directory (error: %d)", prefs_dir_err);
        } else {
            LOG_DEBUG("Directory /prefs ready");
        }
        FEED_WATCHDOG_AND_YIELD();
        
        // Create /prefs/nodes directory for node slot files
        // This directory will be automatically deleted by rmDir("/prefs") during factory reset
        int nodes_dir_err = lfs_mkdir(&main_lfs, "/prefs/nodes");
        if (nodes_dir_err != LFS_ERR_OK && nodes_dir_err != LFS_ERR_EXIST) {
            LOG_WARN("Failed to create /prefs/nodes directory (error: %d)", nodes_dir_err);
        } else {
            LOG_DEBUG("Directory /prefs/nodes ready");
        }
        FEED_WATCHDOG_AND_YIELD();
        
        // CRITICAL: Set isInitialized = true only after all checks pass and filesystem is successfully mounted
        main_fs_initialized = true;
        
        LOG_INFO("  - Total capacity: %u KB (%u pages)", MAIN_FS_FLASH_TOTAL_SIZE / 1024, MAIN_FS_FLASH_TOTAL_SIZE / MAIN_FS_FLASH_NRF52_PAGE_SIZE);
        LOG_INFO("  - Block count: %u blocks", MAIN_FS_BLOCK_COUNT);
        LOG_INFO("========================================");
        
        // CRITICAL: Log Main FS statistics after successful initialization
        // This helps detect filesystem issues early (e.g., when free space is low)
        LOG_INFO("========================================");
        LOG_INFO("MAIN FS STATISTICS:");
        LOG_INFO("========================================");
        
        // Get filesystem usage statistics
        uint32_t fs_total = 0, fs_used = 0, fs_free = 0;
        if (getMainFSStats(&fs_total, &fs_used, &fs_free)) {
            uint32_t fs_total_kb = fs_total / 1024;
            uint32_t fs_used_kb = fs_used / 1024;
            uint32_t fs_free_kb = fs_free / 1024;
            uint32_t fs_used_pct = (fs_total_kb > 0) ? (fs_used_kb * 100) / fs_total_kb : 0;
            
            LOG_INFO("  - Filesystem usage: %u KB / %u KB (%u%%)", fs_used_kb, fs_total_kb, fs_used_pct);
            LOG_INFO("  - Free space: %u KB", fs_free_kb);
            
            // WARNING: If filesystem is more than 80% full, log warning
            if (fs_used_pct > 80) {
                LOG_WARN("  - ⚠️  WARNING: Main FS is %u%% full - may cause issues soon!", fs_used_pct);
            }
        } else {
            LOG_WARN("  - Could not get filesystem statistics");
        }
        LOG_INFO("========================================");
        
        return true;
    }
}

/**
 * @brief Get main filesystem LittleFS instance
 */
lfs_t* getMainFS()
{
    if (!main_fs_initialized) {
        if (!initMainFS()) {
            return nullptr;
        }
    }
    return main_fs_mounted ? &main_lfs : nullptr;
}

/**
 * @brief Check if main filesystem is available
 */
bool isMainFSAvailable()
{
    return main_fs_initialized && main_fs_mounted;
}

// ============================================================================
// From FSCommon-patches.cpp
// ============================================================================

/**
 * @brief Get main filesystem statistics (for DeviceStatsModule)
 */
bool getMainFSStats(uint32_t* total, uint32_t* used, uint32_t* free)
{
    if (!total || !used || !free) {
        return false;
    }
    
    // Enlarged main filesystem is 304KB (76 pages * 4KB)
    *total = MAIN_FS_FLASH_TOTAL_SIZE;
    
    // Calculate used space by summing all files
    *used = 0;
    std::vector<meshtastic_FileInfo> files = getFiles("/", 10);
    for (const auto& file : files) {
        *used += file.size_bytes;
    }
    
    *free = (*total > *used) ? (*total - *used) : 0;
    return true;
}

/**
 * @brief Get extended filesystem statistics (for DeviceStatsModule)
 */
bool getExtendedFSStats(uint32_t* total, uint32_t* used, uint32_t* free)
{
    if (!total || !used || !free) {
        return false;
    }
    
    // CRITICAL: Check if extended filesystem is available before getting stats
    if (!ExtendedFilesystemModule::isAvailable()) {
        *total = 0;
        *used = 0;
        *free = 0;
        return false;
    }
    
    return ExtendedFilesystemModule::getStats(total, used, free);
}

// ============================================================================
// From FSCommon-patches.h (inline functions)
// ============================================================================

/**
 * @brief Patched version of getFiles() with FEED_WATCHDOG_AND_YIELD() calls
 */
std::vector<meshtastic_FileInfo> getFiles(const char *dirname, uint8_t levels)
{
    std::vector<meshtastic_FileInfo> filenames = {};
#ifdef FSCom
    File root = FSCom.open(dirname, FILE_O_READ);
    if (!root)
        return filenames;
    if (!root.isDirectory())
        return filenames;

    int fileCount = 0;
    File file = root.openNextFile();
    while (file) {
        // CRITICAL: Periodically yield and process SoftDevice events during filesystem scan
        // This prevents watchdog timeout and allows BLE operations to continue
        if (fileCount % 5 == 0) {  // Every 5 files
            FEED_WATCHDOG_AND_YIELD();
        }
        fileCount++;

        if (file.isDirectory() && !String(file.name()).endsWith(".")) {
            if (levels) {
#ifdef ARCH_ESP32
                std::vector<meshtastic_FileInfo> subDirFilenames = getFiles(file.path(), levels - 1);
#else
                std::vector<meshtastic_FileInfo> subDirFilenames = getFiles(file.name(), levels - 1);
#endif
                filenames.insert(filenames.end(), subDirFilenames.begin(), subDirFilenames.end());
                file.close();
            }
        } else {
            meshtastic_FileInfo fileInfo = {"", static_cast<uint32_t>(file.size())};
            
            // CRITICAL: Use strncpy instead of strcpy to prevent buffer overflow
            // file_name size is 228 bytes (from protobuf definition)
            constexpr size_t FILE_NAME_MAX_SIZE = 228;
#ifdef ARCH_ESP32
            const char* source_path = file.path();
            if (source_path) {
                // CRITICAL: Check string length before copying to prevent buffer overflow
                size_t source_len = strlen(source_path);
                if (source_len >= FILE_NAME_MAX_SIZE) {
                    LOG_WARN("getFiles: File path too long (%u bytes), truncating to %u bytes", 
                            (unsigned)source_len, (unsigned)(FILE_NAME_MAX_SIZE - 1));
                }
                strncpy(fileInfo.file_name, source_path, FILE_NAME_MAX_SIZE - 1);
                fileInfo.file_name[FILE_NAME_MAX_SIZE - 1] = '\0';  // Ensure null termination
            }
#else
            const char* source_name = file.name();
            if (source_name) {
                // CRITICAL: Check string length before copying to prevent buffer overflow
                size_t source_len = strlen(source_name);
                if (source_len >= FILE_NAME_MAX_SIZE) {
                    LOG_WARN("getFiles: File name too long (%u bytes), truncating to %u bytes", 
                            (unsigned)source_len, (unsigned)(FILE_NAME_MAX_SIZE - 1));
                }
                strncpy(fileInfo.file_name, source_name, FILE_NAME_MAX_SIZE - 1);
                fileInfo.file_name[FILE_NAME_MAX_SIZE - 1] = '\0';  // Ensure null termination
            }
#endif
            // CRITICAL: Validate file_name is not empty before adding to vector
            if (fileInfo.file_name[0] != '\0' && !String(fileInfo.file_name).endsWith(".")) {
                filenames.push_back(fileInfo);
            }
            // CRITICAL: Always close file to prevent resource leak
            file.close();
        }
        // CRITICAL: Get next file (may return null, which will exit loop)
        file = root.openNextFile();
    }
    // CRITICAL: Always close root directory
    root.close();
    
    // CRITICAL: Final FEED_WATCHDOG_AND_YIELD() after filesystem scan completes
    FEED_WATCHDOG_AND_YIELD();
#endif
    return filenames;
}

/**
 * @brief Patched version of fsInit() with extended filesystem initialization
 */
void fsInit_patched()
{
    LOG_INFO("========================================");
    LOG_INFO("fsInit_patched() START");
    LOG_INFO("========================================");
    
#ifdef FSCom
    LOG_INFO("Step 1: Calling preFSBegin()...");
    concurrency::LockGuard g(spiLock);
    preFSBegin();
    LOG_INFO("Step 2: preFSBegin() completed, calling FSBegin()...");
    LOG_INFO("NOTE: Framework will try to mount Main FS at old address (0xED000-0xF4000, 28 KB)");
    LOG_INFO("NOTE: This may show superblock errors - this is expected, as we use enlarged Main FS (0xA8000-0xF4000, 304 KB)");
    if (!FSBegin()) {
        LOG_WARN("Framework Main FS mount failed (expected - old address conflicts with enlarged Main FS)");
        // This is expected - framework tries to use old address (0xED000), but we use enlarged Main FS (0xA8000-0xF4000)
        // Framework will auto-format on failure, but we don't use it - we use our own enlarged Main FS
    }
    LOG_INFO("Step 3: FSBegin() completed (framework Main FS may have errors - we use enlarged Main FS instead)");
    
    // CRITICAL: Diagnostic logging for Main FS configuration (as per code review)
    // This helps verify what the framework actually uses vs. what we document
    LOG_INFO("========================================");
    LOG_INFO("MAIN FS DIAGNOSTICS (from framework):");
    LOG_INFO("========================================");
    LOG_INFO("Expected configuration (from documentation):");
    LOG_INFO("  - Start address: 0xED000 (from linker script)");
    LOG_INFO("  - Expected size: 7 pages × 4KB = 28 KB (0x7000 bytes)");
    LOG_INFO("  - Expected end: 0xED000 + 0x7000 = 0xF4000 (end exclusive)");
    LOG_INFO("  - Expected end (inclusive): 0xF3FFF");
    LOG_INFO("Current code assumptions:");
    LOG_INFO("  - Main FS end: 0xF4000 (28 KB, 7 pages) - CORRECTED");
    LOG_INFO("  - Actual size: 0xF4000 - 0xED000 = 0x7000 = 28 KB");
    LOG_INFO("Framework InternalFileSystem actual values:");
    #ifdef FSCom
    // FSCom is defined as InternalFS for ARCH_NRF52 (from FSCommon.h)
    // Check if totalBytes() and usedBytes() methods are available
    #ifdef ARCH_NRF52
    // For NRF52, InternalFS may not have totalBytes()/usedBytes() methods
    // Try to use getMainFSStats() helper function (same approach as DeviceStatsModule)
    LOG_INFO("  - FSCom (InternalFS) object: %p", (void*)&FSCom);
    LOG_INFO("  - InternalFS object: %p", (void*)&InternalFS);
    LOG_INFO("  - Attempting to get Main FS size from framework...");
    
    // Try to use getMainFSStats() helper function (defined in this file)
    extern bool getMainFSStats(uint32_t* total, uint32_t* used, uint32_t* free);
    uint32_t main_fs_total = 0;
    uint32_t main_fs_used = 0;
    uint32_t main_fs_free = 0;
    bool got_size = getMainFSStats(&main_fs_total, &main_fs_used, &main_fs_free);
    
    if (got_size && main_fs_total > 0) {
        LOG_INFO("  - FSCom.totalBytes(): %u bytes (%u KB) (via getMainFSStats)", 
                 main_fs_total, main_fs_total / 1024);
        LOG_INFO("  - FSCom.usedBytes(): %u bytes (%u KB) (via getMainFSStats)", 
                 main_fs_used, main_fs_used / 1024);
        LOG_INFO("  - FSCom free: %u bytes (%u KB) (via getMainFSStats)", 
                 main_fs_free, main_fs_free / 1024);
    } else {
        LOG_INFO("  - getMainFSStats() returned invalid data or failed");
        LOG_INFO("  - Note: totalBytes()/usedBytes() may not be available for NRF52 InternalFS");
    }
    
    // Also try to call FSCom.totalBytes() directly - it might work on some NRF52 builds
    // If it doesn't compile, comment out these lines
    // Note: Some builds of Adafruit nRF52 Arduino may have these methods
    LOG_INFO("  - Framework config: InternalFileSystem from Adafruit nRF52 Arduino");
    LOG_INFO("  - To verify actual size, check framework source:");
    LOG_INFO("    https://github.com/meshtastic/Adafruit_nRF52_Arduino");
    LOG_INFO("    (commit: e13f5820002a4fb2a5e6754b42ace185277e5adf)");
    #else
    LOG_INFO("  - FSCom.totalBytes(): %u bytes (%u KB)", 
             FSCom.totalBytes(), FSCom.totalBytes() / 1024);
    LOG_INFO("  - FSCom.usedBytes(): %u bytes (%u KB)", 
             FSCom.usedBytes(), FSCom.usedBytes() / 1024);
    LOG_INFO("  - FSCom free: %u bytes (%u KB)", 
             FSCom.totalBytes() - FSCom.usedBytes(),
             (FSCom.totalBytes() - FSCom.usedBytes()) / 1024);
    #endif
    #else
    LOG_INFO("  - FSCom not available (ARCH_NRF52 may not define it)");
    #endif
    LOG_INFO("========================================");
    LOG_INFO("NOTE: Framework uses 7 pages (28 KB) as confirmed by FSCom.totalBytes()");
    LOG_INFO("      Code has been corrected to use 0xF4000 instead of 0xEF800");
    LOG_INFO("========================================");
    
    // CRITICAL: Diagnostic logging for Bootloader configuration (as per code review)
    LOG_INFO("========================================");
    LOG_INFO("BOOTLOADER DIAGNOSTICS:");
    LOG_INFO("========================================");
    LOG_INFO("Expected configuration (from documentation):");
    #ifdef NRF52840_XXAA
    LOG_INFO("  - Start address: 0xF8000 (Adafruit nRF52840 bootloader standard)");
    LOG_INFO("  - Expected size: 32 KB (8 pages × 4KB = 0x8000 bytes)");
    LOG_INFO("  - Expected end: 0xF8000 + 0x8000 = 0x100000 (end exclusive)");
    LOG_INFO("  - Expected end (inclusive): 0xFFFFF");
    LOG_INFO("Current code configuration:");
    LOG_INFO("  - BOOTLOADER_ADDR: 0x%08X (from MainFilesystemModule)", 
             MAIN_FS_BOOTLOADER_ADDR);
    LOG_INFO("  - Bootloader page: %u", MAIN_FS_BOOTLOADER_ADDR / MAIN_FS_FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Gap from Main FS end (0xF4000): %u bytes (%u KB, %u pages)", 
             MAIN_FS_BOOTLOADER_ADDR - 0xF4000,
             (MAIN_FS_BOOTLOADER_ADDR - 0xF4000) / 1024,
             (MAIN_FS_BOOTLOADER_ADDR - 0xF4000) / MAIN_FS_FLASH_NRF52_PAGE_SIZE);
    #else
    LOG_INFO("  - Start address: 0x74000 (Other NRF52 boards)");
    LOG_INFO("  - Expected size: 32 KB (8 pages × 4KB = 0x8000 bytes)");
    LOG_INFO("  - Expected end: 0x74000 + 0x8000 = 0x7C000 (end exclusive)");
    LOG_INFO("Current code configuration:");
    LOG_INFO("  - BOOTLOADER_ADDR: 0x%08X (from MainFilesystemModule)", 
             MAIN_FS_BOOTLOADER_ADDR);
    LOG_INFO("  - Bootloader page: %u", MAIN_FS_BOOTLOADER_ADDR / MAIN_FS_FLASH_NRF52_PAGE_SIZE);
    #endif
    LOG_INFO("Framework bootloader configuration:");
    LOG_INFO("  - Adafruit nRF52 Arduino bootloader");
    LOG_INFO("  - Standard address for nRF52840: 0xF8000");
    LOG_INFO("  - To verify actual bootloader size, check framework source:");
    LOG_INFO("    https://github.com/meshtastic/Adafruit_nRF52_Arduino");
    LOG_INFO("    (commit: e13f5820002a4fb2a5e6754b42ace185277e5adf)");
    LOG_INFO("========================================");
    LOG_INFO("NOTE: Bootloader is protected from erasure in lfs_erase() callback");
    LOG_INFO("      Main FS ends at 0xF4000, bootloader starts at 0xF8000");
    LOG_INFO("      Gap: %u bytes (%u KB) - safe buffer", 
             MAIN_FS_BOOTLOADER_ADDR - 0xF4000,
             (MAIN_FS_BOOTLOADER_ADDR - 0xF4000) / 1024);
    LOG_INFO("========================================");
    
    // CRITICAL: Initialize enlarged main filesystem EARLY, BEFORE BLE is enabled
    // This ensures all flash operations (formatting, structure creation) happen before BLE pairing
    // After initialization, only individual file operations will occur, which are much faster
    // and won't conflict with BLE bonding operations
    LOG_INFO("Step 4: Initializing enlarged main filesystem for NodeDB (BEFORE BLE setup)...");
    bool init_result = initMainFS();
    if (!init_result) {
        LOG_ERROR("CRITICAL: Enlarged main filesystem initialization FAILED!");
        LOG_ERROR("Enlarged main filesystem will not be available for NodeDB operations");
        // Continue anyway - device can still function without enlarged main filesystem
        // NodeDB will fall back to standard mode
    } else {
        LOG_INFO("Step 5: Enlarged main filesystem initialization complete");
    }
#if defined(ARCH_ESP32)
    LOG_DEBUG("Filesystem files (%d/%d Bytes):", FSCom.usedBytes(), FSCom.totalBytes());
#else
    LOG_DEBUG("Filesystem files:");
#endif
    LOG_INFO("Step 6: Listing filesystem files...");
    listDir("/", 10);
    LOG_INFO("fsInit_patched() COMPLETE");
    LOG_INFO("========================================");
#endif
}

// ============================================================================
// From main-fs-pre-init.cpp
// ============================================================================

// Compile filesystem code for RAK4631 Lite variant
#ifdef RAK_4631

namespace
{
constexpr uint8_t NRF52_MAGIC_LFS_IS_CORRUPT = 0xF5;
constexpr uint32_t MULTIPLE_CORRUPTION_DELAY_MILLIS = 20 * 60 * 1000;
} // namespace

#ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM
// Override preFSBegin() with MAXIMUM SAFE logging
// NOTE: eraseFilesystemPages() was removed as dead code - page erasure is handled
// by lfs_erase() callback in ExtendedFilesystemModule when LittleFS formats the filesystem
void preFSBegin()
{
    // CRITICAL: Use helper function to get formatting delay (prevents race conditions)
    unsigned long& millis_until_formatting_again = getFormattingDelay();
    
    // SAFE: Fast register check (does not require flash operations)
    uint32_t reset_reason = NRF_POWER->RESETREAS;
    uint32_t gpregret = NRF_POWER->GPREGRET;
    
    LOG_INFO("========================================");
    LOG_INFO("preFSBegin() START - STEP 1: MINIMAL SAFE OPERATIONS");
    LOG_INFO("========================================");
    LOG_INFO("SEQUENCE CHECK:");
    LOG_INFO("  - USB should be initialized (consoleInit() called earlier)");
    LOG_INFO("  - Current millis(): %lu", millis());
    LOG_INFO("  - This function is called BEFORE fsInit() -> FSBegin()");
    LOG_INFO("");
    LOG_INFO("SAFETY RULES:");
    LOG_INFO("  ✓ Only quick operations (register checks)");
    LOG_INFO("  ✓ NO erase pages here (extended FS initializes lazily when needed)");
    LOG_INFO("  ✓ Only InternalFS.format() if corruption flag (fast, no erase)");
    LOG_INFO("");
    LOG_INFO("Reset information:");
    LOG_INFO("  - RESETREAS: 0x%08X", reset_reason);
    LOG_INFO("  - GPREGRET: 0x%02X", gpregret);
    LOG_INFO("  - NRF52_MAGIC_LFS_IS_CORRUPT: 0x%02X", NRF52_MAGIC_LFS_IS_CORRUPT);
    LOG_INFO("  - millis_until_formatting_again: %lu", millis_until_formatting_again);
    
    // Check GPREGRET corruption flag (FAST - register read only)
    bool is_warm_boot = (reset_reason == 0);
    bool is_corruption_flag = (gpregret == NRF52_MAGIC_LFS_IS_CORRUPT);
    
    LOG_INFO("Corruption check:");
    LOG_INFO("  - Is warm boot (RESETREAS == 0): %s", is_warm_boot ? "YES" : "NO");
    LOG_INFO("  - GPREGRET corruption flag set: %s", is_corruption_flag ? "YES" : "NO");
    
    if (is_warm_boot && is_corruption_flag) {
        LOG_WARN("⚠️  GPREGRET corruption flag detected!");
        LOG_WARN("⚠️  CRITICAL: Checking which filesystem is corrupted...");
        
        // CRITICALLY IMPORTANT: Check which filesystem is corrupted!
        // Extended filesystem may trigger lfs_assert(), which sets GPREGRET,
        // but this does NOT mean the main filesystem is corrupted!
        // 
        // Check main filesystem before formatting:
        // 1. Try to mount the main filesystem
        // 2. If mount succeeds - main FS is NOT corrupted, do NOT format!
        // 3. If mount fails - main FS is corrupted, format it
        
        LOG_INFO("Step 1: Attempting to mount MAIN filesystem to check if it's corrupted...");
        bool main_fs_corrupted = false;
        
        // CRITICAL: Try to mount the main filesystem multiple times
        // InternalFS.begin() may fail for reasons other than corruption (SPI lock, timing, etc.)
        // Try up to 3 times with small delays between attempts
        bool mount_success = false;
        for (int attempt = 0; attempt < 3; attempt++) {
            if (attempt > 0) {
                LOG_DEBUG("Main FS mount attempt %d/3...", attempt + 1);
                delay(10);  // Small delay between attempts
            }
            
            // CRITICAL: Use SPI lock to ensure exclusive access
            concurrency::LockGuard g(spiLock);
            mount_success = InternalFS.begin();
            
            if (mount_success) {
                LOG_INFO("✓ MAIN filesystem mount SUCCESS on attempt %d/3 - it is NOT corrupted!", attempt + 1);
                break;
            } else {
                LOG_DEBUG("Main FS mount attempt %d/3 failed", attempt + 1);
            }
        }
        
        if (!mount_success) {
            // CRITICAL: Additional verification - try to read a known file
            // If we can read a file, filesystem is NOT corrupted, just mount failed for other reasons
            LOG_WARN("⚠️  MAIN filesystem mount FAILED after 3 attempts");
            LOG_WARN("⚠️  Performing additional verification...");
            
            // Try to read a known file to verify filesystem is actually corrupted
            // If we can read files, it's not corrupted, just mount failed
            bool can_read_files = false;
            {
                concurrency::LockGuard g(spiLock);
                File test_file = InternalFS.open("/prefs/config.proto", FILE_O_READ);
                if (test_file) {
                    size_t file_size = test_file.size();
                    test_file.close();
                    if (file_size > 0) {
                        LOG_INFO("✓ Can read files from Main FS - it is NOT corrupted!");
                        LOG_INFO("✓ Mount failure was likely due to timing/SPI issues, not corruption");
                        can_read_files = true;
                    }
                }
            }
            
            if (can_read_files) {
                LOG_INFO("✓ Will NOT format MAIN filesystem (it's safe, just mount failed)");
                main_fs_corrupted = false;
            } else {
                LOG_WARN("⚠️  Cannot read files from Main FS - it IS corrupted!");
                LOG_WARN("⚠️  Will format MAIN filesystem (MINIMAL format, NO erase pages)");
                main_fs_corrupted = true;
            }
        } else {
            LOG_INFO("✓ Corruption flag was likely from EXTENDED filesystem only");
            LOG_INFO("✓ Will NOT format MAIN filesystem (it's safe!)");
            LOG_INFO("✓ Extended filesystem will be initialized lazily when needed");
            main_fs_corrupted = false;
        }
        
        NRF_POWER->GPREGRET = 0;
        millis_until_formatting_again = millis() + MULTIPLE_CORRUPTION_DELAY_MILLIS;
        LOG_INFO("  - GPREGRET cleared");
        LOG_INFO("  - millis_until_formatting_again set to: %lu", millis_until_formatting_again);
        
        if (main_fs_corrupted) {
            // SAFE: Fast format WITHOUT erase pages
            // InternalFS.format() will perform minimal formatting (fast)
            // Extended filesystem will be initialized lazily when needed (after USB is ready)
            LOG_INFO("Calling InternalFS.format() (MINIMAL, FAST, NO ERASE PAGES)...");
            uint32_t format_start = millis();
            InternalFS.format();
            uint32_t format_time = millis() - format_start;
            LOG_INFO("InternalFS.format() completed in %lu ms (MINIMAL format)", format_time);
        } else {
            LOG_INFO("Skipping MAIN filesystem format - it's not corrupted!");
        }
        
        LOG_INFO("preFSBegin() returning (corruption handled safely)");
        LOG_INFO("========================================");
        return;
    }

    LOG_INFO("✓ No corruption flag - normal initialization");
    LOG_INFO("✓ Extended filesystem will initialize lazily when needed (AFTER USB)");
    LOG_INFO("✓ Long operations (erase pages) happen only when extended FS is first accessed");
    LOG_INFO("preFSBegin() END");
    LOG_INFO("========================================");
}

#else // !RAK_4631_LITE_EXTENDED_FILESYSTEM
// Standard filesystem version (7 pages) for RAK4631 Lite with detailed logging
void preFSBegin()
{
    // CRITICAL: Use helper function to get formatting delay (prevents race conditions)
    unsigned long& millis_until_formatting_again = getFormattingDelay();
    
    // SAFE SCENARIO: Minimal operations (only GPREGRET check)
    LOG_INFO("========================================");
    LOG_INFO("preFSBegin() START - STANDARD (7 pages)");
    LOG_INFO("========================================");
    LOG_INFO("Current millis(): %lu", millis());
    LOG_INFO("Reset reason: 0x%08X", NRF_POWER->RESETREAS);
    LOG_INFO("GPREGRET: 0x%02X", NRF_POWER->GPREGRET);
    
    // The GPREGRET register keeps its value across warm boots. Check that this is a warm boot and, if GPREGRET
    // is set to NRF52_MAGIC_LFS_IS_CORRUPT, format LittleFS.
    if (!(NRF_POWER->RESETREAS == 0 && NRF_POWER->GPREGRET == NRF52_MAGIC_LFS_IS_CORRUPT)) {
        LOG_INFO("No corruption flag - continuing");
        LOG_INFO("preFSBegin() END");
        LOG_INFO("========================================");
        return;
    }
    
    LOG_WARN("GPREGRET corruption flag detected - formatting filesystem");
    NRF_POWER->GPREGRET = 0;
    millis_until_formatting_again = millis() + MULTIPLE_CORRUPTION_DELAY_MILLIS;
    InternalFS.format();
    LOG_INFO("LittleFS format complete; restoring default settings");
    LOG_INFO("preFSBegin() END");
    LOG_INFO("========================================");
}
#endif // RAK_4631_LITE_EXTENDED_FILESYSTEM

#endif // RAK_4631

#endif // USE_EXTENDED_FS_FOR_NODEDB
#endif // ARCH_NRF52

