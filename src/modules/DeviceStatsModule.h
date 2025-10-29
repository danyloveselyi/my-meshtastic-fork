#pragma once
#include "Observer.h"
#include "SinglePortModule.h"

/**
 * Text message handling for meshtastic - draws on the OLED display the most recent received message
 */
class DeviceStatsModule : public SinglePortModule, public Observable<const meshtastic_MeshPacket *>
{
  public:
    /** Constructor
     * name is for debugging output
     */
    DeviceStatsModule() : SinglePortModule("text", meshtastic_PortNum_TEXT_MESSAGE_APP) {}

  protected:
    /** Called to handle a particular incoming message

    @return ProcessMessage::STOP if you've guaranteed you've handled this message and no other handlers should be considered for
    it
    */
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;

  private:
      void sendAutoReply(const meshtastic_MeshPacket &original);
      void sendMemoryStats(uint32_t toNode);
      void formatMemoryStats(char* buffer, size_t bufferSize, const char* prefix = "");
      void formatDetailedMemoryStats(char* buffer, size_t bufferSize);
      void formatPacketStats(char* buffer, size_t bufferSize);
      void formatPowerStats(char* buffer, size_t bufferSize);
      void formatRadioStats(char* buffer, size_t bufferSize);
      void formatStatusInfo(char* buffer, size_t bufferSize);
      void formatNodesInfo(char* buffer, size_t bufferSize);
      void formatDebugInfo(char* buffer, size_t bufferSize);

      // Monitoring variables
      uint32_t monitoringNodeId = 0;  // Node ID that requested monitoring (0 = disabled)
      uint32_t lastMonitorTime = 0;   // Last time we sent monitoring stats
      uint32_t monitorIntervalMs = 30000; // Monitor interval in milliseconds (default 30s)
      uint32_t monitorMessageCounter = 0; // Sequential counter for monitoring messages (safe overflow)
      uint32_t lastMemoryCheck = 0;   // Last memory check time (moved from static)

      // Interactive setup variables
      uint32_t waitingIntervalNodeId = 0; // Node ID waiting for interval input (0 = not waiting)
      uint32_t waitingMaxNodesNodeId = 0; // Node ID waiting for max nodes input (0 = not waiting)
      
      // Packet statistics tracking (private internals)
      uint32_t lastMinuteReset = 0;   // Timestamp of last minute reset
      uint32_t lastHourReset = 0;     // Timestamp of last hour reset
      uint32_t lastTxCount = 0;       // Previous TX count for delta calculation
      uint32_t lastRxCount = 0;       // Previous RX count for delta calculation

  public:
      void doPeriodicWork(); // Called periodically to send monitoring updates
      
      // Packet statistics for last minute/hour (public for formatPacketStats access)
      uint32_t txLastMinute = 0;      // TX packets in last minute
      uint32_t rxLastMinute = 0;      // RX packets in last minute
      uint32_t txLastHour = 0;        // TX packets in last hour
      uint32_t rxLastHour = 0;        // RX packets in last hour
};

extern DeviceStatsModule *deviceStatsModule;