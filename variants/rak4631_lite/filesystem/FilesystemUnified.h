/**
 * @file FilesystemUnified.h
 * @brief Unified filesystem module for RAK4631 Lite
 * 
 * This file consolidates all filesystem-related functionality:
 * - ExtendedFilesystemModule: Extended filesystem (272 KB) for NodeDB
 * - FSCommon Patches: Patches for FSCommon.cpp functions
 * - Main FS Pre-init: Pre-initialization logic for main filesystem
 * 
 * Previously split across multiple files:
 * - modules/ExtendedFilesystemModule/ExtendedFilesystemModule.h
 * - filesystem/FSCommon-patches.h
 * - filesystem/main/main-fs-pre-init.cpp (preFSBegin declaration)
 */

#pragma once

#include "configuration.h"
#include "mesh/NodeDB.h"цВ

#ifdef ARCH_NRF52
#ifdef USE_EXTENDED_FS_FOR_NODEDB

// ============================================================================
// Helper Macros
// ============================================================================

/**
 * @brief Macro to feed watchdog and yield (for NRF52 platform)
 * 
 * This macro calls nrf52Loop() to process SoftDevice events and feed watchdog,
 * then calls yield() to allow other tasks to run.
 * 
 * Usage: FEED_WATCHDOG_AND_YIELD();
 */
#include <Arduino.h>  // For yield()
#ifdef ARCH_NRF52
extern void nrf52Loop();
#define FEED_WATCHDOG_AND_YIELD() do { nrf52Loop(); yield(); } while(0)
#else
#define FEED_WATCHDOG_AND_YIELD() do { yield(); } while(0)
#endif

// Include LittleFS headers BEFORE class definition to ensure types are defined
#include "../../../src/platform/stm32wl/littlefs/lfs.h"
#include "../../../src/platform/stm32wl/littlefs/lfs_util.h"

// Forward declaration (lfs_t is a typedef)
typedef struct lfs lfs_t;

// ============================================================================
// ExtendedFilesystemModule (from ExtendedFilesystemModule.h)
// ============================================================================

/**
 * @brief Extended Filesystem Module
 * 
 * Infrastructure module for managing extended filesystem (272 KB) for NodeDB.
 * This module handles initialization, mounting, formatting, and provides
 * access to the LittleFS instance for NodeDB operations.
 */
class ExtendedFilesystemModule {
private:
    // Static state
    static bool isInitialized;
    static bool isMounted;
    static bool format_in_progress;
    static uint32_t erased_pages_bitmap[3];  // Bitmap to track erased pages during format (68 pages: 0xA8000-0xEBFFF, pages 168-235)
    static lfs_t extended_lfs;
    static struct lfs_config extended_lfs_cfg;
    
    // Buffers for LittleFS (required for operation)
    static uint8_t read_buffer[128];  // 128 byte buffer (matches read_size)
    static uint8_t prog_buffer[128];  // 128 byte buffer (matches prog_size)
    static uint8_t lookahead_buffer[8];  // Lookahead buffer (8 bytes = 64 blocks, must be multiple of 32 blocks)
    
    // Configuration constants (shared with eraseFilesystemPages to avoid duplication)
    // Made public so eraseFilesystemPages can access them
public:
    static constexpr uint32_t FLASH_NRF52_PAGE_SIZE = 4096;
    #ifdef NRF52840_XXAA
        static constexpr uint32_t EXTENDED_LFS_FLASH_ADDR = 0xA8000;  // After application, limited by linker script
        static constexpr uint32_t BOOTLOADER_ADDR = 0xF8000;  // Adafruit nRF52840 bootloader standard address
    #else
        static constexpr uint32_t EXTENDED_LFS_FLASH_ADDR = 0x6D000;  // Other NRF52 boards
        static constexpr uint32_t BOOTLOADER_ADDR = 0x74000;
    #endif
    static constexpr uint32_t EXTENDED_LFS_FLASH_TOTAL_SIZE = 68 * FLASH_NRF52_PAGE_SIZE;  // 68 pages = 272 KB (sufficient for 340+ nodes, ends at 0xEBFFF before Main FS at 0xED000)
    
private:
    // Физика флеша: блоки LittleFS соответствуют страницам флеша (4096 байт)
    static constexpr uint32_t EXTENDED_LFS_BLOCK_SIZE = FLASH_NRF52_PAGE_SIZE;  // 4096 bytes - блоки соответствуют страницам флеша
    static constexpr uint32_t EXTENDED_LFS_BLOCK_COUNT = EXTENDED_LFS_FLASH_TOTAL_SIZE / EXTENDED_LFS_BLOCK_SIZE;  // 68 blocks (272 KB / 4096 bytes)
    static constexpr uint32_t EXTENDED_LFS_LOOKAHEAD = 64;  // Lookahead (must be multiple of 32 per LittleFS requirements, increased from 32 to 64 for better coverage of 68 blocks)
    
