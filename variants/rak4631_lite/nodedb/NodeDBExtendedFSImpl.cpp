/**
 * @file NodeDBExtendedFSImpl.cpp
 * @brief Core implementation for extended filesystem support for NodeDB on RAK4631 Lite variant
 * 
 * This file provides the core implementation allowing NodeDB to use a larger filesystem (80 pages, 320 KB)
 * mounted separately from the main filesystem (7 pages, 28 KB).
 * 
 * Main filesystem (7 pages, 0xED000): Used for config, channels, device state
 * Extended filesystem (80 pages, 0x80000): Used for NodeDB (nodes.proto) only
 * 
 * This allows storing 400+ nodes without affecting the main system filesystem.
 */

#include "../../../src/FSCommon.h"
#include "mesh/NodeDB.h"
#include "configuration.h"
#include "softdevice/nrf_soc.h"
#include "error.h"
#include <Arduino.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Forward declaration for nrf52Loop() (for feeding watchdog during flash operations)
#ifdef ARCH_NRF52
extern void nrf52Loop();
#endif

// Include LittleFS headers from STM32 implementation (same API)
// We use direct LittleFS API calls, not Adafruit wrapper
#include "../../../src/platform/stm32wl/littlefs/lfs.h"
#include "../../../src/platform/stm32wl/littlefs/lfs_util.h"
#include <pb_decode.h>
#include <pb_encode.h>

// Include Adafruit LittleFS namespace for FILE_O_READ/FILE_O_WRITE constants
#if defined(ARCH_NRF52)
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
#endif

#ifdef ARCH_NRF52

#ifdef USE_EXTENDED_FS_FOR_NODEDB

namespace ExtendedNodeDBFS
{
    // Extended filesystem configuration (80 pages for NodeDB)
    constexpr uint32_t FLASH_NRF52_PAGE_SIZE = 4096;
    #ifdef NRF52840_XXAA
        constexpr uint32_t EXTENDED_LFS_FLASH_ADDR = 0x80000;  // After application
        constexpr uint32_t BOOTLOADER_ADDR = 0xF4000;  // NRF52840 bootloader
    #else
        constexpr uint32_t EXTENDED_LFS_FLASH_ADDR = 0x6D000;  // Other NRF52 boards
        constexpr uint32_t BOOTLOADER_ADDR = 0x74000;
    #endif
    constexpr uint32_t EXTENDED_LFS_FLASH_TOTAL_SIZE = 80 * FLASH_NRF52_PAGE_SIZE;  // 80 pages = 320 KB
    constexpr uint32_t EXTENDED_LFS_BLOCK_SIZE = 4096;  // 4 KB blocks
    constexpr uint32_t EXTENDED_LFS_BLOCK_COUNT = EXTENDED_LFS_FLASH_TOTAL_SIZE / EXTENDED_LFS_BLOCK_SIZE;  // 80 blocks
    constexpr uint32_t EXTENDED_LFS_LOOKAHEAD = 128;  // Standard lookahead
    
    // LittleFS instance and config for extended filesystem (NodeDB only)
    lfs_t extended_lfs;
    struct lfs_config extended_lfs_cfg;
    
    // Buffers for LittleFS (required for operation)
    uint8_t read_buffer[EXTENDED_LFS_BLOCK_SIZE];
    uint8_t prog_buffer[EXTENDED_LFS_BLOCK_SIZE];
    uint8_t lookahead_buffer[EXTENDED_LFS_LOOKAHEAD / 8];
    
    bool isInitialized = false;
    bool isMounted = false;
    
    // LittleFS read callback
    static int lfs_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
    {
        (void)c;  // Unused parameter
        
        if (!buffer || !size) {
            // CRITICAL: Disable logging during flash operations to prevent buffer corruption
            return LFS_ERR_INVAL;
        }
        
        uint32_t address = EXTENDED_LFS_FLASH_ADDR + (block * EXTENDED_LFS_BLOCK_SIZE + off);
        
        // CRITICAL: Disable logging during flash operations to prevent buffer corruption
        // Read from flash memory
        memcpy(buffer, (void *)address, size);
        
        return LFS_ERR_OK;
    }
    
