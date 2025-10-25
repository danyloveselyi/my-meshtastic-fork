#include <cctype>
#include <cstring>

// Helper: trim leading/trailing whitespace in-place
void trim(char* s) {
    if (!s) return;
    // Trim leading
    char* start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    // Trim trailing
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len-1])) s[--len] = 0;
}
#include "DeviceStatsModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "buzz.h"
#include "configuration.h"
#include "mesh/Router.h"
#include "memGet.h"
#include "mesh/mesh-pb-constants.h"
#include "variant.h"
#include "RadioLibInterface.h"
#include "airtime.h"
#include "modules/Telemetry/DeviceTelemetry.h"
#include "PowerStatus.h"
#include "mesh/PacketHistory.h"
#include <algorithm>

extern Router *router;

// dynamic_max_nodes defined in DynamicNodes.cpp with weak linkage
// Variants can override by defining in variant.cpp
extern const uint32_t DEFAULT_MAX_NODES;
extern uint32_t dynamic_max_nodes;

// Hardcoded PIN code for device stats module
static const char* MONITORING_PIN_CODE = "123456";

// Context variables for interactive commands (per user)
// For /setmaxnodes: waiting for nodes,pin
static uint32_t waitingSetMaxNodesNodeId = 0;
// For /monstart: waiting for interval,pin
static uint32_t waitingMonStartNodeId = 0;
// For /monstop: waiting for pin
static uint32_t waitingMonStopNodeId = 0;

// DEFAULT_MAX_NODES is defined per variant:
// - rak4631_eth_gw: extern const uint32_t DEFAULT_MAX_NODES = 350
// - rak4631: extern const uint32_t DEFAULT_MAX_NODES = 80
// Note: No fallback needed as all variants should define this

DeviceStatsModule *deviceStatsModule;

ProcessMessage DeviceStatsModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Check if module is enabled via RangeTest configuration toggle in phone app
    // This allows dynamic enable/disable without recompiling firmware
    if (!moduleConfig.range_test.enabled) {
        return ProcessMessage::CONTINUE; // Module disabled, let other modules handle
    }

#ifdef DEBUG_PORT
    auto &p = mp.decoded;
    LOG_INFO("Received text msg from=0x%0x, id=0x%x, msg=%.*s", mp.from, mp.id, p.payload.size, p.payload.bytes);
#endif

    powerFSM.trigger(EVENT_RECEIVED_MSG);
    // If this is a direct message (not a broadcast), send an auto-reply
    if (mp.to == nodeDB->getNodeNum() && mp.to != NODENUM_BROADCAST) {
        sendAutoReply(mp);
    }
    notifyObservers(&mp);

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}


