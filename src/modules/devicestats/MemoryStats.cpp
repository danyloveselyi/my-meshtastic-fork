#include "MemoryStats.h"
#include "configuration.h"
#include "memGet.h" // For fallback to existing memGet if needed

// Platform-specific includes
#ifdef ARCH_NRF52
#include "freertosinc.h"
#include <nrf.h>
extern "C" void* _sbrk(int incr);
extern char __HeapBase;
extern char __HeapLimit;
extern uint8_t __flash_arduino_start[];
extern uint8_t __flash_arduino_end[];
#endif

#ifdef ARCH_ESP32
#include <esp_system.h>
#include <esp_heap_caps.h>
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include <esp_psram.h>
#endif
#endif

#ifdef ARCH_RP2040
#include <hardware/flash.h>
extern char __flash_binary_start;
extern char __flash_binary_end;
#endif

// ============================================================================
// NRF52 Implementation
// ============================================================================

#ifdef ARCH_NRF52

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

static uint32_t nrf52FlashUsedBytes()
{
    // Calculate actual firmware size from linker symbols
    uintptr_t flashStart = reinterpret_cast<uintptr_t>(__flash_arduino_start);
    uintptr_t flashEnd = reinterpret_cast<uintptr_t>(__flash_arduino_end);

    if (flashEnd > flashStart) {
        return static_cast<uint32_t>(flashEnd - flashStart);
    }

    return 0;
}

uint32_t MemoryStats::getHeapTotal()
{
    return nrf52HeapTotalBytes();
}

uint32_t MemoryStats::getHeapFree()
{
    return nrf52HeapFreeBytes();
}

uint32_t MemoryStats::getFlashTotal()
{
    // NRF52840 has 1MB flash
    return 1024 * 1024;
}

uint32_t MemoryStats::getFlashUsed()
{
    return nrf52FlashUsedBytes();
}

#endif // ARCH_NRF52

// ============================================================================
// ESP32 Implementation
// ============================================================================

#ifdef ARCH_ESP32

uint32_t MemoryStats::getHeapTotal()
{
    return ESP.getHeapSize();
}

uint32_t MemoryStats::getHeapFree()
{
    return ESP.getFreeHeap();
}

uint32_t MemoryStats::getFlashTotal()
{
    return ESP.getFlashChipSize();
}

uint32_t MemoryStats::getFlashUsed()
{
    return ESP.getSketchSize();
}

uint32_t MemoryStats::getPsramTotal()
{
#ifdef CONFIG_IDF_TARGET_ESP32S3
    return ESP.getPsramSize();
#else
    return 0;
#endif
}

uint32_t MemoryStats::getPsramFree()
{
#ifdef CONFIG_IDF_TARGET_ESP32S3
    return ESP.getFreePsram();
#else
    return 0;
#endif
}

#endif // ARCH_ESP32

// ============================================================================
// RP2040 Implementation
// ============================================================================

#ifdef ARCH_RP2040

extern "C" char* sbrk(int incr);

uint32_t MemoryStats::getHeapTotal()
{
    // RP2040 has 264KB SRAM
    return 264 * 1024;
}

uint32_t MemoryStats::getHeapFree()
{
    char* heapEnd = sbrk(0);
    return static_cast<uint32_t>(&__StackLimit - heapEnd);
}

uint32_t MemoryStats::getFlashTotal()
{
    // Typically 2MB or 16MB depending on board
    return PICO_FLASH_SIZE_BYTES;
}

uint32_t MemoryStats::getFlashUsed()
{
    uintptr_t start = reinterpret_cast<uintptr_t>(&__flash_binary_start);
    uintptr_t end = reinterpret_cast<uintptr_t>(&__flash_binary_end);
    return static_cast<uint32_t>(end - start);
}

#endif // ARCH_RP2040

// ============================================================================
// Common implementations (derived values)
// ============================================================================

uint32_t MemoryStats::getHeapUsed()
{
    uint32_t total = getHeapTotal();
    uint32_t free = getHeapFree();
    return (total > free) ? (total - free) : 0;
}

uint32_t MemoryStats::getFlashFree()
{
    uint32_t total = getFlashTotal();
    uint32_t used = getFlashUsed();
    return (total > used) ? (total - used) : 0;
}

// Default implementations for platforms without PSRAM
#ifndef ARCH_ESP32
uint32_t MemoryStats::getPsramTotal()
{
    return 0;
}

uint32_t MemoryStats::getPsramFree()
{
    return 0;
}
#endif
