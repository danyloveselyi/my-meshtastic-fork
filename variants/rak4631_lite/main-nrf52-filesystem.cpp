/**
 * @file main-nrf52-filesystem.cpp
 * @brief Detailed filesystem logging for RAK4631 Lite variant (DEBUG MODE)
 * 
 * This file provides detailed filesystem logging to diagnose startup issues.
 * Extended filesystem support is TEMPORARILY DISABLED for debugging.
 * 
 * Current configuration: Standard 7 pages (28 KB) filesystem
 */

#include "configuration.h"
#include "InternalFileSystem.h"
#include "softdevice/nrf_soc.h"
#include "error.h"
#include "main.h"
#include <Adafruit_LittleFS.h>
#include <cstring>

// Only compile extended filesystem code if enabled
#ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM

namespace
{
// Current filesystem size version: 80 pages (320 KB) - OPTIMIZED for NRF52840!
constexpr uint32_t LFS_SIZE_VERSION = 80; // Number of pages (320 KB - sufficient for 400+ nodes)
constexpr const char* LFS_SIZE_VERSION_FILE = "/.lfs_size_version";
constexpr uint8_t NRF52_MAGIC_LFS_IS_CORRUPT = 0xF5;
constexpr uint32_t MULTIPLE_CORRUPTION_DELAY_MILLIS = 20 * 60 * 1000;
} // namespace

