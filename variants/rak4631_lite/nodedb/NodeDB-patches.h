/**
 * @file NodeDB-patches.h
 * @brief Patches for NodeDB.cpp memory optimization and virtual backend support
 * 
 * This file provides memory optimization patches for NodeDB.cpp to prevent
 * allocation failures and support virtual backend. These patches are included
 * in NodeDB.cpp via conditional compilation to keep changes isolated to this variant.
 * 
 * Location: variants/rak4631_lite/nodedb/
 * Purpose: Isolate memory optimization and virtual backend changes from main codebase
 */

#pragma once

#ifdef USE_EXTENDED_FS_FOR_NODEDB
#ifdef ARCH_NRF52

#include "configuration.h"
#include "memGet.h"
#include "NodeDBVirtualBackend.h"
#include "mesh/NodeDB.h"  // For NodeNum, meshtastic_NodeInfoLite, etc.
#include <ErriezCRC32.h>  // For crc32Buffer()
#include <vector>
#include <cstdint>
#include <algorithm>
#include <functional>  // For std::function

// Forward declarations for global variables (defined in NodeDB.cpp)
extern meshtastic_NodeDatabase nodeDatabase;
extern meshtastic_DeviceState devicestate;
extern meshtastic_LocalConfig config;
extern meshtastic_ChannelFile channelFile;
extern NodeDB *nodeDB;

/**
 * @brief Patched version of meshNodes resize logic with memory check
 * 
 * This function checks available memory before resizing to prevent allocation
 * failures. It's used as a replacement for direct resize() calls in NodeDB.cpp.
 * 
 * @param meshNodes Pointer to the vector to resize
 * @param numMeshNodes Current number of nodes
 * @param maxNumNodes Maximum number of nodes to allocate
 */
inline void resizeMeshNodesSafely(std::vector<meshtastic_NodeInfoLite>* meshNodes, 
                                   int numMeshNodes, 
                                   uint32_t maxNumNodes)
{
    if (!meshNodes) return;
    
    // CRITICAL OPTIMIZATION: Check available memory before resize to prevent allocation failure
    // If vector is empty or small, resize might try to allocate MAX_NUM_NODES * sizeof(NodeInfoLite)
    // which can be 227KB - too much for available memory (181KB free)
    uint32_t freeHeap = memGet.getFreeHeap();
    uint32_t requiredMemory = MAX_NUM_NODES * sizeof(meshtastic_NodeInfoLite);
    
    // Only resize if we have enough memory, otherwise use current size or safe size
    if (freeHeap >= requiredMemory + 40960) {  // 40 KB safety margin
        meshNodes->resize(maxNumNodes);
    } else {
        // Calculate safe size based on available memory
        uint32_t safeMaxNodes = (freeHeap > 40960) ? ((freeHeap - 40960) / sizeof(meshtastic_NodeInfoLite)) : 1;
        if (safeMaxNodes > 256) safeMaxNodes = 256;  // Limit to reasonable maximum
        if (safeMaxNodes < (size_t)numMeshNodes) safeMaxNodes = numMeshNodes;  // At least current size
        
        if (meshNodes->size() < safeMaxNodes) {
            LOG_WARN("NodeDB: Insufficient memory for maxNumNodes (%u), using safe size (%u nodes, %u bytes free)", 
                    maxNumNodes, safeMaxNodes, freeHeap);
            meshNodes->resize(safeMaxNodes);
        } else {
            // Vector already has enough capacity, just ensure it's at least numMeshNodes
            if (meshNodes->size() < (size_t)numMeshNodes) {
                meshNodes->resize(numMeshNodes);
            }
        }
    }
}

/**
 * @brief Patched version of node insertion logic for virtual backend
 * 
 * This function handles node insertion when virtual backend is enabled.
 * It adds nodes to virtual backend instead of expanding the vector, preventing
 * memory allocation failures.
 * 
 * @param node Node data to insert
 * @param vec Pointer to the vector (should only contain local node)
 * @return true if node was successfully handled
 */
