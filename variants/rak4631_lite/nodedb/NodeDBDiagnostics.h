/**
 * @file NodeDBDiagnostics.h
 * @brief Diagnostic functions for NodeDB virtual backend debugging and monitoring
 */

#pragma once

#include "configuration.h"

#ifdef USE_EXTENDED_FS_FOR_NODEDB

#include <cstdint>
#include <cstddef>

namespace NodeDBDiagnostics {

/**
 * @brief Memory statistics structure
 */
struct MemoryStats {
    uint32_t freeHeap;
    uint32_t heapTotal;
    uint32_t usedHeap;
    uint32_t allocationFailures;
};

/**
 * @brief Component statistics structure
 */
struct ComponentStats {
    bool cacheInitialized;
    uint32_t cacheSize;
    uint32_t cacheMaxSize;
    bool indexInitialized;
    uint32_t indexSize;
    uint32_t indexMaxSize;
    bool queueInitialized;
    uint32_t queueSize;
    uint32_t queueMaxSize;
    bool backendInitialized;
    bool backendEnabled;
};

/**
 * @brief Backend status structure
 */
struct BackendStatus {
    MemoryStats memory;
    ComponentStats components;
    uint32_t totalNodes;
    uint32_t cachedNodes;
    uint32_t freeFlashSlots;
    bool indexLoaded;
};

/**
 * @brief Print memory statistics
 */
void printMemoryStats();

/**
 * @brief Print component statistics
 */
void printComponentStats();

/**
 * @brief Print backend status
 */
void printBackendStatus();

/**
 * @brief Get memory statistics
 * @return MemoryStats structure
 */
MemoryStats getMemoryStats();

/**
 * @brief Get component statistics
 * @return ComponentStats structure
 */
ComponentStats getComponentStats();

/**
 * @brief Get backend status
 * @return BackendStatus structure
 */
BackendStatus getBackendStatus();

/**
 * @brief Validate backend integrity
 * @return true if backend is valid, false if corrupted
 */
bool validateBackendIntegrity();

/**
 * @brief Dump full backend state for debugging
 */
void dumpBackendState();

} // namespace NodeDBDiagnostics

#endif // USE_EXTENDED_FS_FOR_NODEDB