    // LittleFS program callback
    static int lfs_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
    {
        (void)c;  // Unused parameter
        
        if (!buffer || !size) {
            // CRITICAL: Disable logging during flash operations to prevent buffer corruption
            return LFS_ERR_INVAL;
        }
        
        uint32_t address = EXTENDED_LFS_FLASH_ADDR + (block * EXTENDED_LFS_BLOCK_SIZE + off);
        
        // Check if SoftDevice is enabled
        uint8_t sd_enabled = 0;
        uint32_t sd_result = sd_softdevice_is_enabled(&sd_enabled);
        bool use_async = (sd_result == NRF_SUCCESS && sd_enabled);
        
        // CRITICAL: Disable logging during flash operations to prevent buffer corruption
        
        uint32_t write_start_time = millis();
        uint32_t words_written = 0;
        uint32_t words_total = (size + 3) / 4;  // Round up to word count
        
        // CRITICAL OPTIMIZATION: Use batch writing instead of word-by-word
        // sd_flash_write can write up to 1024 words (one page) at once
        // This reduces operations from 1024 calls to 16-32 calls, improving speed by 32-64x!
        // NOTE: Smaller batch size (32 words) reduces interrupt blocking time from 64-128ms to 32-64ms
        // This is CRITICAL for LoRa packet reception - LoRa packets take 10-100ms to receive
        // Blocking interrupts for >50ms can cause packet loss, so we use 32 words (128 bytes) batches
        constexpr uint32_t MAX_WORDS_PER_WRITE = 1024;  // Maximum words per page (4096 bytes / 4)
        constexpr uint32_t OPTIMAL_BATCH_SIZE = 32;     // Optimal batch size (128 bytes) - balance between speed and LoRa packet reception
        
        // Program flash memory using SoftDevice API with batch writing
        uint32_t i = 0;
        while (i < size) {
            // Calculate how many words we can write in this batch
            uint32_t remaining_words = (size - i) / 4;
            uint32_t words_to_write = (remaining_words > OPTIMAL_BATCH_SIZE) ? OPTIMAL_BATCH_SIZE : remaining_words;
            uint32_t batch_size_bytes = words_to_write * 4;
            uint32_t write_addr = address + i;
            
            // Prepare batch buffer (words must be aligned)
            uint32_t* src_words = (uint32_t*)((uint8_t*)buffer + i);
            
            uint32_t err_code;
            // Retry if busy
            for (uint8_t attempt = 0; attempt < 10; attempt++) {
                // CRITICAL: Write multiple words at once instead of one word at a time
                err_code = sd_flash_write((uint32_t *)write_addr, src_words, words_to_write);
                
                if (err_code == NRF_SUCCESS) {
                    if (use_async) {
                        // Wait for async operation to complete
                        // CRITICAL: Increased timeout to 10000ms (10 seconds) - same as lfs_erase
                        // SoftDevice may be very busy with BLE operations (connections, advertising, data transfer)
                        // Physical write takes ~1-2ms per word, but event delivery can be significantly delayed
                        uint32_t timeout = 10000;
                        uint32_t start_time = millis();
                        bool success = false;
                        
                        // First, try to get any pending events immediately
                        uint32_t evt;
                        for (int i = 0; i < 20; i++) {
                            if (sd_evt_get(&evt) == NRF_SUCCESS) {
                                if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) {
                                    success = true;
                                    break;
                                } else if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
                                    LOG_ERROR("Flash operation ERROR event at address 0x%08X", write_addr);
                                    return LFS_ERR_IO;
                                }
                            } else {
                                break;  // No more events available
                            }
                        }
                        
                        // If not immediately successful, wait with polling
                        if (!success) {
                            // OPTIMIZATION: Check flash REPEATEDLY every 2-5ms since physical write takes only 1-2ms
                            // This reduces write time from 1000ms per word to 2-5ms per word
                            // Increased timeout to 1000ms (1 second) for better reliability when SoftDevice is very busy
                            uint32_t poll_timeout = 1000;  // Maximum timeout for polling events
                            uint32_t flash_check_delay = 2;  // First check after 2ms (physical write is 1-2ms)
                            uint32_t flash_check_interval = 3;  // Then check every 3ms (total 5ms between checks)
                            uint32_t last_flash_check = 0;  // Time of last flash check
                            
                            while ((millis() - start_time) < poll_timeout) {
                                uint32_t elapsed = millis() - start_time;
                                
                                // Check for events multiple times per loop (improved polling)
                                for (int i = 0; i < 20; i++) {
                                    if (sd_evt_get(&evt) == NRF_SUCCESS) {
                                        if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) {
                                            success = true;
                                            break;
                                        } else if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
                                            LOG_ERROR("Flash operation ERROR event at address 0x%08X", write_addr);
                                            return LFS_ERR_IO;
                                        }
                                    } else {
                                        break;  // No more events available
                                    }
                                }
                                if (success) break;
                                
                                // CRITICAL FIX: Check flash REPEATEDLY, not just once!
                                // First check after 2ms, then every 3ms (total 5ms interval)
                                // This allows detecting write completion immediately when it happens
                                uint32_t time_since_last_check = elapsed - last_flash_check;
                                bool should_check_flash = false;
                                
                                if (last_flash_check == 0 && elapsed >= flash_check_delay) {
                                    // First check after initial delay
                                    should_check_flash = true;
                                } else if (last_flash_check > 0 && time_since_last_check >= flash_check_interval) {
                                    // Subsequent checks at regular intervals
                                    should_check_flash = true;
                                }
                                
                                if (should_check_flash) {
                                    last_flash_check = elapsed;
                                    // Verify batch was written by checking first and last words of the batch
                                    // This is faster than checking all words, and sufficient for verification
                                    uint32_t first_written = *(volatile uint32_t*)write_addr;
                                    uint32_t last_written = *(volatile uint32_t*)(write_addr + batch_size_bytes - 4);
                                    uint32_t first_expected = src_words[0];
                                    uint32_t last_expected = src_words[words_to_write - 1];
                                    
                                    if (first_written == first_expected && last_written == last_expected) {
                                        // Batch appears to be written correctly
                                        // Only log if it took more than 5ms (indicates event was lost)
                                        if (elapsed > 5) {
                                            LOG_DEBUG("Flash write event not received for batch at 0x%08X (%u words) after %u ms, but data appears written", 
                                                    write_addr, words_to_write, elapsed);
                                        }
                                        success = true;  // Consider it successful
                                        break;  // Exit polling loop immediately
                                    }
                                    // If data doesn't match, continue polling and check again later
                                }
                                
                            // Yield CPU to allow other tasks and interrupts to be processed
                            // CRITICAL: This allows LoRa interrupts to be processed during flash write wait
                            // CRITICAL: Call nrf52Loop() to process SoftDevice events and feed watchdog
                            // This matches native logic where nrf52Loop() is called from loop()
                            if (!success) {
                                yield();  // Minimal delay to yield CPU
                                #ifdef ARCH_NRF52
                                nrf52Loop();  // Process SoftDevice events and feed watchdog
                                #endif
                            } else {
                                break;  // Success found, exit immediately
                            }
                            }
                        }
                        
                        // Final check: If still not successful after polling, verify flash one more time
                        if (!success) {
                            uint32_t elapsed = millis() - start_time;
                            // Verify batch was written by checking first and last words
                            uint32_t first_written = *(volatile uint32_t*)write_addr;
                            uint32_t last_written = *(volatile uint32_t*)(write_addr + batch_size_bytes - 4);
                            uint32_t first_expected = src_words[0];
                            uint32_t last_expected = src_words[words_to_write - 1];
                            
                            if (first_written == first_expected && last_written == last_expected) {
                                LOG_WARN("Flash write event not received for batch at 0x%08X (%u words) after %u ms, but data appears written", 
                                        write_addr, words_to_write, elapsed);
                                LOG_WARN("Continuing - SoftDevice event may have been lost, but write completed");
                                success = true;  // Consider it successful
                            } else {
                                LOG_ERROR("Flash write timeout AND data mismatch at address 0x%08X (first: read 0x%08X, expected 0x%08X, waited %u ms)", 
                                         write_addr, first_written, first_expected, elapsed);
                                LOG_ERROR("This indicates write operation failed or was blocked");
                                return LFS_ERR_IO;
                            }
                        }
                    }
                    // Batch written successfully - advance by batch size
                    words_written += words_to_write;
                    i += batch_size_bytes;  // Move to next batch
                    
                    // CRITICAL: Add small pause between batches to allow LoRa interrupts to be processed
                    // This gives radio time to receive packets and put them in queue
                    // Without this pause, continuous flash writes can block all interrupts for too long
                    if (i < size) {  // Only pause if there are more batches to write
                        // Yield CPU multiple times to ensure LoRa interrupts are processed
                        // LoRa packets take 10-100ms to receive, so we need to give interrupts time
                        for (int y = 0; y < 3; y++) {
                            yield();  // Yield CPU to allow LoRa interrupts and other tasks to be processed
                        }
                        // Small delay to ensure pending interrupts are processed
                        delay(1);  // 1ms pause - allows interrupt processing without significant delay
                    }
                    
