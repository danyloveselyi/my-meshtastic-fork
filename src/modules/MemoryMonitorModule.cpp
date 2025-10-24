#include "MemoryMonitorModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "memGet.h"
#include "configuration.h"
#include "mesh/mesh-pb-constants.h"
#include "variant.h"

MemoryMonitorModule *memoryMonitorModule;

MemoryMonitorModule::MemoryMonitorModule() : ProtobufModule("MemoryMonitor", meshtastic_PortNum_TELEMETRY_APP, &meshtastic_Telemetry_msg)
{
    // Initialize with disabled monitoring
    monitoringEnabled = false;
    monitoringInterval = 0;
}

int32_t MemoryMonitorModule::runOnce()
{
    if (!monitoringEnabled || monitoringInterval == 0) {
        return 60000; // Check every minute if monitoring is disabled
    }

    uint32_t now = millis();
    // Handle millis() overflow safely
    uint32_t elapsed = (now >= lastMemoryCheck) ? (now - lastMemoryCheck) : (UINT32_MAX - lastMemoryCheck + now + 1);
    if (elapsed >= monitoringInterval) {
        printMemoryStats();

        // Track memory usage extremes
        uint32_t freeHeap = memGet.getFreeHeap();
        uint32_t heapTotal = memGet.getHeapSize();
        uint32_t heapUsed = (heapTotal >= freeHeap) ? (heapTotal - freeHeap) : 0; // Prevent underflow

        if (freeHeap < minHeapSeen) {
            minHeapSeen = freeHeap;
            LOG_WARN("⚠️  NEW MIN HEAP: %u bytes free (used: %u/%u)", freeHeap, heapUsed, heapTotal);
        }

        if (heapUsed > maxHeapUsed) {
            maxHeapUsed = heapUsed;
            LOG_INFO("📈 NEW MAX HEAP USAGE: %u bytes used (free: %u/%u)", heapUsed, freeHeap, heapTotal);
        }

        // Check if memory is critically low - platform-specific thresholds
        uint32_t criticalThreshold = (heapTotal < 32768) ? 2048 : 8192; // 2KB for small systems, 8KB for larger
        if (freeHeap < criticalThreshold) {
            LOG_ERROR("🚨 CRITICAL: Only %u bytes heap free! Memory leak possible!", freeHeap);
            // Log the critical memory state for debugging
            LOG_ERROR("Free heap: %u, Total heap: %u, Nodes: %u/%u",
                     freeHeap, heapTotal, nodeDB ? nodeDB->getNumMeshNodes() : 0, dynamic_max_nodes);
        }

        lastMemoryCheck = now;
    }

    return monitoringInterval > 0 ? monitoringInterval : 60000;
}

void MemoryMonitorModule::printMemoryStats()
{
    uint32_t flashTotal = memGet.getFlashTotal() / 1024;
    uint32_t flashUsed = memGet.getFlashUsed() / 1024;
    uint32_t flashFree = memGet.getFlashFree() / 1024;
    uint32_t heapTotal = memGet.getHeapSize() / 1024;
    uint32_t heapFree = memGet.getFreeHeap() / 1024;
    uint32_t heapUsed = heapTotal - heapFree;
    uint32_t nodes = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    uint32_t nodesOnline = nodeDB ? nodeDB->getNumOnlineMeshNodes() : 0;

    LOG_INFO("💾 Memory: Flash=%uKB used/%uKB free, Heap=%uKB used/%uKB free, Nodes=%u online/%u total",
             flashUsed, flashFree, heapUsed, heapFree, nodesOnline, nodes);
}

