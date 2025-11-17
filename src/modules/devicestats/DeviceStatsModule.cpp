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
#include "modules/Telemetry/EnvironmentTelemetry.h"
#include "modules/NeighborInfoModule.h"
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

/**
 * Check if system time is synchronized and valid for statistics
 * @param currentTime Output: current unix timestamp (if valid)
 * @param timeQuality Output: RTC quality level
 * @return true if time is synchronized from network/NTP/GPS and timestamp is valid
 */
static bool checkTimeSynchronization(uint32_t& currentTime, RTCQuality& timeQuality)
{
    currentTime = getTime(true);
    timeQuality = getRTCQuality();
    // Time is valid if we have quality from network/NTP/GPS AND timestamp is in reasonable range
    return (timeQuality >= RTCQualityFromNet && currentTime > 1000000000 && currentTime < 2000000000);
}

/**
 * Calculate available node slots in database
 * @param storedNodes Current number of nodes in database
 * @param maxNodes Maximum allowed nodes
 * @return Available slots (can be negative if over limit)
 */
static int32_t calculateAvailableNodeSlots(uint32_t storedNodes, uint32_t maxNodes)
{
    return static_cast<int32_t>(maxNodes) - static_cast<int32_t>(storedNodes);
}

// Hardcoded PIN code for device stats module
static const char* MONITORING_PIN_CODE = "123456";

// Context variables for interactive commands (per user)
// Using std::atomic for thread-safety in RTOS environment
// For /monstart: waiting for interval,pin
static std::atomic<uint32_t> waitingMonStartNodeId{0};
// For /monstop: waiting for pin
static std::atomic<uint32_t> waitingMonStopNodeId{0};

DeviceStatsModule *deviceStatsModule;

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
    LOG_INFO(DS_LOG_PREFIX "Received text msg from=0x%x, id=0x%x, msg=%.*s", mp.from, mp.id, p.payload.size, p.payload.bytes);
#endif

    powerFSM.trigger(EVENT_RECEIVED_MSG);

    // Determine if we should send auto-reply based on range_test.save configuration
    // If range_test.save = true: send auto-reply for ALL messages (echo mode)
    // If range_test.save = false: send auto-reply only for COMMANDS (starting with '/')
    bool shouldSendAutoReply = false;
    
    // Check if this is a direct message to this node
    bool isDirectMessage = (mp.to == nodeDB->getNodeNum() && mp.to != NODENUM_BROADCAST);
    
    // Check if this is a broadcast message
    bool isBroadcast = (mp.to == NODENUM_BROADCAST);
    
    // Check if message starts with '/' (command)
    bool isCommand = (mp.decoded.payload.size > 0 && mp.decoded.payload.bytes[0] == '/');
    
    if (moduleConfig.range_test.save) {
        // Auto-reply enabled: respond to ALL messages (echo mode)
        // Respond to direct messages OR broadcast messages
        shouldSendAutoReply = (isDirectMessage || isBroadcast);
    } else {
        // Auto-reply disabled: respond ONLY to commands
        // Commands can be sent as direct messages OR broadcast (if starts with '/')
        bool isBroadcastCommand = isBroadcast && isCommand;
        shouldSendAutoReply = (isDirectMessage || isBroadcastCommand);
    }

    if (shouldSendAutoReply) {
        sendAutoReply(mp);
    }
    notifyObservers(&mp);

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}


/**
 * Check if command string matches keyword (case-insensitive, space-separated)
 * @param commandText Command text to check
 * @param keyword Keyword to match
 * @return true if command matches keyword
 */
static bool doesCommandMatch(const char* commandText, const char* keyword)
{
    size_t keywordLen = strlen(keyword);
    size_t commandLen = strlen(commandText);

    if (commandLen < keywordLen) {
        return false;
    }

    if (strncmp(commandText, keyword, keywordLen) != 0) {
        return false;
    }

    // Exact match or followed by space
    if (commandLen == keywordLen) {
        return true;
    }

    return commandText[keywordLen] == ' ';
}

