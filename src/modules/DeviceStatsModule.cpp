#include <cctype>
#include <cstring>

// Helper: trim leading/trailing whitespace in-place
// Embedded-safe version with bounded string operations
#define MAX_TRIM_LENGTH 512  // Safety limit for string operations

void trim(char* s) {
    if (!s) return;
    // Trim leading
    char* start = s;
    size_t max_scan = MAX_TRIM_LENGTH;
    while (*start && isspace((unsigned char)*start) && max_scan-- > 0) start++;
    
    // Use strnlen instead of strlen for safety (prevents unbounded memory reads)
    size_t len = strnlen(start, MAX_TRIM_LENGTH);
    if (start != s) memmove(s, start, len + 1);
    
    // Trim trailing
    len = strnlen(s, MAX_TRIM_LENGTH);
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
#include "RTC.h"
#include <algorithm>
#include <time.h>

extern Router *router;

// dynamic_max_nodes defined in DynamicNodes.cpp with weak linkage
// Variants can override by defining in variant.cpp
extern const uint32_t DEFAULT_MAX_NODES;
extern uint32_t dynamic_max_nodes;

// Hardcoded PIN code for device stats module
static const char* MONITORING_PIN_CODE = "123456";

// Context variables for interactive commands (per user)
// Marked as volatile for RTOS thread-safety (prevents compiler optimization issues)
// For /setmaxnodes: waiting for nodes,pin
static volatile uint32_t waitingSetMaxNodesNodeId = 0;
// For /monstart: waiting for interval,pin
static volatile uint32_t waitingMonStartNodeId = 0;
// For /monstop: waiting for pin
static volatile uint32_t waitingMonStopNodeId = 0;

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
    // Use static buffers to avoid stack overflow on embedded systems (768 bytes is too much for stack)
    // Static buffers are thread-safe here because this function is called from message handler
    static char originalText[256];
    static char lowerText[256];
    static char replyBuffer[256];

    // Critical: validate payload pointer before memcpy (embedded safety)
    if (!original.decoded.payload.bytes || original.decoded.payload.size == 0) {
        LOG_ERROR("Invalid payload - bytes is NULL or size is 0");
        return;
    }

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
                 "📋 COMMANDS\n"
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
        // Use strtok_r for thread-safety in RTOS environment
        char* saveptr = nullptr;
        char* valueStr = strtok_r(originalText, ",", &saveptr);
        char* pinStr = strtok_r(nullptr, ",", &saveptr);
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
                    // Reserve capacity without changing size (avoid creating empty nodes)
                    nodeDB->meshNodes->reserve(dynamic_max_nodes);
                }
                waitingSetMaxNodesNodeId = 0;
                snprintf(replyBuffer, sizeof(replyBuffer), "Max nodes capacity increased to %d. Reserved: ~%.1fKB.", 
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
        // Use strtok_r for thread-safety in RTOS environment
        char* saveptr = nullptr;
        char* valueStr = strtok_r(originalText, ",", &saveptr);
        char* pinStr = strtok_r(nullptr, ",", &saveptr);
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

    // Critical: check router pointer before use (embedded safety)
    if (!router) {
        LOG_ERROR("Router is NULL - cannot send reply!");
        // Reset all waiting states to prevent hanging
        waitingSetMaxNodesNodeId = 0;
        waitingMonStartNodeId = 0;
        waitingMonStopNodeId = 0;
        return;
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
    // Use static buffers to avoid stack overflow (850 bytes is too much for embedded stack)
    static char statsBuffer[400];
    static char prefixedBuffer[450];
    char prefix[20];  // Keep small buffer on stack
    snprintf(prefix, sizeof(prefix), "[ %u ] ", monitorMessageCounter);
    formatDetailedMemoryStats(statsBuffer, sizeof(statsBuffer));
    
    // Add prefix to the detailed report
    snprintf(prefixedBuffer, sizeof(prefixedBuffer), "%s%s", prefix, statsBuffer);

    // Critical: check router pointer before use (embedded safety)
    if (!router) {
        LOG_ERROR("Router is NULL - cannot send memory stats!");
        monitoringNodeId = 0;  // Disable monitoring
        return;
    }

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

    // Update packet statistics (minute/hour counters)
    if (RadioLibInterface::instance) {
        uint32_t currentTx = RadioLibInterface::instance->txGood;
        uint32_t currentRx = RadioLibInterface::instance->rxGood + RadioLibInterface::instance->rxBad;
        
        // Initialize on first run
        if (lastMinuteReset == 0) {
            lastMinuteReset = now;
            lastHourReset = now;
            lastTxCount = currentTx;
            lastRxCount = currentRx;
        }
        
        // Calculate deltas since last update
        uint32_t txDelta = (currentTx >= lastTxCount) ? (currentTx - lastTxCount) : 0;
        uint32_t rxDelta = (currentRx >= lastRxCount) ? (currentRx - lastRxCount) : 0;
        
        // Add to minute and hour counters
        txLastMinute += txDelta;
        rxLastMinute += rxDelta;
        txLastHour += txDelta;
        rxLastHour += rxDelta;
        
        // Reset minute counter every 60 seconds
        if (now - lastMinuteReset >= 60000) {
            txLastMinute = txDelta; // Keep only current delta
            rxLastMinute = rxDelta;
            lastMinuteReset = now;
        }
        
        // Reset hour counter every 3600 seconds
        if (now - lastHourReset >= 3600000) {
            txLastHour = txDelta; // Keep only current delta
            rxLastHour = rxDelta;
            lastHourReset = now;
        }
        
        // Update last counts for next delta calculation
        lastTxCount = currentTx;
        lastRxCount = currentRx;
    }

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

    // Get memory statistics with safety checks and validation
    float flashTotal = memGet.getFlashTotal() / 1024.0f; // KB
    float flashFree = memGet.getFlashFree() / 1024.0f;   // KB
    uint32_t heapTotal = memGet.getHeapSize() / 1024;    // KB
    float heapFree = memGet.getFreeHeap() / 1024.0f;     // KB
    
    // Validate memory values (check for reasonable ranges)
    bool memValid = true;
    if (flashTotal > 1000000.0f || flashFree > flashTotal || flashTotal <= 0.0f) {
        // Invalid flash values (> 1GB or negative)
        memValid = false;
        flashTotal = 0.0f;
        flashFree = 0.0f;
    }
    if (heapTotal > 100000 || heapFree > heapTotal || heapTotal == 0) {
        // Invalid heap values (> 100MB or inconsistent)
        memValid = false;
        heapTotal = 0;
        heapFree = 0.0f;
    }
    
    // Get node statistics
    uint32_t totalNodes = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    uint32_t maxNodes = dynamic_max_nodes;

    // Calculate used amounts with overflow protection
    float flashUsed = (flashTotal >= flashFree) ? (flashTotal - flashFree) : 0.0f;
    uint32_t heapUsed = (heapTotal >= (uint32_t)heapFree) ? (heapTotal - (uint32_t)heapFree) : 0;
    uint32_t freeSlots = (maxNodes > totalNodes) ? (maxNodes - totalNodes) : 0;

    // Format detailed memory report (optimized - no duplication)
    int result;
    if (memValid) {
        result = snprintf(buffer, bufferSize,
            "📊 MEM\n"
            "Flash: %.0f/%.0fKB\n"
            "Heap: %u/%uKB\n"
            "Nodes: %u/%u\n"
            "Q: %u Pool: ~22",
            flashTotal, flashUsed,
            heapTotal, heapUsed,
            totalNodes, maxNodes,
            MAX_RX_TOPHONE);
    } else {
        // Fallback if memory stats are invalid
        result = snprintf(buffer, bufferSize,
            "📊 MEM\n"
            "Nodes: %u/%u\n"
            "⚠️ Stats unavailable",
            totalNodes, maxNodes);
    }
        
    // Debug log to verify the message is being formatted correctly
    LOG_INFO("MemoryStats: valid=%d flash=%.0f heap=%u nodes=%u", 
             memValid, flashTotal, heapTotal, totalNodes);
                 
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

    // Get packet statistics from RadioLibInterface with validation
    uint32_t txGood = 0, rxGood = 0, rxBad = 0, txRelay = 0;
    float channelUtil = 0.0f, airUtilTx = 0.0f;
    bool radioValid = false;
    
    if (RadioLibInterface::instance) {
        txGood = RadioLibInterface::instance->txGood;
        rxGood = RadioLibInterface::instance->rxGood;
        rxBad = RadioLibInterface::instance->rxBad;
        txRelay = RadioLibInterface::instance->txRelay;
        radioValid = true;
    }
    
    // Get airtime statistics with validation
    bool airtimeValid = false;
    if (airTime) {
        channelUtil = airTime->channelUtilizationPercent();
        airUtilTx = airTime->utilizationTXPercent();
        // Validate airtime values (check for NaN or negative values)
        if (channelUtil >= 0.0f && channelUtil <= 100.0f && 
            airUtilTx >= 0.0f && airUtilTx <= 100.0f) {
            airtimeValid = true;
        } else {
            // Reset to zero if invalid
            channelUtil = 0.0f;
            airUtilTx = 0.0f;
        }
    }
    
    // Get router statistics with validation
    uint32_t rxDupe = 0, txRelayCanceled = 0;
    bool routerValid = false;
    if (router) {
        rxDupe = router->rxDupe;
        txRelayCanceled = router->txRelayCanceled;
        routerValid = true;
    }
    
    // Calculate success rate
    uint32_t rxTotal = rxGood + rxBad;
    float rxSuccessRate = (rxTotal > 0) ? (rxGood * 100.0f / rxTotal) : 0.0f;
    
    // Calculate relay efficiency
    uint32_t relayTotal = txRelay + txRelayCanceled;
    float relaySuccessRate = (relayTotal > 0) ? (txRelay * 100.0f / relayTotal) : 0.0f;

    // Use real counters for last minute and hour (set by doPeriodicWork)
    // If counters are not initialized yet, show 0
    uint32_t txMin = deviceStatsModule ? deviceStatsModule->txLastMinute : 0;
    uint32_t rxMin = deviceStatsModule ? deviceStatsModule->rxLastMinute : 0;
    uint32_t txHr = deviceStatsModule ? deviceStatsModule->txLastHour : 0;
    uint32_t rxHr = deviceStatsModule ? deviceStatsModule->rxLastHour : 0;

    // Format packet statistics report with clear sections
    int result;
    
    // Check if we have valid data from all sources
    if (radioValid && routerValid && airtimeValid) {
        // Full report with real minute/hour counters
        result = snprintf(buffer, bufferSize,
            "📦 PKT\n"
            "TX: %u (%u/m, %u/h)\n"
            "RX: %u/%u (%.0f%%) (%u/m, %u/h)\n"
            "Relay: %u/%u Dup: %u\n"
            "Ch: %.1f%% Air: %.1f%%",
            txGood, txMin, txHr,
            rxGood, rxTotal, rxSuccessRate, rxMin, rxHr,
            txRelay, relayTotal, rxDupe,
            channelUtil, airUtilTx);
    } else {
        // Simplified report if some data is missing
        result = snprintf(buffer, bufferSize,
            "📦 PKT\n"
            "TX: %u RX: %u/%u\n"
            "Relay: %u Dup: %u\n"
            "⚠️ Initializing...",
            txGood,
            rxGood, rxTotal,
            txRelay, rxDupe);
    }
                 
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Packet stats message truncated");
    }
    
    // Debug log for diagnostics
    LOG_INFO("PacketStats: radio=%d airtime=%d router=%d (tx/rx min=%u/%u hr=%u/%u)",
             radioValid, airtimeValid, routerValid, txMin, rxMin, txHr, rxHr);
}

