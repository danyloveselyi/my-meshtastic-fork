#include "TextMessageModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "buzz.h"
#include "configuration.h"
#include "mesh/Router.h"
#include "memGet.h"
#include "mesh/mesh-pb-constants.h"
#include "variant.h"
#include <algorithm>
#include <cctype>
#include <cstring>
extern Router *router;

// DEFAULT_MAX_NODES is defined per variant:
// - rak4631_eth_gw: extern const uint32_t DEFAULT_MAX_NODES = 350
// - rak4631: extern const uint32_t DEFAULT_MAX_NODES = 80
// Note: No fallback needed as all variants should define this

TextMessageModule *textMessageModule;

ProcessMessage TextMessageModule::handleReceived(const meshtastic_MeshPacket &mp)
{
#ifdef DEBUG_PORT
    auto &p = mp.decoded;
    LOG_INFO("Received text msg from=0x%0x, id=0x%x, msg=%.*s", mp.from, mp.id, p.payload.size, p.payload.bytes);
#endif
    // For router mode: do not store messages to save RAM
    // devicestate.rx_text_message = mp;
    // devicestate.has_rx_text_message = true;

    powerFSM.trigger(EVENT_RECEIVED_MSG);
    // If this is a direct message (not a broadcast), send an auto-reply
    if (mp.to == nodeDB->getNodeNum() && mp.to != NODENUM_BROADCAST) {
        sendAutoReply(mp);
    }
    notifyObservers(&mp);

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}


void TextMessageModule::sendAutoReply(const meshtastic_MeshPacket &original)
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

    // Check if this is a memory stats command
    if (isCommand && (strcmp(trimmed, "mem") == 0 || strcmp(trimmed, "memory") == 0)) {
        // Create reply with memory stats using shared formatting
        char statsBuffer[200];
        formatMemoryStats(statsBuffer, sizeof(statsBuffer));
        replyText = statsBuffer;
    } else if (isCommand && (strcmp(trimmed, "mon start") == 0 || strcmp(trimmed, "monstart") == 0)) {
        // Start interactive setup for monitoring interval
        waitingIntervalNodeId = original.from;
        replyText = "Send interval in seconds (10-86400). Ex: 30, 3600(1h)";
    } else if (isCommand && (strcmp(trimmed, "mon stop") == 0 || strcmp(trimmed, "monstop") == 0)) {
        monitoringNodeId = 0;
        waitingIntervalNodeId = 0;
        monitorMessageCounter = 0;
        replyText = "Memory monitoring stopped.";
    } else if (isCommand && (strcmp(trimmed, "maxnodes") == 0 || strcmp(trimmed, "max nodes") == 0)) {
        // Show current max nodes setting with safety info
        snprintf(replyBuffer, sizeof(replyBuffer), 
                 "Max nodes: %u (default: %u). Memory: ~%.1fKB. Send /setmaxnodes to increase.", 
                 dynamic_max_nodes, DEFAULT_MAX_NODES, (dynamic_max_nodes * 250) / 1024.0f);
        replyText = replyBuffer;
    } else if (isCommand && (strcmp(trimmed, "setmaxnodes") == 0 || strcmp(trimmed, "set max nodes") == 0)) {
        // Ask for max nodes value with safety warning
        waitingMaxNodesNodeId = original.from;
        snprintf(replyBuffer, sizeof(replyBuffer), 
                 "Increase max nodes (%u-1000). Current: %u. Cannot decrease below default for safety. Example: 400", 
                 DEFAULT_MAX_NODES, dynamic_max_nodes);
        replyText = replyBuffer;
    } else if (isCommand && (strcmp(trimmed, "help") == 0 || strcmp(trimmed, "?") == 0 || strcmp(trimmed, "commands") == 0)) {
        // Show available commands with safety note
        snprintf(replyBuffer, sizeof(replyBuffer), 
                 "Commands: /mem /monstart /monstop /maxnodes /setmaxnodes /help. Note: setmaxnodes can only increase (safe default: %u)", 
                 DEFAULT_MAX_NODES);
        replyText = replyBuffer;
    } else if (waitingMaxNodesNodeId == original.from) {
        // User is setting max nodes value with safety protection
        // SAFETY: Only allow increases from default to prevent data loss from std::vector::resize()
        // Decreasing would use resize() which removes elements from the end, potentially losing recent nodes
        int maxNodes = atoi(originalText);
        if (maxNodes >= DEFAULT_MAX_NODES && maxNodes <= 1000) {
            // Safe range: can only increase from default, never decrease
            dynamic_max_nodes = maxNodes;
            // Safely resize node database vector (only growing, never shrinking)
            if (nodeDB && nodeDB->meshNodes) {
                nodeDB->meshNodes->resize(dynamic_max_nodes);
                // No truncation needed since we only allow increases
            }
            waitingMaxNodesNodeId = 0; // Clear waiting state
            snprintf(replyBuffer, sizeof(replyBuffer), "Max nodes increased to %d. Memory usage: ~%.1fKB.", 
                     maxNodes, (maxNodes * 250) / 1024.0f);
            replyText = replyBuffer;
        } else if (maxNodes < DEFAULT_MAX_NODES) {
            // Protection: cannot decrease below safe default
            waitingMaxNodesNodeId = 0; // Clear waiting state
            snprintf(replyBuffer, sizeof(replyBuffer), 
                     "Error: Cannot set below default %u nodes. Current: %u. Decreasing node limit can lose recent node data. Use reboot to reset to default.", 
                     DEFAULT_MAX_NODES, dynamic_max_nodes);
            replyText = replyBuffer;
        } else {
            // Invalid range
            replyText = "Invalid. Use 350-1000 (cannot decrease below default).";
        }
    } else if (waitingIntervalNodeId == original.from) {
        // User is setting up monitoring interval
        int interval = atoi(originalText);
        if (interval >= 10 && interval <= 86400) {
            monitorIntervalMs = interval * 1000; // Convert to milliseconds
            monitoringNodeId = original.from;
            lastMonitorTime = millis();
            waitingIntervalNodeId = 0; // Clear waiting state
            monitorMessageCounter = 0; // Reset counter for new monitoring session

            snprintf(replyBuffer, sizeof(replyBuffer),
                     "Monitor ON: %ds intervals. Send /monstop to disable.", interval);
            replyText = replyBuffer;
        } else {
            replyText = "Invalid interval! Use 10-86400 seconds.";
        }
    } else {
        // Regular auto-reply for other messages - limit length to avoid memory issues
        snprintf(replyBuffer, sizeof(replyBuffer), "Auto-reply: %.200s", originalText);
        replyText = replyBuffer;
    }

    meshtastic_MeshPacket *reply = router->allocForSending();
    if (!reply) return;

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