inline bool handleNodeInsertionForVirtualBackend(const meshtastic_NodeInfoLite& node, 
                                                  std::vector<meshtastic_NodeInfoLite>* vec)
{
    if (!vec) return false;
    
    // CRITICAL: If virtual backend is enabled, don't expand vector
    // Instead, add ALL nodes to virtual backend (which uses flash storage)
    // Vector should only contain local node (1 node max)
    if (NodeDBVirtualBackend::isEnabled()) {
        // Virtual backend is enabled - add node to virtual backend instead of vector
        // This prevents vector expansion which would cause memory allocation failure
        // Get or create node in virtual backend
        meshtastic_NodeInfoLite* backend_node = NodeDBVirtualBackend::getOrCreateNode(node.num);
        if (backend_node) {
            *backend_node = node;  // Copy node data to virtual backend
        }
        
        // Only add to vector if it's local node AND vector is empty
        // Vector should never have more than 1 node when virtual backend is enabled
        if (node.num == 0 && vec->size() == 0) {
            // Local node and vector is empty - safe to add (only 1 node)
            vec->push_back(node);
        }
        // For all other cases (network nodes or vector already has local node):
        // Don't add to vector - virtual backend handles it
        return true;
    }
    
    // Standard behavior: add to vector
    // WARNING: This can cause memory allocation failure if vector expands too much
    vec->push_back(node);
    return true;
}

/**
 * @brief Calculate saveWhat flags based on CRC changes and file existence
 * 
 * This function checks CRC after all modifications and also checks if files exist.
 * It ensures files are saved on first boot or after factory reset even if CRC didn't change.
 * 
 * @param devicestateCRC Initial CRC of devicestate (before modifications)
 * @param nodeDatabaseCRC Initial CRC of nodeDatabase (before modifications)
 * @param configCRC Initial CRC of config (before modifications)
 * @param channelFileCRC Initial CRC of channelFile (before modifications)
 * @param saveWhat Current saveWhat flags (will be modified)
 * @return Updated saveWhat flags
 */
inline int calculateSaveWhatFlags(uint32_t devicestateCRC, 
                                   uint32_t nodeDatabaseCRC, 
                                   uint32_t configCRC, 
                                   uint32_t channelFileCRC,
                                   int saveWhat)
{
    // CRITICAL: Check CRC AFTER all modifications to detect changes
    // If CRC changed, data was modified and needs to be saved
    extern meshtastic_DeviceState devicestate;
    extern meshtastic_NodeDatabase nodeDatabase;
    extern meshtastic_LocalConfig config;
    extern meshtastic_ChannelFile channelFile;
    
    if (devicestateCRC != crc32Buffer(&devicestate, sizeof(devicestate)))
        saveWhat |= SEGMENT_DEVICESTATE;
    if (nodeDatabaseCRC != crc32Buffer(&nodeDatabase, sizeof(nodeDatabase)))
        saveWhat |= SEGMENT_NODEDATABASE;
    if (configCRC != crc32Buffer(&config, sizeof(config)))
        saveWhat |= SEGMENT_CONFIG;
    if (channelFileCRC != crc32Buffer(&channelFile, sizeof(channelFile)))
        saveWhat |= SEGMENT_CHANNELS;
    
    // CRITICAL: Always save config and deviceState on first boot or if files don't exist
    // This ensures files are created even if CRC didn't change (e.g., after factory reset)
    #ifdef FSCom
    // Use string literals directly (same as defined in NodeDB.h as static constexpr)
    const char* configFileName = "/prefs/config.proto";
    const char* deviceStateFileName = "/prefs/device.proto";
    const char* moduleConfigFileName = "/prefs/module.proto";
    const char* channelFileName = "/prefs/channels.proto";
    
    if (!FSCom.exists(configFileName)) {
        saveWhat |= SEGMENT_CONFIG;
        LOG_DEBUG("Config file doesn't exist, will save");
    }
    if (!FSCom.exists(deviceStateFileName)) {
        saveWhat |= SEGMENT_DEVICESTATE;
        LOG_DEBUG("DeviceState file doesn't exist, will save");
    }
    if (!FSCom.exists(moduleConfigFileName)) {
        saveWhat |= SEGMENT_MODULECONFIG;
        LOG_DEBUG("ModuleConfig file doesn't exist, will save");
    }
    if (!FSCom.exists(channelFileName)) {
        saveWhat |= SEGMENT_CHANNELS;
        LOG_DEBUG("Channel file doesn't exist, will save");
    }
    #endif
    
    return saveWhat;
}

