#pragma once

#include <cstdint>

/**
 * @brief Self-contained memory statistics implementation
 *
 * This class provides platform-specific memory statistics without modifying
 * existing memGet.cpp/h files. Isolated for easier merging with upstream.
 */
class MemoryStats {
public:
    /**
     * @brief Get total heap size in bytes
     * @return Total heap size, or 0 if unavailable
     */
    static uint32_t getHeapTotal();

    /**
     * @brief Get free heap size in bytes
     * @return Free heap size, or 0 if unavailable
     */
    static uint32_t getHeapFree();

    /**
     * @brief Get used heap size in bytes
     * @return Used heap size (total - free)
     */
    static uint32_t getHeapUsed();

    /**
     * @brief Get total flash/ROM size in bytes
     * @return Total flash size, or 0 if unavailable
     */
    static uint32_t getFlashTotal();

    /**
     * @brief Get used flash/ROM size in bytes
     * @return Used flash size (firmware size), or 0 if unavailable
     */
    static uint32_t getFlashUsed();

    /**
     * @brief Get free flash/ROM size in bytes
     * @return Free flash size (total - used)
     */
    static uint32_t getFlashFree();

    /**
     * @brief Get PSRAM total size in bytes (ESP32-S3 only)
     * @return PSRAM total size, or 0 if unavailable/not supported
     */
    static uint32_t getPsramTotal();

    /**
     * @brief Get PSRAM free size in bytes (ESP32-S3 only)
     * @return PSRAM free size, or 0 if unavailable/not supported
     */
    static uint32_t getPsramFree();
};
