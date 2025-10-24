#pragma once
#include "Observer.h"
#include "SinglePortModule.h"

/**
 * Text message handling for meshtastic - draws on the OLED display the most recent received message
 */
class TextMessageModule : public SinglePortModule, public Observable<const meshtastic_MeshPacket *>
{
  public:
    /** Constructor
     * name is for debugging output
     */
    TextMessageModule() : SinglePortModule("text", meshtastic_PortNum_TEXT_MESSAGE_APP) {}

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

      // Monitoring variables
      uint32_t monitoringNodeId = 0;  // Node ID that requested monitoring (0 = disabled)
      uint32_t lastMonitorTime = 0;   // Last time we sent monitoring stats
      uint32_t monitorIntervalMs = 30000; // Monitor interval in milliseconds (default 30s)
      uint32_t monitorMessageCounter = 0; // Sequential counter for monitoring messages (safe overflow)
      uint32_t lastMemoryCheck = 0;   // Last memory check time (moved from static)

      // Interactive setup variables
      uint32_t waitingIntervalNodeId = 0; // Node ID waiting for interval input (0 = not waiting)
      uint32_t waitingMaxNodesNodeId = 0; // Node ID waiting for max nodes input (0 = not waiting)

  public:
      void doPeriodicWork(); // Called periodically to send monitoring updates
};

extern TextMessageModule *textMessageModule;