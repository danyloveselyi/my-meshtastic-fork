#pragma once

#include "ProtobufModule.h"

/**
 * Memory monitoring module for continuous RAM/Flash tracking
 * Useful for debugging memory leaks and optimization
 */
class MemoryMonitorModule : public ProtobufModule<meshtastic_Telemetry>
{
  public:
    MemoryMonitorModule();

    void printMemoryStats();
    void printDetailedMemoryStats();
    void startContinuousMonitoring(uint32_t intervalMs = 30000); // default 30 sec
    void stopContinuousMonitoring();

  protected:
    virtual int32_t runOnce();
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *decoded) override;

  private:
    uint32_t lastMemoryCheck = 0;
    uint32_t monitoringInterval = 0; // 0 = disabled
    uint32_t minHeapSeen = UINT32_MAX;
    uint32_t maxHeapUsed = 0;
    bool monitoringEnabled = false;
};

extern MemoryMonitorModule *memoryMonitorModule;