void DeviceStatsModule::formatPowerStats(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR("Invalid buffer for power stats formatting");
        return;
    }

    // Get power status information with validation
    uint8_t batteryPercent = 0;
    uint16_t batteryVoltageMv = 0;
    const char* powerSource = "Unknown";
    const char* chargingStatus = "";
    bool powerValid = false;
    
    if (powerStatus) {
        batteryPercent = powerStatus->getBatteryChargePercent();
        batteryVoltageMv = powerStatus->getBatteryVoltageMv();
        
        // Validate battery values (percent should be 0-100, voltage should be reasonable)
        if (batteryPercent <= 100 && batteryVoltageMv <= 5000) {
            powerValid = true;
            
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
    }
    
    // Format power statistics report (optimized - no voltage duplication)
    int result;
    if (powerValid) {
        result = snprintf(buffer, bufferSize,
            "🔋 PWR\n"
            "%s%s\n"
            "%u%% %.2fV",
            powerSource, chargingStatus,
            batteryPercent, batteryVoltageMv / 1000.0f);
    } else {
        // Fallback if power status is unavailable or invalid
        result = snprintf(buffer, bufferSize,
            "🔋 PWR\n"
            "⚠️ Unavailable");
    }
                 
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Power stats message truncated");
    }
    
    // Debug log for diagnostics
    LOG_INFO("PowerStats: valid=%d battery=%u%% voltage=%umV", 
             powerValid, batteryPercent, batteryVoltageMv);
}