                    // CRITICAL: Disable logging during flash operations to prevent buffer corruption
                    break;
                } else if (err_code == NRF_ERROR_BUSY) {
                    // CRITICAL: Disable logging during flash operations to prevent buffer corruption
                    delay(50);
                    continue;
                } else {
                    // Log specific error codes for debugging
                    if (err_code == NRF_ERROR_FORBIDDEN) {  // 0x0000000F
                        LOG_ERROR("========================================");
                        LOG_ERROR("CRITICAL: SoftDevice FORBIDDEN write!");
                        LOG_ERROR("========================================");
                        LOG_ERROR("Address: 0x%08X (block %u, offset %u)", write_addr, block, off);
                        LOG_ERROR("Base address: 0x%08X", address);
                        LOG_ERROR("Extended FS start: 0x%08X", EXTENDED_LFS_FLASH_ADDR);
                        LOG_ERROR("Error: NRF_ERROR_FORBIDDEN (0x%08X)", err_code);
                        LOG_ERROR("");
                        LOG_ERROR("PROBLEM: SoftDevice is blocking writes to this address!");
                        LOG_ERROR("Address 0x%08X is outside SoftDevice's allowed application flash area", write_addr);
                        LOG_ERROR("");
                        LOG_ERROR("SOLUTION: Need to use direct NVMC access instead of SoftDevice API");
                        LOG_ERROR("OR: Configure SoftDevice to allow writes to this address range");
                        LOG_ERROR("========================================");
                    } else if (err_code == NRF_ERROR_INVALID_ADDR) {
                        LOG_ERROR("sd_flash_write INVALID_ADDR at address 0x%08X (error: 0x%08X)", write_addr, err_code);
                    } else {
                        LOG_ERROR("sd_flash_write failed at address 0x%08X with error: 0x%08X", write_addr, err_code);
                    }
                    return LFS_ERR_IO;
                }
            }
        }
        
        // CRITICAL: Disable logging during flash operations to prevent buffer corruption
        return LFS_ERR_OK;
    }
    
    // LittleFS erase callback
    static int lfs_erase(const struct lfs_config *c, lfs_block_t block)
    {
        (void)c;  // Unused parameter
        
        uint32_t page_number = (EXTENDED_LFS_FLASH_ADDR + (block * EXTENDED_LFS_BLOCK_SIZE)) / FLASH_NRF52_PAGE_SIZE;
        uint32_t page_addr = page_number * FLASH_NRF52_PAGE_SIZE;
        
        // CRITICAL: Disable logging during flash operations to prevent buffer corruption
        
        // Safety check: ensure we don't erase bootloader
        #ifdef NRF52840_XXAA
        if (page_number >= (BOOTLOADER_ADDR / FLASH_NRF52_PAGE_SIZE)) {
            LOG_ERROR("ERROR: Attempted to erase bootloader page %lu!", page_number);
            return LFS_ERR_INVAL;
        }
        #endif
        
        // Check if SoftDevice is enabled
        uint8_t sd_enabled = 0;
        uint32_t sd_result = sd_softdevice_is_enabled(&sd_enabled);
        bool use_async = (sd_result == NRF_SUCCESS && sd_enabled);
        
        // CRITICAL: Disable logging during flash operations to prevent buffer corruption
        
        uint32_t erase_start_time = millis();
        
        // Erase flash page using SoftDevice API
        uint32_t err_code;
        
        // Retry if busy
        for (uint8_t attempt = 0; attempt < 10; attempt++) {
            // CRITICAL: Disable logging during flash operations to prevent buffer corruption
            err_code = sd_flash_page_erase(page_number);
            
            // CRITICAL: Disable logging during flash operations to prevent buffer corruption
            
            if (err_code == NRF_SUCCESS) {
                if (use_async) {
                    // Wait for async operation to complete
                    // CRITICAL: Increased timeout to 10000ms (10 seconds) - SoftDevice may be very busy with BLE
                    // Physical erase takes ~85-100ms per page, but SoftDevice event delivery can be delayed
                    // when BLE is active (connections, advertising, data transfer)
                    uint32_t timeout = 10000;
                    uint32_t start_time = millis();
                    bool success = false;
                    
                    // First, try to get any pending events immediately
                    uint32_t evt;
                    for (int i = 0; i < 20; i++) {
                        if (sd_evt_get(&evt) == NRF_SUCCESS) {
                            if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) {
                                success = true;
                                break;
                            } else if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
                                LOG_ERROR("Flash operation ERROR event for page %u", page_number);
                                return LFS_ERR_IO;
                            }
                        } else {
                            break;  // No more events available
                        }
                    }
                    
                    // If not immediately successful, wait with polling
                    if (!success) {
                        // OPTIMIZATION: Check flash REPEATEDLY every 50ms since physical erase takes only 85-100ms
                        // This reduces erase time from 10 seconds to ~100-200ms per page
                        uint32_t flash_check_delay = 100;  // First check after 100ms (physical erase is 85-100ms)
                        uint32_t flash_check_interval = 50;  // Then check every 50ms
                        uint32_t last_flash_check = 0;  // Time of last flash check
                        uint32_t page_addr = page_number * FLASH_NRF52_PAGE_SIZE;
                        
                        while ((millis() - start_time) < timeout) {
                            uint32_t elapsed = millis() - start_time;
                            
                            // Check for events multiple times per loop
                            for (int i = 0; i < 20; i++) {
                                if (sd_evt_get(&evt) == NRF_SUCCESS) {
                                    if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) {
                                        success = true;
                                        break;
                                    } else if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
                                        LOG_ERROR("Flash operation ERROR event for page %u", page_number);
                                        return LFS_ERR_IO;
                                    }
                                } else {
                                    break;  // No more events available
                                }
                            }
                            if (success) break;
                            
                            // CRITICAL OPTIMIZATION: Check flash REPEATEDLY, not just once!
                            // First check after 100ms, then every 50ms (total 150ms interval)
                            // This allows detecting erase completion immediately when it happens
                            uint32_t time_since_last_check = elapsed - last_flash_check;
                            bool should_check_flash = false;
                            
                            if (last_flash_check == 0 && elapsed >= flash_check_delay) {
                                // First check after initial delay
                                should_check_flash = true;
                            } else if (last_flash_check > 0 && time_since_last_check >= flash_check_interval) {
                                // Subsequent checks at regular intervals
                                should_check_flash = true;
                            }
                            
                            if (should_check_flash) {
                                last_flash_check = elapsed;
                                // Verify page is erased by reading first word (erased = 0xFFFFFFFF)
                                uint32_t first_word = *(volatile uint32_t*)page_addr;
                                
                                if (first_word == 0xFFFFFFFF) {
                                    // Page appears to be erased
                                    // Only log if it took more than 150ms (indicates event was lost)
                                    if (elapsed > 150) {
                                        LOG_WARN("Page %u erase event not received, but page appears erased (0xFFFFFFFF at 0x%08X)", 
                                                page_number, page_addr);
                                        LOG_WARN("Continuing - SoftDevice event may have been lost, but erase completed");
                                    }
                                    success = true;  // Consider it successful
                                    break;  // Exit polling loop immediately
                                }
                                // If page not erased, continue polling and check again later
                            }
                            
                            // Yield CPU to allow other tasks and interrupts to be processed
                            // CRITICAL: Call nrf52Loop() to process SoftDevice events and feed watchdog
                            // This matches native logic where nrf52Loop() is called from loop()
                            if (!success) {
                                yield();  // Minimal delay to yield CPU
                                #ifdef ARCH_NRF52
                                nrf52Loop();  // Process SoftDevice events and feed watchdog
                                #endif
                            } else {
                                break;  // Success found, exit immediately
                            }
                        }
                    }
                    
                    // Final check: If still not successful after polling, verify flash one more time
                    if (!success) {
                        uint32_t elapsed = millis() - start_time;
                        uint32_t page_addr = page_number * FLASH_NRF52_PAGE_SIZE;
                        // Verify page is erased by reading first word
                        uint32_t first_word = *(volatile uint32_t*)page_addr;
                        
                        if (first_word == 0xFFFFFFFF) {
                            LOG_WARN("Page %u erase event not received after %u ms, but page appears erased (0xFFFFFFFF at 0x%08X)", 
                                    page_number, elapsed, page_addr);
                            LOG_WARN("Continuing - SoftDevice event may have been lost, but erase completed");
                            success = true;  // Consider it successful
                        } else {
                            LOG_ERROR("Page %u erase timeout AND page not erased (read 0x%08X at 0x%08X, expected 0xFFFFFFFF, waited %u ms)", 
                                     page_number, first_word, page_addr, elapsed);
                            LOG_ERROR("This indicates erase operation failed or was blocked");
                            return LFS_ERR_IO;
                        }
                    }
                } else {
                    // SoftDevice not enabled - operation is synchronous, no event needed
                    // Just verify the operation completed by checking if we can proceed
                    // Note: According to SoftDevice docs, if SD is disabled, operation completes immediately
                    // CRITICAL: Disable logging during flash operations to prevent buffer corruption
                    return LFS_ERR_OK;
                }
                // CRITICAL: Disable logging during flash operations to prevent buffer corruption
                return LFS_ERR_OK;
            } else if (err_code == NRF_ERROR_BUSY) {
                delay(50);
                continue;
            } else {
                // Log specific error codes for debugging
                if (err_code == NRF_ERROR_FORBIDDEN) {
                    LOG_ERROR("========================================");
                    LOG_ERROR("CRITICAL: SoftDevice FORBIDDEN erase!");
                    LOG_ERROR("========================================");
                    LOG_ERROR("Page number: %u (address: 0x%08X)", page_number, page_number * FLASH_NRF52_PAGE_SIZE);
                    LOG_ERROR("Extended FS start: 0x%08X", EXTENDED_LFS_FLASH_ADDR);
                    LOG_ERROR("Error: NRF_ERROR_FORBIDDEN (0x%08X)", err_code);
                    LOG_ERROR("");
                    LOG_ERROR("PROBLEM: SoftDevice is blocking erase of this page!");
                    LOG_ERROR("Page %u (0x%08X) is outside SoftDevice's allowed application flash area", 
                             page_number, page_number * FLASH_NRF52_PAGE_SIZE);
                    LOG_ERROR("");
                    LOG_ERROR("SOLUTION: Need to use direct NVMC access instead of SoftDevice API");
                    LOG_ERROR("OR: Configure SoftDevice to allow erase/write to this address range");
                    LOG_ERROR("========================================");
                } else if (err_code == NRF_ERROR_INVALID_ADDR) {
                    LOG_ERROR("sd_flash_page_erase INVALID_ADDR for page %u (error: 0x%08X)", page_number, err_code);
                } else {
                    LOG_ERROR("sd_flash_page_erase failed for page %u with error: 0x%08X", page_number, err_code);
                }
                return LFS_ERR_IO;
            }
        }
        
        LOG_ERROR("sd_flash_page_erase failed for page %u after all retries", page_number);
        return LFS_ERR_IO;
    }
    
    // LittleFS sync callback (no-op for flash)
    static int lfs_sync(const struct lfs_config *c)
    {
        (void)c;  // Unused parameter
        return LFS_ERR_OK;
    }
    
    /**
     * @brief Initialize and mount extended filesystem for NodeDB
     * @return true if successful, false otherwise
     */
    bool init()
    {
        if (isInitialized) {
            return isMounted;
        }
        
        isInitialized = true;
        
        LOG_INFO("========================================");
        LOG_INFO("INITIALIZING EXTENDED FILESYSTEM FOR NODEDB");
        LOG_INFO("========================================");
        LOG_INFO("Configuration:");
        LOG_INFO("  - Address: 0x%08X (page %u)", EXTENDED_LFS_FLASH_ADDR, EXTENDED_LFS_FLASH_ADDR / FLASH_NRF52_PAGE_SIZE);
        LOG_INFO("  - Size: %u bytes (%u KB, %u pages)", EXTENDED_LFS_FLASH_TOTAL_SIZE, EXTENDED_LFS_FLASH_TOTAL_SIZE / 1024, EXTENDED_LFS_FLASH_TOTAL_SIZE / FLASH_NRF52_PAGE_SIZE);
        LOG_INFO("  - Block size: %u bytes", EXTENDED_LFS_BLOCK_SIZE);
        LOG_INFO("  - Block count: %u blocks", EXTENDED_LFS_BLOCK_COUNT);
        LOG_INFO("  - Lookahead: %u", EXTENDED_LFS_LOOKAHEAD);
        LOG_INFO("  - End address: 0x%08X (page %u)", 
                 EXTENDED_LFS_FLASH_ADDR + EXTENDED_LFS_FLASH_TOTAL_SIZE - 1, 
                 (EXTENDED_LFS_FLASH_ADDR + EXTENDED_LFS_FLASH_TOTAL_SIZE - 1) / FLASH_NRF52_PAGE_SIZE);
        LOG_INFO("  - Bootloader start: 0x%08X (page %u)", BOOTLOADER_ADDR, BOOTLOADER_ADDR / FLASH_NRF52_PAGE_SIZE);
        
        // Safety check
        uint32_t ext_fs_end = EXTENDED_LFS_FLASH_ADDR + EXTENDED_LFS_FLASH_TOTAL_SIZE - 1;
        if (ext_fs_end >= BOOTLOADER_ADDR) {
            LOG_ERROR("ERROR: Extended filesystem would overlap bootloader!");
            LOG_ERROR("  - Extended FS end: 0x%08X (page %u)", ext_fs_end, ext_fs_end / FLASH_NRF52_PAGE_SIZE);
            LOG_ERROR("  - Bootloader start: 0x%08X (page %u)", BOOTLOADER_ADDR, BOOTLOADER_ADDR / FLASH_NRF52_PAGE_SIZE);
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
        
        extended_lfs_cfg.read_size = EXTENDED_LFS_BLOCK_SIZE;
        extended_lfs_cfg.prog_size = EXTENDED_LFS_BLOCK_SIZE;
        extended_lfs_cfg.block_size = EXTENDED_LFS_BLOCK_SIZE;
        extended_lfs_cfg.block_count = EXTENDED_LFS_BLOCK_COUNT;
        extended_lfs_cfg.lookahead = EXTENDED_LFS_LOOKAHEAD;
        
        extended_lfs_cfg.read_buffer = read_buffer;
        extended_lfs_cfg.prog_buffer = prog_buffer;
        extended_lfs_cfg.lookahead_buffer = lookahead_buffer;
        
        // CRITICAL: Before formatting, erase all pages in the filesystem region
        // LittleFS format only formats previously used blocks, not newly allocated ones.
        // When filesystem size increases, new pages contain garbage data, causing "Bad block" errors.
        // Helper function to erase all pages in extended filesystem region
        auto eraseExtendedFSPages = [&]() -> bool {
            LOG_INFO("========================================");
            LOG_INFO("ERASING EXTENDED FILESYSTEM PAGES");
            LOG_INFO("========================================");
            LOG_INFO("START: Erasing all pages in extended filesystem region before format...");
            uint32_t erase_start_time = millis();
            
            // Calculate first and last page numbers with BOOTLOADER PROTECTION!
            constexpr uint32_t first_page = EXTENDED_LFS_FLASH_ADDR / FLASH_NRF52_PAGE_SIZE;
            constexpr uint32_t bootloader_page = BOOTLOADER_ADDR / FLASH_NRF52_PAGE_SIZE;
            constexpr uint32_t calculated_last_page = (EXTENDED_LFS_FLASH_ADDR + EXTENDED_LFS_FLASH_TOTAL_SIZE - 1) / FLASH_NRF52_PAGE_SIZE;
            
            // CRITICAL: Ensure we NEVER erase bootloader pages!
            constexpr uint32_t last_page = (calculated_last_page >= bootloader_page) ? (bootloader_page - 1) : calculated_last_page;
            
            if (calculated_last_page >= bootloader_page) {
                LOG_ERROR("ERROR: Requested size would overlap bootloader!");
                LOG_ERROR("  - Calculated last page: %u", calculated_last_page);
                LOG_ERROR("  - Bootloader starts at page: %u", bootloader_page);
                return false;
            }
            
            uint32_t total_pages = last_page - first_page + 1;
            LOG_INFO("Configuration:");
            LOG_INFO("  - First page: %u (address: 0x%08X)", first_page, first_page * FLASH_NRF52_PAGE_SIZE);
            LOG_INFO("  - Last page: %u (address: 0x%08X)", last_page, (last_page + 1) * FLASH_NRF52_PAGE_SIZE - 1);
            LOG_INFO("  - Total pages to erase: %u", total_pages);
            LOG_INFO("  - Total size: %u KB (%u pages × 4 KB)", total_pages * 4, total_pages);
            LOG_INFO("  - Bootloader protection: page %u (safe)", bootloader_page);
            
            // Check if SoftDevice is enabled
            uint8_t sd_enabled = 0;
            uint32_t sd_result = sd_softdevice_is_enabled(&sd_enabled);
            bool use_async = (sd_result == NRF_SUCCESS && sd_enabled);
            
            LOG_INFO("SoftDevice status:");
            LOG_INFO("  - Enabled: %s", use_async ? "YES (async operations)" : "NO (sync operations)");
            
            uint32_t pages_erased = 0;
            uint32_t pages_failed = 0;
            uint32_t total_attempts = 0;
            
            LOG_INFO("Starting page erase loop...");
            
            for (uint32_t page_number = first_page; page_number <= last_page; page_number++) {
                uint32_t page_start_time = millis();
                uint32_t err_code;
                bool erase_success = false;
                uint32_t page_attempts = 0;
                
                // CRITICAL: Disable logging during flash operations to prevent buffer corruption
                // Progress logging removed to prevent log buffer corruption
                
                // Retry if busy (up to 10 attempts)
                for (uint8_t attempt = 0; attempt < 10; attempt++) {
                    total_attempts++;
                    page_attempts++;
                    
                    err_code = sd_flash_page_erase(page_number);
                    
                    if (err_code == NRF_ERROR_BUSY) {
                        if (attempt < 3) {
                            delay(50);
                        } else {
                            delay(100);  // Longer delay after multiple busy responses
                        }
                        continue;
                    }
                    
                    if (err_code == NRF_SUCCESS) {
                        // Wait for async operation to complete if SoftDevice is enabled
                        if (use_async) {
                            // CRITICAL: Increased timeout to 10000ms (10 seconds) - same as lfs_erase
                            // SoftDevice may be very busy with BLE operations (connections, advertising, data transfer)
                            // Physical erase takes ~85-100ms per page, but event delivery can be significantly delayed
                            uint32_t timeout = 10000;
                            uint32_t wait_start = millis();
                            bool got_success = false;
                            
                            // First, try to get any pending events immediately
                            uint32_t evt;
                            for (int i = 0; i < 20; i++) {
                                if (sd_evt_get(&evt) == NRF_SUCCESS) {
                                    if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) {
                                        erase_success = true;
                                        got_success = true;
                                        break;
                                    } else if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
                                        LOG_ERROR("Flash operation ERROR event for page %u", page_number);
                                        break;
                                    }
                                } else {
                                    break;  // No more events available
                                }
                            }
                            
                            // If not immediately successful, wait with polling
                            if (!got_success) {
                                while ((millis() - wait_start) < timeout) {
                                    // Check for events multiple times per loop
                                    for (int i = 0; i < 20; i++) {
                                        if (sd_evt_get(&evt) == NRF_SUCCESS) {
                                            if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) {
                                                erase_success = true;
                                                got_success = true;
                                                break;
                                            } else if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
                                                LOG_ERROR("Flash operation ERROR event for page %u", page_number);
                                                break;
                                            }
                                        } else {
                                            break;  // No more events available
                                        }
                                    }
                                    if (got_success) break;
                                    yield();  // Yield CPU to allow other tasks and interrupts to be processed
                                }
                            }
                            
                            if (!erase_success && (millis() - wait_start) >= timeout) {
                                LOG_ERROR("Timeout waiting for page %u erase completion (waited %u ms)", 
                                         page_number, (millis() - wait_start));
                            }
                        } else {
                            // SoftDevice not enabled - operation is synchronous
                            erase_success = true;
                        }
                        
                        if (erase_success) {
                            pages_erased++;
                            // CRITICAL: Disable logging during flash operations to prevent buffer corruption
                            break;
                        }
                    } else {
                        if (attempt < 5) {
                            delay(20);
                        } else {
                            LOG_ERROR("Page %u erase failed (error: 0x%08lX, attempt %u)", 
                                     page_number, err_code, attempt + 1);
                            break;
                        }
                    }
                }
                
                if (!erase_success) {
                    pages_failed++;
                    uint32_t page_time = millis() - page_start_time;
                    LOG_ERROR("FAILED: Page %u erase failed after %u attempts (time: %u ms)", 
                             page_number, page_attempts, page_time);
                }
            }
            
            uint32_t total_erase_time = millis() - erase_start_time;
            
            LOG_INFO("========================================");
            LOG_INFO("ERASE SUMMARY");
            LOG_INFO("========================================");
            LOG_INFO("  - Total pages: %u", total_pages);
            LOG_INFO("  - Pages erased successfully: %u", pages_erased);
            LOG_INFO("  - Pages failed: %u", pages_failed);
            LOG_INFO("  - Total attempts: %u", total_attempts);
            // Avoid float formatting to prevent log buffer corruption
            LOG_INFO("  - Total time: %u ms (%u.%02u seconds)", total_erase_time, total_erase_time / 1000, (total_erase_time % 1000) / 10);
            uint32_t avg_ms = total_pages > 0 ? total_erase_time / total_pages : 0;
            LOG_INFO("  - Average time per page: %u ms", avg_ms);
            
            if (pages_failed == 0) {
                LOG_INFO("STATUS: SUCCESS - All pages erased successfully!");
                LOG_INFO("========================================");
                return true;
            } else {
                LOG_ERROR("STATUS: FAILED - %u pages failed to erase!", pages_failed);
                LOG_ERROR("Filesystem may be corrupted - reformat may fail!");
                LOG_INFO("========================================");
                return false;
            }
        };
        
        // Version file to track filesystem format
        constexpr const char* VERSION_FILE = "/.extended_fs_version";
        constexpr uint32_t EXPECTED_VERSION = 1;  // Version 1 = 80 pages extended filesystem
        
        // Try to mount filesystem
        LOG_INFO("Attempting to mount extended filesystem...");
        int mount_result = lfs_mount(&extended_lfs, &extended_lfs_cfg);
        
        bool need_reformat = false;
        
        if (mount_result != LFS_ERR_OK) {
            LOG_WARN("Extended filesystem mount failed (error: %d) - will erase pages and format...", mount_result);
            need_reformat = true;
        } else {
            // Mount succeeded - check version file to ensure filesystem is valid
            LOG_INFO("Mount succeeded - checking version file...");
            lfs_file_t version_file;
            int open_result = lfs_file_open(&extended_lfs, &version_file, VERSION_FILE, LFS_O_RDONLY);
            
            if (open_result != LFS_ERR_OK) {
                LOG_WARN("Version file not found - filesystem may be corrupted or old format, will reformat...");
                need_reformat = true;
            } else {
                // Read version
                char version_str[16] = {0};
                lfs_ssize_t read_result = lfs_file_read(&extended_lfs, &version_file, version_str, sizeof(version_str) - 1);
                lfs_file_close(&extended_lfs, &version_file);
                
                if (read_result > 0) {
                    uint32_t saved_version = strtoul(version_str, nullptr, 10);
                    LOG_INFO("Version file found: version %lu (expected: %u)", saved_version, EXPECTED_VERSION);
                    
                    if (saved_version != EXPECTED_VERSION) {
                        LOG_WARN("Version mismatch (%lu != %u) - will reformat...", saved_version, EXPECTED_VERSION);
                        need_reformat = true;
                    } else {
                        LOG_INFO("Version matches - filesystem is valid");
                    }
                } else {
                    LOG_WARN("Version file is empty - will reformat...");
                    need_reformat = true;
                }
            }
        }
        
        if (need_reformat) {
            // Unmount if mounted
            if (mount_result == LFS_ERR_OK) {
                LOG_INFO("Unmounting filesystem before reformat...");
                lfs_unmount(&extended_lfs);
            }
            
            LOG_WARN("========================================");
            LOG_WARN("REFORMATTING EXTENDED FILESYSTEM");
            LOG_WARN("========================================");
            LOG_WARN("Reason: %s", 
                     mount_result != LFS_ERR_OK ? "Mount failed" :
                     "Version mismatch or missing version file");
            
            // CRITICAL: Erase all pages before formatting to avoid "Bad block" errors
            bool erase_success = eraseExtendedFSPages();
            
            if (!erase_success) {
                LOG_ERROR("ERASE FAILED - Cannot proceed with format!");
                return false;
            }
            
            // Format filesystem
            int format_result = lfs_format(&extended_lfs, &extended_lfs_cfg);
            
            if (format_result != LFS_ERR_OK) {
                LOG_ERROR("FORMAT FAILED! Error code: %d", format_result);
                return false;
            }
            
            // Mount again after format
            mount_result = lfs_mount(&extended_lfs, &extended_lfs_cfg);
            
            if (mount_result != LFS_ERR_OK) {
                LOG_ERROR("MOUNT FAILED AFTER FORMAT! Error code: %d", mount_result);
                return false;
            }
            
            // Create version file
            lfs_file_t version_file;
            int create_result = lfs_file_open(&extended_lfs, &version_file, VERSION_FILE, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
            
            if (create_result == LFS_ERR_OK) {
                char version_str[16];
                snprintf(version_str, sizeof(version_str), "%u", EXPECTED_VERSION);
                lfs_ssize_t write_result = lfs_file_write(&extended_lfs, &version_file, version_str, strlen(version_str));
                lfs_file_close(&extended_lfs, &version_file);
                
                if (write_result <= 0) {
                    LOG_WARN("Failed to write version file");
                }
            } else {
                LOG_WARN("Failed to create version file (error: %d)", create_result);
            }
            
            LOG_INFO("REFORMAT COMPLETED SUCCESSFULLY");
        }
        
        isMounted = true;
        LOG_INFO("SUCCESS: Extended filesystem mounted for NodeDB!");
        LOG_INFO("  - Total capacity: %u KB (%u pages)", EXTENDED_LFS_FLASH_TOTAL_SIZE / 1024, EXTENDED_LFS_FLASH_TOTAL_SIZE / FLASH_NRF52_PAGE_SIZE);
        LOG_INFO("  - Block count: %u blocks", EXTENDED_LFS_BLOCK_COUNT);
        LOG_INFO("========================================");
        
        return true;
    }
    
    /**
     * @brief Check if extended filesystem is available for NodeDB
     */
    bool isAvailable()
    {
        return isMounted;
    }
    
    /**
     * @brief Get the LittleFS instance for extended filesystem
     */
    lfs_t* getLFS()
    {
        return isMounted ? &extended_lfs : nullptr;
    }
    
    /**
     * @brief Unmount extended filesystem (if needed)
     */
    void unmount()
    {
        if (isMounted) {
            lfs_unmount(&extended_lfs);
            isMounted = false;
        }
    }
    
    /**
     * @brief Force reformat extended filesystem (for corruption recovery)
     * @return true if reformat was successful
     */
    bool forceReformat()
    {
        uint32_t force_reformat_start = millis();
        
        LOG_WARN("========================================");
        LOG_WARN("FORCING REFORMAT OF EXTENDED FILESYSTEM");
        LOG_WARN("========================================");
        LOG_WARN("Reason: Filesystem corruption detected");
        LOG_WARN("Timestamp: %u ms since boot", force_reformat_start);
        
        // Unmount if mounted
        if (isMounted) {
            lfs_unmount(&extended_lfs);
            isMounted = false;
        }
        
        // Reset initialization flag
        isInitialized = false;
        
        // Force reformat by directly calling the reformat logic
        // (don't use init() because it checks version and may skip reformat)
        
        // Helper function to erase all pages (same as in init())
        auto eraseExtendedFSPages = [&]() -> bool {
            LOG_INFO("Erasing all pages in extended filesystem region before format...");
            
            constexpr uint32_t first_page = EXTENDED_LFS_FLASH_ADDR / FLASH_NRF52_PAGE_SIZE;
            constexpr uint32_t bootloader_page = BOOTLOADER_ADDR / FLASH_NRF52_PAGE_SIZE;
            constexpr uint32_t calculated_last_page = (EXTENDED_LFS_FLASH_ADDR + EXTENDED_LFS_FLASH_TOTAL_SIZE - 1) / FLASH_NRF52_PAGE_SIZE;
            constexpr uint32_t last_page = (calculated_last_page >= bootloader_page) ? (bootloader_page - 1) : calculated_last_page;
            
            if (calculated_last_page >= bootloader_page) {
                LOG_ERROR("ERROR: Requested size would overlap bootloader!");
                return false;
            }
            
            uint8_t sd_enabled = 0;
            uint32_t sd_result = sd_softdevice_is_enabled(&sd_enabled);
            bool use_async = (sd_result == NRF_SUCCESS && sd_enabled);
            
            uint32_t pages_erased = 0;
            uint32_t pages_failed = 0;
            
            for (uint32_t page_number = first_page; page_number <= last_page; page_number++) {
                uint32_t err_code;
                bool erase_success = false;
                
                for (uint8_t attempt = 0; attempt < 10; attempt++) {
                    err_code = sd_flash_page_erase(page_number);
                    
                    if (err_code == NRF_ERROR_BUSY) {
                        delay(50);
                        continue;
                    }
                    
                    if (err_code == NRF_SUCCESS) {
                        if (use_async) {
                            uint32_t timeout = 500;
                            uint32_t start_time = millis();
                            
                            while ((millis() - start_time) < timeout) {
                                uint32_t evt;
                                if (sd_evt_get(&evt) == NRF_SUCCESS) {
                                    if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) {
                                        erase_success = true;
                                        break;
                                    } else if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
                                        LOG_ERROR("Flash operation ERROR for page %u", page_number);
                                        break;
                                    }
                                }
                                yield();  // Yield CPU to allow other tasks and interrupts to be processed
                            }
                        } else {
                            erase_success = true;
                        }
                        
                        if (erase_success) {
                            pages_erased++;
                            break;
                        }
                    } else {
                        if (err_code == NRF_ERROR_FORBIDDEN) {
                            LOG_ERROR("CRITICAL: sd_flash_page_erase FORBIDDEN for page %u!", page_number);
                            LOG_ERROR("SoftDevice is blocking erase of this page!");
                            LOG_ERROR("Page address: 0x%08X", page_number * FLASH_NRF52_PAGE_SIZE);
                        } else {
                            LOG_ERROR("Page %u erase failed (error: 0x%08X)", page_number, err_code);
                        }
                        if (attempt < 5) {
                            delay(20);
                        } else {
                            break;
                        }
                    }
                }
                
                if (!erase_success) {
                    // Verify erase by reading first word of page
                    uint32_t page_addr = page_number * FLASH_NRF52_PAGE_SIZE;
                    uint32_t first_word = *(volatile uint32_t*)page_addr;
                    if (first_word == 0xFFFFFFFF) {
                        // Page appears erased even though event wasn't received
                        LOG_WARN("Page %u erase event not received, but page appears erased (0xFFFFFFFF at 0x%08X)", 
                                page_number, page_addr);
                        pages_erased++;  // Count as succeeded
                    } else {
                        LOG_ERROR("Page %u erase FAILED - first word is 0x%08X (expected 0xFFFFFFFF)", 
                                 page_number, first_word);
                        pages_failed++;
                    }
                }
            }
            
            LOG_INFO("Page erase complete: %u succeeded, %u failed", pages_erased, pages_failed);
            if (pages_failed > 0) {
                LOG_ERROR("CRITICAL: %u pages failed to erase - filesystem may be corrupted!", pages_failed);
            }
            return (pages_failed == 0);
        };
        
        // Erase all pages
        if (!eraseExtendedFSPages()) {
            LOG_ERROR("Failed to erase pages before force reformat!");
            return false;
        }
        
        // Format filesystem
        int format_result = lfs_format(&extended_lfs, &extended_lfs_cfg);
        
        if (format_result != LFS_ERR_OK) {
            LOG_ERROR("Extended filesystem format failed (error: %d)!", format_result);
            return false;
        }
        
        // Mount after format
        int mount_result = lfs_mount(&extended_lfs, &extended_lfs_cfg);
        
        if (mount_result != LFS_ERR_OK) {
            LOG_ERROR("Extended filesystem mount failed after format (error: %d)!", mount_result);
            return false;
        }
        
        // Create version file
        constexpr const char* VERSION_FILE = "/.extended_fs_version";
        constexpr uint32_t EXPECTED_VERSION = 1;
        
        lfs_file_t version_file;
        int create_result = lfs_file_open(&extended_lfs, &version_file, VERSION_FILE, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
        
        if (create_result == LFS_ERR_OK) {
            char version_str[16];
            snprintf(version_str, sizeof(version_str), "%u", EXPECTED_VERSION);
            lfs_ssize_t write_result = lfs_file_write(&extended_lfs, &version_file, version_str, strlen(version_str));
            lfs_file_close(&extended_lfs, &version_file);
            
            if (write_result <= 0) {
                LOG_WARN("Failed to write version file");
            }
        } else {
            LOG_WARN("Failed to create version file (error: %d)", create_result);
        }
        
        isMounted = true;
        isInitialized = true;
        
        LOG_INFO("FORCE REFORMAT COMPLETED SUCCESSFULLY");
        
        return true;
    }
}

