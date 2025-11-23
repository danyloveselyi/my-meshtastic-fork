/**
 * @file FSCommon-fallback.cpp
 * @brief Detailed filesystem diagnostics for RAK4631 Lite variant
 * 
 * This file provides MAXIMUM detailed logging of filesystem state for debugging.
 * Extended filesystem support is TEMPORARILY DISABLED.
 */

#include "FSCommon.h"
#include "configuration.h"
#include "InternalFileSystem.h"
#include "softdevice/nrf_soc.h"
#include "error.h"
#include <Adafruit_LittleFS.h>
#include <cstdio>
#include <cstring>

// Always compile this for detailed filesystem diagnostics
#if defined(ARCH_NRF52)

#ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM
namespace
{
// Extended filesystem configuration (TEMPORARILY DISABLED)
constexpr uint32_t LFS_SIZE_VERSION = 80;
constexpr const char* LFS_SIZE_VERSION_FILE = "/.lfs_size_version";
} // namespace
#endif

// Detailed filesystem diagnostics - ALWAYS enabled for debugging
// MAXIMUM LOGGING: Every step is logged for debugging 80-page issue
void fsInitExtended()
{
    LOG_INFO("========================================");
    LOG_INFO("fsInitExtended() START - MAXIMUM DETAILED LOG");
    LOG_INFO("========================================");
    LOG_INFO("Called AFTER fsInit() -> FSBegin() -> InternalFS.begin()");
    LOG_INFO("USB/Serial should be initialized at this point");
    LOG_INFO("Current millis(): %lu", millis());
    
    // ========================================
    // MEMORY MAP - COMPLETE INFORMATION
    // ========================================
    LOG_INFO("========================================");
    LOG_INFO("MEMORY MAP - COMPLETE INFORMATION");
    LOG_INFO("========================================");
    
    // NRF52840 memory map constants
    constexpr uint32_t NRF52840_FLASH_START = 0x00000000;
    constexpr uint32_t NRF52840_FLASH_END = 0x00100000;  // 1 MB total flash
    constexpr uint32_t NRF52840_FLASH_SIZE = 0x00100000;
    
    constexpr uint32_t SOFTDEVICE_START = 0x00000000;
    constexpr uint32_t SOFTDEVICE_END = 0x00027000;      // 156 KB for S140
    constexpr uint32_t SOFTDEVICE_SIZE = 0x00027000;
    
    constexpr uint32_t APPLICATION_START = 0x00027000;   // After SoftDevice
    #ifdef NRF52840_XXAA
        // From linker script: ORIGIN = 0x27000, LENGTH = 0xED000 - 0x27000
        constexpr uint32_t APPLICATION_END_STANDARD = 0x000ED000;  // Standard: ~1016 KB total, ~860 KB app
        // Custom linker script (if enabled): ORIGIN = 0x27000, LENGTH = 0x80000 - 0x27000
        constexpr uint32_t APPLICATION_END_CUSTOM = 0x00080000;    // Custom: 512 KB limit
        constexpr uint32_t APPLICATION_SIZE_STANDARD = APPLICATION_END_STANDARD - APPLICATION_START;  // ~860 KB
        constexpr uint32_t APPLICATION_SIZE_CUSTOM = APPLICATION_END_CUSTOM - APPLICATION_START;      // ~485 KB
    #endif
    
    #ifdef NRF52840_XXAA
        // Default filesystem addresses (from standard InternalFileSystem.cpp)
        // NOTE: These are from Adafruit_nRF52_Arduino library
        // Standard: LFS_FLASH_ADDR = 0xED000 (or may be calculated dynamically)
        constexpr uint32_t FILESYSTEM_ADDR_STANDARD = 0x000ED000;  // Standard: after application (page 237)
        constexpr uint32_t FILESYSTEM_SIZE_STANDARD = 7 * 4096;    // 28 KB (7 pages)
        constexpr uint32_t FILESYSTEM_END_STANDARD = FILESYSTEM_ADDR_STANDARD + FILESYSTEM_SIZE_STANDARD - 1;  // 0xF3FFF (page 243)
        
        // Extended filesystem addresses (from custom fork)
        // Custom fork sets: LFS_FLASH_ADDR = 0x80000, LFS_FLASH_TOTAL_SIZE = 80 * 4096
        constexpr uint32_t FILESYSTEM_ADDR_EXTENDED = 0x00080000;  // Custom: after app (linker limited) (page 128)
        constexpr uint32_t FILESYSTEM_SIZE_EXTENDED = 80 * 4096;   // 320 KB (80 pages)
        constexpr uint32_t FILESYSTEM_END_EXTENDED = FILESYSTEM_ADDR_EXTENDED + FILESYSTEM_SIZE_EXTENDED - 1;  // 0xD0000 - 1 (page 207, inclusive)
        
        // Bootloader addresses (from bootloader HEX file analysis)
        constexpr uint32_t BOOTLOADER_ADDR = 0x000F4000;  // Bootloader starts here (page 244)
        constexpr uint32_t BOOTLOADER_END = 0x000FFFFF;   // End of flash (1 MB - 1) (page 255)
        constexpr uint32_t BOOTLOADER_SIZE = BOOTLOADER_END - BOOTLOADER_ADDR + 1;  // 48 KB (12 pages: 244-255)
        constexpr uint32_t BOOTLOADER_SETTINGS_ADDR = 0x000FF000;  // Bootloader settings page (page 255)
        constexpr uint32_t BOOTLOADER_SETTINGS_SIZE = 4096;  // 1 page
        
        // Critical gap analysis
        uint32_t gap_standard = BOOTLOADER_ADDR - FILESYSTEM_END_STANDARD - 1;
        uint32_t gap_extended = BOOTLOADER_ADDR - FILESYSTEM_END_EXTENDED - 1;
        
        LOG_INFO("Critical Gap Analysis:");
        LOG_INFO("  - Standard filesystem gap to bootloader: %u bytes (%u KB, %u pages)", 
                 gap_standard, gap_standard / 1024, gap_standard / 4096);
        LOG_INFO("  - Extended filesystem gap to bootloader: %u bytes (%u KB, %u pages)", 
                 gap_extended, gap_extended / 1024, gap_extended / 4096);
        LOG_INFO("  - Standard gap status: %s", 
                 gap_standard >= (64 * 1024) ? "SAFE (>=64 KB)" : 
                 gap_standard >= (32 * 1024) ? "OK (>=32 KB)" : 
                 gap_standard >= (16 * 1024) ? "WARNING (<32 KB)" : 
                 gap_standard >= (4 * 1024) ? "RISKY (<16 KB)" : "CRITICAL (<4 KB)");
        LOG_INFO("  - Extended gap status: %s", 
                 gap_extended >= (64 * 1024) ? "SAFE (>=64 KB)" : 
                 gap_extended >= (32 * 1024) ? "OK (>=32 KB)" : 
                 gap_extended >= (16 * 1024) ? "WARNING (<32 KB)" : 
                 gap_extended >= (4 * 1024) ? "RISKY (<16 KB)" : "CRITICAL (<4 KB)");
    #endif
    
    LOG_INFO("NRF52840 Flash Memory (Total: 1 MB = 1024 KB):");
    LOG_INFO("  - Start: 0x%08X (0)", NRF52840_FLASH_START);
    LOG_INFO("  - End:   0x%08X (%u KB)", NRF52840_FLASH_END - 1, NRF52840_FLASH_SIZE / 1024);
    LOG_INFO("  - Size:  %u KB", NRF52840_FLASH_SIZE / 1024);
    
    LOG_INFO("SoftDevice Region (S140):");
    LOG_INFO("  - Start: 0x%08X", SOFTDEVICE_START);
    LOG_INFO("  - End:   0x%08X", SOFTDEVICE_END - 1);
    LOG_INFO("  - Size:  %u KB", SOFTDEVICE_SIZE / 1024);
    
    #ifdef NRF52840_XXAA
    LOG_INFO("Application Region:");
    #ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM
    LOG_INFO("  - Config: CUSTOM (linker script limited)");
    LOG_INFO("  - Start:  0x%08X", APPLICATION_START);
    LOG_INFO("  - End:    0x%08X (linker limited)", APPLICATION_END_CUSTOM - 1);
    LOG_INFO("  - Size:   %u KB", APPLICATION_SIZE_CUSTOM / 1024);
    #else
    LOG_INFO("  - Config: STANDARD");
    LOG_INFO("  - Start:  0x%08X", APPLICATION_START);
    LOG_INFO("  - End:    0x%08X", APPLICATION_END_STANDARD - 1);
    LOG_INFO("  - Size:   %u KB", APPLICATION_SIZE_STANDARD / 1024);
    #endif
    
    LOG_INFO("Filesystem Region:");
    #ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM
    LOG_INFO("  - Config: EXTENDED (80 pages, 320 KB)");
    LOG_INFO("  - Start:  0x%08X (page %u)", FILESYSTEM_ADDR_EXTENDED, FILESYSTEM_ADDR_EXTENDED / 4096);
    LOG_INFO("  - End:    0x%08X (page %u, inclusive)", FILESYSTEM_END_EXTENDED, FILESYSTEM_END_EXTENDED / 4096);
    LOG_INFO("  - Size:   %u KB (%u pages)", FILESYSTEM_SIZE_EXTENDED / 1024, FILESYSTEM_SIZE_EXTENDED / 4096);
    LOG_INFO("  - Page range: %u - %u", FILESYSTEM_ADDR_EXTENDED / 4096, FILESYSTEM_END_EXTENDED / 4096);
    #else
    LOG_INFO("  - Config: STANDARD (7 pages, 28 KB)");
    LOG_INFO("  - Start:  0x%08X (page %u)", FILESYSTEM_ADDR_STANDARD, FILESYSTEM_ADDR_STANDARD / 4096);
    LOG_INFO("  - End:    0x%08X (page %u, inclusive)", FILESYSTEM_END_STANDARD, FILESYSTEM_END_STANDARD / 4096);
    LOG_INFO("  - Size:   %u KB (%u pages)", FILESYSTEM_SIZE_STANDARD / 1024, FILESYSTEM_SIZE_STANDARD / 4096);
    LOG_INFO("  - Page range: %u - %u", FILESYSTEM_ADDR_STANDARD / 4096, FILESYSTEM_END_STANDARD / 4096);
    #endif
    
    LOG_INFO("Bootloader Region:");
    LOG_INFO("  - Start:           0x%08X (page %u)", BOOTLOADER_ADDR, BOOTLOADER_ADDR / 4096);
    LOG_INFO("  - End:             0x%08X (page %u)", BOOTLOADER_END, BOOTLOADER_END / 4096);
    LOG_INFO("  - Size:            %u KB", BOOTLOADER_SIZE / 1024);
    LOG_INFO("  - Settings page:   0x%08X (page %u)", BOOTLOADER_SETTINGS_ADDR, BOOTLOADER_SETTINGS_ADDR / 4096);
    LOG_INFO("  - Settings size:   %u KB (1 page)", BOOTLOADER_SETTINGS_SIZE / 1024);
    LOG_INFO("  - Page range:      %u - %u", BOOTLOADER_ADDR / 4096, BOOTLOADER_END / 4096);
    
    // Overlap checks
    LOG_INFO("Overlap Analysis:");
    #ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM
    uint32_t fs_end = FILESYSTEM_END_EXTENDED;
    uint32_t fs_start = FILESYSTEM_ADDR_EXTENDED;
    bool overlap_with_app = (fs_start < APPLICATION_END_CUSTOM);
    bool overlap_with_bootloader = (fs_end > BOOTLOADER_ADDR);
    LOG_INFO("  - Filesystem vs Application:");
    LOG_INFO("    * Filesystem start: 0x%08X", fs_start);
    LOG_INFO("    * Application end:  0x%08X", APPLICATION_END_CUSTOM - 1);
    LOG_INFO("    * Overlap: %s", overlap_with_app ? "YES (ERROR!)" : "NO (OK)");
    LOG_INFO("  - Filesystem vs Bootloader:");
    LOG_INFO("    * Filesystem end:   0x%08X (page %u)", fs_end - 1, (fs_end - 1) / 4096);
    LOG_INFO("    * Bootloader start: 0x%08X (page %u)", BOOTLOADER_ADDR, BOOTLOADER_ADDR / 4096);
    LOG_INFO("    * Overlap: %s", overlap_with_bootloader ? "YES (CRITICAL ERROR!)" : "NO (OK)");
    if (overlap_with_bootloader) {
        uint32_t overlap_pages = (fs_end - BOOTLOADER_ADDR) / 4096;
        LOG_ERROR("    * OVERLAP SIZE: %u pages (%u KB) - CRITICAL!", overlap_pages, overlap_pages * 4);
    }
    #else
    uint32_t fs_end = FILESYSTEM_END_STANDARD;
    uint32_t fs_start = FILESYSTEM_ADDR_STANDARD;
    bool overlap_with_app = (fs_start < APPLICATION_END_STANDARD);
    bool overlap_with_bootloader = (fs_end > BOOTLOADER_ADDR);
    LOG_INFO("  - Filesystem vs Application:");
    LOG_INFO("    * Filesystem start: 0x%08X", fs_start);
    LOG_INFO("    * Application end:  0x%08X", APPLICATION_END_STANDARD - 1);
    LOG_INFO("    * Overlap: %s", overlap_with_app ? "YES (ERROR!)" : "NO (OK)");
    LOG_INFO("  - Filesystem vs Bootloader:");
    LOG_INFO("    * Filesystem end:   0x%08X (page %u)", fs_end - 1, (fs_end - 1) / 4096);
    LOG_INFO("    * Bootloader start: 0x%08X (page %u)", BOOTLOADER_ADDR, BOOTLOADER_ADDR / 4096);
    LOG_INFO("    * Overlap: %s", overlap_with_bootloader ? "YES (CRITICAL ERROR!)" : "NO (OK)");
    if (overlap_with_bootloader) {
        uint32_t overlap_pages = (fs_end - BOOTLOADER_ADDR) / 4096;
        LOG_ERROR("    * OVERLAP SIZE: %u pages (%u KB) - CRITICAL!", overlap_pages, overlap_pages * 4);
    }
    #endif
    
    // Safety margins (calculated correctly: end + 1 to start)
    #ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM
    uint32_t margin_to_bootloader = BOOTLOADER_ADDR - (fs_end + 1);  // +1 because fs_end is inclusive
    LOG_INFO("  - Safety margin to bootloader:");
    LOG_INFO("    * Filesystem end (inclusive): 0x%08X (page %u)", fs_end, fs_end / 4096);
    LOG_INFO("    * Bootloader start:           0x%08X (page %u)", BOOTLOADER_ADDR, BOOTLOADER_ADDR / 4096);
    LOG_INFO("    * Margin: %u bytes (%u KB)", margin_to_bootloader, margin_to_bootloader / 1024);
    LOG_INFO("    * Pages:  %u pages", margin_to_bootloader / 4096);
    LOG_INFO("    * Status: %s", margin_to_bootloader >= (64 * 1024) ? "SAFE (>=64 KB)" : 
                                  margin_to_bootloader >= (32 * 1024) ? "OK (>=32 KB)" : 
                                  margin_to_bootloader >= (16 * 1024) ? "WARNING (<32 KB)" : 
                                  margin_to_bootloader >= (4 * 1024) ? "RISKY (<16 KB)" : "CRITICAL (<4 KB)");
    #else
    uint32_t margin_to_bootloader = BOOTLOADER_ADDR - (fs_end + 1);  // +1 because fs_end is inclusive
    LOG_INFO("  - Safety margin to bootloader:");
    LOG_INFO("    * Filesystem end (inclusive): 0x%08X (page %u)", fs_end, fs_end / 4096);
    LOG_INFO("    * Bootloader start:           0x%08X (page %u)", BOOTLOADER_ADDR, BOOTLOADER_ADDR / 4096);
    LOG_INFO("    * Margin: %u bytes (%u KB)", margin_to_bootloader, margin_to_bootloader / 1024);
    LOG_INFO("    * Pages:  %u pages", margin_to_bootloader / 4096);
    LOG_INFO("    * Status: %s", margin_to_bootloader >= (64 * 1024) ? "SAFE (>=64 KB)" : 
                                  margin_to_bootloader >= (32 * 1024) ? "OK (>=32 KB)" : 
                                  margin_to_bootloader >= (16 * 1024) ? "WARNING (<32 KB)" : 
                                  margin_to_bootloader >= (4 * 1024) ? "RISKY (<16 KB)" : "CRITICAL (<4 KB)");
    #endif
    #endif // NRF52840_XXAA
    
    LOG_INFO("========================================");
    
    // Check version file to see what filesystem configuration was used
    LOG_INFO("Checking filesystem version file (.lfs_size_version):");
    LOG_INFO("  ⚠️  IMPORTANT: This file is ONLY used by our CUSTOM variant code!");
    LOG_INFO("  ⚠️  Standard Meshtastic framework COMPLETELY IGNORES this file!");
    LOG_INFO("  ⚠️  Standard framework uses hardcoded constants from InternalFileSystem.cpp");
    LOG_INFO("  ⚠️  If this file contains '80', it's a REMNANT from PREVIOUS CUSTOM firmware!");
    LOG_INFO("  ⚠️  Current firmware (STANDARD) did NOT format it to 80 pages!");
    
    auto checkVersionFile = FSCom.open("/.lfs_size_version", FILE_O_READ);
    if (checkVersionFile) {
        char versionStr[16] = {0};
        size_t bytesRead = checkVersionFile.readBytes(versionStr, sizeof(versionStr) - 1);
        checkVersionFile.close();
        if (bytesRead > 0) {
            uint32_t savedVersion = strtoul(versionStr, nullptr, 10);
            LOG_INFO("  - Version file found: '%s' (%zu bytes)", versionStr, bytesRead);
            LOG_INFO("  - This file indicates: PREVIOUS CUSTOM firmware formatted for %lu pages (%lu KB)", savedVersion, savedVersion * 4);
            LOG_INFO("  - THIS IS AN ARTIFACT from previous flash - NOT from current firmware!");
            
            #ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM
            uint32_t currentVersion = 80;
            #else
            uint32_t currentVersion = 7;
            #endif
            
            LOG_INFO("  - Current firmware configuration: %u pages (%u KB)", currentVersion, currentVersion * 4);
            LOG_INFO("  - Current framework: %s", 
                     #ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM
                     "CUSTOM (80 pages)"
                     #else
                     "STANDARD Meshtastic (7 pages)"
                     #endif
                     );
            
            if (savedVersion != currentVersion) {
                LOG_INFO("  - File shows different size - THIS IS NORMAL!");
                LOG_INFO("    * Previous format (from file): %lu pages - CUSTOM firmware (old)", savedVersion);
                LOG_INFO("    * Current config:              %u pages - %s (current)", currentVersion,
                         #ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM
                         "CUSTOM firmware"
                         #else
                         "STANDARD framework"
                         #endif
                         );
                LOG_INFO("  - Standard framework uses its own hardcoded addresses (0xED000, 7 pages)");
                LOG_INFO("  - Standard framework does NOT read or use this file!");
                LOG_INFO("  - This mismatch is HARMLESS - it's just a leftover file!");
            } else {
                LOG_INFO("  - Version matches current config: %lu pages", savedVersion);
            }
        } else {
            LOG_INFO("  - Version file is empty");
        }
    } else {
        LOG_INFO("  - Version file NOT found (standard filesystem or first boot)");
    }
    
    LOG_INFO("========================================");
    
    // Get filesystem information
    // Note: FSCom is an object, not a pointer, so we can't use it as bool
    // Instead, we'll try to open a file to check if mounted
    LOG_INFO("Filesystem mount status: checking...");
    
#ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM
    {
    LOG_INFO("========================================");
    LOG_INFO("EXTENDED FILESYSTEM LOGIC - 80 PAGES");
    LOG_INFO("========================================");
    LOG_INFO("Configuration:");
    LOG_INFO("  - LFS_SIZE_VERSION: %u pages (%u KB)", LFS_SIZE_VERSION, LFS_SIZE_VERSION * 4);
    LOG_INFO("  - LFS_SIZE_VERSION_FILE: %s", LFS_SIZE_VERSION_FILE);
    
    // Check if version file exists and matches current version
    LOG_INFO("Checking version file: %s", LFS_SIZE_VERSION_FILE);
    auto versionFile = FSCom.open(LFS_SIZE_VERSION_FILE, FILE_O_READ);
    bool needFormat = false;

    if (!versionFile) {
        // Version file doesn't exist - old filesystem or new format, needs format
        LOG_INFO("Version file NOT FOUND - will format for %u pages", LFS_SIZE_VERSION);
        LOG_INFO("  - This means filesystem is old format or first boot");
        needFormat = true;
    } else {
        LOG_INFO("Version file FOUND - reading version...");
        // Read version from file
        char versionStr[16] = {0};
        size_t bytesRead = versionFile.readBytes(versionStr, sizeof(versionStr) - 1);
        LOG_INFO("  - Bytes read: %zu", bytesRead);
        LOG_INFO("  - Version string: '%s'", versionStr);
        versionFile.close();

        if (bytesRead > 0) {
            uint32_t savedVersion = strtoul(versionStr, nullptr, 10);
            LOG_INFO("  - Parsed saved version: %lu pages", savedVersion);
            LOG_INFO("  - Current version: %u pages", LFS_SIZE_VERSION);
            
            if (savedVersion != LFS_SIZE_VERSION) {
                LOG_INFO("Version MISMATCH: %lu -> %u pages - WILL FORMAT", savedVersion, LFS_SIZE_VERSION);
                needFormat = true;
            } else {
                LOG_INFO("Version MATCHES: %lu pages - no formatting needed", savedVersion);
            }
        } else {
            LOG_WARN("Version file is EMPTY - will format");
            needFormat = true;
        }
    }

    if (needFormat) {
        LOG_INFO("========================================");
        LOG_INFO("FORMATTING FILESYSTEM - DETAILED LOG");
        LOG_INFO("========================================");
        LOG_INFO("Reason: %s", 
                 !versionFile ? "Version file not found" : 
                 "Version mismatch");
        
        LOG_INFO("Step 1: Unmounting filesystem...");
        FSCom.end();
        LOG_INFO("Filesystem unmounted");
        
        // CRITICAL: InternalFS.format() only formats what was previously mounted (old size),
        // it does NOT erase new pages. When filesystem size increases, new pages
        // contain garbage data, causing "Bad block" and "No more free space" errors.
        //
        // Solution: Manually erase ALL pages BEFORE formatting (with bootloader protection).
        
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
        
        LOG_INFO("Step 2: Erasing all %u pages before formatting", LFS_SIZE_VERSION);
        
        LOG_INFO("Configuration constants:");
        LOG_INFO("  - FLASH_NRF52_PAGE_SIZE: %u bytes", FLASH_NRF52_PAGE_SIZE);
        LOG_INFO("  - LFS_FLASH_ADDR: 0x%X (%u)", LFS_FLASH_ADDR, LFS_FLASH_ADDR);
        LOG_INFO("  - BOOTLOADER_ADDR: 0x%X (%u)", BOOTLOADER_ADDR, BOOTLOADER_ADDR);
        LOG_INFO("  - LFS_FLASH_TOTAL_SIZE: %u bytes (%u KB)", LFS_FLASH_TOTAL_SIZE, LFS_FLASH_TOTAL_SIZE / 1024);
        
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
            LOG_ERROR("ERROR: Requested size would overlap bootloader!");
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
        uint32_t erase_start_time = millis();
        
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
        
        uint32_t total_erase_time = millis() - erase_start_time;
        LOG_INFO("========================================");
        LOG_INFO("Page erase summary:");
        LOG_INFO("  - Total pages: %lu", last_page - first_page + 1);
        LOG_INFO("  - Pages erased successfully: %lu", pages_erased);
        LOG_INFO("  - Pages failed: %lu", pages_failed);
        LOG_INFO("  - Total attempts: %lu", total_attempts);
        LOG_INFO("  - Total erase time: %lu ms", total_erase_time);
        LOG_INFO("========================================");
        
        if (pages_failed > 0) {
            LOG_ERROR("WARNING: %lu pages failed to erase!", pages_failed);
        } else {
            LOG_INFO("All pages erased successfully!");
        }
        
        // Now format the filesystem - all pages are erased, so format will work correctly
        LOG_INFO("Step 3: Formatting filesystem...");
        uint32_t format_start = millis();
        InternalFS.format();
        uint32_t format_time = millis() - format_start;
        LOG_INFO("InternalFS.format() completed in %lu ms", format_time);
        
        // Remount with new size configuration
        LOG_INFO("Step 4: Remounting filesystem...");
        uint32_t mount_start = millis();
        bool mount_success = FSBegin();
        uint32_t mount_time = millis() - mount_start;
        
        if (mount_success) {
            LOG_INFO("Filesystem remounted successfully in %lu ms", mount_time);
            
            LOG_INFO("Step 5: Creating version file...");
            auto writeFile = FSCom.open(LFS_SIZE_VERSION_FILE, FILE_O_WRITE);
            if (writeFile) {
                char versionStr[16];
                snprintf(versionStr, sizeof(versionStr), "%lu", (unsigned long)LFS_SIZE_VERSION);
                size_t bytes_written = writeFile.write((uint8_t*)versionStr, strlen(versionStr));
                writeFile.close();
                LOG_INFO("Version file created successfully:");
                LOG_INFO("  - File: %s", LFS_SIZE_VERSION_FILE);
                LOG_INFO("  - Version: %u pages", LFS_SIZE_VERSION);
                LOG_INFO("  - Bytes written: %zu", bytes_written);
                LOG_INFO("Filesystem formatted and version %u saved", LFS_SIZE_VERSION);
            } else {
                LOG_ERROR("Failed to create version file: %s", LFS_SIZE_VERSION_FILE);
            }
        } else {
            LOG_ERROR("FAILED to remount filesystem after format (time: %lu ms)", mount_time);
        }
    } else {
        // Version matches - filesystem is correct size
        LOG_INFO("Version matches - filesystem is correct size (%u pages)", LFS_SIZE_VERSION);
        LOG_INFO("No formatting needed");
    }
    LOG_INFO("Extended filesystem logic completed");
    } // Extended filesystem code
#endif // RAK_4631_LITE_EXTENDED_FILESYSTEM
    
    // Standard filesystem diagnostics - ALWAYS enabled
    LOG_INFO("========================================");
    LOG_INFO("FILESYSTEM DIAGNOSTICS - FINAL STATUS");
    LOG_INFO("========================================");
    LOG_INFO("Filesystem configuration:");
    LOG_INFO("  - Architecture: NRF52");
    #ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM
    LOG_INFO("  - Extended filesystem: ENABLED");
    LOG_INFO("  - Size: 80 pages (%u KB)", 80 * 4);
    LOG_INFO("  - Address: 0x80000");
    LOG_INFO("  - Total size: 320 KB");
    #else
    LOG_INFO("  - Extended filesystem: DISABLED");
    LOG_INFO("  - Size: 7 pages (28 KB)");
    LOG_INFO("  - Standard configuration");
    #endif
    
    // Try to get filesystem info (may not be available)
    LOG_INFO("Filesystem files:");
    LOG_INFO("========================================");
    
    // Log all files in filesystem
    LOG_INFO("Scanning filesystem for all files...");
    std::vector<meshtastic_FileInfo> files = getFiles("/", 10);
    LOG_INFO("Total files found: %u", (unsigned)files.size());
    
    uint32_t totalSize = 0;
    if (files.size() > 0) {
        for (const auto& file : files) {
            LOG_INFO("  File: %s, Size: %u bytes", file.file_name, file.size_bytes);
            totalSize += file.size_bytes;
        }
    } else {
        LOG_INFO("  No files found in filesystem");
    }
    
    LOG_INFO("Total filesystem usage: %u bytes (%u KB)", totalSize, totalSize / 1024);
    #ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM
    LOG_INFO("Filesystem capacity: 320 KB");
    LOG_INFO("Filesystem free: %u bytes (%u KB)", (320 * 1024) - totalSize, ((320 * 1024) - totalSize) / 1024);
    #else
    LOG_INFO("Filesystem capacity: 28 KB");
    LOG_INFO("Filesystem free: %u bytes (%u KB)", (28 * 1024) - totalSize, ((28 * 1024) - totalSize) / 1024);
    #endif
    LOG_INFO("========================================");
    LOG_INFO("Filesystem diagnostics COMPLETE");
    LOG_INFO("Current millis(): %lu", millis());
    LOG_INFO("========================================");
    
    // Initialize extended filesystem for NodeDB (80 pages, 320 KB)
    // Main filesystem remains on 7 pages (28 KB) for configs
    // Extended filesystem is used ONLY for NodeDB (nodes.proto)
    #ifdef USE_EXTENDED_FS_FOR_NODEDB
    // Note: useExtendedFSForNodeDB() will call ExtendedNodeDBFS::init() which already logs initialization
    extern bool useExtendedFSForNodeDB();
    bool extended_available = useExtendedFSForNodeDB();
    if (extended_available) {
        LOG_INFO("Extended filesystem status: READY for NodeDB (80 pages, 320 KB)");
    } else {
        LOG_ERROR("Extended filesystem status: FAILED - NodeDB will use main filesystem (7 pages, 28 KB)");
    }
    #endif
    
    // Test filesystem on 80 pages - TESTING IS SAFE
    // Main filesystem remains on 7 pages (safe)
    // Test filesystem is created on 80 pages (0x80000) for diagnostics
    // Currently disabled - requires custom framework with correct constants
    #ifdef TEST_80_PAGES_FILESYSTEM
    // test80PagesFilesystem(); // TODO: Requires custom framework with LFS_FLASH_ADDR = 0x80000
    LOG_INFO("TEST 80 PAGES FILESYSTEM: Placeholder - requires custom framework update");
    #endif
}

