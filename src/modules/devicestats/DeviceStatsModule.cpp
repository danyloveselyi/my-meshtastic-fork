#include <cctype>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <atomic>

// Safety constants for embedded systems
#define MAX_TRIM_LENGTH 512              // Safety limit for string operations
#define MINIMUM_SAFE_FREE_HEAP 8192      // 8KB minimum free heap (prevent OOM)
#define MAX_NODES_ARRAY_BOUND 500        // Maximum nodes to iterate safely
#define MAX_MESSAGE_RETRIES 3            // Retry failed sends up to 3 times

// Helper: trim leading/trailing whitespace in-place
// Embedded-safe version with bounded string operations
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
#include "MemoryStats.h"
#include "PacketStats.h"
#include "NodeStats.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "SerialConsole.h"
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
#include "RTC.h"
#include "FSCommon.h"
#include "SPILock.h"
#include <time.h>

extern Router *router;

#ifndef DEVICE_STATS_VERBOSE_LOGGING_DEFAULT
#define DEVICE_STATS_VERBOSE_LOGGING_DEFAULT 0
#endif

#define DS_LOG_PREFIX "DeviceStats: "
#define DS_LOG_ENABLED() DeviceStatsModule::isVerboseLoggingEnabled()
#define DS_LOG_DEBUG(...)                                                                                               \
    do {                                                                                                                \
        if (DS_LOG_ENABLED()) {                                                                                         \
            LOG_DEBUG(__VA_ARGS__);                                                                                     \
        }                                                                                                               \
    } while (0)
#define DS_LOG_INFO(...)                                                                                                \
    do {                                                                                                                \
        if (DS_LOG_ENABLED()) {                                                                                         \
            LOG_INFO(__VA_ARGS__);                                                                                      \
        }                                                                                                               \
    } while (0)
#define DS_LOG_WARN(...)                                                                                                \
    do {                                                                                                                \
        if (DS_LOG_ENABLED()) {                                                                                         \
            LOG_WARN(__VA_ARGS__);                                                                                      \
        }                                                                                                               \
    } while (0)

static void formatBytesHuman(uint32_t bytes, char* out, size_t outSize)
{
    if (!out || outSize == 0) {
        return;
    }

    out[0] = '\0';

    // Always format as KB with 3 decimal places for consistency
    // Example: 28455 bytes = 27.788KB
    float kilobytes = bytes / 1024.0f;

    snprintf(out, outSize, "%.3fKB", kilobytes);
}

// dynamic_max_nodes defined in DynamicNodes.cpp with weak linkage
// Variants can override by defining in variant.cpp
extern const uint32_t DEFAULT_MAX_NODES;
extern uint32_t dynamic_max_nodes;

// Hardcoded PIN code for device stats module
static const char* MONITORING_PIN_CODE = "123456";

// Context variables for interactive commands (per user)
// Using std::atomic for thread-safety in RTOS environment
// For /setmaxnodes: waiting for nodes,pin
static std::atomic<uint32_t> waitingSetMaxNodesNodeId{0};
// For /monstart: waiting for interval,pin
static std::atomic<uint32_t> waitingMonStartNodeId{0};
// For /monstop: waiting for pin
static std::atomic<uint32_t> waitingMonStopNodeId{0};

// DEFAULT_MAX_NODES is defined per variant:
// - rak4631_eth_gw: extern const uint32_t DEFAULT_MAX_NODES = 350
// - rak4631: extern const uint32_t DEFAULT_MAX_NODES = 80
// Note: No fallback needed as all variants should define this

DeviceStatsModule *deviceStatsModule;

// Helper function to get consistent max nodes value across all functions
// Returns the MAXIMUM of compile-time and runtime limits
// Runtime limit can only INCREASE capacity, never decrease below compile limit
static inline uint32_t getEffectiveMaxNodes()
{
    return std::max(dynamic_max_nodes, static_cast<uint32_t>(MAX_NUM_NODES));
}

bool DeviceStatsModule::isVerboseLoggingEnabled()
{
#if DEVICE_STATS_VERBOSE_LOGGING_DEFAULT
    return true;
#else
    bool enabled = config.security.debug_log_api_enabled;
#ifdef DEBUG_PORT
    if (console && console->isConnected()) {
        enabled = true;
    }
#endif
    return enabled;
#endif
}

ProcessMessage DeviceStatsModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Check if module is enabled via RangeTest configuration toggle in phone app
    // This allows dynamic enable/disable without recompiling firmware
    if (!moduleConfig.range_test.enabled) {
        return ProcessMessage::CONTINUE; // Module disabled, let other modules handle
    }

#ifdef DEBUG_PORT
    auto &p = mp.decoded;
    LOG_INFO(DS_LOG_PREFIX "Received text msg from=0x%0x, id=0x%x, msg=%.*s", mp.from, mp.id, p.payload.size, p.payload.bytes);