// Wrapper function to check if extended filesystem should be used for NodeDB
bool useExtendedFSForNodeDB()
{
    // Initialize extended filesystem if not already done
    if (!ExtendedNodeDBFS::isInitialized) {
        ExtendedNodeDBFS::init();
    }
    
    // Return true if extended filesystem is mounted and available
    return ExtendedNodeDBFS::isAvailable();
}

// Get LittleFS instance for extended filesystem (for NodeDB operations)
lfs_t* getExtendedFSForNodeDB()
{
    if (!ExtendedNodeDBFS::isInitialized) {
        ExtendedNodeDBFS::init();
    }
    
    return ExtendedNodeDBFS::getLFS();
}

// Check if a filename is for NodeDB (should use extended filesystem)
bool isNodeDBFile(const char* filename)
{
    if (!filename) return false;
    
    // Check if this is the node database file
    // nodeDatabaseFileName = "/prefs/nodes.proto"
    return (strcmp(filename, "/prefs/nodes.proto") == 0);
}

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

/**
 * @brief Load protobuf from extended filesystem
 * 
 * CRITICAL OPTIMIZATION: This function ignores the protoSize parameter and uses
 * the actual file size instead. This prevents excessive memory allocation when
 * getMaxNodesAllocatedSize() (227KB) is passed but the file is only 3 bytes.
 * 
 * @param filename File path to load
 * @param protoSize Expected size (IGNORED - uses actual file size instead)
 * @param objSize Size of destination object
 * @param fields Protobuf field descriptors
 * @param dest_struct Destination structure pointer
 * @return LoadFileResult indicating success or failure
 */