void MemoryMonitorModule::printDetailedMemoryStats()
{
    uint32_t flashTotal = memGet.getFlashTotal();
    uint32_t flashUsed = memGet.getFlashUsed();
    uint32_t flashFree = memGet.getFlashFree();
    uint32_t heapTotal = memGet.getHeapSize();
    uint32_t heapFree = memGet.getFreeHeap();
    uint32_t heapUsed = (heapTotal >= heapFree) ? (heapTotal - heapFree) : 0; // Prevent underflow
    uint32_t nodes = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    uint32_t nodesOnline = nodeDB ? nodeDB->getNumOnlineMeshNodes() : 0;
    uint32_t maxNodes = dynamic_max_nodes;

    LOG_INFO("=== 📊 DETAILED MEMORY REPORT ===");
    LOG_INFO("Flash Memory:");
    LOG_INFO("  Total: %u bytes (%.1f KB)", flashTotal, flashTotal/1024.0f);
    if (flashTotal > 0) {
        LOG_INFO("  Used:  %u bytes (%.1f KB) - %.1f%%", flashUsed, flashUsed/1024.0f, (flashUsed*100.0f)/flashTotal);
        LOG_INFO("  Free:  %u bytes (%.1f KB) - %.1f%%", flashFree, flashFree/1024.0f, (flashFree*100.0f)/flashTotal);
    }

    LOG_INFO("Heap Memory:");
    LOG_INFO("  Total: %u bytes (%.1f KB)", heapTotal, heapTotal/1024.0f);
    if (heapTotal > 0) {
        LOG_INFO("  Used:  %u bytes (%.1f KB) - %.1f%%", heapUsed, heapUsed/1024.0f, (heapUsed*100.0f)/heapTotal);
        LOG_INFO("  Free:  %u bytes (%.1f KB) - %.1f%%", heapFree, heapFree/1024.0f, (heapFree*100.0f)/heapTotal);
    }

    if (minHeapSeen != UINT32_MAX) {
        LOG_INFO("  Min seen: %u bytes (%.1f KB)", minHeapSeen, minHeapSeen/1024.0f);
        LOG_INFO("  Max used: %u bytes (%.1f KB)", maxHeapUsed, maxHeapUsed/1024.0f);
    }

    LOG_INFO("Node Database:");
    LOG_INFO("  Online nodes: %u", nodesOnline);
    LOG_INFO("  Total nodes:  %u", nodes);
    LOG_INFO("  Max nodes:    %u", maxNodes);
    LOG_INFO("  Free slots:   %u", (maxNodes >= nodes) ? (maxNodes - nodes) : 0);
    if (maxNodes > 0) {
        LOG_INFO("  DB usage:     %.1f%%", (nodes*100.0f)/maxNodes);
    }

    // Estimate memory per node
    if (nodes > 0) {
        // Rough estimate: each node uses ~250 bytes
        uint32_t estimatedNodeMemory = nodes * 250;
        LOG_INFO("  Est. memory:  %u bytes (%.1f KB) for %u nodes", estimatedNodeMemory, estimatedNodeMemory/1024.0f, nodes);
    }

    // Queue and Buffer Status (simplified - queues are private)
    LOG_INFO("Queue Status:");
    LOG_INFO("  toPhoneQueue: max %u packets", MAX_RX_TOPHONE);
    LOG_INFO("  statusQueue:  max %u packets", MAX_RX_TOPHONE);
    LOG_INFO("  notifQueue:   max %u packets", MAX_RX_TOPHONE/2);

    // Packet Pool Status
    LOG_INFO("Packet Pool:");
    uint32_t maxPackets = MAX_RX_TOPHONE + 4 + 2 * 8 + 2; // Estimated calculation
    LOG_INFO("  Est. MAX_PACKETS: %u (calculated)", maxPackets);
    LOG_INFO("  Packet size:  %u bytes", sizeof(meshtastic_MeshPacket));
    LOG_INFO("  Est. pool mem: %u bytes (%.1f KB)", maxPackets * sizeof(meshtastic_MeshPacket), (maxPackets * sizeof(meshtastic_MeshPacket))/1024.0f);

    // Buffer Status
    LOG_INFO("Buffer Status:");
    LOG_INFO("  Radio buffer: %u bytes (MAX_LORA_PAYLOAD_LEN)", MAX_LORA_PAYLOAD_LEN);
    LOG_INFO("  fromRadioQueue: %u max packets", 4); // MAX_RX_FROMRADIO = 4

    LOG_INFO("================================");
}

void MemoryMonitorModule::startContinuousMonitoring(uint32_t intervalMs)
{
    monitoringEnabled = true;
    monitoringInterval = intervalMs;
    lastMemoryCheck = millis();
    LOG_INFO("🔍 Memory monitoring started - interval: %u seconds", intervalMs/1000);

    // Print initial stats
    printDetailedMemoryStats();
}

void MemoryMonitorModule::stopContinuousMonitoring()
{
    monitoringEnabled = false;
    monitoringInterval = 0;
    LOG_INFO("⏹️  Memory monitoring stopped");
}

bool MemoryMonitorModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *decoded)
{
    // We don't handle incoming telemetry, just monitor memory
    return false;
}