void DeviceStatsModule::sendAutoReply(const meshtastic_MeshPacket &original)
{
    // Use fixed size buffers instead of std::string to avoid dynamic allocation
    char originalText[256];
    char lowerText[256];
    char replyBuffer[256];

    // Safely copy original text
    size_t textLen = std::min((size_t)original.decoded.payload.size, sizeof(originalText) - 1);
    memcpy(originalText, original.decoded.payload.bytes, textLen);
    originalText[textLen] = '\0';

    // Create lowercase copy for comparison
    strncpy(lowerText, originalText, sizeof(lowerText) - 1);
    lowerText[sizeof(lowerText) - 1] = '\0';
    for (size_t i = 0; lowerText[i]; i++) {
        lowerText[i] = tolower(lowerText[i]);
    }

    // Trim leading spaces
    char* trimmed = lowerText;
    while (*trimmed == ' ') trimmed++;

    // Check for command prefix '/'
    bool isCommand = (originalText[0] == '/');
    if (isCommand && *trimmed == '/') {
        trimmed++; // Skip the '/' for comparison
    }

    const char* replyText = nullptr;

    // Reset other waiting states when user starts a new command (to avoid conflicts)
    if (isCommand) {
        if (strcmp(trimmed, "setmaxnodes") != 0 && strcmp(trimmed, "set max nodes") != 0) {
            waitingSetMaxNodesNodeId = 0;
        }
        if (strcmp(trimmed, "monstart") != 0 && strcmp(trimmed, "mon start") != 0) {
            waitingMonStartNodeId = 0;
        }
        if (strcmp(trimmed, "monstop") != 0 && strcmp(trimmed, "mon stop") != 0) {
            waitingMonStopNodeId = 0;
        }
    }

    // Use a single static buffer to avoid stack overflow (instead of multiple 400-byte buffers on stack)
    static char commandBuffer[300];  // Static buffer shared by all commands
    
    // Check if this is a memory stats command
    if (isCommand && (strcmp(trimmed, "mem") == 0 || strcmp(trimmed, "memory") == 0)) {
        formatDetailedMemoryStats(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (strcmp(trimmed, "packets") == 0 || strcmp(trimmed, "packet") == 0)) {
        formatPacketStats(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (strcmp(trimmed, "power") == 0 || strcmp(trimmed, "battery") == 0)) {
        formatPowerStats(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (strcmp(trimmed, "radio") == 0 || strcmp(trimmed, "rf") == 0)) {
        formatRadioStats(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (strcmp(trimmed, "status") == 0 || strcmp(trimmed, "info") == 0)) {
        formatStatusInfo(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (strcmp(trimmed, "nodes") == 0 || strcmp(trimmed, "network") == 0)) {
        formatNodesInfo(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (strcmp(trimmed, "debug") == 0 || strcmp(trimmed, "dbg") == 0)) {
        formatDebugInfo(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (strcmp(trimmed, "mon start") == 0 || strcmp(trimmed, "monstart") == 0)) {
        // Start interactive setup for monitoring interval
        waitingMonStartNodeId = original.from;
        snprintf(replyBuffer, sizeof(replyBuffer),
            "Start monitoring. Enter interval (10-86400 sec) and PIN. Example: 30,1234");
        replyText = replyBuffer;
    } else if (isCommand && (strcmp(trimmed, "mon stop") == 0 || strcmp(trimmed, "monstop") == 0)) {
        // Start interactive PIN check for monitoring stop
        waitingMonStopNodeId = original.from;
        snprintf(replyBuffer, sizeof(replyBuffer),
            "Stop monitoring. Enter PIN. Example: 1234");
        replyText = replyBuffer;
    } else if (isCommand && (strcmp(trimmed, "maxnodes") == 0 || strcmp(trimmed, "max nodes") == 0)) {
        // Show current max nodes setting with safety info
        snprintf(replyBuffer, sizeof(replyBuffer), 
                 "Max nodes: %u (default: %u). Memory: ~%.1fKB. Send /setmaxnodes to increase.", 
                 dynamic_max_nodes, DEFAULT_MAX_NODES, (dynamic_max_nodes * 250) / 1024.0f);
        replyText = replyBuffer;
    } else if (isCommand && (strcmp(trimmed, "setmaxnodes") == 0 || strcmp(trimmed, "set max nodes") == 0)) {
        // Ask for max nodes value with safety warning
        waitingSetMaxNodesNodeId = original.from;
        snprintf(replyBuffer, sizeof(replyBuffer), 
            "Increase max nodes (%u-1000). Free heap: %.1fKB. Cannot decrease below default for safety. Enter nodes,pin. Example: 400,1234", 
            DEFAULT_MAX_NODES, memGet.getFreeHeap() / 1024.0f);
        replyText = replyBuffer;
    } else if (isCommand && (strcmp(trimmed, "help") == 0 || strcmp(trimmed, "?") == 0 || strcmp(trimmed, "commands") == 0)) {
        // Show available commands with safety note
        snprintf(replyBuffer, sizeof(replyBuffer), 
                 "📋 Commands:\n"
                 "/mem /packets /power\n"
                 "/radio /status /nodes\n"
                 "/debug /maxnodes\n"
                 "/setmaxnodes /monstart /monstop\n"
                 "Nodes=%u", 
                 dynamic_max_nodes);
        replyText = replyBuffer;
    } else if (waitingSetMaxNodesNodeId == original.from) {
        // User is setting max nodes value with PIN
        // Format: value,pin
        char* valueStr = strtok(originalText, ",");
        char* pinStr = strtok(nullptr, ",");
        int maxNodes = valueStr ? atoi(valueStr) : 0;
        if (pinStr) trim(pinStr);
        bool pinOk = pinStr && strcmp(pinStr, MONITORING_PIN_CODE) == 0;
        if (!pinOk) {
            waitingSetMaxNodesNodeId = 0;
            replyText = "Failed: PIN code incorrect.";
        } else if (maxNodes >= DEFAULT_MAX_NODES && maxNodes <= 1000) {
            // Check if we have enough memory for the increase
            uint32_t currentNodes = nodeDB ? nodeDB->meshNodes->size() : 0;
            uint32_t additionalMemory = (maxNodes - currentNodes) * 250; // ~250 bytes per node
            uint32_t freeHeap = memGet.getFreeHeap();
            
            if (freeHeap < additionalMemory + 4096) { // Keep 4KB safety margin
                waitingSetMaxNodesNodeId = 0;
                snprintf(replyBuffer, sizeof(replyBuffer), 
                         "Error: Not enough memory. Need %u bytes, have %u free. Current: %u nodes.", 
                         additionalMemory, freeHeap, currentNodes);
                replyText = replyBuffer;
            } else {
                dynamic_max_nodes = maxNodes;
                if (nodeDB && nodeDB->meshNodes) {
                    nodeDB->meshNodes->resize(dynamic_max_nodes);
                }
                waitingSetMaxNodesNodeId = 0;
                snprintf(replyBuffer, sizeof(replyBuffer), "Max nodes increased to %d. Memory usage: ~%.1fKB.", 
                         maxNodes, (maxNodes * 250) / 1024.0f);
                replyText = replyBuffer;
            }
        } else if (maxNodes < DEFAULT_MAX_NODES) {
            waitingSetMaxNodesNodeId = 0;
            snprintf(replyBuffer, sizeof(replyBuffer), 
                     "Error: Cannot set below DEFAULT_MAX_NODES (%u) nodes. Current: %u. Decreasing node limit can lose recent node data. Use reboot to reset to default.", 
                     DEFAULT_MAX_NODES, dynamic_max_nodes);
            replyText = replyBuffer;
        } else {
            replyText = "Invalid. Use 350-1000 (cannot decrease below default).";
        }
    } else if (waitingMonStartNodeId == original.from) {
        // User is setting up monitoring interval with PIN (monstart)
        // Format: value,pin
        char* valueStr = strtok(originalText, ",");
        char* pinStr = strtok(nullptr, ",");
        int interval = valueStr ? atoi(valueStr) : 0;
        if (pinStr) trim(pinStr);
        bool pinOk = pinStr && strcmp(pinStr, MONITORING_PIN_CODE) == 0;
        if (!pinOk) {
            waitingMonStartNodeId = 0;
            replyText = "Failed: PIN code incorrect.";
        } else if (interval >= 10 && interval <= 86400) {
            monitorIntervalMs = interval * 1000;
            monitoringNodeId = original.from;
            lastMonitorTime = millis();
            waitingMonStartNodeId = 0;
            monitorMessageCounter = 0;
            snprintf(replyBuffer, sizeof(replyBuffer),
                     "Monitor ON: %ds intervals. Send /monstop to disable.", interval);
            replyText = replyBuffer;
        } else {
            replyText = "Invalid interval! Use 10-86400 seconds.";
        }
    } else if (waitingMonStopNodeId == original.from) {
        // /monstop PIN check (expect only PIN)
        char* pinStr = originalText;
        trim(pinStr);
        bool pinOk = pinStr && strcmp(pinStr, MONITORING_PIN_CODE) == 0;
        if (pinOk) {
            monitoringNodeId = 0;
            waitingMonStopNodeId = 0;
            monitorMessageCounter = 0;
            replyText = "Memory monitoring stopped.";
        } else {
            // Do not reset waitingMonStopNodeId, allow retry
            replyText = "Failed: PIN code incorrect. Try again.";
        }
    }

    // If no replyText was set, send default auto-reply
    if (!replyText) {
        snprintf(replyBuffer, sizeof(replyBuffer), "Auto-reply: %.200s", originalText);
        replyText = replyBuffer;
    }

    meshtastic_MeshPacket *reply = router->allocForSending();
    if (!reply) {
        LOG_ERROR("Failed to allocate packet - memory exhausted! Resetting command states.");
        // Reset all waiting states to prevent hanging
        waitingSetMaxNodesNodeId = 0;
        waitingMonStartNodeId = 0;
        waitingMonStopNodeId = 0;
        return;
    }

    reply->to = original.from;
    reply->decoded.want_response = false;
    reply->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;

    size_t replyLen = strlen(replyText);
    reply->decoded.payload.size = std::min(replyLen, sizeof(reply->decoded.payload.bytes));
    memcpy(reply->decoded.payload.bytes, replyText, reply->decoded.payload.size);

    service->sendToMesh(reply, RX_SRC_LOCAL, true);

#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    LOG_INFO("Sent auto-reply to node 0x%0x", original.from);
#endif
}

void DeviceStatsModule::sendMemoryStats(uint32_t toNode)
{
    // Increment message counter for monitoring sequence (safe overflow)
    monitorMessageCounter++;
    // Counter will safely overflow from UINT32_MAX back to 0 after ~136 years at 30s intervals

    // Format monitoring message with counter in square brackets using detailed formatting
    char statsBuffer[400];
    char prefix[20];
    snprintf(prefix, sizeof(prefix), "[ %u ] ", monitorMessageCounter);
    formatDetailedMemoryStats(statsBuffer, sizeof(statsBuffer));
    
    // Add prefix to the detailed report
    char prefixedBuffer[450];
    snprintf(prefixedBuffer, sizeof(prefixedBuffer), "%s%s", prefix, statsBuffer);

    // Allocate packet with safety check for long-term monitoring
    meshtastic_MeshPacket *reply = router->allocForSending();
    if (!reply) {
        LOG_ERROR("Failed to allocate packet for memory stats - memory exhausted! Disabling monitoring.");
        // Disable monitoring to prevent further memory allocation attempts
        monitoringNodeId = 0;
        return;
    }

    reply->to = toNode;
    reply->decoded.want_response = false;
    reply->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;

    size_t msgLen = strlen(prefixedBuffer);
    reply->decoded.payload.size = std::min(msgLen, sizeof(reply->decoded.payload.bytes));
    memcpy(reply->decoded.payload.bytes, prefixedBuffer, reply->decoded.payload.size);

    service->sendToMesh(reply, RX_SRC_LOCAL, true);
}

void DeviceStatsModule::doPeriodicWork()
{
    // Check if module is enabled via RangeTest configuration toggle
    if (!moduleConfig.range_test.enabled) {
        return; // Module disabled, skip periodic work
    }

    uint32_t now = millis();

    // Check if monitoring is enabled and it's time to send update
    if (monitoringNodeId != 0) {
        // Handle millis() overflow safely (works for years of continuous operation)
        uint32_t elapsed = (now >= lastMonitorTime) ? (now - lastMonitorTime) : (UINT32_MAX - lastMonitorTime + now + 1);
        if (elapsed >= monitorIntervalMs) {
            // Additional safety check before sending
            if (memGet.getFreeHeap() < MINIMUM_SAFE_FREE_HEAP) {
                LOG_WARN("Skipping monitoring message due to low memory: %u bytes", memGet.getFreeHeap());
            } else {
                sendMemoryStats(monitoringNodeId);
            }
            lastMonitorTime = now;
        }
    }

    // Periodic memory monitoring every 5 minutes (moved from static for thread safety)
    uint32_t memElapsed = (now >= lastMemoryCheck) ? (now - lastMemoryCheck) : (UINT32_MAX - lastMemoryCheck + now + 1);
    if (memElapsed > 300000) { // 5 minutes = 300,000ms
        uint32_t freeHeap = memGet.getFreeHeap();
        if (freeHeap < MINIMUM_SAFE_FREE_HEAP * 2) {
            LOG_WARN("Low memory detected: %u bytes free (threshold: %u)", freeHeap, MINIMUM_SAFE_FREE_HEAP * 2);
            // Log additional diagnostics for long-term monitoring
            if (nodeDB) {
                uint32_t nodeCount = nodeDB->getNumMeshNodes();
                LOG_INFO("Memory diagnostics - Nodes: %u/%u, Monitoring: %s, Counter: %u", 
                        nodeCount, dynamic_max_nodes,
                        (monitoringNodeId != 0) ? "active" : "inactive",
                        monitorMessageCounter);
            }
        }
        lastMemoryCheck = now;
    }
}

bool DeviceStatsModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return MeshService::isTextPayload(p);
}

void DeviceStatsModule::formatMemoryStats(char* buffer, size_t bufferSize, const char* prefix)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 200) { // Increased buffer size for more info
        LOG_ERROR("Invalid buffer for memory stats formatting");
        return;
    }

    // Get memory and node statistics with safety checks
    float flashTotal = memGet.getFlashTotal() / 1024.0f; // KB
    float flashFree = memGet.getFlashFree() / 1024.0f;   // KB
    uint32_t heapTotal = memGet.getHeapSize() / 1024;    // KB
    float heapFree = memGet.getFreeHeap() / 1024.0f;     // KB
    
    // Protect against null nodeDB (can happen during shutdown)
    uint32_t totalNodes = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    uint32_t maxNodes = dynamic_max_nodes;  // Use dynamic value
    uint32_t freeSlots = (maxNodes > totalNodes) ? (maxNodes - totalNodes) : 0;

    // Calculate used amounts with overflow protection
    float flashUsed = (flashTotal >= flashFree) ? (flashTotal - flashFree) : 0.0f;
    uint32_t heapUsed = (heapTotal >= (uint32_t)heapFree) ? (heapTotal - (uint32_t)heapFree) : 0;

    // Format: total/used(free:amount) - safe for long-term operation
    int result = snprintf(buffer, bufferSize,
             "%sMem: Flash=%.0f/%.0f(free:%.0f)KB Heap=%u/%u(free:%.0f)KB Nodes=%u/%u(free:%u) Queues: Phone=%u Status=%u Notif=%u",
             prefix ? prefix : "",
             flashTotal, flashUsed, flashFree,
             heapTotal, heapUsed, heapFree,
             maxNodes, totalNodes, freeSlots,
             MAX_RX_TOPHONE, MAX_RX_TOPHONE, MAX_RX_TOPHONE/2);
             
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Memory stats message truncated");
    }
}

void DeviceStatsModule::formatDetailedMemoryStats(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR("Invalid buffer for detailed memory stats formatting");
        return;
    }

    // Get memory statistics with safety checks
    float flashTotal = memGet.getFlashTotal() / 1024.0f; // KB
    float flashFree = memGet.getFlashFree() / 1024.0f;   // KB
    uint32_t heapTotal = memGet.getHeapSize() / 1024;    // KB
    float heapFree = memGet.getFreeHeap() / 1024.0f;     // KB

    // Calculate used amounts with overflow protection
    float flashUsed = (flashTotal >= flashFree) ? (flashTotal - flashFree) : 0.0f;
    uint32_t heapUsed = (heapTotal >= (uint32_t)heapFree) ? (heapTotal - (uint32_t)heapFree) : 0;

    // Format detailed memory report (optimized - removed duplicate node info)
    int result = snprintf(buffer, bufferSize,
        "📊 MEMORY REPORT\n"
        "Flash: %.0f/%.0fKB (%.0fKB free)\n"
        "Heap: %u/%uKB (%.0fKB free)\n"
        "Queues: Phone=%u Status=%u Notif=%u\n"
        "Pool: ~22 packets (~11KB)",
        flashTotal, flashUsed, flashFree,
        heapTotal, heapUsed, heapFree,
        MAX_RX_TOPHONE, MAX_RX_TOPHONE, MAX_RX_TOPHONE/2);
        
    // Debug log to verify the message is being formatted correctly
    LOG_INFO("Formatted detailed memory stats: %s", buffer);
                 
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Detailed memory stats message truncated");
    }
}

void DeviceStatsModule::formatPacketStats(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR("Invalid buffer for packet stats formatting");
        return;
    }

    // Get packet statistics from RadioLibInterface
    uint32_t txGood = 0, rxGood = 0, rxBad = 0, txRelay = 0;
    float channelUtil = 0.0f, airUtilTx = 0.0f;
    
    if (RadioLibInterface::instance) {
        txGood = RadioLibInterface::instance->txGood;
        rxGood = RadioLibInterface::instance->rxGood;
        rxBad = RadioLibInterface::instance->rxBad;
        txRelay = RadioLibInterface::instance->txRelay;
    }
    
    // Get airtime statistics
    if (airTime) {
        channelUtil = airTime->channelUtilizationPercent();
        airUtilTx = airTime->utilizationTXPercent();
    }
    
    // Get router statistics
    uint32_t rxDupe = 0, txRelayCanceled = 0;
    if (router) {
        rxDupe = router->rxDupe;
        txRelayCanceled = router->txRelayCanceled;
    }
    
    // Get uptime using millis()
    uint32_t uptimeMs = millis();
    uint32_t uptime = uptimeMs / 1000;
    uint32_t uptimeHours = uptime / 3600;
    uint32_t uptimeMinutes = (uptime % 3600) / 60;
    
    // Calculate rates (packets per minute and per hour)
    float txRateMin = (uptime > 0) ? (txGood * 60.0f / uptime) : 0.0f;
    float txRateHour = (uptime > 0) ? (txGood * 3600.0f / uptime) : 0.0f;
    float rxRateMin = (uptime > 0) ? ((rxGood + rxBad) * 60.0f / uptime) : 0.0f;
    float rxRateHour = (uptime > 0) ? ((rxGood + rxBad) * 3600.0f / uptime) : 0.0f;

    // Format packet statistics report
    int result = snprintf(buffer, bufferSize,
        "📦 PACKET STATS\n"
        "TX: %u total (%.1f/min %.1f/hr)\n"
        "RX: %u good, %u bad\n"
        "RX Rate: %.1f/min %.1f/hr\n"
        "Relay: %u sent, %u canceled\n"
        "Duplicates: %u\n"
        "Channel: %.1f%% util\n"
        "Air TX: %.1f%% util\n"
        "Uptime: %uh %um",
        txGood, txRateMin, txRateHour,
        rxGood, rxBad,
        rxRateMin, rxRateHour,
        txRelay, txRelayCanceled,
        rxDupe,
        channelUtil, airUtilTx,
        uptimeHours, uptimeMinutes);
                 
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Packet stats message truncated");
    }
}