/**
 * @brief Initialize virtual backend in installDefaultNodeDatabase()
 * 
 * This function initializes virtual backend before allocating vector to prevent
 * large memory allocation. Returns true if virtual backend was initialized.
 * 
 * @param nodeDatabase Reference to nodeDatabase structure
 * @param numMeshNodes Reference to numMeshNodes counter
 * @param meshNodes Reference to meshNodes pointer
 * @return true if virtual backend was initialized, false otherwise
 */
inline bool initializeVirtualBackendForNodeDatabase(meshtastic_NodeDatabase& nodeDatabase,
                                                     pb_size_t& numMeshNodes,
                                                     std::vector<meshtastic_NodeInfoLite>*& meshNodes)
{
    // CRITICAL: For RAK4631 with virtual backend, initialize it BEFORE allocating vector
    // This prevents allocating 1234 nodes * 250 bytes = ~308 KB which exceeds available RAM
    // Virtual backend uses flash storage instead, so we only need a small vector for local node
    if (NodeDBVirtualBackend::isAvailable() && !NodeDBVirtualBackend::isEnabled()) {
        // Initialize virtual backend early to prevent large vector allocation
        if (NodeDBVirtualBackend::initialize()) {
            LOG_INFO("NodeDBVirtualBackend: Initialized early to prevent large vector allocation");
            // With virtual backend, we only need vector for local node (1 node)
            // Network nodes will be stored in virtual backend (flash + cache)
            nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(1);  // Only local node in vector
            numMeshNodes = 0;
            meshNodes = &nodeDatabase.nodes;
            return true;  // Virtual backend initialized successfully
        } else {
            LOG_WARN("NodeDBVirtualBackend: Failed to initialize, will use standard allocation with memory checks");
            // Don't do fallback here - let performStandardVectorAllocation() handle it
            return false;  // Virtual backend failed, will use standard allocation
        }
    } else if (NodeDBVirtualBackend::isEnabled()) {
        // Virtual backend already initialized - use minimal vector
        LOG_DEBUG("NodeDB: Virtual backend already enabled, using minimal vector (1 node)");
        nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(1);  // Only local node in vector
        if (nodeDatabase.nodes.size() == 1) {
            numMeshNodes = 0;
            meshNodes = &nodeDatabase.nodes;
            LOG_DEBUG("NodeDB: Minimal vector allocated successfully");
            return true;
        } else {
            LOG_ERROR("NodeDB: Vector allocation failed - size mismatch (expected 1, got %u)", 
                     nodeDatabase.nodes.size());
            return false;
        }
    }
    return false;  // Virtual backend not available
}

/**
 * @brief Standard vector allocation with memory checks and fallbacks
 * 
 * This function performs standard vector allocation with comprehensive memory checks
 * and fallback strategies if allocation fails. Used when virtual backend is not available
 * or failed to initialize.
 * 
 * @param nodeDatabase Reference to nodeDatabase structure
 * @param numMeshNodes Reference to numMeshNodes counter
 * @param meshNodes Reference to meshNodes pointer
 * @param maxNumNodes Maximum number of nodes to allocate (from MAX_NUM_NODES macro)
 */