#ifdef TEST_80_PAGES_FILESYSTEM
/**
 * @brief Test filesystem on 80 pages (SEPARATE from main)
 * 
 * SAFE: Main filesystem remains on 7 pages
 * Test filesystem is created on 80 pages (0x80000 - 0xD0000)
 * 
 * Purpose: Diagnose 80-page problem WITHOUT risk of "bricking" the device
 */
void test80PagesFilesystem()
{
    LOG_INFO("========================================");
    LOG_INFO("TEST 80 PAGES FILESYSTEM - DIAGNOSTIC MODE");
    LOG_INFO("========================================");
    LOG_INFO("SAFETY: Main filesystem remains at 7 pages (0xED000)");
    LOG_INFO("Test filesystem will be created at 80 pages (0x80000)");
    LOG_INFO("This allows testing WITHOUT affecting main filesystem");
    LOG_INFO("");
    LOG_INFO("WARNING: This is for DIAGNOSTIC ONLY!");
    LOG_INFO("Do NOT use this for production!");
    LOG_INFO("========================================");
    
    // Test filesystem configuration (80 pages)
    constexpr uint32_t FLASH_NRF52_PAGE_SIZE = 4096;
    #ifdef NRF52840_XXAA
        constexpr uint32_t TEST_LFS_FLASH_ADDR = 0x80000;  // After application
        constexpr uint32_t BOOTLOADER_ADDR = 0xF4000;  // NRF52840 bootloader
    #else
        constexpr uint32_t TEST_LFS_FLASH_ADDR = 0x6D000;  // Other NRF52 boards
        constexpr uint32_t BOOTLOADER_ADDR = 0x74000;
    #endif
    constexpr uint32_t TEST_LFS_FLASH_TOTAL_SIZE = 80 * FLASH_NRF52_PAGE_SIZE;  // 80 pages = 320 KB
    constexpr uint32_t TEST_LFS_BLOCK_SIZE = 4096;  // 4 KB blocks
    constexpr uint32_t TEST_LFS_BLOCK_COUNT = TEST_LFS_FLASH_TOTAL_SIZE / TEST_LFS_BLOCK_SIZE;  // 80 blocks
    
    LOG_INFO("Test filesystem configuration:");
    LOG_INFO("  - Address: 0x%08X (page %u)", TEST_LFS_FLASH_ADDR, TEST_LFS_FLASH_ADDR / FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Size: %u bytes (%u KB, %u pages)", TEST_LFS_FLASH_TOTAL_SIZE, TEST_LFS_FLASH_TOTAL_SIZE / 1024, TEST_LFS_FLASH_TOTAL_SIZE / FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Block size: %u bytes", TEST_LFS_BLOCK_SIZE);
    LOG_INFO("  - Block count: %u blocks", TEST_LFS_BLOCK_COUNT);
    LOG_INFO("  - End address: 0x%08X (page %u)", TEST_LFS_FLASH_ADDR + TEST_LFS_FLASH_TOTAL_SIZE - 1, (TEST_LFS_FLASH_ADDR + TEST_LFS_FLASH_TOTAL_SIZE - 1) / FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Bootloader start: 0x%08X (page %u)", BOOTLOADER_ADDR, BOOTLOADER_ADDR / FLASH_NRF52_PAGE_SIZE);
    
    // Safety check
    uint32_t test_fs_end = TEST_LFS_FLASH_ADDR + TEST_LFS_FLASH_TOTAL_SIZE - 1;
    uint32_t gap_to_bootloader = BOOTLOADER_ADDR - test_fs_end - 1;
    bool overlap = (test_fs_end >= BOOTLOADER_ADDR);
    
    LOG_INFO("Safety check:");
    LOG_INFO("  - Test filesystem end: 0x%08X (page %u)", test_fs_end, test_fs_end / FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Gap to bootloader: %u bytes (%u KB, %u pages)", gap_to_bootloader, gap_to_bootloader / 1024, gap_to_bootloader / FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Overlap with bootloader: %s", overlap ? "YES (ERROR!)" : "NO (OK)");
    
    if (overlap) {
        LOG_ERROR("ERROR: Test filesystem would overlap bootloader!");
        LOG_ERROR("Cannot proceed with test - SAFETY FIRST!");
        return;
    }
    
    if (gap_to_bootloader < (16 * 1024)) {
        LOG_WARN("WARNING: Gap to bootloader is small (%u KB)", gap_to_bootloader / 1024);
        LOG_WARN("This may be risky - proceed with caution!");
    } else {
        LOG_INFO("Gap status: %s", 
                 gap_to_bootloader >= (64 * 1024) ? "SAFE (>=64 KB)" : 
                 gap_to_bootloader >= (32 * 1024) ? "OK (>=32 KB)" : "WARNING (<32 KB)");
    }
    
    LOG_INFO("========================================");
    LOG_INFO("TEST 1: Erase pages (80 pages)");
    LOG_INFO("========================================");
    
    // Calculate page range for erasing
    uint32_t first_page = TEST_LFS_FLASH_ADDR / FLASH_NRF52_PAGE_SIZE;
    uint32_t bootloader_page = BOOTLOADER_ADDR / FLASH_NRF52_PAGE_SIZE;
    uint32_t calculated_last_page = (TEST_LFS_FLASH_ADDR + TEST_LFS_FLASH_TOTAL_SIZE - 1) / FLASH_NRF52_PAGE_SIZE;
    uint32_t last_page = (calculated_last_page >= bootloader_page) ? (bootloader_page - 1) : calculated_last_page;
    
    LOG_INFO("Page calculations:");
    LOG_INFO("  - First page: %lu (address: 0x%08lX)", first_page, first_page * FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Last page: %lu (address: 0x%08lX)", last_page, (last_page + 1) * FLASH_NRF52_PAGE_SIZE);
    LOG_INFO("  - Total pages: %lu", last_page - first_page + 1);
    LOG_INFO("  - Bootloader page: %lu (address: 0x%08lX)", bootloader_page, bootloader_page * FLASH_NRF52_PAGE_SIZE);
    
    if (calculated_last_page >= bootloader_page) {
        LOG_ERROR("ERROR: Would erase bootloader pages!");
        LOG_ERROR("  - Calculated last page: %lu", calculated_last_page);
        LOG_ERROR("  - Bootloader starts at: %lu", bootloader_page);
        LOG_ERROR("  - Limiting to page: %lu", last_page);
        return;
    }
    
    // Check SoftDevice
    uint8_t sd_enabled = 0;
    uint32_t sd_check_result = sd_softdevice_is_enabled(&sd_enabled);
    bool use_async = (sd_check_result == NRF_SUCCESS && sd_enabled);
    
    LOG_INFO("SoftDevice status:");
    LOG_INFO("  - sd_softdevice_is_enabled() result: %lu", sd_check_result);
    LOG_INFO("  - SoftDevice enabled: %u", sd_enabled);
    LOG_INFO("  - Use async flash operations: %s", use_async ? "YES" : "NO");
    
    LOG_INFO("Starting page erase test...");
    uint32_t erase_start_time = millis();
    uint32_t pages_erased = 0;
    uint32_t pages_failed = 0;
    
    for (uint32_t page_number = first_page; page_number <= last_page; page_number++) {
        uint32_t page_start_time = millis();
        uint32_t err_code;
        bool erase_success = false;
        
        LOG_INFO("Erasing test page %lu/%lu (address: 0x%08lX)...", 
                 page_number - first_page + 1, last_page - first_page + 1,
                 page_number * FLASH_NRF52_PAGE_SIZE);
        
        // Retry if busy (up to 10 attempts)
        for (uint8_t attempt = 0; attempt < 10; attempt++) {
            err_code = sd_flash_page_erase(page_number);
            
            if (err_code == NRF_ERROR_BUSY) {
                LOG_DEBUG("  Page %lu is busy, waiting 50ms...", page_number);
                delay(50);
                continue;
            }
            
            if (err_code == NRF_SUCCESS) {
                LOG_DEBUG("  Page %lu erase initiated successfully", page_number);
                
                // Wait for async operation to complete if SoftDevice is enabled
                if (use_async) {
                    uint32_t timeout = 500;
                    uint32_t start_time = millis();
                    
                    while ((millis() - start_time) < timeout) {
                        uint32_t evt;
                        bool found_our_event = false;
                        
                        while (sd_evt_get(&evt) == NRF_SUCCESS) {
                            if (evt == NRF_EVT_FLASH_OPERATION_SUCCESS) {
                                erase_success = true;
                                found_our_event = true;
                                LOG_DEBUG("  Flash operation SUCCESS event received for page %lu", page_number);
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
                        delay(10);
                    }
                    
                    if (!erase_success && (millis() - start_time) >= timeout) {
                        LOG_ERROR("  TIMEOUT waiting for erase completion page %lu", page_number);
                    }
                } else {
                    erase_success = true;
                }
                
                if (erase_success) {
                    break;
                }
            } else {
                if (attempt < 3) {
                    LOG_DEBUG("  Page %lu erase returned error %lu (attempt %d), retrying", 
                             page_number, err_code, attempt + 1);
                    delay(20);
                } else {
                    LOG_ERROR("  Page %lu erase returned error %lu (attempt %d)", 
                             page_number, err_code, attempt + 1);
                }
            }
        }
        
        if (erase_success) {
            uint32_t page_time = millis() - page_start_time;
            LOG_INFO("  ✓ Page %lu erased successfully in %lu ms", page_number, page_time);
            pages_erased++;
        } else {
            uint32_t page_time = millis() - page_start_time;
            LOG_ERROR("  ✗ Page %lu erase FAILED after all retries (time: %lu ms)!", 
                     page_number, page_time);
            pages_failed++;
        }
    }
    
    uint32_t total_erase_time = millis() - erase_start_time;
    LOG_INFO("========================================");
    LOG_INFO("Page erase test summary:");
    LOG_INFO("  - Total pages: %lu", last_page - first_page + 1);
    LOG_INFO("  - Pages erased successfully: %lu", pages_erased);
    LOG_INFO("  - Pages failed: %lu", pages_failed);
    LOG_INFO("  - Total erase time: %lu ms", total_erase_time);
    LOG_INFO("========================================");
    
    if (pages_failed > 0) {
        LOG_ERROR("ERROR: %lu pages failed to erase!", pages_failed);
        LOG_ERROR("Cannot proceed with filesystem test");
        return;
    }
    
    LOG_INFO("SUCCESS: All %lu pages erased successfully!", pages_erased);
    LOG_INFO("Note: Filesystem format test requires custom framework (not implemented in this test)");
    LOG_INFO("This test only verifies that page erase works correctly");
    LOG_INFO("========================================");
    LOG_INFO("TEST 80 PAGES FILESYSTEM - COMPLETE");
    LOG_INFO("Current millis(): %lu", millis());
    LOG_INFO("========================================");
}
#else
// Test function disabled - will be compiled if TEST_80_PAGES_FILESYSTEM is defined
void test80PagesFilesystem() {}
#endif // TEST_80_PAGES_FILESYSTEM
#else
// If not ARCH_NRF52, provide empty function
void fsInitExtended()
{
}
#endif // ARCH_NRF52