void DeviceStatsModule::formatPowerStats(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR("Invalid buffer for power stats formatting");
        return;
    }

    // Get power status information
    uint8_t batteryPercent = 0;
    uint16_t batteryVoltageMv = 0;
    const char* powerSource = "Unknown";
    const char* chargingStatus = "";
    
    if (powerStatus) {
        batteryPercent = powerStatus->getBatteryChargePercent();
        batteryVoltageMv = powerStatus->getBatteryVoltageMv();
        
        // Determine power source and charging status
        if (powerStatus->getHasUSB()) {
            powerSource = "USB";
            if (powerStatus->getHasBattery() && powerStatus->getIsCharging()) {
                chargingStatus = " (Charging)";
            } else if (powerStatus->getHasBattery()) {
                chargingStatus = " (Full)";
            }
        } else if (powerStatus->getHasBattery()) {
            powerSource = "Battery";
        }
    }
    
    // Format power statistics report
    int result = snprintf(buffer, bufferSize,
        "🔋 POWER STATUS\n"
        "Source: %s%s\n"
        "Battery: %u%% (%.2fV)\n"
        "Voltage: %u mV",
        powerSource, chargingStatus,
        batteryPercent, batteryVoltageMv / 1000.0f,
        batteryVoltageMv);
                 
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Power stats message truncated");
    }
}