inline void performStandardVectorAllocation(meshtastic_NodeDatabase& nodeDatabase,
                                             pb_size_t& numMeshNodes,
                                             std::vector<meshtastic_NodeInfoLite>*& meshNodes,
                                             uint32_t maxNumNodes)
{
    // Standard allocation (for platforms without virtual backend)
    // WARNING: For RAK4631 with MAX_NUM_NODES=1234, this will likely fail due to memory constraints
    // Virtual backend should be used instead
    LOG_DEBUG("NodeDB: Attempting standard vector allocation (MAX_NUM_NODES=%u)", maxNumNodes);
    
    uint32_t freeHeapBefore = memGet.getFreeHeap();
    uint32_t heapTotal = memGet.getHeapSize();
    uint32_t requiredMemory = maxNumNodes * sizeof(meshtastic_NodeInfoLite);
    
    LOG_DEBUG("NodeDB: Memory check - required: %u bytes (%u nodes * %u bytes)", 
             requiredMemory, maxNumNodes, sizeof(meshtastic_NodeInfoLite));
    LOG_DEBUG("NodeDB: Memory status - free: %u bytes, total: %u bytes", freeHeapBefore, heapTotal);
    
    if (freeHeapBefore < requiredMemory + 40960) {  // 40 KB safety margin
        LOG_ERROR("NodeDB: Insufficient memory for standard allocation");
        LOG_ERROR("NodeDB:   Required: %u bytes + 40 KB margin", requiredMemory);
        LOG_ERROR("NodeDB:   Available: %u bytes", freeHeapBefore);
        
        // Try reduced size
        uint32_t safeMaxNodes = (freeHeapBefore > 40960) ? ((freeHeapBefore - 40960) / sizeof(meshtastic_NodeInfoLite)) : 1;
        if (safeMaxNodes > 256) safeMaxNodes = 256;
        if (safeMaxNodes < 1) safeMaxNodes = 1;
        
        LOG_WARN("NodeDB: Attempting reduced allocation (%u nodes instead of %u)", safeMaxNodes, maxNumNodes);
        nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(safeMaxNodes);
        if (nodeDatabase.nodes.size() == safeMaxNodes) {
            numMeshNodes = 0;
            meshNodes = &nodeDatabase.nodes;
            LOG_WARN("NodeDB: Reduced allocation successful (%u nodes)", safeMaxNodes);
            return;
        } else {
            LOG_ERROR("NodeDB: Reduced allocation failed - size mismatch (expected %u, got %u)", 
                     safeMaxNodes, nodeDatabase.nodes.size());
        }
        
        // Last resort: minimal allocation
        LOG_WARN("NodeDB: Attempting minimal allocation (1 node)");
        nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(1);
        if (nodeDatabase.nodes.size() == 1) {
            numMeshNodes = 0;
            meshNodes = &nodeDatabase.nodes;
            LOG_WARN("NodeDB: Minimal allocation successful (device will have limited node capacity)");
            return;
        } else {
            LOG_ERROR("NodeDB: ALL allocation attempts failed - device may not function correctly");
            // Set to empty vector as last resort
            nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>();
            numMeshNodes = 0;
            meshNodes = &nodeDatabase.nodes;
            return;
        }
    }
    
    // Try standard allocation
    nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(maxNumNodes);
    uint32_t freeHeapAfter = memGet.getFreeHeap();
    uint32_t allocatedMemory = freeHeapBefore - freeHeapAfter;
    
    if (nodeDatabase.nodes.size() == maxNumNodes) {
        numMeshNodes = 0;
        meshNodes = &nodeDatabase.nodes;
        LOG_INFO("NodeDB: Standard allocation successful (%u nodes, allocated %u bytes)", 
                maxNumNodes, allocatedMemory);
        return;
    } else {
        LOG_ERROR("NodeDB: Vector size mismatch (expected %u, got %u)", 
                 maxNumNodes, nodeDatabase.nodes.size());
    }
    
    // If we get here, standard allocation failed - try fallback
    LOG_WARN("NodeDB: Standard allocation failed, using fallback");
    uint32_t safeMaxNodes = (freeHeapBefore > 40960) ? ((freeHeapBefore - 40960) / sizeof(meshtastic_NodeInfoLite)) : 1;
    if (safeMaxNodes > 256) safeMaxNodes = 256;
    if (safeMaxNodes < 1) safeMaxNodes = 1;
    
    nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(safeMaxNodes);
    if (nodeDatabase.nodes.size() == safeMaxNodes) {
        numMeshNodes = 0;
        meshNodes = &nodeDatabase.nodes;
        LOG_WARN("NodeDB: Fallback allocation successful (%u nodes)", safeMaxNodes);
    } else {
        LOG_ERROR("NodeDB: Fallback allocation also failed - using minimal");
        nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(1);
        numMeshNodes = 0;
        meshNodes = &nodeDatabase.nodes;
    }
}

/**
 * @brief Reset nodes with virtual backend support
 * 
 * This function resets nodes, handling virtual backend if enabled.
 * 
 * @param nodeDatabase Reference to nodeDatabase structure
 * @param numMeshNodes Reference to numMeshNodes counter
 * @param getNodeNum Function to get local node number
 */