LoadFileResult loadFromExtendedFS(const char *filename, size_t protoSize, size_t objSize, 
                                  const pb_msgdesc_t *fields, void *dest_struct)
{
    (void)protoSize;  // Ignore protoSize - use actual file size instead to prevent excessive allocation
    LoadFileResult state = LoadFileResult::OTHER_FAILURE;
    
    // Use extended filesystem for nodes.proto
    LOG_INFO("Load %s from EXTENDED filesystem (80 pages, 320 KB)", filename);
    
    // Cast to lfs_t* (headers already included at top of file)
    lfs_t* extended_lfs = getExtendedFSForNodeDB();
    if (extended_lfs) {
        // Read file into buffer first, then decode (more reliable than using readcb wrapper)
        // This avoids compatibility issues with File* vs LittleFSFileWrapper*
        lfs_file_t file;
        int open_result = lfs_file_open(extended_lfs, &file, filename, LFS_O_RDONLY);
        
        if (open_result == LFS_ERR_OK) {
            // Get file size
            lfs_soff_t file_size = lfs_file_size(extended_lfs, &file);
            
            if (file_size >= 0 && file_size <= 512 * 1024) {  // Max 512 KB (safety limit)
                // Allocate buffer for file content
                uint8_t* file_buffer = (uint8_t*)malloc(file_size);
                
                if (file_buffer) {
                    // Read entire file
                    lfs_ssize_t read_result = lfs_file_read(extended_lfs, &file, file_buffer, file_size);
                    lfs_file_close(extended_lfs, &file);
                    
                    if (read_result == file_size) {
                        // Decode from buffer
                        pb_istream_t stream = pb_istream_from_buffer(file_buffer, file_size);
                        memset(dest_struct, 0, objSize);
                        if (!pb_decode(&stream, fields, dest_struct)) {
                            LOG_ERROR("Can't decode protobuf %s: %s", filename, PB_GET_ERROR(&stream));
                            state = LoadFileResult::DECODE_FAILED;
                        } else {
                            LOG_INFO("Loaded %s successfully from EXTENDED filesystem (%d bytes)", 
                                    filename, (int)file_size);
                            state = LoadFileResult::LOAD_SUCCESS;
                        }
                    } else {
                        LOG_ERROR("Read %d of %d bytes from '%s'", 
                                 (int)read_result, (int)file_size, filename);
                        state = LoadFileResult::OTHER_FAILURE;
                    }
                    free(file_buffer);
                } else {
                    LOG_ERROR("Failed to allocate buffer for '%s' (size: %d bytes)", 
                             filename, (int)file_size);
                    lfs_file_close(extended_lfs, &file);
                    state = LoadFileResult::OTHER_FAILURE;
                }
            } else {
                LOG_ERROR("File '%s' size invalid or too large: %d bytes", 
                         filename, (int)file_size);
                lfs_file_close(extended_lfs, &file);
                state = LoadFileResult::OTHER_FAILURE;
            }
        } else {
            // File doesn't exist - normal for first boot, will be created on first save
            state = LoadFileResult::OTHER_FAILURE;  // Return failure so standard code can handle first boot
        }
    } else {
        LOG_ERROR("Extended filesystem not available for %s", filename);
    }
    
    return state;
}