void DeviceStatsModule::formatRadioStats(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR("Invalid buffer for radio stats formatting");
        return;
    }

    // Get radio configuration
    float frequency = 0.0f;
    uint8_t channel = 0;
    uint8_t sf = 0;
    float bw = 0.0f;
    int8_t power = 0;
    
    if (RadioLibInterface::instance) {
        frequency = RadioLibInterface::instance->getFreq();
        channel = RadioLibInterface::instance->getChannelNum();
        // Access radio configuration from config
        sf = config.lora.spread_factor;
        bw = config.lora.bandwidth;
        power = config.lora.tx_power;
    }
    
    // Get packet statistics for average rates
    uint32_t txGood = 0, rxGood = 0, rxBad = 0;
    if (RadioLibInterface::instance) {
        txGood = RadioLibInterface::instance->txGood;
        rxGood = RadioLibInterface::instance->rxGood;
        rxBad = RadioLibInterface::instance->rxBad;
    }
    
    // Calculate average packet rates
    uint32_t uptime = millis() / 1000;
    float txAvgMin = (uptime > 0) ? (txGood * 60.0f / uptime) : 0.0f;
    float txAvgHour = (uptime > 0) ? (txGood * 3600.0f / uptime) : 0.0f;
    float rxAvgMin = (uptime > 0) ? ((rxGood + rxBad) * 60.0f / uptime) : 0.0f;
    float rxAvgHour = (uptime > 0) ? ((rxGood + rxBad) * 3600.0f / uptime) : 0.0f;
    
    // Get online nodes count
    uint32_t onlineNodes = nodeDB ? nodeDB->getNumOnlineMeshNodes() : 0;
    
    // Format radio statistics report with packet averages and online nodes
    int result = snprintf(buffer, bufferSize,
        "📡 RADIO CONFIG\n"
        "Freq: %.3f MHz Ch: %u\n"
        "SF: %u BW: %.1f kHz\n"
        "Power: %d dBm\n"
        "Avg TX: %.1f/min %.1f/hr\n"
        "Avg RX: %.1f/min %.1f/hr\n"
        "Online nodes: %u",
        frequency, channel,
        sf, bw,
        power,
        txAvgMin, txAvgHour,
        rxAvgMin, rxAvgHour,
        onlineNodes);
                 
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Radio stats message truncated");
    }
}