inline void resetNodesWithVirtualBackend(meshtastic_NodeDatabase& nodeDatabase,
                                         pb_size_t& numMeshNodes,
                                         std::function<NodeNum()> getNodeNum)
{
    if (NodeDBVirtualBackend::isEnabled()) {
        // Reset virtual backend (clears all network nodes)
        NodeDBVirtualBackend::resetNodes();
        // Vector should only contain local node (1 node)
        numMeshNodes = 1;
        if (nodeDatabase.nodes.size() > 1) {
            nodeDatabase.nodes.resize(1);
        }
        // Clear local node data (keep structure, clear fields)
        if (nodeDatabase.nodes.size() > 0) {
            nodeDatabase.nodes[0] = meshtastic_NodeInfoLite();
            nodeDatabase.nodes[0].num = getNodeNum();
        }
    } else {
        // Standard implementation
        numMeshNodes = 1;
        std::fill(nodeDatabase.nodes.begin() + 1, nodeDatabase.nodes.end(), meshtastic_NodeInfoLite());
    }
}

/**
 * @brief Remove node with virtual backend support
 * 
 * This function removes a node, handling virtual backend if enabled.
 * 
 * @param nodeNum Node number to remove
 * @param getNodeNum Function to get local node number
 * @return true if node was removed, false otherwise
 */
inline bool removeNodeByNumWithVirtualBackend(NodeNum nodeNum, std::function<NodeNum()> getNodeNum)
{
    if (NodeDBVirtualBackend::isEnabled()) {
        // Check if this is the local node (should not be removed)
        if (nodeNum == getNodeNum()) {
            LOG_WARN("NodeDB::removeNodeByNum: Cannot remove local node 0x%x", nodeNum);
            return false;
        }
        
        // Network nodes are in virtual backend
        if (NodeDBVirtualBackend::removeNode(nodeNum)) {
            LOG_DEBUG("NodeDB::removeNodeByNum: Removed network node 0x%x from virtual backend", nodeNum);
            // Virtual backend handles saving internally
            return true;
        } else {
            LOG_DEBUG("NodeDB::removeNodeByNum: Node 0x%x not found in virtual backend", nodeNum);
            return false;
        }
    }
    return false;  // Not handled by virtual backend, use standard implementation
}

/**
 * @brief Read next mesh node with virtual backend support
 * 
 * This function reads next node, handling virtual backend if enabled.
 * 
 * @param readIndex Reference to read index (will be modified)
 * @param numMeshNodes Number of nodes in vector
 * @param meshNodes Pointer to vector
 * @return Pointer to next node, or NULL if no more nodes
 */
inline const meshtastic_NodeInfoLite* readNextMeshNodeWithVirtualBackend(uint32_t& readIndex,
                                                                          pb_size_t numMeshNodes,
                                                                          std::vector<meshtastic_NodeInfoLite>* meshNodes)
{
    // First, iterate through vector (local node)
    if (readIndex < numMeshNodes) {
        return &meshNodes->at(readIndex++);
    }

    // Then iterate through virtual backend (network nodes)
    if (NodeDBVirtualBackend::isEnabled()) {
        // Adjust index for virtual backend (subtract vector size)
        size_t backend_index = readIndex - numMeshNodes;
        size_t backend_count = NodeDBVirtualBackend::getTotalNodeCount();
        
        if (backend_index < backend_count) {
            meshtastic_NodeInfoLite* node = NodeDBVirtualBackend::getNodeByIndex(backend_index);
            readIndex++;  // Advance index
            return node;
        }
    }

    return NULL;
}

/**
 * @brief Get number of online mesh nodes with virtual backend support
 * 
 * This function counts online nodes, including virtual backend nodes.
 * 
 * @param numMeshNodes Number of nodes in vector
 * @param meshNodes Pointer to vector
 * @param localOnly If true, ignore nodes heard via MQTT
 * @param sinceLastSeen Function to calculate time since last seen
 * @return Number of online nodes
 */
