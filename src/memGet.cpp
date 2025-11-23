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
#include <cstddef>

#ifdef ARCH_NRF52
#include "freertosinc.h"
#include <nrf.h>

extern "C" void* _sbrk(int incr);

extern char __HeapBase;
extern char __HeapLimit;
extern uint8_t __flash_arduino_start[];
extern uint8_t __flash_arduino_end[];

static uint32_t nrf52HeapTotalBytes()
{
    char* heapBase = &__HeapBase;
    char* heapLimit = &__HeapLimit;
    if (heapLimit <= heapBase) {
        return 0;
    }
    return static_cast<uint32_t>(heapLimit - heapBase);
}

static uint32_t nrf52HeapFreeBytes()
{
    uint32_t total = nrf52HeapTotalBytes();
    if (total == 0) {
        return 0;
    }

    char* heapBase = &__HeapBase;
    char* heapLimit = &__HeapLimit;
    char* currentBreak = static_cast<char*>(_sbrk(0));

    if (currentBreak == reinterpret_cast<char*>(-1) || currentBreak == nullptr) {
        currentBreak = heapBase;
    }

    if (currentBreak < heapBase) {
        currentBreak = heapBase;
    } else if (currentBreak > heapLimit) {
        currentBreak = heapLimit;
    }

    uint32_t used = static_cast<uint32_t>(currentBreak - heapBase);
    if (used > total) {
        used = total;
    }

    return total - used;
}
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
    uint32_t freeHeap = nrf52HeapFreeBytes();
    if (freeHeap == 0) {
        // Fallback to conservative estimate if computation failed
        return 150 * 1024;
    }
    return freeHeap;
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
    uint32_t total = nrf52HeapTotalBytes();
    if (total == 0) {
        // Fallback to conservative estimate if computation failed
        return 200 * 1024;
    }
    return total;
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

uint32_t MemGet::getFlashTotal()
{
#ifdef ARCH_ESP32
    return ESP.getFlashChipSize();
#elif defined(ARCH_NRF52)
    // nRF52 exposes flash geometry via FICR registers
    return NRF_FICR->CODEPAGESIZE * NRF_FICR->CODESIZE;
#elif defined(ARCH_RP2040)
#ifdef PICO_FLASH_SIZE_BYTES
    return PICO_FLASH_SIZE_BYTES;
#else
    return 0;
#endif
#else
    return 0;
#endif
}

uint32_t MemGet::getFlashUsed()
{
#ifdef ARCH_ESP32
    uint32_t total = getFlashTotal();
    uint32_t freeSpace = ESP.getFreeSketchSpace();
    if (total == 0 || freeSpace > total) {
        return ESP.getSketchSize();
    }
    return total - freeSpace;
#elif defined(ARCH_NRF52)
    return static_cast<uint32_t>(__flash_arduino_end - __flash_arduino_start);
#elif defined(ARCH_RP2040)
    return 0;
#else
    return 0;
#endif
}

uint32_t MemGet::getFlashFree()
{
#ifdef ARCH_ESP32
    return ESP.getFreeSketchSpace();
#else
    uint32_t total = getFlashTotal();
    uint32_t used = getFlashUsed();
    if (total == 0 || used >= total) {
        return (total > used) ? (total - used) : 0;
    }
    return total - used;
#endif
}