void TextMessageModule::sendMemoryStats(uint32_t toNode)
{
    // Increment message counter for monitoring sequence (safe overflow)
    monitorMessageCounter++;
    // Counter will safely overflow from UINT32_MAX back to 0 after ~136 years at 30s intervals

    // Format monitoring message with counter in square brackets using shared formatting
    char statsBuffer[200];
    char prefix[20];
    snprintf(prefix, sizeof(prefix), "[ %u ] ", monitorMessageCounter);
    formatMemoryStats(statsBuffer, sizeof(statsBuffer), prefix);

    // Allocate packet with safety check for long-term monitoring
    meshtastic_MeshPacket *reply = router->allocForSending();
    if (!reply) {
        LOG_ERROR("Failed to allocate packet for memory stats - monitoring may need restart");
        // Don't reset monitoring automatically to avoid infinite loops
        return;
    }

    reply->to = toNode;
    reply->decoded.want_response = false;
    reply->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;

    size_t msgLen = strlen(statsBuffer);
    reply->decoded.payload.size = std::min(msgLen, sizeof(reply->decoded.payload.bytes));
    memcpy(reply->decoded.payload.bytes, statsBuffer, reply->decoded.payload.size);

    service->sendToMesh(reply, RX_SRC_LOCAL, true);
}

void TextMessageModule::doPeriodicWork()
{
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

bool TextMessageModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return MeshService::isTextPayload(p);
}

void TextMessageModule::formatMemoryStats(char* buffer, size_t bufferSize, const char* prefix)
{
    // Safety check for long-term operation
    if (!buffer || bufferSize < 100) {
        LOG_ERROR("Invalid buffer for memory stats formatting");
        return;
    }

    // Get memory and node statistics with safety checks
    float flashTotal = memGet.getFlashTotal() / 1024.0f; // KB
    float flashFree = memGet.getFlashFree() / 1024.0f;   // KB
    uint32_t heapTotal = memGet.getHeapSize() / 1024;    // KB
    float heapFree = memGet.getFreeHeap() / 1024.0f;     // KB
    
    // Protect against null nodeDB (can happen during shutdown)
    uint32_t onlineNodes = nodeDB ? nodeDB->getNumOnlineMeshNodes() : 0;
    uint32_t totalNodes = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    uint32_t maxNodes = dynamic_max_nodes;  // Use dynamic value
    uint32_t freeSlots = (maxNodes > totalNodes) ? (maxNodes - totalNodes) : 0;

    // Calculate used amounts with overflow protection
    float flashUsed = (flashTotal >= flashFree) ? (flashTotal - flashFree) : 0.0f;
    uint32_t heapUsed = (heapTotal >= (uint32_t)heapFree) ? (heapTotal - (uint32_t)heapFree) : 0;

    // Format: total/used(free:amount) - safe for long-term operation
    int result = snprintf(buffer, bufferSize,
             "%sMem Stats: Flash=%.0f/%.0f(free:%.0f)KB Heap=%u/%u(free:%.0f)KB Nodes=%u/%u(free:%u)",
             prefix ? prefix : "",
             flashTotal, flashUsed, flashFree,
             heapTotal, heapUsed, heapFree,
             maxNodes, totalNodes, freeSlots);
             
    // Ensure null termination for safety
    if (result >= (int)bufferSize) {
        buffer[bufferSize - 1] = '\0';
        LOG_WARN("Memory stats message truncated");
    }
}