#endif

    powerFSM.trigger(EVENT_RECEIVED_MSG);

    // Send auto-reply for:
    // 1. Direct messages to this node (mp.to == nodeDB->getNodeNum())
    // 2. Broadcast messages (mp.to == NODENUM_BROADCAST) - but only if it starts with '/'
    // This allows commands to work via broadcast OR direct messaging
    bool isDirectMessage = (mp.to == nodeDB->getNodeNum() && mp.to != NODENUM_BROADCAST);
    bool isBroadcastCommand = (mp.to == NODENUM_BROADCAST) &&
                             (mp.decoded.payload.size > 0) &&
                             (mp.decoded.payload.bytes[0] == '/');

    if (isDirectMessage || isBroadcastCommand) {
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
        LOG_ERROR(DS_LOG_PREFIX "Invalid payload - bytes is NULL or size is 0");
        return;
    }

    // Safely copy original text with bounds checking
    size_t textLen = std::min((size_t)original.decoded.payload.size, sizeof(originalText) - 1);
    if (textLen > 0) {
        memcpy(originalText, original.decoded.payload.bytes, textLen);
    }
    originalText[textLen] = '\0';

    // Trim input before processing to ignore surrounding whitespace
    trim(originalText);

    if (originalText[0] == '\0') {
        LOG_WARN(DS_LOG_PREFIX "Empty message received");
        return;
    }

    strncpy(lowerText, originalText, sizeof(lowerText) - 1);
    lowerText[sizeof(lowerText) - 1] = '\0';
    for (size_t i = 0; lowerText[i] && i < sizeof(lowerText) - 1; i++) {
        lowerText[i] = tolower((unsigned char)lowerText[i]);
    }

    trim(lowerText);
    char* trimmed = lowerText;

    // Check for command prefix '/' and skip it
    bool isCommand = (trimmed[0] == '/');
    if (isCommand) {
        trimmed++; // Skip the '/' for comparison
        // Skip any spaces after '/'
        while (*trimmed == ' ') {
            trimmed++;
        }
    }

    const auto commandEquals = [](const char* value, const char* keyword) -> bool {
        size_t len = strlen(keyword);
        size_t valueLen = strlen(value);

        if (valueLen < len) {
            return false;
        }

        if (strncmp(value, keyword, len) != 0) {
            return false;
        }

        if (valueLen == len) {
            return true;
        }

        return value[len] == ' ';
    };

    const char* replyText = nullptr;

    auto logCommand = [&](const char* cmd) {
        DS_LOG_INFO("DeviceStats command %s from=0x%0x", cmd, original.from);
    };

    // Reset other waiting states when user starts a new command (to avoid conflicts)
    if (isCommand) {
        // Compare safely against trimmed string
        if (!commandEquals(trimmed, "setmaxnodes") &&
            !commandEquals(trimmed, "set max nodes")) {
            waitingSetMaxNodesNodeId = 0;
        }
        if (!commandEquals(trimmed, "monstart") &&
            !commandEquals(trimmed, "mon start")) {
            waitingMonStartNodeId = 0;
        }
        if (!commandEquals(trimmed, "monstop") &&
            !commandEquals(trimmed, "mon stop")) {
            waitingMonStopNodeId = 0;
        }
    }

    // Use a single static buffer to avoid stack overflow (instead of multiple 400-byte buffers on stack)
    static char commandBuffer[300];  // Static buffer shared by all commands

    // Check if this is a memory stats command - use strncmp for safety
    if (isCommand && (commandEquals(trimmed, "mem") || commandEquals(trimmed, "memory"))) {
        logCommand("mem");
        formatDetailedMemoryStats(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (commandEquals(trimmed, "packets") || commandEquals(trimmed, "packet"))) {
        logCommand("packets");
        formatPacketStats(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (commandEquals(trimmed, "power") || commandEquals(trimmed, "battery"))) {
        logCommand("power");
        formatPowerStats(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (commandEquals(trimmed, "radio") || commandEquals(trimmed, "rf"))) {
        logCommand("radio");
        formatRadioStats(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (commandEquals(trimmed, "status") || commandEquals(trimmed, "info"))) {
        logCommand("status");
        formatStatusInfo(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (commandEquals(trimmed, "nodes") || commandEquals(trimmed, "network"))) {
        logCommand("nodes");
        formatNodesInfo(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (commandEquals(trimmed, "debug") || commandEquals(trimmed, "dbg"))) {
        logCommand("debug");
        formatDebugInfo(commandBuffer, sizeof(commandBuffer));
        replyText = commandBuffer;
    } else if (isCommand && (commandEquals(trimmed, "save") || commandEquals(trimmed, "savedb"))) {
        // Manually force database save to disk
        logCommand("save");
        if (nodeDB) {
            nodeDB->saveToDisk();
            snprintf(replyBuffer, sizeof(replyBuffer),
                "💾 Saved %d nodes to disk. Free heap: %u bytes",
                nodeDB->getNumMeshNodes(), memGet.getFreeHeap());
        } else {
            snprintf(replyBuffer, sizeof(replyBuffer), "❌ Error: NodeDB not available");
        }
        replyText = replyBuffer;
    } else if (isCommand && (commandEquals(trimmed, "mon start") || commandEquals(trimmed, "monstart"))) {
        // Start interactive setup for monitoring interval
        waitingMonStartNodeId = original.from;
        logCommand("monstart-request");
        snprintf(replyBuffer, sizeof(replyBuffer),
            "Start monitoring. Enter interval (10-86400 sec) and PIN. Example: 30,1234");
        replyText = replyBuffer;
    } else if (isCommand && (commandEquals(trimmed, "mon stop") || commandEquals(trimmed, "monstop"))) {
        // Start interactive PIN check for monitoring stop
        waitingMonStopNodeId = original.from;
        logCommand("monstop-request");
        snprintf(replyBuffer, sizeof(replyBuffer),
            "Stop monitoring. Enter PIN. Example: 1234");
        replyText = replyBuffer;
    } else if (isCommand && (commandEquals(trimmed, "maxnodes") || commandEquals(trimmed, "max nodes"))) {
        // Show current max nodes setting with safety info
        logCommand("maxnodes");
        snprintf(replyBuffer, sizeof(replyBuffer),
                 "Max nodes: %u (compile: %u). Memory: ~%.1fKB. Send /setmaxnodes to increase.",
                 dynamic_max_nodes, MAX_NUM_NODES, (dynamic_max_nodes * 250) / 1024.0f);
        replyText = replyBuffer;
    } else if (isCommand && (commandEquals(trimmed, "setmaxnodes") || commandEquals(trimmed, "set max nodes"))) {
        // Ask for max nodes value with safety warning
        waitingSetMaxNodesNodeId = original.from;
        logCommand("setmaxnodes-request");
        float freeHeapKB = MemoryStats::getHeapFree() / 1024.0f;
        snprintf(replyBuffer, sizeof(replyBuffer),
            "Increase max nodes (%u-1000). Free heap: %.1fKB. Cannot decrease below compile limit (%u). Enter nodes,pin. Example: 400,1234",
            MAX_NUM_NODES, freeHeapKB, MAX_NUM_NODES);
        replyText = replyBuffer;
    } else if (isCommand && (commandEquals(trimmed, "help") || commandEquals(trimmed, "?") || commandEquals(trimmed, "commands"))) {
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
        char tempBuffer[256];
        strncpy(tempBuffer, originalText, sizeof(tempBuffer) - 1);
        tempBuffer[sizeof(tempBuffer) - 1] = '\0';

        char* saveptr = nullptr;
        char* valueStr = strtok_r(tempBuffer, ",", &saveptr);
        char* pinStr = strtok_r(nullptr, ",", &saveptr);

        int maxNodes = 0;
        if (valueStr) {
            char* endptr = nullptr;
            maxNodes = (int)strtol(valueStr, &endptr, 10);
            // Validate conversion
            if (endptr == valueStr || maxNodes <= 0) {
                maxNodes = 0; // Invalid input
            }
        }

        if (pinStr) trim(pinStr);
        bool pinOk = pinStr && pinStr[0] != '\0' && strcmp(pinStr, MONITORING_PIN_CODE) == 0;

    DS_LOG_INFO("DeviceStats command setmaxnodes apply value=%d from=0x%0x", maxNodes, original.from);

        if (!pinOk) {
            waitingSetMaxNodesNodeId = 0;
            replyText = "Failed: PIN code incorrect.";
            DS_LOG_WARN("DeviceStats setmaxnodes pin failed from=0x%0x", original.from);
        } else if (maxNodes <= 0) {
            waitingSetMaxNodesNodeId = 0;
            replyText = "Invalid value: enter a positive number.";
            DS_LOG_WARN("DeviceStats setmaxnodes invalid non-positive value from=0x%0x", original.from);
        } else if (maxNodes > 1000) {
            waitingSetMaxNodesNodeId = 0;
            replyText = "Error: Maximum allowed is 1000 nodes.";
            DS_LOG_WARN("DeviceStats setmaxnodes above upper bound value=%d from=0x%0x", maxNodes, original.from);
        } else if (maxNodes < (int)MAX_NUM_NODES) {
            waitingSetMaxNodesNodeId = 0;
            snprintf(replyBuffer, sizeof(replyBuffer),
                     "Error: Cannot set below compile limit (%u). Dynamic limit can only INCREASE capacity, not decrease.",
                     MAX_NUM_NODES);
            replyText = replyBuffer;
            DS_LOG_WARN("DeviceStats setmaxnodes below compile limit=%u requested=%d from=0x%0x",
                     MAX_NUM_NODES, maxNodes, original.from);
        } else if (maxNodes <= (int)dynamic_max_nodes) {
            waitingSetMaxNodesNodeId = 0;
            snprintf(replyBuffer, sizeof(replyBuffer),
                     "Error: New limit (%d) must be GREATER than current (%u). Cannot decrease.",
                     maxNodes, dynamic_max_nodes);
            replyText = replyBuffer;
            DS_LOG_WARN("DeviceStats setmaxnodes not increasing current=%u requested=%d from=0x%0x",
                     dynamic_max_nodes, maxNodes, original.from);
        } else {
            uint32_t currentNodes = nodeDB ? NodeStats::getValidNodeCount(nodeDB) : 0;

            if (maxNodes < (int)currentNodes) {
                waitingSetMaxNodesNodeId = 0;
                snprintf(replyBuffer, sizeof(replyBuffer),
                         "Error: Cannot set below current node count (%u).", currentNodes);
                replyText = replyBuffer;
                DS_LOG_WARN("DeviceStats setmaxnodes below current node count current=%u requested=%d from=0x%0x",
                         currentNodes, maxNodes, original.from);
            } else {
                int32_t nodeDelta = maxNodes - static_cast<int32_t>(currentNodes);
                uint32_t additionalMemory = static_cast<uint32_t>(std::max<int32_t>(nodeDelta, 0)) * 250; // ~250 bytes per node
                uint32_t freeHeap = MemoryStats::getHeapFree();

                if (freeHeap < additionalMemory + 4096) { // Keep 4KB safety margin
                    waitingSetMaxNodesNodeId = 0;
                    snprintf(replyBuffer, sizeof(replyBuffer),
                             "Error: Not enough memory. Need %u bytes, have %u free. Current: %u nodes.",
                             additionalMemory, freeHeap, currentNodes);
                    replyText = replyBuffer;
                    DS_LOG_WARN("DeviceStats setmaxnodes denied memory low required=%u free=%u from=0x%0x",
                             additionalMemory, freeHeap, original.from);
                } else {
                    dynamic_max_nodes = static_cast<uint32_t>(maxNodes);
                    // Note: We don't call reserve() to avoid memory allocation issues on constrained devices
                    // The vector will grow dynamically as nodes are added
                    waitingSetMaxNodesNodeId = 0;
                    snprintf(replyBuffer, sizeof(replyBuffer), "Max nodes limit increased to %d (dynamic allocation).",
                             maxNodes);
                    replyText = replyBuffer;
                    DS_LOG_INFO("DeviceStats setmaxnodes success new=%d currentNodes=%u from=0x%0x",
                             maxNodes, currentNodes, original.from);
                }
            }
        }
    } else if (waitingMonStartNodeId == original.from) {
        // User is setting up monitoring interval with PIN (monstart)
        // Format: value,pin
        // Use strtok_r for thread-safety in RTOS environment
        char tempBuffer[256];
        strncpy(tempBuffer, originalText, sizeof(tempBuffer) - 1);
        tempBuffer[sizeof(tempBuffer) - 1] = '\0';

        char* saveptr = nullptr;
        char* valueStr = strtok_r(tempBuffer, ",", &saveptr);
        char* pinStr = strtok_r(nullptr, ",", &saveptr);

        int interval = 0;
        if (valueStr) {
            char* endptr = nullptr;
            interval = (int)strtol(valueStr, &endptr, 10);
            // Validate conversion
            if (endptr == valueStr || interval <= 0) {
                interval = 0; // Invalid input
            }
        }

        if (pinStr) trim(pinStr);
        bool pinOk = pinStr && pinStr[0] != '\0' && strcmp(pinStr, MONITORING_PIN_CODE) == 0;

    DS_LOG_INFO("DeviceStats command monstart apply interval=%d from=0x%0x", interval, original.from);

        if (!pinOk) {
            waitingMonStartNodeId = 0;
            replyText = "Failed: PIN code incorrect.";
            DS_LOG_WARN("DeviceStats monstart pin failed from=0x%0x", original.from);
        } else if (interval >= 10 && interval <= 86400) {
            monitorIntervalMs = (uint32_t)interval * 1000;
            monitoringNodeId = original.from;
            lastMonitorTime = millis();
            waitingMonStartNodeId = 0;
            monitorMessageCounter = 0;
            snprintf(replyBuffer, sizeof(replyBuffer),
                     "Monitor ON: %ds intervals. Send /monstop to disable.", interval);
            replyText = replyBuffer;
            DS_LOG_INFO("DeviceStats monitoring enabled interval=%ds target=0x%0x", interval, original.from);
        } else {
            replyText = "Invalid interval! Use 10-86400 seconds.";
            DS_LOG_WARN("DeviceStats monstart invalid interval value=%d from=0x%0x", interval, original.from);
        }
    } else if (waitingMonStopNodeId == original.from) {
        // /monstop PIN check (expect only PIN)
        char* pinStr = originalText;
        trim(pinStr);
        bool pinOk = (pinStr && pinStr[0] != '\0' && strcmp(pinStr, MONITORING_PIN_CODE) == 0);
        if (pinOk) {
            monitoringNodeId = 0;
            waitingMonStopNodeId = 0;
            monitorMessageCounter = 0;
            replyText = "Memory monitoring stopped.";
            DS_LOG_INFO("DeviceStats monitoring disabled by node 0x%0x", original.from);
        } else {
            // Do not reset waitingMonStopNodeId, allow retry
            replyText = "Failed: PIN code incorrect. Try again.";
            DS_LOG_WARN("DeviceStats monstop pin failed from=0x%0x", original.from);
        }
    }

    // If no replyText was set, send default auto-reply
    if (!replyText) {
        snprintf(replyBuffer, sizeof(replyBuffer), "Auto-reply: %.200s", originalText);
        replyText = replyBuffer;
    DS_LOG_DEBUG("DeviceStats fallback reply to 0x%0x", original.from);
    }

    // Critical: check router pointer before use (embedded safety)
    if (!router) {
        LOG_ERROR(DS_LOG_PREFIX "Router is NULL - cannot send reply!");
        // Reset all waiting states to prevent hanging
        waitingSetMaxNodesNodeId = 0;
        waitingMonStartNodeId = 0;
        waitingMonStopNodeId = 0;
        return;
    }

    meshtastic_MeshPacket *reply = router->allocForSending();
    if (!reply) {
        LOG_ERROR(DS_LOG_PREFIX "Failed to allocate packet - memory exhausted! Resetting command states.");
        // Reset all waiting states to prevent hanging
        waitingSetMaxNodesNodeId = 0;
        waitingMonStartNodeId = 0;
        waitingMonStopNodeId = 0;
        // Don't crash, just return
        return;
    }

    reply->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    reply->decoded.payload.size = 0;

    reply->to = original.from;
    reply->decoded.want_response = false;
    reply->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;

    // CRITICAL FIX: Bounds check on payload
    size_t replyLen = strlen(replyText) > 0 ? strlen(replyText) : 0;
    size_t maxPayloadSize = sizeof(reply->decoded.payload.bytes);

    if (replyLen > maxPayloadSize) {
        LOG_WARN(DS_LOG_PREFIX "Reply message truncated from %u to %u bytes", replyLen, maxPayloadSize);
        replyLen = maxPayloadSize;
    }

    reply->decoded.payload.size = replyLen;
    if (replyLen > 0) {
        memcpy(reply->decoded.payload.bytes, replyText, replyLen);
    }

    // CRITICAL FIX: Add error handling for sendToMesh
    if (!service) {
        LOG_ERROR(DS_LOG_PREFIX "MeshService is NULL - cannot send reply!");
        return;
    }

    service->sendToMesh(reply, RX_SRC_LOCAL, true);

#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    LOG_INFO(DS_LOG_PREFIX "Sent auto-reply to node 0x%0x", original.from);
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

    // CRITICAL FIX: Ensure safe concatenation with bounds checking
    int result = snprintf(prefixedBuffer, sizeof(prefixedBuffer), "%s%s", prefix, statsBuffer);
    if (result < 0 || result >= (int)sizeof(prefixedBuffer)) {
        LOG_WARN(DS_LOG_PREFIX "Monitoring message buffer overflow, using truncated version");
        prefixedBuffer[sizeof(prefixedBuffer) - 1] = '\0';
    }

    // Critical: check router pointer before use (embedded safety)
    if (!router) {
        LOG_ERROR(DS_LOG_PREFIX "Router is NULL - cannot send memory stats!");
        monitoringNodeId = 0;  // Disable monitoring
        return;
    }

    // CRITICAL FIX: Add timeout and retry logic for memory allocation
    meshtastic_MeshPacket *reply = nullptr;
    int retryCount = 0;

    while (!reply && retryCount < MAX_MESSAGE_RETRIES) {
        reply = router->allocForSending();
        if (!reply) {
            retryCount++;
            if (retryCount < MAX_MESSAGE_RETRIES) {
                // Wait a bit before retrying
                delay(10);
            }
        }
    }

    if (!reply) {
        LOG_ERROR(DS_LOG_PREFIX "Failed to allocate monitoring packet after %d retries - memory exhausted! Disabling monitoring.",
                 MAX_MESSAGE_RETRIES);
        // Disable monitoring to prevent further memory allocation attempts
        monitoringNodeId = 0;
        return;
    }

    reply->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    reply->decoded.payload.size = 0;
    reply->to = toNode;
    reply->decoded.want_response = false;
    reply->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;

    // CRITICAL FIX: Safe payload size calculation
    size_t msgLen = strnlen(prefixedBuffer, sizeof(prefixedBuffer));
    size_t maxSize = sizeof(reply->decoded.payload.bytes);

    if (msgLen > maxSize) {
        LOG_WARN(DS_LOG_PREFIX "Monitoring message truncated from %u to %u bytes", msgLen, maxSize);
        msgLen = maxSize;
    }

    reply->decoded.payload.size = msgLen;
    if (msgLen > 0) {
        memcpy(reply->decoded.payload.bytes, prefixedBuffer, msgLen);
    }

    service->sendToMesh(reply, RX_SRC_LOCAL, true);

    DS_LOG_DEBUG("DeviceStats monitoring update #%u sent %u bytes to 0x%0x",
                 monitorMessageCounter, static_cast<unsigned int>(msgLen), toNode);
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
            return; // Don't update stats on first run
        }

        // CRITICAL FIX: Handle counter overflow safely
        // Check if counters have wrapped around (unlikely but possible)
        uint32_t txDelta = (currentTx >= lastTxCount) ? (currentTx - lastTxCount) : currentTx;
        uint32_t rxDelta = (currentRx >= lastRxCount) ? (currentRx - lastRxCount) : currentRx;

        // Sanity check: delta should be reasonable (< 10000 packets per loop)
        if (txDelta > 10000 || rxDelta > 10000) {
            LOG_WARN(DS_LOG_PREFIX "Suspicious packet count delta: tx=%u rx=%u - resetting counters", txDelta, rxDelta);
            lastTxCount = currentTx;
            lastRxCount = currentRx;
            return;
        }

        // CRITICAL FIX: Check for overflow in counters before addition
        // These are uint32_t, max value ~4 billion
        // At 1000 packets/minute, would take ~8 years to overflow
        // Cap at maximum value to prevent wrap-around
        if (txLastMinute > UINT32_MAX - txDelta) {
            txLastMinute = UINT32_MAX; // Cap at maximum
        } else {
            txLastMinute += txDelta;
        }

        if (rxLastMinute > UINT32_MAX - rxDelta) {
            rxLastMinute = UINT32_MAX;
        } else {
            rxLastMinute += rxDelta;
        }

        if (txLastHour > UINT32_MAX - txDelta) {
            txLastHour = UINT32_MAX;
        } else {
            txLastHour += txDelta;
        }

        if (rxLastHour > UINT32_MAX - rxDelta) {
            rxLastHour = UINT32_MAX;
        } else {
            rxLastHour += rxDelta;
        }

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

    // CRITICAL FIX: Check monitoring state before sending
    // Prevent sending too many messages if queue is full
    if (monitoringNodeId != 0) {
        // Handle millis() overflow safely (works for ~49 days continuous operation)
        uint32_t elapsed;
        if (now >= lastMonitorTime) {
            elapsed = now - lastMonitorTime;
        } else {
            // Overflow occurred
            elapsed = (UINT32_MAX - lastMonitorTime) + now + 1;
        }

        if (elapsed >= monitorIntervalMs) {
            // Additional safety check before sending
            uint32_t freeHeap = MemoryStats::getHeapFree();
            if (freeHeap < MINIMUM_SAFE_FREE_HEAP + 2048) {
                LOG_WARN(DS_LOG_PREFIX "Skipping monitoring message due to low memory: %u bytes (threshold: %u)",
                         freeHeap, MINIMUM_SAFE_FREE_HEAP + 2048);
                // Don't disable monitoring, just skip this iteration
                // This allows recovery if memory is freed
            } else {
                // Check router is available before sending
                if (router) {
                    int queueUsed = 0, queueFree = 0;
                    PacketStats::getQueueStats(queueUsed, queueFree, router);
                    if (queueFree > 0) {
                        sendMemoryStats(monitoringNodeId);
                    } else {
                        LOG_WARN(DS_LOG_PREFIX "Cannot send monitoring message: radio queue full");
                        // Don't disable monitoring, just skip
                    }
                } else {
                    LOG_WARN(DS_LOG_PREFIX "Cannot send monitoring message: router unavailable");
                }
            }
            lastMonitorTime = now;
        }
    }

    // Periodic memory monitoring every 5 minutes (moved from static for thread safety)
    uint32_t memElapsed;
    if (now >= lastMemoryCheck) {
        memElapsed = now - lastMemoryCheck;
    } else {
        // Overflow occurred
        memElapsed = (UINT32_MAX - lastMemoryCheck) + now + 1;
    }

    if (memElapsed > 300000) { // 5 minutes = 300,000ms
        uint32_t freeHeap = MemoryStats::getHeapFree();
        if (freeHeap < MINIMUM_SAFE_FREE_HEAP * 2) {
            LOG_WARN(DS_LOG_PREFIX "Low memory detected: %u bytes free (threshold: %u)", freeHeap, MINIMUM_SAFE_FREE_HEAP * 2);
            // Log additional diagnostics for long-term monitoring
            if (nodeDB) {
                uint32_t nodeCount = NodeStats::getValidNodeCount(nodeDB);
                LOG_INFO(DS_LOG_PREFIX "Memory diagnostics - Nodes: %u/%u, Monitoring: %s, Counter: %u",
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
        LOG_ERROR(DS_LOG_PREFIX "Invalid buffer for memory stats formatting");
        return;
    }

    // Get memory and node statistics with safety checks
    uint32_t flashTotal = MemoryStats::getFlashTotal();
    uint32_t flashUsed = MemoryStats::getFlashUsed();  // This is firmware size, not "used" flash!
    uint32_t flashFree = MemoryStats::getFlashFree();
    uint32_t heapTotal = MemoryStats::getHeapTotal();
    uint32_t heapFree = MemoryStats::getHeapFree();

    if (heapFree > heapTotal) {
        heapFree = heapTotal;
    }
    uint32_t heapUsed = (heapTotal >= heapFree) ? (heapTotal - heapFree) : 0;

    // Protect against null nodeDB (can happen during shutdown)
    uint32_t storedNodes = nodeDB ? NodeStats::getValidNodeCount(nodeDB) : 0;
    uint32_t onlineNodes = nodeDB ? nodeDB->getNumOnlineMeshNodes() : 0;
    uint32_t maxNodes = getEffectiveMaxNodes();  // Use consistent max nodes calculation
    // FIXED: Show accurate free slots (can be negative if over limit)
    int32_t freeSlots = static_cast<int32_t>(maxNodes) - static_cast<int32_t>(storedNodes);

    char flashUsedStr[32];
    char flashTotalStr[32];
    char flashFreeStr[32];
    char heapUsedStr[32];
    char heapTotalStr[32];
    char heapFreeStr[32];

    formatBytesHuman(flashUsed, flashUsedStr, sizeof(flashUsedStr));
    formatBytesHuman(flashTotal, flashTotalStr, sizeof(flashTotalStr));
    formatBytesHuman(flashFree, flashFreeStr, sizeof(flashFreeStr));
    formatBytesHuman(heapUsed, heapUsedStr, sizeof(heapUsedStr));
    formatBytesHuman(heapTotal, heapTotalStr, sizeof(heapTotalStr));
    formatBytesHuman(heapFree, heapFreeStr, sizeof(heapFreeStr));

    // Format: used/total(free:amount) - safe for long-term operation
    int result;
    if (freeSlots >= 0) {
        result = snprintf(buffer, bufferSize,
                 "%sMem: Flash=%s/%s(free:%s) Heap=%s/%s(free:%s) Online=%u Stored=%u/%u(free:%d) Queues: Phone=%u Status=%u Notif=%u",
                 prefix ? prefix : "",
                 flashUsedStr, flashTotalStr, flashFreeStr,
                 heapUsedStr, heapTotalStr, heapFreeStr,
                 onlineNodes, storedNodes, maxNodes, freeSlots,
                 MAX_RX_TOPHONE, MAX_RX_TOPHONE, MAX_RX_TOPHONE/2);
    } else {
        result = snprintf(buffer, bufferSize,
                 "%sMem: Flash=%s/%s(free:%s) Heap=%s/%s(free:%s) Online=%u Stored=%u/%u(OVER:%d) Queues: Phone=%u Status=%u Notif=%u",
                 prefix ? prefix : "",
                 flashUsedStr, flashTotalStr, flashFreeStr,
                 heapUsedStr, heapTotalStr, heapFreeStr,
                 onlineNodes, storedNodes, maxNodes, -freeSlots,
                 MAX_RX_TOPHONE, MAX_RX_TOPHONE, MAX_RX_TOPHONE/2);
    }

    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN(DS_LOG_PREFIX "Memory stats message truncated");
    }
}

void DeviceStatsModule::formatDetailedMemoryStats(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR(DS_LOG_PREFIX "Invalid buffer for detailed memory stats formatting");
        return;
    }

    // Get memory statistics with safety checks and validation
    uint32_t flashTotal = 0;
    uint32_t flashUsed = 0;
    uint32_t flashFree = 0;

#ifdef FSCom
    // Get filesystem stats (external flash where node data is stored)
    // Platform-specific implementation to avoid changing common files

#if defined(ARCH_ESP32) || defined(ARCH_RP2040)
    // ESP32 and RP2040 have totalBytes()/usedBytes() methods
    flashTotal = FSCom.totalBytes();
    flashUsed = FSCom.usedBytes();
#elif defined(ARCH_NRF52)
    // NRF52: Calculate filesystem usage by summing all files
    // InternalFileSystem allocates ~28KB (7 pages * 4KB) from 1MB flash
    // We show filesystem stats (what user cares about) not total flash

    flashTotal = 28 * 1024;  // LittleFS area size
    flashUsed = 0;

    // Sum all files in filesystem (recursive, all directories)
    std::vector<meshtastic_FileInfo> files = getFiles("/", 10);
    for (const auto& file : files) {
        flashUsed += file.size_bytes;
    }
#else
    // Other platforms: use internal flash stats as fallback
    flashTotal = MemoryStats::getFlashTotal();
    flashUsed = MemoryStats::getFlashUsed();
#endif

    flashFree = (flashTotal > flashUsed) ? (flashTotal - flashUsed) : 0;
#else
    // Fallback to internal flash stats if no filesystem
    flashTotal = MemoryStats::getFlashTotal();
    flashUsed = MemoryStats::getFlashUsed();
    flashFree = MemoryStats::getFlashFree();
#endif

    uint32_t heapTotal = MemoryStats::getHeapTotal();
    uint32_t heapFree = MemoryStats::getHeapFree();

    bool memValid = true;
    if (flashTotal == 0 || heapTotal == 0) {
        memValid = false;
    }

    if (heapFree > heapTotal) {
        LOG_WARN(DS_LOG_PREFIX "Invalid heap values: total=%u free=%u", heapTotal, heapFree);
        heapFree = std::min(heapFree, heapTotal);
        memValid = false;
    }
    uint32_t heapUsed = (heapTotal >= heapFree) ? (heapTotal - heapFree) : 0;

    // Get node statistics with NULL check
    uint32_t storedNodes = 0;  // Total nodes stored in database
    uint32_t onlineNodes = 0;  // Currently active/online nodes
    if (nodeDB) {
        // CRITICAL FIX: Check nodeDB state is valid
        storedNodes = NodeStats::getValidNodeCount(nodeDB);
        onlineNodes = nodeDB->getNumOnlineMeshNodes();
        // Sanity check node count
        if (storedNodes > MAX_NODES_ARRAY_BOUND) {
            LOG_WARN(DS_LOG_PREFIX "Node count suspiciously high: %u (capping at %u)", storedNodes, MAX_NODES_ARRAY_BOUND);
            storedNodes = MAX_NODES_ARRAY_BOUND;
        }
    }

    // Use consistent max nodes calculation
    uint32_t maxNodes = getEffectiveMaxNodes();

    // FIXED: Show accurate free slots (can be negative if over limit)
    int32_t freeSlots = static_cast<int32_t>(maxNodes) - static_cast<int32_t>(storedNodes);

    char flashUsedStr[32];
    char flashTotalStr[32];
    char flashFreeStr[32];
    char heapUsedStr[32];
    char heapTotalStr[32];
    char heapFreeStr[32];

    formatBytesHuman(flashUsed, flashUsedStr, sizeof(flashUsedStr));
    formatBytesHuman(flashTotal, flashTotalStr, sizeof(flashTotalStr));
    formatBytesHuman(flashFree, flashFreeStr, sizeof(flashFreeStr));
    formatBytesHuman(heapUsed, heapUsedStr, sizeof(heapUsedStr));
    formatBytesHuman(heapTotal, heapTotalStr, sizeof(heapTotalStr));
    formatBytesHuman(heapFree, heapFreeStr, sizeof(heapFreeStr));

    // Format detailed memory report (simplified to <200 chars)
    int result;
    if (memValid) {
        if (freeSlots >= 0) {
            result = snprintf(buffer, bufferSize,
                "📊 Memory\n"
                "Flash: %s/%s (free:%s) | Heap: %s/%s (free:%s)\n"
                "Nodes: %u online, %u/%u stored (%d free)",
                flashUsedStr, flashTotalStr, flashFreeStr,
                heapUsedStr, heapTotalStr, heapFreeStr,
                onlineNodes, storedNodes, maxNodes, freeSlots);
        } else {
            result = snprintf(buffer, bufferSize,
                "📊 Memory\n"
                "Flash: %s/%s (free:%s) | Heap: %s/%s (free:%s)\n"
                "Nodes: %u online, %u/%u stored (OVER %d)",
                flashUsedStr, flashTotalStr, flashFreeStr,
                heapUsedStr, heapTotalStr, heapFreeStr,
                onlineNodes, storedNodes, maxNodes, -freeSlots);
        }
    } else {
        // Fallback if memory stats are invalid
        result = snprintf(buffer, bufferSize,
            "📊 MEM\n"
            "Online: %u | Stored: %u/%u\n"
            "⚠️ Stats unavailable",
            onlineNodes, storedNodes, maxNodes);
    }

    // Debug log to verify the message is being formatted correctly
    LOG_DEBUG(DS_LOG_PREFIX "MemoryStats: valid=%d flash=%u heap=%u online=%u stored=%u",
              static_cast<int>(memValid), static_cast<unsigned>(flashTotal),
              static_cast<unsigned>(heapTotal), static_cast<unsigned>(onlineNodes),
              static_cast<unsigned>(storedNodes));

    // Ensure null termination for safety
    if (result >= (int)bufferSize || result < 0) {
        buffer[bufferSize - 1] = '\0';
        if (result >= (int)bufferSize) {
            LOG_WARN(DS_LOG_PREFIX "Detailed memory stats message truncated");
        }
    }
}

void DeviceStatsModule::formatPacketStats(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR(DS_LOG_PREFIX "Invalid buffer for packet stats formatting");
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
    uint32_t txMin = 0, rxMin = 0, txHr = 0, rxHr = 0;

    // CRITICAL FIX: Safe pointer checks and volatile access
    if (deviceStatsModule) {
        // Use volatile reads to prevent compiler optimizations
        volatile DeviceStatsModule* statsPtr = deviceStatsModule;
        if (statsPtr) {
            txMin = statsPtr->txLastMinute;
            rxMin = statsPtr->rxLastMinute;
            txHr = statsPtr->txLastHour;
            rxHr = statsPtr->rxLastHour;
        }
    }

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
        LOG_WARN(DS_LOG_PREFIX "Packet stats message truncated");
    }

    // Debug log for diagnostics
    LOG_DEBUG(DS_LOG_PREFIX "PacketStats: radio=%d airtime=%d router=%d (tx/rx min=%u/%u hr=%u/%u)",
              radioValid, airtimeValid, routerValid, txMin, rxMin, txHr, rxHr);
}

void DeviceStatsModule::formatPowerStats(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR(DS_LOG_PREFIX "Invalid buffer for power stats formatting");
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
        LOG_WARN(DS_LOG_PREFIX "Power stats message truncated");
    }

    // Debug log for diagnostics
    LOG_INFO(DS_LOG_PREFIX "PowerStats: valid=%d battery=%u%% voltage=%umV",
             powerValid, batteryPercent, batteryVoltageMv);
}

void DeviceStatsModule::formatRadioStats(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR(DS_LOG_PREFIX "Invalid buffer for radio stats formatting");
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
        LOG_WARN(DS_LOG_PREFIX "Radio stats message truncated");
    }

    // Debug log for diagnostics
    LOG_INFO(DS_LOG_PREFIX "RadioStats: radio=%d time=%d (quality=%s) uptime=%d (up=%us freq=%.1f)",
             radioValid, timeValid, RtcName(timeQuality), uptimeValid, uptime, frequency);
}

void DeviceStatsModule::formatStatusInfo(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR(DS_LOG_PREFIX "Invalid buffer for status info formatting");
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
        LOG_WARN(DS_LOG_PREFIX "Status info message truncated");
    }

    // Debug log for diagnostics
    LOG_INFO(DS_LOG_PREFIX "StatusInfo: valid=%d name=%s id=%08x", nodeValid, nodeName, nodeId);
}

void DeviceStatsModule::formatNodesInfo(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR(DS_LOG_PREFIX "Invalid buffer for nodes info formatting");
        return;
    }

    // Get node statistics with safety checks
    uint32_t onlineNodes = nodeDB ? nodeDB->getNumOnlineMeshNodes() : 0;
    uint32_t storedNodes = nodeDB ? NodeStats::getValidNodeCount(nodeDB) : 0;

    // Use consistent max nodes calculation
    uint32_t maxNodes = getEffectiveMaxNodes();

    uint32_t offlineNodes = (storedNodes > onlineNodes) ? (storedNodes - onlineNodes) : 0;
    // FIXED: Show accurate free slots (can be negative if over limit)
    int32_t freeSlots = static_cast<int32_t>(maxNodes) - static_cast<int32_t>(storedNodes);

    // Calculate percentages
    float usagePercent = (maxNodes > 0) ? (storedNodes * 100.0f / maxNodes) : 0.0f;
    float onlinePercent = (storedNodes > 0) ? (onlineNodes * 100.0f / storedNodes) : 0.0f;

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
        // CRITICAL FIX: Iterate only over valid nodes, not array capacity
        uint32_t actualNodeCount = NodeStats::getValidNodeCount(nodeDB);
        // Still need to check all slots since valid nodes can be sparse in array
        uint32_t arrayCapacity = nodeDB->getNumMeshNodes();
        for (uint32_t i = 0; i < arrayCapacity && i < 500; i++) { // Add upper bound check
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
            if (!node || !node->has_user) continue; // Skip empty slots

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
        uint32_t arrayCapacity = nodeDB->getNumMeshNodes();
        for (uint32_t i = 0; i < arrayCapacity && i < 500; i++) {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
            if (!node || !node->has_user) continue; // Skip empty slots
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
        if (freeSlots >= 0) {
            result = snprintf(buffer, bufferSize,
                "🌐 NOD\n"
                "Online: %u/%u (%.0f%%)\n"
                "Recent: %u <10m\n"
                "Stored: %u/%u (free:%d)\n"
                "Time: %s",
                onlineNodes, storedNodes, onlinePercent,
                recentNodes,
                storedNodes, maxNodes, freeSlots,
                timeStr);
        } else {
            result = snprintf(buffer, bufferSize,
                "🌐 NOD\n"
                "Online: %u/%u (%.0f%%)\n"
                "Recent: %u <10m\n"
                "Stored: %u/%u (OVER:%d)\n"
                "Time: %s",
                onlineNodes, storedNodes, onlinePercent,
                recentNodes,
                storedNodes, maxNodes, -freeSlots,
                timeStr);
        }
    } else {
        // Time not synced - show simplified stats
        if (freeSlots >= 0) {
            result = snprintf(buffer, bufferSize,
                "🌐 NOD\n"
                "Stored: %u/%u (free:%d)\n"
                "Time: %s\n"
                "⚠️ Need Net/GPS time for online count",
                storedNodes, maxNodes, freeSlots,
                timeStr);
        } else {
            result = snprintf(buffer, bufferSize,
                "🌐 NOD\n"
                "Stored: %u/%u (OVER:%d)\n"
                "Time: %s\n"
                "⚠️ Need Net/GPS time for online count",
                storedNodes, maxNodes, -freeSlots,
                timeStr);
        }
    }

    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN(DS_LOG_PREFIX "Nodes info message truncated");
    }
}