    // Version file constants
    static constexpr const char* VERSION_FILE = "/.extended_fs_version";
    static constexpr uint32_t EXPECTED_VERSION = 2;  // Version 2 = Added ensureParentDirs() and auto-create directories
                                                    // Incremented from 1 to force reformat on firmware update via UF2
                                                    // (UF2 only flashes app region, leaving old version file intact)
    
    // Internal initialization methods
    static bool initInternal();
    static bool formatFilesystem();
    static bool createVersionFile();
    static bool checkVersionFile();
    static bool validateFlashAddressesBeforeFormat();
    
    // LittleFS callbacks (static, private)
    static int lfs_read(const struct lfs_config *c, lfs_block_t block, 
                       lfs_off_t off, void *buffer, lfs_size_t size);
    static int lfs_prog(const struct lfs_config *c, lfs_block_t block, 
                       lfs_off_t off, const void *buffer, lfs_size_t size);
    static int lfs_erase(const struct lfs_config *c, lfs_block_t block);
    static int lfs_sync(const struct lfs_config *c);
    
    // Helper functions
    static int calcDirSizeRecursive(lfs_t* lfs, const char* path, uint32_t* size, int depth);
    
    // Internal state management helpers (for refactoring - eliminate code duplication)
    static void resetFilesystemState(bool reset_initialized = true, bool reset_mounted = true, bool reset_format = true);
    static bool isFilesystemReady();
    static bool ensureFilesystemInitialized();
    
public:
    /**
     * @brief Constructor - automatically initializes if not already done
     */
    ExtendedFilesystemModule();
    
    /**
     * @brief Initialize extended filesystem (manual init if needed)
     * @return true if successful
     */
    static bool init();
    
    /**
     * @brief Check if extended filesystem is available
     * @return true if mounted and ready
     */
    static bool isAvailable();
    
    /**
     * @brief Get LittleFS instance for extended filesystem
     * @return Pointer to lfs_t instance, or nullptr if not available
     */
    static lfs_t* getExtendedFS();
    
    /**
     * @brief Force reformat extended filesystem (for corruption recovery)
     * @return true if reformat was successful
     */
    static bool forceReformat();
    
    /**
     * @brief Get extended filesystem statistics
     * @param total Total size in bytes (output)
     * @param used Used size in bytes (output)
     * @param free Free size in bytes (output)
     * @return true if extended filesystem is available and stats are valid
     */
    static bool getStats(uint32_t* total, uint32_t* used, uint32_t* free);
    
    /**
     * @brief Open file for overwriting (truncate existing or create new)
     * 
     * This function opens a file with LFS_O_TRUNC and immediately syncs to free old blocks.
     * This prevents "No more free space" errors when filesystem has free space but old blocks
     * aren't freed yet (LittleFS only frees blocks on sync/close, not on open with TRUNC).
     * 
     * @param lfs LittleFS instance (can be extended_lfs or main FS)
     * @param file File handle (output)
     * @param filename File path
     * @return LFS_ERR_OK on success, error code on failure
     */
    static int openFileForOverwrite(lfs_t* lfs, lfs_file_t* file, const char* filename);
    
    /**
     * @brief Load protobuf from filesystem (extended FS if available, fallback to main FS)
     * @param filename File path to load
     * @param protoSize Expected size of protobuf (ignored for extended FS - uses actual file size)
     * @param objSize Size of destination object
     * @param fields Protobuf field descriptors
     * @param dest_struct Destination structure pointer
     * @return LoadFileResult indicating success or failure
     */
    static LoadFileResult loadProto(const char *filename, size_t protoSize, size_t objSize, 
                                   const pb_msgdesc_t *fields, void *dest_struct);
    
    /**
     * @brief Save protobuf to filesystem (extended FS if available, fallback to main FS)
     * @param filename File path to save
     * @param protoSize Expected size of protobuf
     * @param fields Protobuf field descriptors
     * @param dest_struct Source structure pointer
     * @param fullAtomic Whether to use atomic write
     * @return true if save was successful
     */
    static bool saveProto(const char *filename, size_t protoSize,
                         const pb_msgdesc_t *fields, const void *dest_struct, bool fullAtomic);
    
    /**
     * @brief Check if NodeDB save should proceed (radio state checking)
     * @param lastSaveTime Timestamp of last save (not used, kept for compatibility)
     * @return true if save should proceed
     */
    static bool shouldProceedWithSave(uint32_t lastSaveTime);
    
