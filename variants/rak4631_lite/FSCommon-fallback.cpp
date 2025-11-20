/**
 * @file FSCommon-fallback.cpp
 * @brief Extended filesystem logic for RAK4631 Lite variant
 * 
 * This file provides filesystem version checking and automatic formatting
 * when filesystem size changes. Called AFTER filesystem is mounted in fsInit().
 */

#include "FSCommon.h"
#include "configuration.h"
#include "InternalFileSystem.h"
#include "softdevice/nrf_soc.h"
#include "error.h"
#include <Adafruit_LittleFS.h>
#include <cstdio>
#include <cstring>

// Only compile this for RAK4631 Lite variant
#ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM

#if defined(ARCH_NRF52)
namespace
{
// Current filesystem size version: 80 pages (320 KB) - OPTIMIZED for NRF52840!
constexpr uint32_t LFS_SIZE_VERSION = 80; // Number of pages (320 KB - sufficient for 400+ nodes)
constexpr const char* LFS_SIZE_VERSION_FILE = "/.lfs_size_version";
} // namespace

// Check filesystem version AFTER mounting and reformat if needed
void fsInitExtended()
{
    // Check if version file exists and matches current version
    auto versionFile = FSCom.open(LFS_SIZE_VERSION_FILE, FILE_O_READ);
    bool needFormat = false;

    if (!versionFile) {
        // Version file doesn't exist - old filesystem or new format, needs format
        LOG_INFO("LittleFS: size version file not found, formatting for %d pages", LFS_SIZE_VERSION);
        needFormat = true;
    } else {
        // Read version from file
        char versionStr[16] = {0};
        size_t bytesRead = versionFile.readBytes(versionStr, sizeof(versionStr) - 1);
        versionFile.close();

        if (bytesRead > 0) {
            uint32_t savedVersion = strtoul(versionStr, nullptr, 10);
            if (savedVersion != LFS_SIZE_VERSION) {
                LOG_INFO("LittleFS: size changed from %lu to %lu pages, formatting", savedVersion, LFS_SIZE_VERSION);
                needFormat = true;
            }
            // else: version matches, no action needed
        } else {
            LOG_WARN("LittleFS: size version file empty, formatting");
            needFormat = true;
        }
    }

    if (needFormat) {
        // Unmount filesystem first
        FSCom.end();
        
        // CRITICAL: InternalFS.format() only formats what was previously mounted (old size),
        // it does NOT erase new pages. When filesystem size increases, new pages
        // contain garbage data, causing "Bad block" and "No more free space" errors.
        //
        // Solution: Manually erase ALL pages before formatting.
        // This ensures new pages are erased and ready for use.
        
        // Filesystem address and size constants (matching InternalFileSystem.cpp)
        constexpr uint32_t FLASH_NRF52_PAGE_SIZE = 4096;
        #ifdef NRF52840_XXAA
            constexpr uint32_t LFS_FLASH_ADDR = 0x80000;  // After application (linker limited to 0x80000)
        #else
            constexpr uint32_t LFS_FLASH_ADDR = 0x6D000;  // Other NRF52 boards
        #endif
        constexpr uint32_t LFS_FLASH_TOTAL_SIZE = 80 * FLASH_NRF52_PAGE_SIZE;  // 80 pages = 320 KB (144 KB margin to bootloader at 0xF4000)
        
        LOG_INFO("LittleFS: erasing all %d pages before formatting", LFS_SIZE_VERSION);
        
        // Erase all pages in the filesystem region
        uint8_t sd_enabled = 0;
        bool use_async = (sd_softdevice_is_enabled(&sd_enabled) == NRF_SUCCESS && sd_enabled);
        
        // Calculate first and last page numbers
        // CRITICAL: Do not erase bootloader pages! Bootloader starts at 0xF4000 (page 244 for NRF52840)
        constexpr uint32_t first_page = LFS_FLASH_ADDR / FLASH_NRF52_PAGE_SIZE;
        #ifdef NRF52840_XXAA
            constexpr uint32_t BOOTLOADER_ADDR = 0xF4000;  // NRF52840 bootloader starts here
        #else
            constexpr uint32_t BOOTLOADER_ADDR = 0x74000;  // Other NRF52 bootloader address
        #endif
        constexpr uint32_t bootloader_page = BOOTLOADER_ADDR / FLASH_NRF52_PAGE_SIZE;
        constexpr uint32_t calculated_last_page = (LFS_FLASH_ADDR + LFS_FLASH_TOTAL_SIZE - 1) / FLASH_NRF52_PAGE_SIZE;
        
        // Ensure we don't erase bootloader pages
        constexpr uint32_t last_page = (calculated_last_page >= bootloader_page) ? (bootloader_page - 1) : calculated_last_page;
        
        if (calculated_last_page >= bootloader_page) {
            LOG_WARN("LittleFS: WARNING - requested size would overlap bootloader! Limiting to page %lu (max safe: %lu)", last_page, bootloader_page - 1);
        }
        
        LOG_INFO("LittleFS: erasing pages %lu to %lu (total %lu pages, bootloader starts at page %lu)", 
                 first_page, last_page, last_page - first_page + 1, bootloader_page);
        
        for (uint32_t page_number = first_page; page_number <= last_page; page_number++) {
            uint32_t err_code;
            bool erase_success = false;
            
            // Retry if busy (up to 15 attempts)
            for (uint8_t attempt = 0; attempt < 15; attempt++) {
                err_code = sd_flash_page_erase(page_number);
                
                if (err_code == NRF_ERROR_BUSY) {
                    delay(50); // Wait 50ms before retry
                    continue;
                }
                
                if (err_code == NRF_SUCCESS) {
                    // Wait for async operation to complete if SoftDevice is enabled
                    if (use_async) {
                        // Wait for flash operation event
                        uint32_t timeout = 500; // 500ms timeout per page
                        uint32_t start_time = millis();
                        
                        while ((millis() - start_time) < timeout) {
                            // Process all pending events
                            uint32_t evt;
                            bool found_our_event = false;
                            
                            while (sd_evt_get(&evt) == NRF_SUCCESS) {
                                if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) {
                                    erase_success = true;
                                    found_our_event = true;
                                    break;
                                } else if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
                                    LOG_ERROR("LittleFS: flash erase error for page %lu", page_number);
                                    found_our_event = true;
                                    break;
                                }
                                // Continue processing other events (BLE, etc.)
                            }
                            
                            if (found_our_event) {
                                break; // Found our flash operation event
                            }
                            
                            // Wait for next event using sd_app_evt_wait()
                            sd_app_evt_wait(); // This blocks until next SoftDevice event
                        }
                        
                        if (!erase_success && (millis() - start_time) >= timeout) {
                            LOG_ERROR("LittleFS: timeout waiting for erase completion page %lu", page_number);
                        }
                    } else {
                        // If SoftDevice disabled, sd_flash_page_erase is synchronous
                        erase_success = true;
                    }
                    
                    if (erase_success) {
                        break; // Successfully erased
                    }
                } else {
                    // Other error codes - log and retry
                    if (attempt < 5) {
                        LOG_DEBUG("LittleFS: page %lu erase returned %lu (attempt %d), retrying", page_number, err_code, attempt + 1);
                        delay(20);
                    } else {
                        LOG_ERROR("LittleFS: failed to erase page %lu: %lu (attempt %d)", page_number, err_code, attempt + 1);
                    }
                }
            }
            
            if (!erase_success) {
                LOG_ERROR("LittleFS: CRITICAL - failed to erase page %lu after all retries!", page_number);
            }
        }
        
        LOG_INFO("LittleFS: page erase loop completed");
        
        LOG_INFO("LittleFS: all pages erased, formatting filesystem");
        
        // Now format the filesystem - all pages are erased, so format will work correctly
        InternalFS.format();
        
        // Remount with new size configuration
        if (FSBegin()) {
            auto writeFile = FSCom.open(LFS_SIZE_VERSION_FILE, FILE_O_WRITE);
            if (writeFile) {
                char versionStr[16];
                snprintf(versionStr, sizeof(versionStr), "%lu", (unsigned long)LFS_SIZE_VERSION);
                writeFile.write((uint8_t*)versionStr, strlen(versionStr));
                writeFile.close();
                LOG_INFO("LittleFS: formatted and size version %lu saved", LFS_SIZE_VERSION);
            }
        } else {
            LOG_ERROR("LittleFS: failed to mount after format");
        }
    } else {
        // Version matches - filesystem is correct size
        LOG_DEBUG("LittleFS: filesystem version matches (%d pages)", LFS_SIZE_VERSION);
    }
}
#endif

#endif // RAK_4631_LITE_EXTENDED_FILESYSTEM