// Helper function to safely erase filesystem pages (protects bootloader)
// MAXIMUM LOGGING: Every step is logged for debugging
static void eraseFilesystemPages()
{
    LOG_INFO("========================================");
    LOG_INFO("eraseFilesystemPages() START - DETAILED LOG");
    LOG_INFO("========================================");
    
    // Filesystem address and size constants (matching InternalFileSystem.cpp)
    constexpr uint32_t FLASH_NRF52_PAGE_SIZE = 4096;
    #ifdef NRF52840_XXAA
        constexpr uint32_t LFS_FLASH_ADDR = 0x80000;  // After application (linker limited to 0x80000)
        constexpr uint32_t BOOTLOADER_ADDR = 0xF4000;  // NRF52840 bootloader starts here
    #else
        constexpr uint32_t LFS_FLASH_ADDR = 0x6D000;  // Other NRF52 boards
        constexpr uint32_t BOOTLOADER_ADDR = 0x74000;  // Other NRF52 bootloader address
    #endif
    constexpr uint32_t LFS_FLASH_TOTAL_SIZE = 80 * FLASH_NRF52_PAGE_SIZE;  // 80 pages = 320 KB
    
    LOG_INFO("Configuration constants:");
    LOG_INFO("  - FLASH_NRF52_PAGE_SIZE: %u bytes (4 KB)", FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - LFS_FLASH_ADDR: 0x%X (%u)", LFS_FLASH_ADDR, LFS_FLASH_ADDR);
    LOG_INFO("  - BOOTLOADER_ADDR: 0x%X (%u)", BOOTLOADER_ADDR, BOOTLOADER_ADDR);
    LOG_INFO("  - LFS_FLASH_TOTAL_SIZE: %u bytes (%u KB)", LFS_FLASH_TOTAL_SIZE, LFS_FLASH_TOTAL_SIZE / 1024);
    LOG_INFO("  - LFS_SIZE_VERSION: %u pages", LFS_SIZE_VERSION);
    
    // Erase all pages in the filesystem region
    uint8_t sd_enabled = 0;
    uint32_t sd_check_result = sd_softdevice_is_enabled(&sd_enabled);
    bool use_async = (sd_check_result == NRF_SUCCESS && sd_enabled);
    
    LOG_INFO("SoftDevice status:");
    LOG_INFO("  - sd_softdevice_is_enabled() result: %lu", sd_check_result);
    LOG_INFO("  - SoftDevice enabled: %u", sd_enabled);
    LOG_INFO("  - Use async flash operations: %s", use_async ? "YES" : "NO");
    
    // Calculate first and last page numbers with BOOTLOADER PROTECTION!
    constexpr uint32_t first_page = LFS_FLASH_ADDR / FLASH_NRF52_PAGE_SIZE;
    constexpr uint32_t bootloader_page = BOOTLOADER_ADDR / FLASH_NRF52_PAGE_SIZE;
    constexpr uint32_t calculated_last_page = (LFS_FLASH_ADDR + LFS_FLASH_TOTAL_SIZE - 1) / FLASH_NRF52_PAGE_SIZE;
    
    LOG_INFO("Page calculations:");
    LOG_INFO("  - first_page: %lu (address: 0x%lX)", first_page, first_page * FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - bootloader_page: %lu (address: 0x%lX)", bootloader_page, bootloader_page * FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - calculated_last_page: %lu (address: 0x%lX)", calculated_last_page, (calculated_last_page + 1) * FLASH_NRF52_PAGE_SIZE);
    
    // CRITICAL: Ensure we NEVER erase bootloader pages!
    constexpr uint32_t last_page = (calculated_last_page >= bootloader_page) ? (bootloader_page - 1) : calculated_last_page;
    
    if (calculated_last_page >= bootloader_page) {
        LOG_ERROR("LittleFS: ERROR - requested size would overlap bootloader!");
        LOG_ERROR("  - Calculated last page: %lu", calculated_last_page);
        LOG_ERROR("  - Bootloader starts at page: %lu", bootloader_page);
        LOG_ERROR("  - Limiting to page: %lu", last_page);
    } else {
        LOG_INFO("  - Overlap check: OK (no overlap with bootloader)");
    }
    
    LOG_INFO("  - Safe last_page: %lu (address: 0x%lX)", last_page, (last_page + 1) * FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Total pages to erase: %lu", last_page - first_page + 1);
    LOG_INFO("  - Total size to erase: %lu bytes (%lu KB)", 
             (last_page - first_page + 1) * FLASH_NRF52_PAGE_SIZE,
             (last_page - first_page + 1) * FLASH_NRF52_PAGE_SIZE / 1024);
    
    LOG_INFO("Starting page erase loop...");
    
    uint32_t pages_erased = 0;
    uint32_t pages_failed = 0;
    uint32_t total_attempts = 0;
    uint32_t total_time_ms = millis();
    
    for (uint32_t page_number = first_page; page_number <= last_page; page_number++) {
        uint32_t page_start_time = millis();
        uint32_t err_code;
        bool erase_success = false;
        
        LOG_INFO("Erasing page %lu/%lu (address: 0x%lX)...", 
                 page_number, last_page, page_number * FLASH_NRF52_PAGE_SIZE);
        
        // Retry if busy (up to 15 attempts)
        for (uint8_t attempt = 0; attempt < 15; attempt++) {
            total_attempts++;
            uint32_t attempt_start = millis();
            
            LOG_DEBUG("  Attempt %d/15: calling sd_flash_page_erase(%lu)", attempt + 1, page_number);
            err_code = sd_flash_page_erase(page_number);
            uint32_t attempt_time = millis() - attempt_start;
            
            LOG_DEBUG("  sd_flash_page_erase() returned: %lu (time: %lu ms)", err_code, attempt_time);
            
            if (err_code == NRF_ERROR_BUSY) {
                LOG_DEBUG("  Page %lu is busy, waiting 50ms...", page_number);
                delay(50);
                continue;
            }
            
            if (err_code == NRF_SUCCESS) {
                LOG_INFO("  Page %lu erase initiated successfully", page_number);
                
                // Wait for async operation to complete if SoftDevice is enabled
                if (use_async) {
                    uint32_t timeout = 500;
                    uint32_t start_time = millis();
                    uint32_t events_processed = 0;
                    
                    LOG_DEBUG("  Waiting for async erase completion (timeout: %lu ms)...", timeout);
                    
                    while ((millis() - start_time) < timeout) {
                        uint32_t evt;
                        bool found_our_event = false;
                        
                        while (sd_evt_get(&evt) == NRF_SUCCESS) {
                            events_processed++;
                            LOG_DEBUG("  SoftDevice event: %lu", evt);
                            
                            if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) {
                                erase_success = true;
                                found_our_event = true;
                                LOG_INFO("  Flash operation SUCCESS event received for page %lu", page_number);
                                break;
                            } else if (evt == NRF_EVT_FLASH_OPERATION_ERROR) {
                                LOG_ERROR("  Flash operation ERROR event received for page %lu", page_number);
                                found_our_event = true;
                                break;
                            }
                        }
                        
                        if (found_our_event) {
                            break;
                        }
                        
                        // Use delay() instead of sd_app_evt_wait() to prevent hanging
                        delay(10); // Wait 10ms and check again
                    }
                    
                    uint32_t wait_time = millis() - start_time;
                    LOG_DEBUG("  Waited %lu ms, processed %lu events", wait_time, events_processed);
                    
                    if (!erase_success && wait_time >= timeout) {
                        LOG_ERROR("  TIMEOUT waiting for erase completion page %lu (waited %lu ms)", 
                                 page_number, wait_time);
                    }
                } else {
                    LOG_DEBUG("  Synchronous erase - no wait needed");
                    erase_success = true;
                }
                
                if (erase_success) {
                    uint32_t page_time = millis() - page_start_time;
                    LOG_INFO("  Page %lu erased successfully in %lu ms", page_number, page_time);
                    pages_erased++;
                    break;
                }
            } else {
                if (attempt < 5) {
                    LOG_DEBUG("  Page %lu erase returned error %lu (attempt %d), retrying in 20ms", 
                             page_number, err_code, attempt + 1);
                    delay(20);
                } else {
                    LOG_ERROR("  Page %lu erase returned error %lu (attempt %d)", 
                             page_number, err_code, attempt + 1);
                }
            }
        }
        
        if (!erase_success) {
            uint32_t page_time = millis() - page_start_time;
            LOG_ERROR("  CRITICAL - Page %lu erase FAILED after all retries (time: %lu ms)!", 
                     page_number, page_time);
            pages_failed++;
        }
    }
    
    total_time_ms = millis() - total_time_ms;
    
    LOG_INFO("========================================");
    LOG_INFO("Page erase summary:");
    LOG_INFO("  - Total pages: %lu", last_page - first_page + 1);
    LOG_INFO("  - Pages erased successfully: %lu", pages_erased);
    LOG_INFO("  - Pages failed: %lu", pages_failed);
    LOG_INFO("  - Total attempts: %lu", total_attempts);
    LOG_INFO("  - Total time: %lu ms", total_time_ms);
    LOG_INFO("========================================");
    
    if (pages_failed > 0) {
        LOG_ERROR("WARNING: %lu pages failed to erase!", pages_failed);
    } else {
        LOG_INFO("All pages erased successfully!");
    }
}

// Override preFSBegin() with MAXIMUM SAFE logging
// 
// SAFE SCENARIO (step-by-step diagnostic plan):
// 
// CRITICALLY IMPORTANT: This function is called in fsInit() -> preFSBegin()
// USB is already initialized (consoleInit() is called earlier in setup()),
// BUT full USB initialization may not be complete yet!
// 
// SAFE RULES:
// 1. Minimal operations (only GPREGRET check - fast)
// 2. If format is needed - do ONLY InternalFS.format() WITHOUT erase pages (fast)
// 3. NO long operations (erase pages) here!
// 4. All long operations (erase pages, version check) in fsInitExtended() (after USB)
// 5. Maximum logging for diagnostics
//
void preFSBegin()
{
    static unsigned long millis_until_formatting_again = 0;
    
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
    LOG_INFO("  ✓ NO erase pages here (deferred to fsInitExtended)");
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
        
        // Try to mount the main filesystem
        // If mount succeeds - main FS is NOT corrupted
        if (!InternalFS.begin()) {
            LOG_WARN("⚠️  MAIN filesystem mount FAILED - it IS corrupted!");
            LOG_WARN("⚠️  Will format MAIN filesystem (MINIMAL format, NO erase pages)");
            main_fs_corrupted = true;
        } else {
            LOG_INFO("✓ MAIN filesystem mount SUCCESS - it is NOT corrupted!");
            LOG_INFO("✓ Corruption flag was likely from EXTENDED filesystem only");
            LOG_INFO("✓ Will NOT format MAIN filesystem (it's safe!)");
            LOG_INFO("✓ Extended filesystem will be handled in fsInitExtended()");
            main_fs_corrupted = false;
        }
        
        NRF_POWER->GPREGRET = 0;
        millis_until_formatting_again = millis() + MULTIPLE_CORRUPTION_DELAY_MILLIS;
        LOG_INFO("  - GPREGRET cleared");
        LOG_INFO("  - millis_until_formatting_again set to: %lu", millis_until_formatting_again);
        
        if (main_fs_corrupted) {
            // SAFE: Fast format WITHOUT erase pages
            // InternalFS.format() will perform minimal formatting (fast)
            // Full erase of all pages will be in fsInitExtended() (after USB is guaranteed ready)
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
    LOG_INFO("✓ Extended filesystem logic deferred to fsInitExtended() (AFTER USB)");
    LOG_INFO("✓ All long operations (erase pages) will be in fsInitExtended()");
    LOG_INFO("preFSBegin() END");
    LOG_INFO("========================================");
}

#endif // RAK_4631_LITE_EXTENDED_FILESYSTEM