inline size_t getNumOnlineMeshNodesWithVirtualBackend(pb_size_t numMeshNodes,
                                                      std::vector<meshtastic_NodeInfoLite>* meshNodes,
                                                      bool localOnly,
                                                      uint32_t (*sinceLastSeen)(const meshtastic_NodeInfoLite*))
{
    size_t numseen = 0;
    const uint32_t NUM_ONLINE_SECS = 60 * 60 * 2; // 2 hrs

    // First, check vector (local node)
    for (int i = 0; i < numMeshNodes; i++) {
        if (localOnly && meshNodes->at(i).via_mqtt)
            continue;
        if (sinceLastSeen(&meshNodes->at(i)) < NUM_ONLINE_SECS)
            numseen++;
    }

    // Also check virtual backend (network nodes)
    if (NodeDBVirtualBackend::isEnabled()) {
        size_t backend_count = NodeDBVirtualBackend::getTotalNodeCount();
        for (size_t i = 0; i < backend_count; i++) {
            meshtastic_NodeInfoLite* node = NodeDBVirtualBackend::getNodeByIndex(i);
            if (!node) {
                continue;
            }
            if (localOnly && node->via_mqtt) {
                continue;
            }
            if (sinceLastSeen(node) < NUM_ONLINE_SECS) {
                numseen++;
            }
        }
    }

    return numseen;
}

/**
 * @brief Get mesh node with virtual backend support
 * 
 * This function finds a node, checking virtual backend if enabled.
 * 
 * @param n Node number to find
 * @param numMeshNodes Number of nodes in vector
 * @param meshNodes Pointer to vector
 * @return Pointer to node, or NULL if not found
 */
inline meshtastic_NodeInfoLite* getMeshNodeWithVirtualBackend(NodeNum n,
                                                               pb_size_t numMeshNodes,
                                                               std::vector<meshtastic_NodeInfoLite>* meshNodes)
{
    // First, check vector (for local node)
    for (int i = 0; i < numMeshNodes; i++)
        if (meshNodes->at(i).num == n)
            return &meshNodes->at(i);

    // If not found in vector, check virtual backend (for network nodes)
    if (NodeDBVirtualBackend::isEnabled()) {
        // Network nodes are stored in virtual backend
        return NodeDBVirtualBackend::getNode(n);
    }

    return NULL;
}

/**
 * @brief Get number of mesh nodes with virtual backend support
 * 
 * This function returns total node count, including virtual backend nodes.
 * 
 * @param numMeshNodes Number of nodes in vector
 * @return Total number of nodes
 */
inline size_t getNumMeshNodesWithVirtualBackend(pb_size_t numMeshNodes)
{
    if (NodeDBVirtualBackend::isEnabled()) {
        // Vector contains only local node (1 node)
        // Virtual backend contains network nodes
        // Total = vector nodes + virtual backend nodes
        return numMeshNodes + NodeDBVirtualBackend::getTotalNodeCount();
    }
    // Standard implementation: only vector nodes
    return numMeshNodes;
}

/**
 * @brief Check if node database is full with virtual backend support
 * 
 * This function checks if database is full, considering virtual backend.
 * 
 * @param numMeshNodes Number of nodes in vector
 * @param maxNumNodes Maximum number of nodes
 * @return true if database is full
 */
inline bool isFullWithVirtualBackend(pb_size_t numMeshNodes, uint32_t maxNumNodes)
{
    if (NodeDBVirtualBackend::isEnabled()) {
        // Check virtual backend first (it has the real limit)
        if (NodeDBVirtualBackend::isFull()) {
            return true;
        }
        // Also check memory (standard check)
        return (memGet.getFreeHeap() < MINIMUM_SAFE_FREE_HEAP);
    }
    // Standard implementation
    return (numMeshNodes >= maxNumNodes) || (memGet.getFreeHeap() < MINIMUM_SAFE_FREE_HEAP);
}

/**
 * @brief Get or create mesh node with virtual backend support (lazy initialization)
 * 
 * This function handles lazy initialization of virtual backend when first network node is accessed.
 * It also saves local node to virtual backend after initialization.
 * 
 * @param n Node number
 * @param getNodeNum Function to get local node number
 * @param getMeshNode Function to get node from vector
 * @return Pointer to node from virtual backend, or NULL if should use standard implementation
 */
