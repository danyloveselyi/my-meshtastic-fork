/**
 * @file FSCommon-fallback.cpp
 * @brief Extended filesystem fallback logic for RAK4631 Lite variant
 * 
 * This file provides fallback logic for filesystem version file creation
 * when filesystem is auto-formatted by InternalFileSystem::begin().
 */

#include "FSCommon.h"
#include "configuration.h"
#include <cstdio>
#include <cstring>

// Only compile this for RAK4631 Lite variant
#ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM

#if defined(ARCH_NRF52)
// After mount succeeds, ensure version file exists (in case mount failed initially in preFSBegin)
// This is a fallback: if preFSBegin() couldn't mount and InternalFileSystem::begin() auto-formatted,
// the version file may not exist. Check and create it if needed.
// Note: preFSBegin() handles version checking and formatting for size changes, this is just a safety check.
void fsInitExtended()
{
    auto versionFile = FSCom.open("/.lfs_size_version", FILE_O_READ);
    if (!versionFile) {
        // Version file doesn't exist, write it (filesystem was just formatted by InternalFileSystem::begin())
        auto writeFile = FSCom.open("/.lfs_size_version", FILE_O_WRITE);
        if (writeFile) {
            // Current NRF52 filesystem version: 80 pages (320 KB) - matches LFS_SIZE_VERSION in main-nrf52-filesystem.cpp
            constexpr uint32_t NRF52_LFS_VERSION_PAGES = 80;
            char versionStr[16];
            snprintf(versionStr, sizeof(versionStr), "%lu", (unsigned long)NRF52_LFS_VERSION_PAGES);
            writeFile.write((uint8_t*)versionStr, strlen(versionStr));
            writeFile.close();
            LOG_INFO("LittleFS: size version file created (%d pages)", NRF52_LFS_VERSION_PAGES);
        }
    } else {
        versionFile.close();
    }
}
#endif

#endif // RAK_4631_LITE_EXTENDED_FILESYSTEM