void DeviceStatsModule::formatDebugInfo(char* buffer, size_t bufferSize)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 300) {
        LOG_ERROR(DS_LOG_PREFIX "Invalid buffer for debug info formatting");
        return;
    }

    // Get heap for context
    uint32_t freeHeap = MemoryStats::getHeapFree();
    uint32_t totalHeap = MemoryStats::getHeapTotal();
    uint32_t usedHeap = totalHeap - freeHeap;
    uint32_t heapPercent = (totalHeap > 0) ? (usedHeap * 100 / totalHeap) : 0;

    // Get actual node counts (not just max)
    uint32_t storedNodes = nodeDB ? NodeStats::getValidNodeCount(nodeDB) : 0;

    // Check if time is valid before calculating online nodes
    // Use getTime() instead of getValidTime() to work even without RTC chip
    uint32_t currentTime = getTime(true);
    RTCQuality timeQuality = getRTCQuality();
    // Time is valid if we have quality from network/NTP/GPS AND timestamp is in reasonable range
    bool timeValid = (timeQuality >= RTCQualityFromNet && currentTime > 1000000000 && currentTime < 2000000000);
    uint32_t onlineNodes = (timeValid && nodeDB) ? nodeDB->getNumOnlineMeshNodes() : 0;

    // Calculate actual NodeDB memory usage
    uint32_t nodeDbBytes = storedNodes * 250;  // Real memory usage based on actual nodes

    // Get actual network queue usage (critical for mesh stability)
    int fromRadioUsed = 0;
    int fromRadioFree = 4;
    PacketStats::getQueueStats(fromRadioUsed, fromRadioFree, router);
    int fromRadioMax = 4;  // MAX_RX_FROMRADIO

    // Get TX queue status (network transmission queue)
    meshtastic_QueueStatus txStatus = router ? router->getQueueStatus() : meshtastic_QueueStatus{0, 0, 0, 0};
    int txUsed = (txStatus.maxlen > txStatus.free) ? (txStatus.maxlen - txStatus.free) : 0;
    int txMax = txStatus.maxlen > 0 ? txStatus.maxlen : 16;

    // Get PacketHistory (DupeCache) statistics
    uint32_t dupeCacheCount = PacketStats::getPacketHistorySize();
    uint32_t oldestPacketAge = 0; // Not available without modifying PacketHistory

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
            "Online: %u Stored: %u DB:%uKB\n"
            "Dup: %u/%u (%u%%) %us\n"
            "RXQ: %d/%d TXQ: %d/%d\n"
            "T: %s%s",
            usedHeap/1024, totalHeap/1024, heapPercent,
            onlineNodes, storedNodes, nodeDbBytes/1024,
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
            "Stored: %u DB:%uKB\n"
            "Dup: %u/%u %us\n"
            "RXQ: %d/%d TXQ: %d/%d\n"
            "⚠️T: %s%s",
            usedHeap/1024, totalHeap/1024, heapPercent,
            storedNodes, nodeDbBytes/1024,
            dupeCacheCount, dynamic_max_nodes, oldestPacketAge,
            fromRadioUsed, fromRadioMax, txUsed, txMax,
            timeShort,
            criticalityWarning);
    }

    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN(DS_LOG_PREFIX "Debug info message truncated");
    }
}