/**
 * @brief Save protobuf to extended filesystem
 */
bool saveToExtendedFS(const char *filename, size_t protoSize, const pb_msgdesc_t *fields, 
                      const void *dest_struct, bool fullAtomic)
{
    (void)fullAtomic;  // Atomic writes handled by LittleFS
    
    bool okay = false;
    
    // Use extended filesystem for nodes.proto (other nodes from network only)
    LOG_INFO("Save %s to EXTENDED filesystem (80 pages, 320 KB) - other nodes only", filename);
    
    // Cast to lfs_t* (headers already included at top of file)
    // Get fresh pointer each time in case filesystem was reformatted
    lfs_t* extended_lfs = getExtendedFSForNodeDB();
    if (!extended_lfs) {
        LOG_WARN("Extended filesystem pointer is null - extended filesystem unavailable");
        // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
        return false;
    }
    
    // CRITICAL: Extended filesystem uses SoftDevice API directly, NOT SPI!
    // Do NOT use writecb here - it uses spiLock for SPI bus (LoRa radio + main filesystem).
    // Use streaming encoding instead: encode directly to file, no buffer allocation needed!
    // This avoids memory fragmentation issues when free heap is fragmented.
    
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
            (void)mkdir_result;  // Ignore errors - directory may already exist
        }
        slash++; // move past '/'
    }
    
    // Remove old file first (for atomic write)
    // This is safer than truncate - avoids issues with block allocation
    lfs_remove(extended_lfs, filename);  // Ignore result - file may not exist
    
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
    
    // Use streaming encoding - no buffer allocation needed!
    // This avoids memory fragmentation issues when free heap is fragmented
    LfsFileContext ctx;
    ctx.lfs = extended_lfs;
    ctx.file = &file;
    
    pb_ostream_t stream = {&lfs_writecb, &ctx, SIZE_MAX, 0};
    
    bool encode_success = pb_encode(&stream, fields, dest_struct);
    
    if (!encode_success) {
        const char* error = PB_GET_ERROR(&stream);
        LOG_ERROR("Protobuf encoding failed for '%s': %s", filename, error ? error : "unknown");
        lfs_file_close(extended_lfs, &file);
        return false;
    }
    
    size_t encoded_size = stream.bytes_written;
    
    // Check if write was successful by checking file size
    lfs_soff_t file_size = lfs_file_size(extended_lfs, &file);
    if (file_size < 0 || (size_t)file_size != encoded_size) {
        LOG_ERROR("File size mismatch for '%s': expected %u bytes, got %d", 
                 filename, (unsigned)encoded_size, (int)file_size);
        lfs_file_close(extended_lfs, &file);
        return false;
    }
    
    // Sync file before closing (CRITICAL for LittleFS)
    // CRITICAL: lfs_file_sync() can take 200+ ms and internally calls lfs_prog()
    // We need to call nrf52Loop() periodically during sync to process SoftDevice events and feed watchdog
    #ifdef ARCH_NRF52
    nrf52Loop();  // Process any pending SoftDevice events before sync
    #endif
    int sync_result = lfs_file_sync(extended_lfs, &file);
    #ifdef ARCH_NRF52
    nrf52Loop();  // Process any pending SoftDevice events after sync
    #endif
    if (sync_result != LFS_ERR_OK) {
        LOG_ERROR("Sync of '%s' failed (error: %d)", filename, sync_result);
        lfs_file_close(extended_lfs, &file);
        return false;
    }
    
    // Close file
    int close_result = lfs_file_close(extended_lfs, &file);
    if (close_result != LFS_ERR_OK) {
        LOG_WARN("Close of '%s' failed (error: %d)", filename, close_result);
        okay = false;
    } else {
        okay = true;
    }
    
    if (okay) {
        LOG_INFO("Saved %s successfully to EXTENDED filesystem (%u bytes)", filename, (unsigned)encoded_size);
        
        // Verify file was saved to extended filesystem and NOT to main filesystem
        bool found_in_extended = false;
        lfs_file_t verify_file;
        int verify_result = lfs_file_open(extended_lfs, &verify_file, filename, LFS_O_RDONLY);
        if (verify_result == LFS_ERR_OK) {
            found_in_extended = true;
            lfs_file_close(extended_lfs, &verify_file);
        }
        
        bool found_in_main = FSCom.exists(filename);
        
        if (found_in_extended && !found_in_main) {
            LOG_INFO("VERIFIED: %s correctly saved to EXTENDED filesystem (80 pages, 320 KB)", filename);
        } else if (found_in_main) {
            LOG_ERROR("VERIFY FAILED: %s was also found in MAIN filesystem - this should not happen!", filename);
            LOG_ERROR("nodes.proto should ONLY be in extended filesystem!");
        } else if (!found_in_extended) {
            LOG_ERROR("VERIFY FAILED: %s not found in EXTENDED filesystem after save!", filename);
        }
        
        return true;
    } else {
        LOG_ERROR("Can't write %s to extended filesystem!", filename);
        // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
        return false;
    }
}