void DeviceStatsModule::formatStatusInfo(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR("Invalid buffer for status info formatting");
        return;
    }

    // Get uptime
    uint32_t uptimeMs = millis();
    uint32_t uptime = uptimeMs / 1000;
    uint32_t uptimeDays = uptime / 86400;
    uint32_t uptimeHours = (uptime % 86400) / 3600;
    uint32_t uptimeMinutes = (uptime % 3600) / 60;
    
    // Get reboot counter
    uint32_t rebootCount = myNodeInfo.reboot_count;
    
    // Get node info
    const char* nodeName = owner.long_name;
    uint32_t nodeId = nodeDB->getNodeNum();
    
    // Format status information report
    int result = snprintf(buffer, bufferSize,
        "ℹ️ DEVICE STATUS\n"
        "Name: %s\n"
        "Node ID: !%08x\n"
        "Uptime: %ud %uh %um\n"
        "Reboots: %u\n"
        "Role: %s",
        nodeName,
        nodeId,
        uptimeDays, uptimeHours, uptimeMinutes,
        rebootCount,
        config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ? "Router" :
        config.device.role == meshtastic_Config_DeviceConfig_Role_REPEATER ? "Repeater" :
        config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT ? "Client" : "Unknown");
                 
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Status info message truncated");
    }
}

