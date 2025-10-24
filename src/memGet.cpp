/**
 * @file memGet.cpp
 * @brief Implementation of MemGet class that provides functions to get memory information.
 *
 * This file contains the implementation of MemGet class that provides functions to get
 * information about free heap, heap size, free psram and psram size. The functions are
 * implemented for ESP32 and NRF52 architectures. If the platform does not have heap
 * management function implemented, the functions return UINT32_MAX or 0.
 */
#include "memGet.h"
#include "configuration.h"

#ifdef ARCH_NRF52
#include "freertosinc.h"
#endif

MemGet memGet;

/**
 * Returns the amount of free heap memory in bytes.
 * @return uint32_t The amount of free heap memory in bytes.
 */
uint32_t MemGet::getFreeHeap()
{
#ifdef ARCH_ESP32
    return ESP.getFreeHeap();
#elif defined(ARCH_NRF52)
#ifdef HAS_FREE_RTOS
    // Use FreeRTOS heap functions for nRF52
    return xPortGetFreeHeapSize();
#else
    // Fallback for non-FreeRTOS builds
    return UINT32_MAX;
#endif
#elif defined(ARCH_RP2040)
    return rp2040.getFreeHeap();
#else
    // this platform does not have heap management function implemented
    return UINT32_MAX;
#endif
}

/**
 * Returns the size of the heap memory in bytes.
 * @return uint32_t The size of the heap memory in bytes.
 */
uint32_t MemGet::getHeapSize()
{
#ifdef ARCH_ESP32
    return ESP.getHeapSize();
#elif defined(ARCH_NRF52)
#ifdef HAS_FREE_RTOS
    // For nRF52, we need to calculate total heap size
    // This is a simplified approach - in practice, you might want to store the initial heap size
    static uint32_t initialHeapSize = 0;
    if (initialHeapSize == 0) {
        // First call - get actual heap size by measuring free + used
        uint32_t freeHeap = xPortGetFreeHeapSize();
        // nRF52840 typically has ~256KB RAM, with some used by system
        // Estimate total heap as free + some used space
        initialHeapSize = freeHeap + (50 * 1024); // Add 50KB for used space estimate
    }
    return initialHeapSize;
#else
    // Fallback for non-FreeRTOS builds
    return UINT32_MAX;
#endif
#elif defined(ARCH_RP2040)
    return rp2040.getTotalHeap();
#else
    // this platform does not have heap management function implemented
    return UINT32_MAX;
#endif
}

/**
 * Returns the amount of free psram memory in bytes.
 *
 * @return The amount of free psram memory in bytes.
 */
uint32_t MemGet::getFreePsram()
{
#ifdef ARCH_ESP32
    return ESP.getFreePsram();
#elif defined(ARCH_PORTDUINO)
    return 4194252;
#else
    return 0;
#endif
}

/**
 * @brief Returns the size of the PSRAM memory.
 *
 * @return uint32_t The size of the PSRAM memory.
 */
uint32_t MemGet::getPsramSize()
{
#ifdef ARCH_ESP32
    return ESP.getPsramSize();
#elif defined(ARCH_PORTDUINO)
    return 4194252;
#else
    return 0;
#endif
}

/**
 * @brief Returns the total Flash memory size in bytes.
 *
 * @return uint32_t Total Flash memory size.
 */
uint32_t MemGet::getFlashTotal()
{
#ifdef ARCH_ESP32
    return ESP.getFlashChipSize();
#elif defined(ARCH_NRF52)
    // Flash size for nRF52840: usable flash 0xED000 - 0x27000 = 815104 bytes
    return 815104;
#elif defined(ARCH_RP2040)
    return 2 * 1024 * 1024; // 2MB typical
#elif defined(ARCH_PORTDUINO)
    return 0;
#else
    return 0;
#endif
}

/**
 * @brief Returns the used Flash memory size in bytes.
 *
 * @return uint32_t Used Flash memory size.
 */
uint32_t MemGet::getFlashUsed()
{
#ifdef ARCH_ESP32
    return ESP.getSketchSize();
#elif defined(ARCH_NRF52)
    // For nRF52, calculate program size from linker symbols
    extern uint32_t __etext;
    extern uint32_t __data_start__;
    extern uint32_t __data_end__;

    // Calculate used flash: code section + initialized data
    uint32_t codeSize = (uint32_t)&__etext - 0x27000; // From flash start to end of text
    uint32_t dataSize = (uint32_t)&__data_end__ - (uint32_t)&__data_start__;
    return codeSize + dataSize;
#elif defined(ARCH_RP2040)
    return 0; // Not easily available
#elif defined(ARCH_PORTDUINO)
    return 0;
#else
    return 0;
#endif
}

/**
 * @brief Returns the free Flash memory size in bytes.
 *
 * @return uint32_t Free Flash memory size.
 */
uint32_t MemGet::getFlashFree()
{
    uint32_t total = getFlashTotal();
    uint32_t used = getFlashUsed();
    return (total > used) ? (total - used) : 0;
}