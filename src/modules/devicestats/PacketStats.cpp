#include "PacketStats.h"
#include "mesh/Router.h"

void PacketStats::getQueueStats(int& fromRadioUsed, int& fromRadioFree, Router* router)
{
    fromRadioUsed = 0;
    fromRadioFree = 0;

    if (!router) {
        return;
    }

    // Access queue status through Router's public interface
    auto queueStatus = router->getQueueStatus();
    // Calculate used from maxlen and free
    fromRadioUsed = (queueStatus.maxlen > queueStatus.free) ? (queueStatus.maxlen - queueStatus.free) : 0;
    fromRadioFree = queueStatus.free;
}

uint32_t PacketStats::getPacketHistorySize()
{
    // We can't access PacketHistory directly without modifying it,
    // so we return an estimate or 0. DeviceStatsModule can work without this.
    // If needed, this could be implemented by making PacketHistory a friend class,
    // but that would require modifying PacketHistory.h.
    return 0;
}