void DeviceStatsModule::formatNodesInfo(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR("Invalid buffer for nodes info formatting");
        return;
    }

    // Get node statistics with safety checks
    uint32_t onlineNodes = nodeDB ? nodeDB->getNumOnlineMeshNodes() : 0;
    uint32_t totalNodes = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    uint32_t maxNodes = dynamic_max_nodes;
    uint32_t offlineNodes = (totalNodes > onlineNodes) ? (totalNodes - onlineNodes) : 0;
    uint32_t freeSlots = (maxNodes > totalNodes) ? (maxNodes - totalNodes) : 0;
    
    // Calculate percentages
    float usagePercent = (maxNodes > 0) ? (totalNodes * 100.0f / maxNodes) : 0.0f;
    float onlinePercent = (totalNodes > 0) ? (onlineNodes * 100.0f / totalNodes) : 0.0f;
    
    // Format nodes information report
    int result = snprintf(buffer, bufferSize,
        "🌐 NETWORK NODES\n"
        "Online: %u (%.0f%%)\n"
        "Offline: %u\n"
        "Total: %u / %u (%.0f%%)\n"
        "Free slots: %u\n"
        "Default max: %u",
        onlineNodes, onlinePercent,
        offlineNodes,
        totalNodes, maxNodes, usagePercent,
        freeSlots,
        DEFAULT_MAX_NODES);
                 
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Nodes info message truncated");
    }
}