#ifdef USE_EXTENDED_FS_FOR_NODEDB
// C wrapper for initialization (for extern declaration from FSCommon.cpp)
// Use __attribute__((used)) to prevent linker from removing this function with --gc-sections
extern "C" __attribute__((used)) void initExtendedFilesystemForNodeDB()
{
    // Initialize extended filesystem if not already done
    if (!ExtendedNodeDBFS::isInitialized) {
        ExtendedNodeDBFS::init();
    }
}
#endif // USE_EXTENDED_FS_FOR_NODEDB

/**
 * @brief Helper function to recursively calculate directory size in extended filesystem
 */
static int calcDirSizeRecursive(lfs_t* lfs, const char* path, uint32_t* size, int depth)
{
    if (depth > 5 || !lfs || !size) {  // Safety limit
        return LFS_ERR_OK;
    }
    
    lfs_dir_t d;
    int err = lfs_dir_open(lfs, &d, path);
    if (err != LFS_ERR_OK) {
        return err;
    }
    
    struct lfs_info info;
    while (true) {
        err = lfs_dir_read(lfs, &d, &info);
        if (err <= 0) {
            break;
        }
        
        // Skip . and ..
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }
        
        if (info.type == LFS_TYPE_REG) {
            *size += info.size;
        } else if (info.type == LFS_TYPE_DIR) {
            // Build subdirectory path
            char subpath[128];
            if (strcmp(path, "/") == 0) {
                snprintf(subpath, sizeof(subpath), "/%s", info.name);
            } else {
                snprintf(subpath, sizeof(subpath), "%s/%s", path, info.name);
            }
            // Recursively process subdirectories
            calcDirSizeRecursive(lfs, subpath, size, depth + 1);
        }
    }
    
    lfs_dir_close(lfs, &d);
    return LFS_ERR_OK;
}

