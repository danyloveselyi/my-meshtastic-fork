/**
 * @file FSCommon-patches.h
 * @brief Patches for FSCommon.cpp functions to support extended filesystem
 * 
 * This file provides patches/wrappers for FSCommon.cpp functions that need
 * extended filesystem support. These patches are included in FSCommon.cpp
 * via conditional compilation to keep changes isolated to this variant.
 * 
 * Location: variants/rak4631_lite/filesystem/
 * Purpose: Isolate extended filesystem changes from main codebase
 */

#pragma once

#ifdef ARCH_NRF52
#ifdef USE_EXTENDED_FS_FOR_NODEDB

#include "configuration.h"
#include <vector>
#include "FSCommon.h"

// Forward declarations
extern void nrf52Loop();
// preFSBegin() is forward declared in FSCommon.cpp before this include
extern "C" void initExtendedFilesystemForNodeDB();  // From NodeDBExtendedFSImpl.h

/**
 * @brief Patched version of getFiles() with nrf52Loop() calls
 * 
 * This is a wrapper that adds nrf52Loop() calls to prevent watchdog timeouts
 * during long filesystem scans. This version replaces the standard getFiles()
 * when USE_EXTENDED_FS_FOR_NODEDB is enabled.
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
            yield();  // Allow other tasks to run
            nrf52Loop();  // Process SoftDevice events and feed watchdog
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
#ifdef ARCH_ESP32
            strcpy(fileInfo.file_name, file.path());
#else
            strcpy(fileInfo.file_name, file.name());
#endif
            if (!String(fileInfo.file_name).endsWith(".")) {
                filenames.push_back(fileInfo);
            }
            file.close();
        }
        file = root.openNextFile();
    }
    root.close();
    
    // CRITICAL: Final yield and nrf52Loop() after filesystem scan completes
    yield();
    nrf52Loop();  // Process any pending SoftDevice events and feed watchdog
#endif
    return filenames;
}

/**
 * @brief Patched version of fsInit() with extended filesystem initialization
 * 
 * This wrapper adds extended filesystem initialization before BLE setup.
 * The original function is made weak, and this version is used when
 * USE_EXTENDED_FS_FOR_NODEDB is enabled.
 */
void fsInit_patched()
{
#ifdef FSCom
    concurrency::LockGuard g(spiLock);
    preFSBegin();
    if (!FSBegin()) {
        LOG_ERROR("Filesystem mount failed");
        // assert(0); This auto-formats the partition, so no need to fail here.
    }
    // CRITICAL: Initialize extended filesystem EARLY, BEFORE BLE is enabled
    // This ensures all flash operations (formatting, structure creation) happen before BLE pairing
    // After initialization, only individual file operations will occur, which are much faster
    // and won't conflict with BLE bonding operations
    LOG_INFO("Initializing extended filesystem for NodeDB (BEFORE BLE setup)...");
    initExtendedFilesystemForNodeDB();
    LOG_INFO("Extended filesystem initialization complete");
#if defined(ARCH_ESP32)
    LOG_DEBUG("Filesystem files (%d/%d Bytes):", FSCom.usedBytes(), FSCom.totalBytes());
#else
    LOG_DEBUG("Filesystem files:");
#endif
    listDir("/", 10);
#endif
}

#endif // USE_EXTENDED_FS_FOR_NODEDB
#endif // ARCH_NRF52

