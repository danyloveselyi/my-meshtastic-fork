#pragma once

#include <cstdint>

// Forward declarations
class Router;

/**
 * @brief Self-contained packet statistics collector
 *
 * Collects packet and queue statistics without modifying Router.h or PacketHistory.h.
 * Uses only public interfaces where possible.
 */
class PacketStats {
public:
    /**
     * @brief Get radio queue statistics
     * @param fromRadioUsed Output: number of used slots in fromRadio queue
     * @param fromRadioFree Output: number of free slots in fromRadio queue
     * @param router Router instance to query
     */
    static void getQueueStats(int& fromRadioUsed, int& fromRadioFree, Router* router);

    /**
     * @brief Get approximate packet history size
     * @return Estimated number of packets in recent history
     */
    static uint32_t getPacketHistorySize();
};