/**
 * @brief Get extended filesystem statistics for NodeDB
 * @param total Total size in bytes (output)
 * @param used Used size in bytes (output)
 * @param free Free size in bytes (output)
 * @return true if extended filesystem is available and stats are valid
 */
bool getExtendedFSStats(uint32_t* total, uint32_t* used, uint32_t* free)
{
    if (!total || !used || !free) {
        return false;
    }
    
    if (!useExtendedFSForNodeDB()) {
        return false;
    }
    
    // Total size is constant (from ExtendedNodeDBFS namespace)
    *total = ExtendedNodeDBFS::EXTENDED_LFS_FLASH_TOTAL_SIZE;  // 320 KB (80 pages * 4 KB)
    
    // Calculate used space by traversing filesystem and summing file sizes
    lfs_t* lfs = getExtendedFSForNodeDB();
    if (!lfs || !ExtendedNodeDBFS::isMounted) {
        *used = 0;
        *free = *total;
        return false;
    }
    
    *used = 0;
    
    // Calculate used space from root directory
    if (calcDirSizeRecursive(lfs, "/", used, 0) == LFS_ERR_OK) {
        *free = (*total > *used) ? (*total - *used) : 0;
        return true;
    }
    
    // Fallback: just return total if we can't calculate
    *used = 0;
    *free = *total;
    return false;
}

/**
 * @brief Force reformat extended filesystem (for corruption recovery)
 * @return true if reformat was successful
 */
bool forceReformatExtendedFS()
{
    return ExtendedNodeDBFS::forceReformat();
}

/**
 * @brief Wrapper class to use LittleFS file with protobuf readcb/writecb
 * 
 * This class provides a File-like interface for LittleFS files,
 * allowing them to be used with existing protobuf read/write callbacks.
 */
class LittleFSFileWrapper
{
private:
    lfs_t* lfs;
    lfs_file_t file;
    bool isOpen;
    bool isWriteMode;
    
public:
    LittleFSFileWrapper(lfs_t* fs) : lfs(fs), isOpen(false), isWriteMode(false)
    {
        // Initialize lfs_file_t
        memset(&file, 0, sizeof(lfs_file_t));
    }
    
    ~LittleFSFileWrapper()
    {
        if (isOpen) {
            close();
        }
    }
    
    bool open(const char* path, uint8_t mode)
    {
        if (isOpen) {
            close();
        }
        
        int flags = 0;
        if (mode == FILE_O_READ || mode == Adafruit_LittleFS_Namespace::FILE_O_READ) {
            flags = LFS_O_RDONLY;
            isWriteMode = false;
        } else if (mode == FILE_O_WRITE || mode == Adafruit_LittleFS_Namespace::FILE_O_WRITE) {
            flags = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
            isWriteMode = true;
        } else {
            return false;
        }
        
        int result = lfs_file_open(lfs, &file, path, flags);
        if (result == LFS_ERR_OK) {
            isOpen = true;
            return true;
        }
        
        return false;
    }
    
    void close()
    {
        if (isOpen) {
            lfs_file_close(lfs, &file);
            isOpen = false;
        }
    }
    
    // File-like interface for readcb
    int read(uint8_t* buf, size_t size)
    {
        if (!isOpen || isWriteMode) return 0;
        
        lfs_ssize_t result = lfs_file_read(lfs, &file, buf, size);
        return (result >= 0) ? result : 0;
    }
    
    int read()
    {
        if (!isOpen || isWriteMode) return -1;
        
        uint8_t byte;
        lfs_ssize_t result = lfs_file_read(lfs, &file, &byte, 1);
        return (result == 1) ? byte : -1;
    }
    
    // Print-like interface for writecb
    size_t write(uint8_t byte)
    {
        if (!isOpen || !isWriteMode) return 0;
        
        lfs_ssize_t result = lfs_file_write(lfs, &file, &byte, 1);
        return (result == 1) ? 1 : 0;
    }
    
    size_t write(const uint8_t* buf, size_t size)
    {
        if (!isOpen || !isWriteMode) return 0;
        
        lfs_ssize_t result = lfs_file_write(lfs, &file, buf, size);
        return (result >= 0) ? result : 0;
    }
    
    bool available()
    {
        if (!isOpen || isWriteMode) return false;
        
        lfs_soff_t pos = lfs_file_tell(lfs, &file);
        lfs_soff_t size = lfs_file_size(lfs, &file);
        
        return (pos < size);
    }
    
    size_t size()
    {
        if (!isOpen) return 0;
        
        lfs_soff_t fileSize = lfs_file_size(lfs, &file);
        return (fileSize >= 0) ? fileSize : 0;
    }
    
    // Check if file is open
    operator bool() const
    {
        return isOpen;
    }
    
    // Sync file (flush)
    bool sync()
    {
        if (!isOpen) return false;
        
        // CRITICAL: lfs_file_sync() can take 200+ ms and internally calls lfs_prog()
        // We need to call nrf52Loop() periodically during sync to process SoftDevice events and feed watchdog
        #ifdef ARCH_NRF52
        nrf52Loop();  // Process any pending SoftDevice events before sync
        #endif
        int result = lfs_file_sync(lfs, &file);
        #ifdef ARCH_NRF52
        nrf52Loop();  // Process any pending SoftDevice events after sync
        #endif
        return (result == LFS_ERR_OK);
    }
};

#else
// If USE_EXTENDED_FS_FOR_NODEDB is not defined, provide empty functions
bool useExtendedFSForNodeDB()
{
    return false;
}

lfs_t* getExtendedFSForNodeDB()
{
    return nullptr;
}

bool isNodeDBFile(const char* filename)
{
    (void)filename;
    return false;
}

bool getExtendedFSStats(uint32_t* total, uint32_t* used, uint32_t* free)
{
    (void)total;
    (void)used;
    (void)free;
    return false;
}
#endif // USE_EXTENDED_FS_FOR_NODEDB

#else
// If not ARCH_NRF52, provide empty functions
bool useExtendedFSForNodeDB()
{
    return false;
}

void* getExtendedFSForNodeDB()
{
    return nullptr;
}

bool isNodeDBFile(const char* filename)
{
    (void)filename;
    return false;
}

bool getExtendedFSStats(uint32_t* total, uint32_t* used, uint32_t* free)
{
    (void)total;
    (void)used;
    (void)free;
    return false;
}
#endif // ARCH_NRF52