void DeviceStatsModule::formatDebugInfo(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR("Invalid buffer for debug info formatting");
        return;
    }

    // Get heap for context
    uint32_t freeHeap = memGet.getFreeHeap();
    uint32_t totalHeap = memGet.getHeapSize();
    uint32_t usedHeap = totalHeap - freeHeap;
    uint32_t heapPercent = (totalHeap > 0) ? (usedHeap * 100 / totalHeap) : 0;
    
    // Get actual node counts (not just max)
    uint32_t totalNodes = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    uint32_t onlineNodes = nodeDB ? nodeDB->getNumOnlineMeshNodes() : 0;
    
    // Calculate actual NodeDB memory usage
    uint32_t nodeDbBytes = totalNodes * 250;  // Real memory usage based on actual nodes
    
    // Get actual network queue usage (critical for mesh stability)
    int fromRadioUsed = router ? router->getFromRadioQueueSize() : 0;
    int fromRadioFree = router ? router->getFromRadioQueueFree() : 4;
    int fromRadioMax = 4;  // MAX_RX_FROMRADIO
    
    // Get TX queue status (network transmission queue)
    meshtastic_QueueStatus txStatus = router ? router->getQueueStatus() : meshtastic_QueueStatus{0, 0, 0, 0};
    int txUsed = (txStatus.maxlen > txStatus.free) ? (txStatus.maxlen - txStatus.free) : 0;
    int txMax = txStatus.maxlen > 0 ? txStatus.maxlen : 16;
    
    // Get PacketHistory (DupeCache) statistics
    uint32_t dupeCacheCount = 0;
    uint32_t oldestPacketAge = 0;
    
    if (router) {
        dupeCacheCount = router->getPacketCount();
        oldestPacketAge = router->getOldestPacketAge();
    }
    
    // Determine criticality based on oldest packet age and cache fullness
    const char* criticalityWarning = "";
    uint32_t cacheFullness = (dynamic_max_nodes > 0) ? (dupeCacheCount * 100 / dynamic_max_nodes) : 0;
    
    if (oldestPacketAge < 90 && dupeCacheCount > 200) {
        criticalityWarning = "\n⚠️ CRITICAL: Packets <90s evicted!";
    } else if (oldestPacketAge < 120 && dupeCacheCount > 230) {
        criticalityWarning = "\n⚠️ WARNING: DupeCache filling fast";
    } else if (cacheFullness > 85) {
        criticalityWarning = "\n⚠️ DupeCache >85% full";
    }
    
    // Format debug information report focusing on oldest packet age (most critical metric)
    int result = snprintf(buffer, bufferSize,
        "🔧 DEBUG INFO\n"
        "Heap: %u/%uKB (%u%%)\n"
        "Nodes: %u online / %u total\n"
        "NodeDB: ~%uKB (%u×250b)\n"
        "DupeCache: %u/%u pkts (%u%%)\n"
        "📌 Oldest packet: %us ago\n"
        "RX Q: %d/%d (free:%d)\n"
        "TX Q: %d/%d (free:%d)%s",
        usedHeap/1024, totalHeap/1024, heapPercent,
        onlineNodes, totalNodes,
        nodeDbBytes/1024, totalNodes,
        dupeCacheCount, dynamic_max_nodes, cacheFullness,
        oldestPacketAge,
        fromRadioUsed, fromRadioMax, fromRadioFree,
        txUsed, txMax, txStatus.free,
        criticalityWarning);
                 
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Debug info message truncated");
    }
}