inline meshtastic_NodeInfoLite* getOrCreateMeshNodeLazyInit(NodeNum n,
                                                             std::function<NodeNum()> getNodeNum,
                                                             std::function<meshtastic_NodeInfoLite*(NodeNum)> getMeshNode)
{
    if (n != getNodeNum()) {
        // This is a network node (not local) - use virtual backend
        // Initialize virtual backend on first network node access (lazy initialization)
        if (NodeDBVirtualBackend::isAvailable() && !NodeDBVirtualBackend::isEnabled()) {
            // Backend is available but not initialized - initialize it now
            if (!NodeDBVirtualBackend::initialize()) {
                LOG_ERROR("NodeDBVirtualBackend: Failed to initialize, falling back to standard implementation");
                // Fall through to standard implementation
                return NULL;
            } else {
                // CRITICAL: Do NOT call loadFromDisk() immediately after initialization
                // loadFromDisk() can take a long time (scanning filesystem, reading nodes)
                // This can cause watchdog timeout if called during packet processing
                // Instead, loadFromDisk() will be called lazily when nodes are actually accessed
                // This prevents blocking packet processing and watchdog timeout
                LOG_DEBUG("NodeDBVirtualBackend: Initialized successfully, index will load lazily on first access");
                
                #ifdef PAUSE_ON_START
                // DEBUG MODE: Pause execution but keep console working
                LOG_INFO("========================================");
                LOG_INFO("PAUSE MODE ENABLED - Execution paused");
                LOG_INFO("Console is active, watchdog is fed");
                LOG_INFO("Press reset to continue or disable PAUSE_ON_START");
                LOG_INFO("========================================");
                #ifdef ARCH_NRF52
                extern void nrf52Loop();
                while (true) {
                    // Feed watchdog to prevent reboot
                    nrf52Loop();
                    yield();  // Allow console and other tasks to run
                    delay(100);  // Small delay to prevent CPU spinning
                }
                #else
                while (true) {
                    yield();  // Allow console and other tasks to run
                    delay(100);  // Small delay to prevent CPU spinning
                }
                #endif
                #endif
                
                // CRITICAL: Save local node to virtual backend after initialization
                // Local node is already in RAM vector, but needs to be in virtual backend too
                // This is safe because it only saves one node (local node), not all nodes
                NodeNum localNodeNum = getNodeNum();
                meshtastic_NodeInfoLite* localNode = getMeshNode(localNodeNum);
                if (localNode && localNode->has_user) {
                    // Local node exists in RAM - save it to virtual backend
                    meshtastic_NodeInfoLite* savedNode = NodeDBVirtualBackend::getOrCreateNode(localNodeNum);
                    if (savedNode) {
                        // Copy local node data to virtual backend
                        *savedNode = *localNode;
                        // Mark local node as protected (never evict)
                        NodeDBVirtualBackend::markProtected(localNodeNum, true);
                        LOG_DEBUG("NodeDBVirtualBackend: Saved local node 0x%x to virtual backend", localNodeNum);
                    }
                }
            }
        }
        
        // Try to use virtual backend if initialized
        if (NodeDBVirtualBackend::isEnabled()) {
            meshtastic_NodeInfoLite* result = NodeDBVirtualBackend::getOrCreateNode(n);
            if (result) {
                return result;  // Virtual backend succeeded
            }
            // If virtual backend failed, fall through to standard implementation
        }
    }
    
    return NULL;  // Not handled by virtual backend, use standard implementation
}

/**
 * @brief Update from packet with virtual backend support
 * 
 * This function updates node from packet, using virtual backend if enabled.
 * 
 * @param mp Pointer to mesh packet
 * @param getFrom Function to get source node number
 */
inline void updateFromWithVirtualBackend(const meshtastic_MeshPacket* mp,
                                         NodeNum (*getFrom)(const meshtastic_MeshPacket*))
{
    if (mp->from) {
        // Use virtual backend if enabled (handles hot data updates without flash writes)
        if (NodeDBVirtualBackend::isEnabled()) {
            NodeDBVirtualBackend::updateFromPacket(getFrom(mp), mp);
        }
    }
}

#endif // ARCH_NRF52
#endif // USE_EXTENDED_FS_FOR_NODEDB