void DeviceStatsModule::sendAutoReply(const meshtastic_MeshPacket &original)
{
    // Use static buffers to avoid stack overflow on embedded systems (768 bytes is too much for stack)
    // Static buffers are thread-safe here because this function is called from message handler
    static char receivedMessageText[256];
    static char lowerCaseCommandText[256];
    static char replyMessageBuffer[256];

    // Critical: validate payload pointer before memcpy (embedded safety)
    if (!original.decoded.payload.bytes || original.decoded.payload.size == 0) {
        LOG_ERROR(DS_LOG_PREFIX "Invalid payload - bytes is NULL or size is 0");
        return;
    }

    // Safely copy received message text with bounds checking
    size_t messageTextLength = std::min((size_t)original.decoded.payload.size, sizeof(receivedMessageText) - 1);
    if (messageTextLength > 0) {
        memcpy(receivedMessageText, original.decoded.payload.bytes, messageTextLength);
    }
    receivedMessageText[messageTextLength] = '\0';

    // Trim input before processing to ignore surrounding whitespace
    trim(receivedMessageText);

    if (receivedMessageText[0] == '\0') {
        LOG_WARN(DS_LOG_PREFIX "Empty message received");
        return;
    }

    // Convert to lowercase for case-insensitive command matching
    strncpy(lowerCaseCommandText, receivedMessageText, sizeof(lowerCaseCommandText) - 1);
    lowerCaseCommandText[sizeof(lowerCaseCommandText) - 1] = '\0';
    for (size_t i = 0; lowerCaseCommandText[i] && i < sizeof(lowerCaseCommandText) - 1; i++) {
        lowerCaseCommandText[i] = tolower((unsigned char)lowerCaseCommandText[i]);
    }

    trim(lowerCaseCommandText);
    char* commandTextWithoutPrefix = lowerCaseCommandText;

    // Check for command prefix '/' and skip it
    bool isCommandMessage = (commandTextWithoutPrefix[0] == '/');
    if (isCommandMessage) {
        commandTextWithoutPrefix++; // Skip the '/' for comparison
        // Skip any spaces after '/'
        while (*commandTextWithoutPrefix == ' ') {
            commandTextWithoutPrefix++;
        }
    }

    const char* replyMessageText = nullptr;

    auto logCommandExecution = [&](const char* commandName) {
        DS_LOG_INFO("DeviceStats command %s from=0x%x", commandName, original.from);
    };

    // Reset other waiting states when user starts a new command (to avoid conflicts)
    if (isCommandMessage) {
        // Compare safely against command text
        if (!doesCommandMatch(commandTextWithoutPrefix, "monstart") &&
            !doesCommandMatch(commandTextWithoutPrefix, "mon start")) {
            waitingMonStartNodeId = 0;
        }
        if (!doesCommandMatch(commandTextWithoutPrefix, "monstop") &&
            !doesCommandMatch(commandTextWithoutPrefix, "mon stop")) {
            waitingMonStopNodeId = 0;
        }
    }

    // Use a single static buffer to avoid stack overflow (instead of multiple 400-byte buffers on stack)
    static char commandResponseBuffer[300];  // Static buffer shared by all commands

    // Check if this is a memory stats command - use strncmp for safety
    if (isCommandMessage && (doesCommandMatch(commandTextWithoutPrefix, "mem") || doesCommandMatch(commandTextWithoutPrefix, "memory"))) {
        logCommandExecution("mem");
        formatDetailedMemoryStats(commandResponseBuffer, sizeof(commandResponseBuffer));
        replyMessageText = commandResponseBuffer;
    } else if (isCommandMessage && (doesCommandMatch(commandTextWithoutPrefix, "packets") || doesCommandMatch(commandTextWithoutPrefix, "packet"))) {
        logCommandExecution("packets");
        formatPacketStats(commandResponseBuffer, sizeof(commandResponseBuffer));
        replyMessageText = commandResponseBuffer;
    } else if (isCommandMessage && (doesCommandMatch(commandTextWithoutPrefix, "power") || doesCommandMatch(commandTextWithoutPrefix, "battery"))) {
        logCommandExecution("power");
        formatPowerStats(commandResponseBuffer, sizeof(commandResponseBuffer));
        replyMessageText = commandResponseBuffer;
    } else if (isCommandMessage && (doesCommandMatch(commandTextWithoutPrefix, "radio") || doesCommandMatch(commandTextWithoutPrefix, "rf"))) {
        logCommandExecution("radio");
        formatRadioStats(commandResponseBuffer, sizeof(commandResponseBuffer));
        replyMessageText = commandResponseBuffer;
    } else if (isCommandMessage && (doesCommandMatch(commandTextWithoutPrefix, "status") || doesCommandMatch(commandTextWithoutPrefix, "info"))) {
        logCommandExecution("status");
        formatStatusInfo(commandResponseBuffer, sizeof(commandResponseBuffer));
        replyMessageText = commandResponseBuffer;
    } else if (isCommandMessage && (doesCommandMatch(commandTextWithoutPrefix, "nodes") || doesCommandMatch(commandTextWithoutPrefix, "network"))) {
        logCommandExecution("nodes");
        formatNodesInfo(commandResponseBuffer, sizeof(commandResponseBuffer));
        replyMessageText = commandResponseBuffer;
    } else if (isCommandMessage && (doesCommandMatch(commandTextWithoutPrefix, "debug") || doesCommandMatch(commandTextWithoutPrefix, "dbg"))) {
        logCommandExecution("debug");
        formatDebugInfo(commandResponseBuffer, sizeof(commandResponseBuffer));
        replyMessageText = commandResponseBuffer;
    } else if (isCommandMessage && (doesCommandMatch(commandTextWithoutPrefix, "save") || doesCommandMatch(commandTextWithoutPrefix, "savedb"))) {
        // Manually force database save to disk
        logCommandExecution("save");
        if (nodeDB) {
            nodeDB->saveToDisk();
            snprintf(replyMessageBuffer, sizeof(replyMessageBuffer),
                "💾 Saved %d nodes to disk. Free heap: %u bytes",
                nodeDB->getNumMeshNodes(), memGet.getFreeHeap());
        } else {
            snprintf(replyMessageBuffer, sizeof(replyMessageBuffer), "❌ Error: NodeDB not available");
        }
        replyMessageText = replyMessageBuffer;
    } else if (isCommandMessage && (doesCommandMatch(commandTextWithoutPrefix, "mon start") || doesCommandMatch(commandTextWithoutPrefix, "monstart"))) {
        // Start interactive setup for monitoring interval
        waitingMonStartNodeId = original.from;
        logCommandExecution("monstart-request");
        snprintf(replyMessageBuffer, sizeof(replyMessageBuffer),
            "Start monitoring. Enter interval (10-86400 sec) and PIN. Example: 30,1234");
        replyMessageText = replyMessageBuffer;
    } else if (isCommandMessage && (doesCommandMatch(commandTextWithoutPrefix, "mon stop") || doesCommandMatch(commandTextWithoutPrefix, "monstop"))) {
        // Start interactive PIN check for monitoring stop
        waitingMonStopNodeId = original.from;
        logCommandExecution("monstop-request");
        snprintf(replyMessageBuffer, sizeof(replyMessageBuffer),
            "Stop monitoring. Enter PIN. Example: 1234");
        replyMessageText = replyMessageBuffer;
    } else if (isCommandMessage && (doesCommandMatch(commandTextWithoutPrefix, "help") || doesCommandMatch(commandTextWithoutPrefix, "?") || doesCommandMatch(commandTextWithoutPrefix, "commands"))) {
        // Show available commands
        snprintf(replyMessageBuffer, sizeof(replyMessageBuffer),
                 "📋 COMMANDS\n"
                 "/mem /packets /power\n"
                 "/radio /status /nodes\n"
                 "/debug /monstart /monstop");
        replyMessageText = replyMessageBuffer;
    } else if (waitingMonStartNodeId == original.from) {
        // User is setting up monitoring interval with PIN (monstart)
        // Format: value,pin
        // Use strtok_r for thread-safety in RTOS environment
        char monitoringInputBuffer[256];
        strncpy(monitoringInputBuffer, receivedMessageText, sizeof(monitoringInputBuffer) - 1);
        monitoringInputBuffer[sizeof(monitoringInputBuffer) - 1] = '\0';

        char* saveptr = nullptr;
        char* intervalValueStr = strtok_r(monitoringInputBuffer, ",", &saveptr);
        char* pinCodeStr = strtok_r(nullptr, ",", &saveptr);

        int monitoringIntervalSeconds = 0;
        if (intervalValueStr) {
            char* endptr = nullptr;
            monitoringIntervalSeconds = (int)strtol(intervalValueStr, &endptr, 10);
            // Validate conversion
            if (endptr == intervalValueStr || monitoringIntervalSeconds <= 0) {
                monitoringIntervalSeconds = 0; // Invalid input
            }
        }

        if (pinCodeStr) trim(pinCodeStr);
        bool isPinCodeCorrect = pinCodeStr && pinCodeStr[0] != '\0' && strcmp(pinCodeStr, MONITORING_PIN_CODE) == 0;

    DS_LOG_INFO("DeviceStats command monstart apply interval=%d from=0x%x", monitoringIntervalSeconds, original.from);

        if (!isPinCodeCorrect) {
            waitingMonStartNodeId = 0;
            replyMessageText = "Failed: PIN code incorrect.";
            DS_LOG_WARN("DeviceStats monstart pin failed from=0x%x", original.from);
        } else if (monitoringIntervalSeconds >= 10 && monitoringIntervalSeconds <= 86400) {
            monitorIntervalMs = (uint32_t)monitoringIntervalSeconds * 1000;
            monitoringNodeId = original.from;
            lastMonitorTime = millis();
            waitingMonStartNodeId = 0;
            monitorMessageCounter = 0;
            snprintf(replyMessageBuffer, sizeof(replyMessageBuffer),
                     "Monitor ON: %ds intervals. Send /monstop to disable.", monitoringIntervalSeconds);
            replyMessageText = replyMessageBuffer;
            DS_LOG_INFO("DeviceStats monitoring enabled interval=%ds target=0x%x", monitoringIntervalSeconds, original.from);
        } else {
            replyMessageText = "Invalid interval! Use 10-86400 seconds.";
            DS_LOG_WARN("DeviceStats monstart invalid interval value=%d from=0x%x", monitoringIntervalSeconds, original.from);
        }
    } else if (waitingMonStopNodeId == original.from) {
        // /monstop PIN check (expect only PIN)
        char* pinCodeStr = receivedMessageText;
        trim(pinCodeStr);
        bool isPinCodeCorrect = (pinCodeStr && pinCodeStr[0] != '\0' && strcmp(pinCodeStr, MONITORING_PIN_CODE) == 0);
        if (isPinCodeCorrect) {
            monitoringNodeId = 0;
            waitingMonStopNodeId = 0;
            monitorMessageCounter = 0;
            replyMessageText = "Memory monitoring stopped.";
            DS_LOG_INFO("DeviceStats monitoring disabled by node 0x%x", original.from);
        } else {
            // Do not reset waitingMonStopNodeId, allow retry
            replyMessageText = "Failed: PIN code incorrect. Try again.";
            DS_LOG_WARN("DeviceStats monstop pin failed from=0x%x", original.from);
        }
    }

    // If no reply message was set, check if we should send echo auto-reply
    if (!replyMessageText) {
        // Only send echo auto-reply if range_test.save is enabled
        if (moduleConfig.range_test.save) {
            // Echo mode: send back the received text
            snprintf(replyMessageBuffer, sizeof(replyMessageBuffer), "Auto-reply: %.200s", receivedMessageText);
            replyMessageText = replyMessageBuffer;
            DS_LOG_DEBUG("DeviceStats echo auto-reply to 0x%x", original.from);
        } else {
            // Auto-reply disabled: don't send echo for non-command messages
            // This means if it's not a command, we return without sending anything
            return;
        }
    }

    // Critical: check router pointer before use (embedded safety)
    if (!router) {
        LOG_ERROR(DS_LOG_PREFIX "Router is NULL - cannot send reply!");
        // Reset all waiting states to prevent hanging
        waitingMonStartNodeId = 0;
        waitingMonStopNodeId = 0;
        return;
    }

    meshtastic_MeshPacket *reply = router->allocForSending();
    if (!reply) {
        LOG_ERROR(DS_LOG_PREFIX "Failed to allocate packet - memory exhausted! Resetting command states.");
        // Reset all waiting states to prevent hanging
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
    size_t replyMessageLength = strlen(replyMessageText) > 0 ? strlen(replyMessageText) : 0;
    size_t maxPayloadSize = sizeof(reply->decoded.payload.bytes);

    if (replyMessageLength > maxPayloadSize) {
        LOG_WARN(DS_LOG_PREFIX "Reply message truncated from %u to %u bytes", replyMessageLength, maxPayloadSize);
        replyMessageLength = maxPayloadSize;
    }

    reply->decoded.payload.size = replyMessageLength;
    if (replyMessageLength > 0) {
        memcpy(reply->decoded.payload.bytes, replyMessageText, replyMessageLength);
    }

    // CRITICAL FIX: Add error handling for sendToMesh
    if (!service) {
        LOG_ERROR(DS_LOG_PREFIX "MeshService is NULL - cannot send reply!");
        packetPool.release(reply);
        return;
    }

    // sendToMesh queues the packet asynchronously - it does NOT block radio
    // The packet will be sent when radio is available via txQueue
    // This ensures replies are never lost, just queued for later transmission
    // NOTE: sendToMesh will free the packet automatically (via router->sendLocal -> router->send)
    // If send fails, it returns ERRNO_SHOULD_RELEASE and MeshService releases the packet
    service->sendToMesh(reply, RX_SRC_LOCAL, true);

#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    LOG_INFO(DS_LOG_PREFIX "Queued auto-reply to node 0x%x", original.from);
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

    // sendToMesh queues the packet asynchronously - it does NOT block radio
    // The packet will be sent when radio is available via txQueue
    // NOTE: sendToMesh will free the packet automatically (via router->sendLocal -> router->send)
    // If send fails, it returns ERRNO_SHOULD_RELEASE and MeshService releases the packet
    service->sendToMesh(reply, RX_SRC_LOCAL, true);

    DS_LOG_DEBUG("DeviceStats monitoring update #%u queued %u bytes to 0x%x",
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
        // NOTE: txLastMinute already contains accumulated packets for the last minute
        // When reset occurs, start fresh with current delta for the new minute
        if (now - lastMinuteReset >= 60000) {
            txLastMinute = txDelta; // Start new minute with current delta
            rxLastMinute = rxDelta;
            lastMinuteReset = now;
        }

        // Reset hour counter every 3600 seconds
        // NOTE: txLastHour already contains accumulated packets for the last hour
        // When reset occurs, start fresh with current delta for the new hour
        if (now - lastHourReset >= 3600000) {
            txLastHour = txDelta; // Start new hour with current delta
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
                    // Check TX queue status (fromRadioQueue is private, so we can't check RX queue)
                    meshtastic_QueueStatus txStatus = router->getQueueStatus();
                    if (txStatus.free > 0) {
                        sendMemoryStats(monitoringNodeId);
                    } else {
                        LOG_WARN(DS_LOG_PREFIX "Cannot send monitoring message: TX queue full");
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
                        nodeCount, MAX_NUM_NODES,
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
    // NOTE: Main filesystem is always 28KB (7 pages) for config files
    // Extended filesystem for NodeDB (80 pages, 320 KB) is handled separately via getExtendedFSStats()
    flashTotal = 28 * 1024;  // LittleFS area size (7 pages * 4KB = 28 KB)
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
    uint32_t maxNodes = MAX_NUM_NODES;

    // Calculate available node slots (can be negative if over limit)
    int32_t availableNodeSlots = calculateAvailableNodeSlots(storedNodes, maxNodes);

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

    // Get extended filesystem stats for NodeDB (if enabled)
    uint32_t nodeDBFlashTotal = 0;
    uint32_t nodeDBFlashUsed = 0;
    uint32_t nodeDBFlashFree = 0;
    bool nodeDBFSEnabled = false;
    
    #ifdef USE_EXTENDED_FS_FOR_NODEDB
    extern bool getExtendedFSStats(uint32_t* total, uint32_t* used, uint32_t* free);
    nodeDBFSEnabled = getExtendedFSStats(&nodeDBFlashTotal, &nodeDBFlashUsed, &nodeDBFlashFree);
    #endif
    
    char nodeDBFlashUsedStr[32] = "0.000KB";
    char nodeDBFlashTotalStr[32] = "0.000KB";
    char nodeDBFlashFreeStr[32] = "0.000KB";
    
    if (nodeDBFSEnabled) {
        formatBytesHuman(nodeDBFlashUsed, nodeDBFlashUsedStr, sizeof(nodeDBFlashUsedStr));
        formatBytesHuman(nodeDBFlashTotal, nodeDBFlashTotalStr, sizeof(nodeDBFlashTotalStr));
        formatBytesHuman(nodeDBFlashFree, nodeDBFlashFreeStr, sizeof(nodeDBFlashFreeStr));
    }
    
    // Format detailed memory report (simplified to <300 chars)
    int result;
    if (memValid) {
        if (nodeDBFSEnabled) {
            // Extended filesystem enabled - show both main FS and NodeDB FS
            if (availableNodeSlots >= 0) {
                result = snprintf(buffer, bufferSize,
                    "📊 Memory\n"
                    "Config FS: %s/%s\n"
                    "NodeDB FS: %s/%s\n"
                    "Heap: %s/%s\n"
                    "Nodes: %u/%u online %d free",
                    flashUsedStr, flashTotalStr,
                    nodeDBFlashUsedStr, nodeDBFlashTotalStr,
                    heapUsedStr, heapTotalStr,
                    onlineNodes, storedNodes, availableNodeSlots);
            } else {
                result = snprintf(buffer, bufferSize,
                    "📊 Memory\n"
                    "Config FS: %s/%s\n"
                    "NodeDB FS: %s/%s\n"
                    "Heap: %s/%s\n"
                    "Nodes: %u/%u online OVER %d",
                    flashUsedStr, flashTotalStr,
                    nodeDBFlashUsedStr, nodeDBFlashTotalStr,
                    heapUsedStr, heapTotalStr,
                    onlineNodes, storedNodes, -availableNodeSlots);
            }
        } else {
            // Standard filesystem only
            if (availableNodeSlots >= 0) {
                result = snprintf(buffer, bufferSize,
                    "📊 Memory\n"
                    "Flash: %s/%s\n"
                    "Heap: %s/%s\n"
                    "Nodes: %u/%u online %d free",
                    flashUsedStr, flashTotalStr,
                    heapUsedStr, heapTotalStr,
                    onlineNodes, storedNodes, availableNodeSlots);
            } else {
                result = snprintf(buffer, bufferSize,
                    "📊 Memory\n"
                    "Flash: %s/%s\n"
                    "Heap: %s/%s\n"
                    "Nodes: %u/%u online OVER %d",
                    flashUsedStr, flashTotalStr,
                    heapUsedStr, heapTotalStr,
                    onlineNodes, storedNodes, -availableNodeSlots);
            }
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

    // Get last RX packet time (most recent last_heard from any node)
    // Only calculate if time is synchronized (from network/NTP/GPS)
    uint32_t lastRxTime = 0;
    uint32_t lastRxAge = 0;
    uint32_t currentTime = 0;
    RTCQuality timeQuality = RTCQualityNone;
    bool isTimeSynchronized = checkTimeSynchronization(currentTime, timeQuality);
    
    if (isTimeSynchronized && nodeDB) {
        uint32_t numNodes = nodeDB->getNumMeshNodes();
        for (uint32_t i = 0; i < numNodes && i < MAX_NODES_ARRAY_BOUND; i++) {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
            if (node && node->has_user && node->last_heard > 0) {
                if (node->last_heard > lastRxTime) {
                    lastRxTime = node->last_heard;
                }
            }
        }
        lastRxAge = (lastRxTime > 0 && currentTime > lastRxTime) ? (currentTime - lastRxTime) : 0;
    }

    // Format packet statistics report with clear sections
    int result;

    // Check if we have valid data from all sources
    if (radioValid && routerValid && airtimeValid) {
        // Full report with real minute/hour counters
        if (lastRxAge > 0 && lastRxAge < 86400) { // Show if less than 24 hours
            uint32_t lastRxMinutes = lastRxAge / 60;
            result = snprintf(buffer, bufferSize,
                "📦 Packets\n"
                "TX: %u (%u/m %u/h)\n"
                "RX: %u/%u %.0f%% (%u/m %u/h)\n"
                "Relay: %u/%u Dup: %u\n"
                "Ch: %.1f%% Air: %.1f%%\n"
                "Last RX: %um ago",
                txGood, txMin, txHr,
                rxGood, rxTotal, rxSuccessRate, rxMin, rxHr,
                txRelay, relayTotal, rxDupe,
                channelUtil, airUtilTx,
                lastRxMinutes);
        } else {
            result = snprintf(buffer, bufferSize,
                "📦 Packets\n"
                "TX: %u (%u/m %u/h)\n"
                "RX: %u/%u %.0f%% (%u/m %u/h)\n"
                "Relay: %u/%u Dup: %u\n"
                "Ch: %.1f%% Air: %.1f%%",
                txGood, txMin, txHr,
                rxGood, rxTotal, rxSuccessRate, rxMin, rxHr,
                txRelay, relayTotal, rxDupe,
                channelUtil, airUtilTx);
        }
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
            "🔋 Power\n"
            "Source: %s%s\n"
            "Battery: %u%% %.2fV",
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
    uint32_t currentTime = 0;
    RTCQuality timeQuality = RTCQualityNone;
    bool isTimeSynchronized = checkTimeSynchronization(currentTime, timeQuality);
    uint32_t onlineNodes = (isTimeSynchronized && nodeDB) ? nodeDB->getNumOnlineMeshNodes() : 0;

    // Format radio statistics report (optimized - no packet/nodes duplication)
    int result;
    if (radioValid) {
        result = snprintf(buffer, bufferSize,
            "📡 Radio\n"
            "Freq: %.3f MHz Ch%u\n"
            "SF%u BW%.0f Pwr: %ddBm",
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
    uint32_t currentTimeForLog = 0;
    RTCQuality timeQualityForLog = RTCQualityNone;
    bool isTimeSynchronizedForLog = checkTimeSynchronization(currentTimeForLog, timeQualityForLog);
    LOG_INFO(DS_LOG_PREFIX "RadioStats: radio=%d time=%d (quality=%s) uptime=%d (up=%us freq=%.1f)",
             radioValid, isTimeSynchronizedForLog, RtcName(timeQualityForLog), uptimeValid, uptime, frequency);
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

    // Get device temperature (if available)
    // Try to get temperature from environment telemetry module
    float deviceTemp = 0.0f;
    bool tempValid = false;
    #if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
    // Access environment telemetry module (declared in Modules.cpp)
    extern EnvironmentTelemetryModule *environmentTelemetryModule;
    if (environmentTelemetryModule) {
        meshtastic_Telemetry telemetry;
        if (environmentTelemetryModule->getEnvironmentTelemetry(&telemetry)) {
            if (telemetry.which_variant == meshtastic_Telemetry_environment_metrics_tag &&
                telemetry.variant.environment_metrics.has_temperature) {
                deviceTemp = telemetry.variant.environment_metrics.temperature;
                // Validate temperature range (-40 to +85 C is reasonable for embedded devices)
                if (deviceTemp >= -40.0f && deviceTemp <= 85.0f) {
                    tempValid = true;
                }
            }
        }
    }
    #endif

    // Format status information report (optimized)
    int result;
    if (nodeValid) {
        const char* roleShort =
            config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ? "R" :
            config.device.role == meshtastic_Config_DeviceConfig_Role_REPEATER ? "Rep" :
            config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT ? "C" : "?";

        if (tempValid) {
            result = snprintf(buffer, bufferSize,
                "ℹ️ Status\n"
                "Name: %s ID: !%08x\n"
                "Uptime: %ud %uh %um\n"
                "Reboots: %u Role: %s\n"
                "Temp: %.1f°C",
                nodeName,
                nodeId,
                uptimeDays, uptimeHours, uptimeMinutes,
                rebootCount, roleShort,
                deviceTemp);
        } else {
            result = snprintf(buffer, bufferSize,
                "ℹ️ Status\n"
                "Name: %s ID: !%08x\n"
                "Uptime: %ud %uh %um\n"
                "Reboots: %u Role: %s",
                nodeName,
                nodeId,
                uptimeDays, uptimeHours, uptimeMinutes,
                rebootCount, roleShort);
        }
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
    uint32_t maxNodes = MAX_NUM_NODES;

    uint32_t offlineNodes = (storedNodes > onlineNodes) ? (storedNodes - onlineNodes) : 0;
    // Calculate available node slots (can be negative if over limit)
    int32_t availableNodeSlots = calculateAvailableNodeSlots(storedNodes, maxNodes);

    // Calculate percentages
    float usagePercent = (maxNodes > 0) ? (storedNodes * 100.0f / maxNodes) : 0.0f;
    float onlinePercent = (storedNodes > 0) ? (onlineNodes * 100.0f / storedNodes) : 0.0f;

    // Diagnostic info: check time sync and last_heard values
    uint32_t currentTime = 0;
    RTCQuality timeQuality = RTCQualityNone;
    bool isTimeSynchronized = checkTimeSynchronization(currentTime, timeQuality);
    uint32_t nodesWithZeroTime = 0;
    uint32_t recentNodes = 0; // seen < 10 min

    // Calculate uptime
    uint32_t uptimeSeconds = millis() / 1000;
    uint32_t uptimeDays = uptimeSeconds / 86400;
    uint32_t uptimeHours = (uptimeSeconds % 86400) / 3600;
    uint32_t uptimeMinutes = (uptimeSeconds % 3600) / 60;

    // Only calculate recent/online if time is synchronized
    if (nodeDB && isTimeSynchronized) {
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

    if (isTimeSynchronized) {
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
    if (isTimeSynchronized) {
        // Show online/recent stats if time is valid
        if (availableNodeSlots >= 0) {
            result = snprintf(buffer, bufferSize,
                "🌐 Nodes\n"
                "Online: %u/%u %.0f%%\n"
                "Recent: %u <10m\n"
                "Stored: %u/%u free:%d\n"
                "Time: %s",
                onlineNodes, storedNodes, onlinePercent,
                recentNodes,
                storedNodes, maxNodes, availableNodeSlots,
                timeStr);
        } else {
            result = snprintf(buffer, bufferSize,
                "🌐 Nodes\n"
                "Online: %u/%u %.0f%%\n"
                "Recent: %u <10m\n"
                "Stored: %u/%u OVER:%d\n"
                "Time: %s",
                onlineNodes, storedNodes, onlinePercent,
                recentNodes,
                storedNodes, maxNodes, -availableNodeSlots,
                timeStr);
        }
    } else {
        // Time not synced - show simplified stats
        if (availableNodeSlots >= 0) {
            result = snprintf(buffer, bufferSize,
                "🌐 Nodes\n"
                "Stored: %u/%u free:%d\n"
                "Time: %s\n"
                "⚠️ Need Net/GPS time",
                storedNodes, maxNodes, availableNodeSlots,
                timeStr);
        } else {
            result = snprintf(buffer, bufferSize,
                "🌐 Nodes\n"
                "Stored: %u/%u OVER:%d\n"
                "Time: %s\n"
                "⚠️ Need Net/GPS time",
                storedNodes, maxNodes, -availableNodeSlots,
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

    // Check if time is synchronized before calculating online nodes
    uint32_t currentTime = 0;
    RTCQuality timeQuality = RTCQualityNone;
    bool isTimeSynchronized = checkTimeSynchronization(currentTime, timeQuality);
    uint32_t onlineNodes = (isTimeSynchronized && nodeDB) ? nodeDB->getNumOnlineMeshNodes() : 0;

    // Calculate actual NodeDB memory usage
    uint32_t nodeDbBytes = storedNodes * 250;  // Real memory usage based on actual nodes

    // Get TX queue status (network transmission queue)
    // Note: RX queue (fromRadioQueue) is private in Router, so we can't get its status
    meshtastic_QueueStatus txStatus = router ? router->getQueueStatus() : meshtastic_QueueStatus{0, 0, 0, 0};
    int txUsed = (txStatus.maxlen > txStatus.free) ? (txStatus.maxlen - txStatus.free) : 0;
    int txMax = txStatus.maxlen > 0 ? txStatus.maxlen : 16;

    // Get PacketHistory (DupeCache) statistics
    uint32_t dupeCacheCount = PacketStats::getPacketHistorySize();
    uint32_t oldestPacketAge = 0; // Not available without modifying PacketHistory

    // Get neighbor count (if NeighborInfoModule is enabled)
    uint32_t neighborCount = 0;
    bool neighborCountValid = false;
    if (neighborInfoModule && moduleConfig.neighbor_info.enabled) {
        // Access neighbors vector size through a workaround
        // Note: neighbors is private, but we can estimate from nodeDB
        // For now, we'll use a simple heuristic: count nodes with last_heard < 2 hours
        if (nodeDB && isTimeSynchronized) {
            uint32_t numNodes = nodeDB->getNumMeshNodes();
            for (uint32_t i = 0; i < numNodes && i < MAX_NODES_ARRAY_BOUND; i++) {
                meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
                if (node && node->has_user && node->last_heard > 0) {
                    uint32_t age = (currentTime > node->last_heard) ? (currentTime - node->last_heard) : 0;
                    if (age < 7200) { // Less than 2 hours
                        neighborCount++;
                    }
                }
            }
            neighborCountValid = true;
        }
    }

    // Determine criticality based on oldest packet age and cache fullness
    const char* criticalityWarning = "";
    uint32_t cacheFullness = (MAX_NUM_NODES > 0) ? (dupeCacheCount * 100 / MAX_NUM_NODES) : 0;

    if (oldestPacketAge < 90 && dupeCacheCount > 200) {
        criticalityWarning = "\n⚠️ CRITICAL: Packets <90s evicted!";
    } else if (oldestPacketAge < 120 && dupeCacheCount > 230) {
        criticalityWarning = "\n⚠️ WARNING: DupeCache filling fast";
    } else if (cacheFullness > 85) {
        criticalityWarning = "\n⚠️ DupeCache >85% full";
    }

    // Format debug information report (optimized - compact format)
    int result;
    if (isTimeSynchronized) {
        if (neighborCountValid) {
            result = snprintf(buffer, bufferSize,
                "🔧 Debug\n"
                "Heap: %u/%uKB %u%%\n"
                "Nodes: %u online %u stored DB:%uKB\n"
                "DupeCache: %u/%u %u%% %us\n"
                "TX Queue: %d/%d\n"
                "Neighbors: %u Time: %s%s",
                usedHeap/1024, totalHeap/1024, heapPercent,
                onlineNodes, storedNodes, nodeDbBytes/1024,
                dupeCacheCount, MAX_NUM_NODES, cacheFullness, oldestPacketAge,
                txUsed, txMax,
                neighborCount,
                RtcName(timeQuality),
                criticalityWarning);
        } else {
            result = snprintf(buffer, bufferSize,
                "🔧 Debug\n"
                "Heap: %u/%uKB %u%%\n"
                "Nodes: %u online %u stored DB:%uKB\n"
                "DupeCache: %u/%u %u%% %us\n"
                "TX Queue: %d/%d\n"
                "Time: %s%s",
                usedHeap/1024, totalHeap/1024, heapPercent,
                onlineNodes, storedNodes, nodeDbBytes/1024,
                dupeCacheCount, MAX_NUM_NODES, cacheFullness, oldestPacketAge,
                txUsed, txMax,
                RtcName(timeQuality),
                criticalityWarning);
        }
    } else {
        // Show simplified when time is not synced
        const char* timeShort =
            (timeQuality == RTCQualityNone) ? "None" :
            (timeQuality == RTCQualityDevice) ? "RTC" : "?";

        if (neighborCountValid) {
            result = snprintf(buffer, bufferSize,
                "🔧 Debug\n"
                "Heap: %u/%uKB %u%%\n"
                "Nodes: %u stored DB:%uKB\n"
                "DupeCache: %u/%u %us\n"
                "TX Queue: %d/%d\n"
                "Neighbors: %u ⚠️Time: %s%s",
                usedHeap/1024, totalHeap/1024, heapPercent,
                storedNodes, nodeDbBytes/1024,
                dupeCacheCount, MAX_NUM_NODES, oldestPacketAge,
                txUsed, txMax,
                neighborCount,
                timeShort,
                criticalityWarning);
        } else {
            result = snprintf(buffer, bufferSize,
                "🔧 Debug\n"
                "Heap: %u/%uKB %u%%\n"
                "Nodes: %u stored DB:%uKB\n"
                "DupeCache: %u/%u %us\n"
                "TX Queue: %d/%d\n"
                "⚠️Time: %s%s",
                usedHeap/1024, totalHeap/1024, heapPercent,
                storedNodes, nodeDbBytes/1024,
                dupeCacheCount, MAX_NUM_NODES, oldestPacketAge,
                txUsed, txMax,
                timeShort,
                criticalityWarning);
        }
    }

    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN(DS_LOG_PREFIX "Debug info message truncated");
    }
}