void DeviceStatsModule::formatRadioStats(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR("Invalid buffer for radio stats formatting");
        return;
    }

    // Get radio configuration with validation
    float frequency = 0.0f;
    uint8_t channel = 0;
    uint8_t sf = 9;      // Default SF9
    float bw = 125.0f;   // Default 125 kHz
    int8_t power = 17;   // Default 17 dBm
    bool radioValid = false;
    
    if (RadioLibInterface::instance) {
        frequency = RadioLibInterface::instance->getFreq();
        channel = RadioLibInterface::instance->getChannelNum();
        // Try to get actual radio settings from config (fallback to defaults)
        if (config.lora.spread_factor > 0) {
            sf = config.lora.spread_factor;
        }
        if (config.lora.bandwidth > 0) {
            bw = config.lora.bandwidth;
        }
        if (config.lora.tx_power != 0) {
            power = config.lora.tx_power;
        }
        // Validate frequency (should be in valid range: 150-960 MHz for LoRa)
        if (frequency >= 150.0f && frequency <= 960.0f) {
            radioValid = true;
        }
    }
    
    // Get packet statistics for average rates
    uint32_t txGood = 0, rxGood = 0, rxBad = 0;
    if (RadioLibInterface::instance) {
        txGood = RadioLibInterface::instance->txGood;
        rxGood = RadioLibInterface::instance->rxGood;
        rxBad = RadioLibInterface::instance->rxBad;
    }
    
    // Calculate average packet rates with validation
    uint32_t uptime = millis() / 1000;
    bool uptimeValid = (uptime >= 10); // Minimum 10 seconds for accurate rates
    float txAvgMin = (uptimeValid) ? (txGood * 60.0f / uptime) : 0.0f;
    float txAvgHour = (uptimeValid) ? (txGood * 3600.0f / uptime) : 0.0f;
    float rxAvgMin = (uptimeValid) ? ((rxGood + rxBad) * 60.0f / uptime) : 0.0f;
    float rxAvgHour = (uptimeValid) ? ((rxGood + rxBad) * 3600.0f / uptime) : 0.0f;
    
    // Get online nodes count with time validation
    // Use getTime() instead of getValidTime() to work even without RTC chip
    uint32_t currentTime = getTime(true);
    RTCQuality timeQuality = getRTCQuality();
    // Time is valid if we have any quality > None AND timestamp is in reasonable range
    bool timeValid = (timeQuality >= RTCQualityFromNet && currentTime > 1000000000 && currentTime < 2000000000);
    uint32_t onlineNodes = (timeValid && nodeDB) ? nodeDB->getNumOnlineMeshNodes() : 0;
    
    // Format radio statistics report (optimized - no packet/nodes duplication)
    int result;
    if (radioValid) {
        result = snprintf(buffer, bufferSize,
            "📡 RF\n"
            "%.3f MHz Ch%u\n"
            "SF%u BW%.0f %ddBm",
            frequency, channel,
            sf, bw, power);
    } else {
        result = snprintf(buffer, bufferSize,
            "📡 RF\n"
            "⚠️ Initializing...");
    }
                 
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Radio stats message truncated");
    }
    
    // Debug log for diagnostics
    LOG_INFO("RadioStats: radio=%d time=%d (quality=%s) uptime=%d (up=%us freq=%.1f)",
             radioValid, timeValid, RtcName(timeQuality), uptimeValid, uptime, frequency);
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
    
    // Get node info with validation
    const char* nodeName = owner.long_name;
    uint32_t nodeId = 0;
    bool nodeValid = false;
    
    // Validate node name (check if not null and not empty)
    if (!nodeName || nodeName[0] == '\0') {
        nodeName = "Unknown";
    }
    
    // Safely get node ID
    if (nodeDB) {
        nodeId = nodeDB->getNodeNum();
        nodeValid = true;
    }
    
    // Format status information report (optimized)
    int result;
    if (nodeValid) {
        const char* roleShort = 
            config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ? "R" :
            config.device.role == meshtastic_Config_DeviceConfig_Role_REPEATER ? "Rep" :
            config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT ? "C" : "?";
        
        result = snprintf(buffer, bufferSize,
            "ℹ️ STA\n"
            "%s !%08x\n"
            "Up: %ud %uh %um\n"
            "Boots: %u Role: %s",
            nodeName,
            nodeId,
            uptimeDays, uptimeHours, uptimeMinutes,
            rebootCount, roleShort);
    } else {
        result = snprintf(buffer, bufferSize,
            "ℹ️ STA\n"
            "%s\n"
            "Up: %ud %uh %um\n"
            "⚠️ No NodeDB",
            nodeName,
            uptimeDays, uptimeHours, uptimeMinutes);
    }
                 
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Status info message truncated");
    }
    
    // Debug log for diagnostics
    LOG_INFO("StatusInfo: valid=%d name=%s id=%08x", nodeValid, nodeName, nodeId);
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
    
    // Diagnostic info: check time sync and last_heard values
    // Use getTime() instead of getValidTime() to work even without RTC chip
    uint32_t currentTime = getTime(true); // Get current time (unix timestamp)
    RTCQuality timeQuality = getRTCQuality();
    // Time is valid if we have quality from network/NTP/GPS AND timestamp is in reasonable range
    bool timeValid = (timeQuality >= RTCQualityFromNet && currentTime > 1000000000 && currentTime < 2000000000);
    uint32_t nodesWithZeroTime = 0;
    uint32_t recentNodes = 0; // seen < 10 min
    
    // Calculate uptime
    uint32_t uptimeSeconds = millis() / 1000;
    uint32_t uptimeDays = uptimeSeconds / 86400;
    uint32_t uptimeHours = (uptimeSeconds % 86400) / 3600;
    uint32_t uptimeMinutes = (uptimeSeconds % 3600) / 60;
    
    // Only calculate recent/online if time is valid
    if (nodeDB && timeValid) {
        for (int i = 0; i < totalNodes; i++) {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
            if (!node) continue;
            
            if (node->last_heard == 0) {
                nodesWithZeroTime++;
            } else {
                uint32_t delta = currentTime > node->last_heard ? 
                                (currentTime - node->last_heard) : 0;
                if (delta < 600) { // < 10 minutes
                    recentNodes++;
                }
            }
        }
    } else if (nodeDB) {
        // Time invalid - count nodes with zero time
        for (int i = 0; i < totalNodes; i++) {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
            if (!node) continue;
            if (node->last_heard == 0) {
                nodesWithZeroTime++;
            }
        }
    }
    
    // Format time string (readable) with quality info
    char timeStr[48];
    const char* qualityStr = RtcName(timeQuality);
    
    if (timeValid) {
        struct tm *timeinfo = localtime((time_t*)&currentTime);
        char timeBuffer[24];
        strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", timeinfo);
        snprintf(timeStr, sizeof(timeStr), "%s (%s)", timeBuffer, qualityStr);
    } else {
        // Show why time is not valid
        if (timeQuality == RTCQualityNone) {
            snprintf(timeStr, sizeof(timeStr), "Not set (None)");
        } else if (timeQuality == RTCQualityDevice) {
            snprintf(timeStr, sizeof(timeStr), "RTC only (need Net/GPS)");
        } else {
            snprintf(timeStr, sizeof(timeStr), "Invalid (%s)", qualityStr);
        }
    }
    
    // Format nodes information report (optimized - no uptime duplication)
    int result;
    if (timeValid) {
        // Show online/recent stats if time is valid
        result = snprintf(buffer, bufferSize,
            "🌐 NOD\n"
            "On: %u (%.0f%%) <2h\n"
            "Rec: %u <10m\n"
            "Tot: %u/%u (%.0f%%)\n"
            "T: %s",
            onlineNodes, onlinePercent,
            recentNodes,
            totalNodes, maxNodes, usagePercent,
            timeStr);
    } else {
        // Time not synced - show simplified stats
        result = snprintf(buffer, bufferSize,
            "🌐 NOD\n"
            "Tot: %u/%u (%.0f%%)\n"
            "T: %s\n"
            "⚠️ Need Net/GPS time",
            totalNodes, maxNodes, usagePercent,
            timeStr);
    }
                 
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
    
    // Check if time is valid before calculating online nodes
    // Use getTime() instead of getValidTime() to work even without RTC chip
    uint32_t currentTime = getTime(true);
    RTCQuality timeQuality = getRTCQuality();
    // Time is valid if we have quality from network/NTP/GPS AND timestamp is in reasonable range
    bool timeValid = (timeQuality >= RTCQualityFromNet && currentTime > 1000000000 && currentTime < 2000000000);
    uint32_t onlineNodes = (timeValid && nodeDB) ? nodeDB->getNumOnlineMeshNodes() : 0;
    
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
    
    // Format debug information report (optimized - compact format)
    int result;
    if (timeValid) {
        result = snprintf(buffer, bufferSize,
            "🔧 DBG\n"
            "H: %u/%uKB (%u%%)\n"
            "N: %u/%u DB:%uKB\n"
            "Dup: %u/%u (%u%%) %us\n"
            "RXQ: %d/%d TXQ: %d/%d\n"
            "T: %s%s",
            usedHeap/1024, totalHeap/1024, heapPercent,
            onlineNodes, totalNodes, nodeDbBytes/1024,
            dupeCacheCount, dynamic_max_nodes, cacheFullness, oldestPacketAge,
            fromRadioUsed, fromRadioMax, txUsed, txMax,
            RtcName(timeQuality),
            criticalityWarning);
    } else {
        // Show simplified when time is not synced
        const char* timeShort = 
            (timeQuality == RTCQualityNone) ? "None" :
            (timeQuality == RTCQualityDevice) ? "RTC" : "?";
        
        result = snprintf(buffer, bufferSize,
            "🔧 DBG\n"
            "H: %u/%uKB (%u%%)\n"
            "N: %u DB:%uKB\n"
            "Dup: %u/%u %us\n"
            "RXQ: %d/%d TXQ: %d/%d\n"
            "⚠️T: %s%s",
            usedHeap/1024, totalHeap/1024, heapPercent,
            totalNodes, nodeDbBytes/1024,
            dupeCacheCount, dynamic_max_nodes, oldestPacketAge,
            fromRadioUsed, fromRadioMax, txUsed, txMax,
            timeShort,
            criticalityWarning);
    }
                 
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Debug info message truncated");
    }
}