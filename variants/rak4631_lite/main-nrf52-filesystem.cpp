/**
 * @file main-nrf52-filesystem.cpp
 * @brief Extended filesystem configuration for RAK4631 Lite variant
 * 
 * This file provides extended filesystem support (80 pages, 320 KB) for RAK4631 Lite.
 * It overrides the preFSBegin() function for basic corruption handling.
 * 
 * Memory Layout:
 * - Application: 0x27000 - 0x80000 (356 KB)
 * - Filesystem: 0x80000 - 0xD0000 (80 pages, 320 KB)
 * - Bootloader: 0xF4000 - 0xFFFFF (48 KB)
 * 
 * Note: Filesystem version checking and formatting is handled in
 * variants/rak4631_lite/FSCommon-fallback.cpp (fsInitExtended()).
 * This avoids double mounting issues.
 */

#include "configuration.h"
#include "InternalFileSystem.h"
#include "softdevice/nrf_soc.h"
#include "error.h"
#include "main.h"
#include <Adafruit_LittleFS.h>

// Only compile this for RAK4631 Lite variant
#ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM

// Override preFSBegin() with basic corruption handling
void preFSBegin()
{
    // The GPREGRET register keeps its value across warm boots. Check that this is a warm boot and, if GPREGRET
    // is set to NRF52_MAGIC_LFS_IS_CORRUPT, format LittleFS.
    constexpr uint8_t NRF52_MAGIC_LFS_IS_CORRUPT = 0xF5;
    constexpr uint32_t MULTIPLE_CORRUPTION_DELAY_MILLIS = 20 * 60 * 1000;
    static unsigned long millis_until_formatting_again = 0;

    if (NRF_POWER->RESETREAS == 0 && NRF_POWER->GPREGRET == NRF52_MAGIC_LFS_IS_CORRUPT) {
        NRF_POWER->GPREGRET = 0;
        millis_until_formatting_again = millis() + MULTIPLE_CORRUPTION_DELAY_MILLIS;
        InternalFS.format();
        LOG_INFO("LittleFS format complete; restoring default settings");
        return;
    }

    // Note: Filesystem version checking is done in fsInitExtended() AFTER mounting
    // This avoids double mounting (preFSBegin() is called BEFORE fsInit() -> FSBegin())
    // See variants/rak4631_lite/FSCommon-fallback.cpp for version checking logic
}

#endif // RAK_4631_LITE_EXTENDED_FILESYSTEM