    /**
     * @brief Save NodeDB to disk with variant-specific optimizations
     * @param lastNodeDbSave Reference to last save timestamp (updated on success)
     * @return true if save was successful
     */
    static bool saveNodeDatabaseToDisk(uint32_t &lastNodeDbSave);
    
    /**
     * @brief Check if a filename is for NodeDB (should use extended filesystem)
     * @param filename File path to check
     * @return true if this file should use extended filesystem
     */
    static bool isNodeDBFile(const char* filename);
};

// Global instance pointer
extern ExtendedFilesystemModule* extendedFilesystemModule;

// C wrapper for initialization (deprecated - Extended FS removed, kept for compatibility)
// Main FS initialization is now handled by initMainFS() called from fsInit_patched()
extern "C" void initExtendedFilesystemForNodeDB();

// C wrapper for force reformat (deprecated - Extended FS removed, kept for compatibility)
extern "C" bool forceReformatExtendedFS();

// ============================================================================
// FSCommon Patches (from FSCommon-patches.h)
// ============================================================================

#include <vector>
#include "FSCommon.h"

// Forward declarations
extern void nrf52Loop();

/**
 * @brief Patched version of getFiles() with nrf52Loop() calls
 * 
 * This is a wrapper that adds nrf52Loop() calls to prevent watchdog timeouts
 * during long filesystem scans. This version replaces the standard getFiles()
 * when USE_EXTENDED_FS_FOR_NODEDB is enabled.
 */
std::vector<meshtastic_FileInfo> getFiles(const char *dirname, uint8_t levels);

/**
 * @brief Patched version of fsInit() with extended filesystem initialization
 * 
 * This wrapper adds extended filesystem initialization before BLE setup.
 * The original function is made weak, and this version is used when
 * USE_EXTENDED_FS_FOR_NODEDB is enabled.
 */
void fsInit_patched();

/**
 * @brief Get main filesystem statistics (for DeviceStatsModule)
 * @param total Total size in bytes (output)
 * @param used Used size in bytes (output)
 * @param free Free size in bytes (output)
 * @return true if stats are valid
 */
bool getMainFSStats(uint32_t* total, uint32_t* used, uint32_t* free);

/**
 * @brief Get main filesystem LittleFS instance for direct access
 * @return Pointer to lfs_t instance, or nullptr if not available
 * 
 * This function returns the LittleFS instance for the enlarged Main FS (304 KB).
 * Used by NodeDBPersistentBackend for direct file operations.
 */
#ifdef ARCH_NRF52
lfs_t* getMainFS();

/**
 * @brief Check if main filesystem is available and ready
 * @return true if main filesystem is mounted and ready
 */
bool isMainFSAvailable();
#endif

/**
 * @brief Get extended filesystem statistics (for DeviceStatsModule)
 * @param total Total size in bytes (output)
 * @param used Used size in bytes (output)
 * @param free Free size in bytes (output)
 * @return true if extended filesystem is available and stats are valid
 * 
 * @deprecated Extended FS is being removed. Use getMainFSStats() instead.
 */
bool getExtendedFSStats(uint32_t* total, uint32_t* used, uint32_t* free);

// ============================================================================
// Main FS Pre-init (from main-fs-pre-init.cpp)
// ============================================================================

/**
 * @brief Pre-initialization function for main filesystem
 * 
 * This function is called before mounting the main filesystem.
 * It prepares flash pages and checks for corruption flags.
 * preFSBegin() is forward declared in FSCommon.cpp before this include.
 */
void preFSBegin();

#else
// If USE_EXTENDED_FS_FOR_NODEDB is not defined, provide empty class
class ExtendedFilesystemModule {
public:
    static bool isAvailable() { return false; }
    static void* getExtendedFS() { return nullptr; }
    static bool isNodeDBFile(const char* filename) { (void)filename; return false; }
    static bool getStats(uint32_t* total, uint32_t* used, uint32_t* free) {
        (void)total; (void)used; (void)free;
        return false;
    }
};
#endif // USE_EXTENDED_FS_FOR_NODEDB

#else
// If not ARCH_NRF52, provide same empty class (no duplication)
class ExtendedFilesystemModule {
public:
    static bool isAvailable() { return false; }
    static void* getExtendedFS() { return nullptr; }
    static bool isNodeDBFile(const char* filename) { (void)filename; return false; }
    static bool getStats(uint32_t* total, uint32_t* used, uint32_t* free) {
        (void)total; (void)used; (void)free;
        return false;
    }
};
#endif // ARCH_NRF52

