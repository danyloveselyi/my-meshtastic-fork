/**
 * @file NodeDBDiagnostics.cpp
 * @brief Diagnostic functions implementation for NodeDB virtual backend
 */

#include "NodeDBDiagnostics.h"
#include "NodeDBVirtualBackend.h"
#include "NodeCache.h"
#include "NodeIndex.h"
#include "NodeDBWriteQueue.h"
#include "NodeStorage.h"
#include "../../../src/memGet.h"

#ifdef USE_EXTENDED_FS_FOR_NODEDB

#ifdef ARCH_NRF52
extern uint32_t getAllocationFailureCount();
#endif

namespace NodeDBDiagnostics {

MemoryStats getMemoryStats()
{
    MemoryStats stats;
    stats.freeHeap = memGet.getFreeHeap();
    stats.heapTotal = memGet.getHeapSize();
    stats.usedHeap = (stats.heapTotal > stats.freeHeap) ? (stats.heapTotal - stats.freeHeap) : 0;
    
    #ifdef ARCH_NRF52
    stats.allocationFailures = getAllocationFailureCount();
    #else
    stats.allocationFailures = 0;
    #endif
    
    return stats;
}

ComponentStats getComponentStats()
{
    ComponentStats stats;
    
    // Cache stats - using getCacheSize() and getMaxCacheSize()
    stats.cacheSize = NodeCache::getCacheSize();
    stats.cacheMaxSize = NodeCache::getMaxCacheSize();
    stats.cacheInitialized = (stats.cacheMaxSize > 0);
    
    // Index stats - using getNodeCount()
    stats.indexSize = NodeIndex::getNodeCount();
    // Use MAX_NODES_INDEX if defined, otherwise MAX_NUM_NODES
    #ifdef MAX_NODES_INDEX
    stats.indexMaxSize = MAX_NODES_INDEX;
    #elif defined(MAX_NODES_SLOTS)
    stats.indexMaxSize = MAX_NODES_SLOTS;
    #elif defined(MAX_NUM_NODES)
    stats.indexMaxSize = MAX_NUM_NODES;
    #else
    stats.indexMaxSize = 0;
    #endif
    // Index is initialized if we can get node count (returns 0 if not initialized, but that's valid)
    stats.indexInitialized = (stats.indexMaxSize > 0);
    
    // Queue stats
    stats.queueInitialized = NodeDBWriteQueue::isInitialized();
    if (stats.queueInitialized) {
        stats.queueSize = NodeDBWriteQueue::getSize();
        stats.queueMaxSize = NodeDBWriteQueue::getMaxSize();
    } else {
        stats.queueSize = 0;
        stats.queueMaxSize = 0;
    }
    
    // Backend stats
    stats.backendInitialized = NodeDBVirtualBackend::isEnabled();
    stats.backendEnabled = NodeDBVirtualBackend::isEnabled();
    
    return stats;
}

BackendStatus getBackendStatus()
{
    BackendStatus status;
    status.memory = getMemoryStats();
    status.components = getComponentStats();
    
    if (status.components.backendEnabled) {
        status.totalNodes = NodeDBVirtualBackend::getTotalNodeCount();
        status.cachedNodes = NodeDBVirtualBackend::getCachedNodeCount();
        status.freeFlashSlots = NodeDBVirtualBackend::getFreeFlashSlots();
        // Note: index_loaded is internal, we can't access it directly
        // We'll infer it from whether we have nodes
        status.indexLoaded = (status.totalNodes > 0 || status.components.indexSize > 0);
    } else {
        status.totalNodes = 0;
        status.cachedNodes = 0;
        status.freeFlashSlots = 0;
        status.indexLoaded = false;
    }
    
    return status;
}

void printMemoryStats()
{
    MemoryStats stats = getMemoryStats();
    LOG_INFO("=== NodeDB Memory Statistics ===");
    LOG_INFO("Free heap: %u bytes (%u.%02u KB)", stats.freeHeap, stats.freeHeap / 1024, (stats.freeHeap % 1024) * 100 / 1024);
    LOG_INFO("Used heap: %u bytes (%u.%02u KB)", stats.usedHeap, stats.usedHeap / 1024, (stats.usedHeap % 1024) * 100 / 1024);
    LOG_INFO("Total heap: %u bytes (%u.%02u KB)", stats.heapTotal, stats.heapTotal / 1024, (stats.heapTotal % 1024) * 100 / 1024);
    uint32_t heap_usage_pct = (stats.heapTotal > 0) ? (stats.usedHeap * 100) / stats.heapTotal : 0;
    LOG_INFO("Heap usage: %u%%", heap_usage_pct);
    LOG_INFO("Allocation failures: %u", stats.allocationFailures);
}

void printComponentStats()
{
    ComponentStats stats = getComponentStats();
    LOG_INFO("=== NodeDB Component Statistics ===");
    LOG_INFO("Backend: %s (initialized: %s)", 
             stats.backendEnabled ? "ENABLED" : "DISABLED",
             stats.backendInitialized ? "YES" : "NO");
    LOG_INFO("Cache: %s (size: %u/%u)", 
             stats.cacheInitialized ? "INITIALIZED" : "NOT INITIALIZED",
             stats.cacheSize, stats.cacheMaxSize);
    LOG_INFO("Index: %s (size: %u/%u)", 
             stats.indexInitialized ? "INITIALIZED" : "NOT INITIALIZED",
             stats.indexSize, stats.indexMaxSize);
    LOG_INFO("Write Queue: %s (size: %u/%u)", 
             stats.queueInitialized ? "INITIALIZED" : "NOT INITIALIZED",
             stats.queueSize, stats.queueMaxSize);
}

void printBackendStatus()
{
    BackendStatus status = getBackendStatus();
    LOG_INFO("=== NodeDB Backend Status ===");
    printMemoryStats();
    printComponentStats();
    
    if (status.components.backendEnabled) {
        LOG_INFO("=== NodeDB Data Statistics ===");
        LOG_INFO("Total nodes: %u", status.totalNodes);
        LOG_INFO("Cached nodes: %u", status.cachedNodes);
        LOG_INFO("Free flash slots: %u", status.freeFlashSlots);
        LOG_INFO("Index loaded: %s", status.indexLoaded ? "YES" : "NO");
    } else {
        LOG_INFO("Backend is disabled - no data statistics available");
    }
}

bool validateBackendIntegrity()
{
    LOG_INFO("=== NodeDB Backend Integrity Check ===");
    
    BackendStatus status = getBackendStatus();
    
    // Check memory
    if (status.memory.freeHeap < 10240) {  // Less than 10 KB free
        LOG_WARN("Memory check: LOW free heap (%u bytes)", status.memory.freeHeap);
    } else {
        LOG_INFO("Memory check: OK (%u bytes free)", status.memory.freeHeap);
    }
    
    if (status.memory.allocationFailures > 0) {
        LOG_WARN("Memory check: %u allocation failures detected", status.memory.allocationFailures);
    } else {
        LOG_INFO("Memory check: No allocation failures");
    }
    
    // Check components
    bool allOk = true;
    
    if (!status.components.backendInitialized) {
        LOG_WARN("Backend: NOT INITIALIZED");
        allOk = false;
    } else {
        LOG_INFO("Backend: INITIALIZED");
    }
    
    if (!status.components.cacheInitialized) {
        LOG_WARN("Cache: NOT INITIALIZED");
        allOk = false;
    } else {
        LOG_INFO("Cache: OK (size: %u/%u)", status.components.cacheSize, status.components.cacheMaxSize);
    }
    
    if (!status.components.indexInitialized) {
        LOG_WARN("Index: NOT INITIALIZED");
        allOk = false;
    } else {
        LOG_INFO("Index: OK (size: %u/%u)", status.components.indexSize, status.components.indexMaxSize);
    }
    
    if (!status.components.queueInitialized) {
        LOG_WARN("Write Queue: NOT INITIALIZED");
        // Queue is optional, so this is not critical
    } else {
        LOG_INFO("Write Queue: OK (size: %u/%u)", status.components.queueSize, status.components.queueMaxSize);
    }
    
    // Validate index integrity if backend is enabled
    if (status.components.backendEnabled) {
        if (NodeDBVirtualBackend::validateIndexIntegrity()) {
            LOG_INFO("Index integrity: VALID");
        } else {
            LOG_WARN("Index integrity: INVALID - may need rebuild");
            allOk = false;
        }
    }
    
    if (allOk) {
        LOG_INFO("=== Integrity Check: PASSED ===");
    } else {
        LOG_WARN("=== Integrity Check: FAILED ===");
    }
    
    return allOk;
}

void dumpBackendState()
{
    LOG_INFO("========================================");
    LOG_INFO("NodeDB Backend Full State Dump");
    LOG_INFO("========================================");
    printBackendStatus();
    LOG_INFO("========================================");
    validateBackendIntegrity();
    LOG_INFO("========================================");
}

} // namespace NodeDBDiagnostics

#endif // USE_EXTENDED_FS_FOR_NODEDB

