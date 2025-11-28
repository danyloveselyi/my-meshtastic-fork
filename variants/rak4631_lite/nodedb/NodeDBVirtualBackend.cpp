/**
 * @file NodeDBVirtualBackend.cpp
 * @brief Virtual NodeDB backend implementation
 */

#include "NodeDBVirtualBackend.h"
#include "NodeStorage.h"
#include "NodeIndex.h"
#include "NodeCache.h"
#include "NodeHotData.h"
#include "NodeDBFilesystemAdapter.h"
#include "NodeDBBackgroundTask.h"
#include "../../../src/mesh/NodeDB.h"
#include "../../../src/mesh/MeshTypes.h"
#include "../../../src/mesh/TypeConversions.h"
#include "../../../src/gps/RTC.h"
#include "../../../src/memGet.h"
#include "NodeDBExtendedFSImpl.h"
// Include LittleFS headers for directory operations
#include "../../../src/platform/stm32wl/littlefs/lfs.h"
#include "../../../src/platform/stm32wl/littlefs/lfs_util.h"
#include <cstring>
#include <cstdio>

#ifdef ARCH_NRF52
// Forward declaration for nrf52Loop() (defined in src/platform/nrf52/main-nrf52.cpp)
extern void nrf52Loop();
#endif

#ifdef USE_EXTENDED_FS_FOR_NODEDB

namespace NodeDBVirtualBackend {

static bool backend_enabled = false;
static bool backend_initialized = false;
static bool index_loaded = false;  // Track if index was loaded from disk (lazy loading)
static uint32_t last_save_time = 0;
static constexpr uint32_t SAVE_THROTTLE_MS = 60 * 1000;  // 1 minute

// Incremental load state
struct IncrementalLoadState {
    enum State {
        IDLE,           // Not loading
        SCANNING_DIR,   // Scanning directory for slot files
        REBUILDING,     // Rebuilding index from slots
        PRELOADING      // Preloading cache
    };
    
    State state;
    uint32_t current_index;      // Current position in index
    uint32_t total_nodes;        // Total nodes to process
    uint32_t nodes_processed;    // Nodes processed so far
    uint32_t preload_target;      // Target number of nodes to preload
    uint32_t preload_count;      // Nodes preloaded so far
    bool time_valid;             // Whether time is valid for priority calculation
    uint32_t now;                 // Current time (if valid)
    
    // Static buffer for sorting (replaces stack allocation)
    struct NodeWithPriority {
        NodeNum num;
        uint32_t priority;
        uint32_t last_heard;
        uint16_t slot_id;
    };
    static constexpr uint32_t MAX_SORT_NODES = 100;  // Reduced from 200 to save stack
    NodeWithPriority sort_buffer[MAX_SORT_NODES];
    uint32_t sort_count;
    uint32_t sort_processed;      // How many sorted nodes we've processed
    
    IncrementalLoadState() : state(IDLE), current_index(0), total_nodes(0), 
                             nodes_processed(0), preload_target(0), preload_count(0),
                             time_valid(false), now(0), sort_count(0), sort_processed(0) {}
};

static IncrementalLoadState incremental_load_state;

/**
 * @brief Initialize the virtual backend
 */
/**
 * @brief Initialize with specific sizes (internal helper)
 */
static bool initializeWithSizes(uint32_t cacheSize, uint32_t indexSize)
{
    if (backend_initialized) {
        return true;
    }
    
    LOG_DEBUG("NodeDBVirtualBackend: Attempting initialization with cache=%u, index=%u", cacheSize, indexSize);
    
    // Initialize storage
    if (!NodeStorage::initialize()) {
        LOG_ERROR("NodeDBVirtualBackend: Failed to initialize storage");
        return false;
    }
    
    // Initialize index
    if (!NodeIndex::initialize(indexSize)) {
        LOG_ERROR("NodeDBVirtualBackend: Failed to initialize index (size: %u)", indexSize);
        return false;
    }
    
    // Initialize cache
    if (!NodeCache::initialize(cacheSize)) {
        LOG_ERROR("NodeDBVirtualBackend: Failed to initialize cache (size: %u)", cacheSize);
        // Cleanup index if cache failed
        // Note: NodeIndex doesn't have cleanup, but that's okay - it will be retried
        return false;
    }
    
    // CRITICAL: Do NOT initialize background task during startup
    // Background task can cause flash operations during BLE setup, which corrupts device name buffer
    // In committed code, there is no background task - flash operations are synchronous when needed
    // Background task will be initialized lazily on first write operation if needed
    // NodeDBBackgroundTask::initialize();  // DISABLED to match committed code behavior
    
    backend_initialized = true;
    backend_enabled = true;
    
    // CRITICAL: Reset index_loaded flag on initialization to ensure
    // loadFromDisk() will load index from flash on first call
    index_loaded = false;
    
    LOG_INFO("NodeDBVirtualBackend: Initialized successfully (cache: %u, index: %u)", cacheSize, indexSize);
    
    return true;
}

/**
 * @brief Initialize with reduced sizes (graceful degradation)
 * Tries multiple size configurations, automatically reducing if memory is insufficient
 */
bool initializeWithReducedSizes()
{
    if (backend_initialized) {
        return true;
    }
    
    // Maximum slots and index - use MAX_NUM_NODES from variant.h
    #ifndef MAX_NODES_SLOTS
    #define MAX_NODES_SLOTS MAX_NUM_NODES
    #endif
    
    #ifndef MAX_NODES_INDEX
    #define MAX_NODES_INDEX MAX_NODES_SLOTS
    #endif
    
    #ifndef MAX_NODES_CACHE
    #define MAX_NODES_CACHE 300
    #endif
    
    // Try multiple size configurations, from largest to smallest
    struct SizeConfig {
        uint32_t cache;
        uint32_t index;
        const char* name;
    };
    
    SizeConfig configs[] = {
        {MAX_NODES_CACHE, MAX_NODES_INDEX, "FULL"},           // Attempt 1: Full sizes
        {200, 500, "REDUCED"},                                 // Attempt 2: Reduced sizes
        {100, 200, "MINIMAL"},                                 // Attempt 3: Minimal sizes
        {50, 100, "BASIC"}                                     // Attempt 4: Basic functionality only
    };
    
    for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
        LOG_INFO("NodeDBVirtualBackend: Attempt %u/%u - %s (cache: %u, index: %u)", 
                 (unsigned)(i + 1), (unsigned)(sizeof(configs) / sizeof(configs[0])), configs[i].name, 
                 configs[i].cache, configs[i].index);
        
        if (initializeWithSizes(configs[i].cache, configs[i].index)) {
            if (i > 0) {
                LOG_WARN("NodeDBVirtualBackend: Initialized with REDUCED sizes (%s) due to memory constraints", 
                         configs[i].name);
                LOG_WARN("NodeDBVirtualBackend: Cache: %u nodes, Index: %u nodes (requested: cache=%u, index=%u)", 
                         configs[i].cache, configs[i].index, MAX_NODES_CACHE, MAX_NODES_INDEX);
            }
            return true;
        }
        
        LOG_WARN("NodeDBVirtualBackend: Attempt %u/%u (%s) failed, trying next size...", 
                 (unsigned)(i + 1), (unsigned)(sizeof(configs) / sizeof(configs[0])), configs[i].name);
    }
    
    LOG_ERROR("NodeDBVirtualBackend: All initialization attempts failed - virtual backend disabled");
    return false;
}

bool initialize()
{
    if (backend_initialized) {
        return true;
    }
    
    // Use graceful degradation by default
    return initializeWithReducedSizes();
}

/**
 * @brief Check if virtual backend is enabled (initialized and ready)
 */
bool isEnabled()
{
    return backend_enabled && backend_initialized;
}

/**
 * @brief Check if virtual backend is available (compiled in and can be initialized)
 * This returns true even if backend is not yet initialized (lazy initialization)
 * This allows NodeDB to attempt initialization on first use
 */
bool isAvailable()
{
    // Virtual backend is always available if compiled in
    // It will be initialized on first use (lazy initialization)
    return true;
}

/**
 * @brief Get a node by node number
 * 
 * CRITICAL: This function will attempt to initialize the backend if not already initialized.
 * If initialization fails (e.g., not enough memory), it returns nullptr to prevent
 * falling back to standard implementation which would try to allocate ~247 KB in RAM.
 */
meshtastic_NodeInfoLite* getNode(NodeNum nodeNum)
{
    // CRITICAL: Do NOT initialize here - this can be called from getMeshNode()
    // during device startup. If backend is not initialized, return nullptr
    // to allow standard implementation to be used instead.
    if (!isEnabled()) {
        return nullptr;  // Backend not initialized - allow fallback to standard implementation
    }
    
    // CRITICAL: Lazy load index from flash on first node access
    // This prevents blocking device startup or packet processing
    if (!index_loaded) {
        // CRITICAL: Feed watchdog before loading from disk (can take time)
        #ifdef ARCH_NRF52
        ::nrf52Loop();
        yield();
        #endif
        
        // Load index from flash (this may take time, but we're already in a node access context)
        if (!loadFromDisk()) {
            LOG_WARN("NodeDBVirtualBackend: Failed to load index from flash, will retry on next access");
            // Don't mark as loaded - allow retry on next access
            return nullptr;
        }
        
        // CRITICAL: Feed watchdog after loading from disk
        #ifdef ARCH_NRF52
        ::nrf52Loop();
        yield();
        #endif
        
        index_loaded = true;  // Mark as loaded to prevent repeated loading
    }
    
    // Try to get from cache (loads from flash if not cached)
    return NodeCache::getNode(nodeNum);
}

/**
 * @brief Get or create a node
 */
meshtastic_NodeInfoLite* getOrCreateNode(NodeNum nodeNum)
{
    // CRITICAL: Do NOT initialize here - initialization must happen explicitly
    // when first network node is accessed, not during device startup.
    // This prevents blocking device startup with filesystem operations.
    if (!isEnabled()) {
        return nullptr;  // Backend not initialized - must initialize before use
    }
    
    // CRITICAL: Lazy load index from flash on first node access
    // This prevents blocking device startup or packet processing
    if (!index_loaded) {
        // CRITICAL: Feed watchdog before loading from disk (can take time)
        #ifdef ARCH_NRF52
        ::nrf52Loop();
        yield();
        #endif
        
        // Load index from flash (this may take time, but we're already in a node access context)
        if (!loadFromDisk()) {
            LOG_WARN("NodeDBVirtualBackend: Failed to load index from flash, will retry on next access");
            // Don't mark as loaded - allow retry on next access
            // But still try to create node (may work if index is not critical)
        } else {
            // CRITICAL: Feed watchdog after loading from disk
            #ifdef ARCH_NRF52
            ::nrf52Loop();
            yield();
            #endif
            
            index_loaded = true;  // Mark as loaded to prevent repeated loading
        }
    }
    
    // Check if node exists
    meshtastic_NodeInfoLite* node = getNode(nodeNum);
    if (node) {
        return node;
    }
    
    // ========================================================================
    // EVICTION LOGIC: Free space before adding new node
    // ========================================================================
    // Algorithm:
    // 1. If index is full (MAX_NODES_INDEX) → remove oldest node from flash (frees slot and index entry)
    // 2. If flash is full → remove oldest node from flash (frees slot)
    // 3. Add new node to cache
    // 4. If cache is full → evict from cache (putNode() handles this automatically)
    // ========================================================================
    
    // Get MAX_NODES_INDEX (must be <= MAX_NODES_SLOTS, both use MAX_NUM_NODES from variant.h)
    #ifndef MAX_NODES_INDEX
    #ifdef MAX_NODES_SLOTS
    #define MAX_NODES_INDEX MAX_NODES_SLOTS  // Use same value as slots (they must be equal)
    #else
    #define MAX_NODES_INDEX MAX_NUM_NODES  // Use MAX_NUM_NODES from variant.h (1234 for RAK4631)
    #endif
    #endif
    
    // Step 1: Check if index is full (MAX_NODES_INDEX = MAX_NUM_NODES = 1234)
    // This is the PRIMARY limit - when index is full, we must evict to add new node
    if (NodeIndex::getNodeCount() >= MAX_NODES_INDEX) {
        LOG_WARN("NodeDBVirtualBackend: Index full (%u/%u nodes), evicting oldest node to make room for 0x%x", 
                 NodeIndex::getNodeCount(), MAX_NODES_INDEX, nodeNum);
        // Find oldest non-protected node (can be in cache or not)
        // allowCached=true because we need to free index entry and flash slot
        NodeNum node_to_evict = NodeIndex::getEvictionCandidate(true);
        if (node_to_evict != 0) {
            LOG_INFO("NodeDBVirtualBackend: Step 1 - Evicting node 0x%x (oldest) to free index entry for 0x%x", 
                    node_to_evict, nodeNum);
            // removeNode() removes from flash, index, and cache (frees slot and index entry)
            removeNode(node_to_evict);
        } else {
            LOG_ERROR("NodeDBVirtualBackend: All nodes protected, cannot evict. Cannot add node 0x%x (index full: %u/%u)", 
                     nodeNum, NodeIndex::getNodeCount(), MAX_NODES_INDEX);
            return nullptr;  // All nodes protected, cannot evict
        }
    }
    
    // Step 2: Check if flash is full - need to free a slot (secondary check)
    // This should not happen if index is not full, but check anyway for safety
    if (getFreeFlashSlots() == 0) {
        LOG_WARN("NodeDBVirtualBackend: Flash full (%u slots used), evicting oldest node to make room for 0x%x", 
                 NodeStorage::getSlotCount(), nodeNum);
        // Find oldest non-protected node (can be in cache or not)
        // allowCached=true because we need to free flash slot, not just cache space
        NodeNum node_to_evict = NodeIndex::getEvictionCandidate(true);
        if (node_to_evict != 0) {
            LOG_INFO("NodeDBVirtualBackend: Step 2 - Evicting node 0x%x (oldest) from flash to free slot for 0x%x", 
                    node_to_evict, nodeNum);
            // removeNode() removes from flash, index, and cache (frees slot)
            removeNode(node_to_evict);
        } else {
            LOG_ERROR("NodeDBVirtualBackend: All nodes protected, cannot evict. Cannot add node 0x%x", nodeNum);
            return nullptr;  // All nodes protected, cannot evict
        }
    }
    
    // Step 3: Check if cache is full (but flash has space) - need to free cache space
    // Note: This is handled automatically by putNode() below, but we can pre-evict
    // to ensure cache has space before adding new node
    if (NodeCache::getCacheSize() >= NodeCache::getMaxCacheSize()) {
        LOG_DEBUG("NodeDBVirtualBackend: Step 3 - Cache full (%u/%u), pre-evicting to make room for node 0x%x", 
                 NodeCache::getCacheSize(), NodeCache::getMaxCacheSize(), nodeNum);
        // Find non-cached, non-protected node to evict from cache
        // allowCached=false because we only need cache space, flash already has space
        NodeNum node_to_evict_from_cache = NodeIndex::getEvictionCandidate(false);
        if (node_to_evict_from_cache != 0) {
            // evictNode() only removes from cache, keeps in flash
            NodeCache::evictNode(node_to_evict_from_cache);
        }
        // Note: If cache is still full after pre-eviction, putNode() will handle it
    }
    
    // RAM pressure handling (aggressive cache eviction)
    if (memGet.getFreeHeap() < MINIMUM_SAFE_FREE_HEAP) {
        LOG_WARN("NodeDBVirtualBackend: Low memory (%u bytes free), aggressive cache eviction", 
                 memGet.getFreeHeap());
        // Evict multiple non-protected nodes (from cache only, not flash)
        for (int i = 0; i < 10 && memGet.getFreeHeap() < MINIMUM_SAFE_FREE_HEAP; i++) {
            NodeNum node_to_evict_for_memory = NodeIndex::getEvictionCandidate(false);
            if (node_to_evict_for_memory != 0) {
                NodeCache::evictNode(node_to_evict_for_memory);
            } else {
                break;  // No more candidates
            }
        }
    }
    
    // ========================================================================
    // ADD NEW NODE: Allocate resources and add to all structures
    // ========================================================================
    
    // Step 4: Allocate flash slot for new node
    uint16_t slotId = NodeStorage::allocateSlot(nodeNum);
    if (slotId == UINT16_MAX) {
        LOG_ERROR("NodeDBVirtualBackend: Failed to allocate slot for node 0x%x", nodeNum);
        return nullptr;
    }
    
    // Step 5: Add to index
    if (!NodeIndex::addNode(nodeNum, slotId)) {
        LOG_ERROR("NodeDBVirtualBackend: Failed to add node 0x%x to index", nodeNum);
        NodeStorage::deleteSlot(slotId);
        return nullptr;
    }
    
    // Step 6: Create new node structure
    meshtastic_NodeInfoLite newNode = {};
    newNode.num = nodeNum;
    
    // Step 7: Add to cache (putNode() will evict from cache if needed)
    // Note: putNode() automatically evicts least recently used node if cache is full
    if (!NodeCache::putNode(nodeNum, &newNode)) {
        LOG_ERROR("NodeDBVirtualBackend: Failed to add node 0x%x to cache (cache may be full with protected nodes)", nodeNum);
        NodeIndex::removeNode(nodeNum);
        NodeStorage::deleteSlot(slotId);
        return nullptr;
    }
    
    // Mark as dirty (needs initial flash write)
    NodeCache::markDirty(nodeNum);
    
    // CRITICAL: Feed watchdog after creating new node (may trigger writeNodeToSlot later)
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    // Get from cache
    return getNode(nodeNum);
}

/**
 * @brief Update a node from a mesh packet
 */
bool updateFromPacket(NodeNum nodeNum, const meshtastic_MeshPacket* packet)
{
    // CRITICAL: Do NOT initialize here - backend must be initialized before use
    if (!isEnabled() || !packet) {
        return false;
    }
    
    // CRITICAL: Feed watchdog before getOrCreateNode (may trigger filesystem operations)
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    // Get or create node
    // CRITICAL: This may trigger loadFromDisk() if index not loaded, or writeNodeToSlot() if node is new
    meshtastic_NodeInfoLite* node = getOrCreateNode(nodeNum);
    
    // CRITICAL: Feed watchdog after getOrCreateNode (may have triggered filesystem operations)
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();
    #endif
    
    if (!node) {
        return false;
    }
    
    // Update hot data (in RAM only, never written to flash)
    // These fields update frequently (every packet or during routing) and are NEVER written to flash
    if (packet->rx_time) {
        node->last_heard = packet->rx_time;
        NodeIndex::updateLastHeard(nodeNum, packet->rx_time);
    }
    
    if (packet->rx_snr) {
        node->snr = packet->rx_snr;  // Hot data - never written to flash
    }
    
    node->via_mqtt = packet->via_mqtt;  // Hot data - never written to flash
    
    // Update hops_away (hot data)
    if (packet->hop_start != 0 && packet->hop_limit <= packet->hop_start) {
        node->has_hops_away = true;
        node->hops_away = packet->hop_start - packet->hop_limit;
    }
    
    // Note: next_hop is updated in NextHopRouter::sniffReceived() and is also hot data
    // It's stored in RAM cache and never written to flash
    
    // Note: Hot data updates (last_heard, snr, hops_away, via_mqtt, next_hop) are NOT marked as dirty
    // They are only in RAM and never written to flash to prevent wear
    
    return true;
}

/**
 * @brief Update user information for a node
 */
bool updateUser(NodeNum nodeNum, const meshtastic_User* user)
{
    // CRITICAL: Do NOT initialize here - backend must be initialized before use
    if (!isEnabled() || !user) {
        return false;
    }
    
    // Get or create node
    meshtastic_NodeInfoLite* node = getOrCreateNode(nodeNum);
    if (!node) {
        return false;
    }
    
    // Update cold data (user info - stored in flash)
    // Note: NodeInfoLite uses UserLite, but updateUser receives User
    // Use TypeConversions helper to convert User to UserLite
    node->has_user = true;
    node->user = TypeConversions::ConvertToUserLite(*user);
    
    // Mark as dirty (needs flash write)
    NodeCache::markDirty(nodeNum);
    
    // Update index flags
    NodeIndexEntry* index_entry = NodeIndex::findNode(nodeNum);
    if (index_entry) {
        index_entry->flags |= NodeIndexEntry::FLAG_HAS_USER;
    }
    
    return true;
}

/**
 * @brief Remove a node
 */
bool removeNode(NodeNum nodeNum)
{
    // CRITICAL: Do NOT initialize here - backend must be initialized before use
    if (!isEnabled()) {
        return false;
    }
    
    // Get index entry
    NodeIndexEntry* index_entry = NodeIndex::findNode(nodeNum);
    if (!index_entry) {
        return false;  // Node doesn't exist
    }
    
    // Evict from cache
    NodeCache::evictNode(nodeNum);
    
    // Delete from flash
    NodeStorage::deleteSlot(index_entry->slot_id);
    
    // Remove from index
    NodeIndex::removeNode(nodeNum);
    
    return true;
}

/**
 * @brief Reset all nodes (clear cache, index, and storage)
 */
bool resetNodes()
{
    // CRITICAL: Do NOT initialize here - backend must be initialized before use
    if (!isEnabled()) {
        return false;
    }
    
    LOG_INFO("NodeDBVirtualBackend: Resetting all nodes...");
    
    // Get all node numbers from index before clearing
    #ifndef MAX_NODES_INDEX
    #ifdef MAX_NODES_SLOTS
    #define MAX_NODES_INDEX MAX_NODES_SLOTS  // Use same value as slots
    #else
    #define MAX_NODES_INDEX MAX_NUM_NODES  // Use MAX_NUM_NODES from variant.h
    #endif
    #endif
    NodeNum node_nums[MAX_NODES_INDEX];
    uint32_t node_count = NodeIndex::getAllNodeNums(node_nums, MAX_NODES_INDEX);
    
    // Delete all slots from storage
    for (uint32_t i = 0; i < node_count; i++) {
        NodeIndexEntry* index_entry = NodeIndex::findNode(node_nums[i]);
        if (index_entry) {
            NodeStorage::deleteSlot(index_entry->slot_id);
        }
    }
    
    // Clear cache (writes dirty entries to flash first)
    NodeCache::clear();
    
    // Clear index
    NodeIndex::clear();
    
    LOG_INFO("NodeDBVirtualBackend: All nodes reset (cleared %u nodes)", node_count);
    
    return true;
}

/**
 * @brief Check if node database is full
 */
bool isFull()
{
    if (!isEnabled()) {
        return false;
    }
    
    // Check cache size
    if (NodeCache::getCacheSize() >= NodeCache::getMaxCacheSize()) {
        return true;
    }
    
    // Check flash capacity
    if (NodeStorage::getFreeSlotCount() == 0) {
        return true;
    }
    
    // Check index capacity (must be <= MAX_NODES_SLOTS)
    #ifndef MAX_NODES_INDEX
    #ifdef MAX_NODES_SLOTS
    #define MAX_NODES_INDEX MAX_NODES_SLOTS  // Use same value as slots
    #else
    #define MAX_NODES_INDEX MAX_NUM_NODES  // Use MAX_NUM_NODES from variant.h
    #endif
    #endif
    if (NodeIndex::getNodeCount() >= MAX_NODES_INDEX) {
        return true;
    }
    
    return false;
}

/**
 * @brief Get total number of nodes
 */
size_t getTotalNodeCount()
{
    // CRITICAL: Do NOT initialize here - this can be called from /mem command
    // which should be fast. If backend is not initialized, return 0.
    // Initialization should happen on first actual node access, not on stats query.
    if (!isEnabled()) {
        return 0;  // Backend not initialized or disabled
    }
    
    // CRITICAL: Safe call - NodeIndex::getNodeCount() checks initialization internally
    return NodeIndex::getNodeCount();
}

/**
 * @brief Get number of cached nodes
 */
size_t getCachedNodeCount()
{
    // CRITICAL: Check backend state before accessing cache
    if (!backend_initialized || !backend_enabled) {
        return 0;
    }
    
    // CRITICAL: Safe call - NodeCache::getCacheSize() checks initialization internally
    return NodeCache::getCacheSize();
}

/**
 * @brief Get number of free flash slots
 */
uint32_t getFreeFlashSlots()
{
    // CRITICAL: Check backend state before accessing storage
    if (!backend_initialized || !backend_enabled) {
        return 0;
    }
    
    // CRITICAL: Safe call - NodeStorage::getFreeSlotCount() uses NodeIndex::getNodeCount()
    // which checks initialization internally
    return NodeStorage::getFreeSlotCount();
}

/**
 * @brief Load all nodes from flash (on startup)
 */
bool loadFromDisk()
{
    // CRITICAL: Do NOT initialize here - initialization must happen explicitly
    // before calling this function. This prevents blocking device startup.
    if (!isEnabled()) {
        return false;  // Backend not initialized - must initialize before loading
    }
    
    // If already loaded, return success immediately
    if (index_loaded) {
        return true;
    }
    
    // If incremental load is in progress, continue from where we left off
    // The incremental loading logic is handled below via incremental_load_state
    // Background task will call loadFromDisk() repeatedly until it returns true
    
    // CRITICAL: Feed watchdog at the start of loadFromDisk()
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();  // Yield to allow other tasks to run
    #endif
    
    LOG_INFO("NodeDBVirtualBackend: Starting incremental load from flash...");
    
    // Initialize incremental load state
    incremental_load_state = IncrementalLoadState();
    incremental_load_state.state = IncrementalLoadState::SCANNING_DIR;
    
    // CRITICAL: Try to load index from flash first (preserves last_heard timestamps)
    // This is essential for:
    // 1. Preloading cache with most active nodes (based on last_heard)
    // 2. Evicting oldest nodes when flash is full (based on last_heard)
    bool index_loaded_from_flash = NodeIndex::loadFromFlash();
    
    // CRITICAL: Feed watchdog after index load attempt
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    yield();  // Yield to allow other tasks to run
    #endif
    
    // CRITICAL: Check if index load failed - if so, we need to rebuild from slots
    // Don't set index_loaded = true until we successfully rebuild or validate index
    if (!index_loaded_from_flash) {
        LOG_DEBUG("NodeIndex: Index file not found or load failed - will rebuild from slots");
    }
    
    // Check if index is empty (first boot or after reset, or load failed)
    uint32_t index_count = NodeIndex::getNodeCount();
    
    if (index_count == 0) {
        // Index is empty - rebuild from slot files
        // Note: last_heard will be 0 for all nodes (lost on reboot)
        // This is acceptable for first boot, but subsequent boots should load from flash
        LOG_INFO("NodeDBVirtualBackend: Index is empty, rebuilding from slot files...");
        
        // OPTIMIZED: Scan directory instead of checking each slot individually
        // This is much faster (only reads existing files, not all 1234 slots)
        uint32_t slots_found = 0;
        
        LOG_INFO("NodeDBVirtualBackend: Scanning /nodes directory to rebuild index...");
        
        // CRITICAL: Feed watchdog before accessing filesystem
        // getExtendedFSForNodeDB() may initialize filesystem, which can take time
        #ifdef ARCH_NRF52
        ::nrf52Loop();
        yield();  // Yield to allow other tasks to run
        #endif
        
        // Use extended filesystem for directory scan
        // CRITICAL: This may trigger filesystem initialization if not already initialized
        lfs_t* extended_lfs = getExtendedFSForNodeDB();
        
        // CRITICAL: Feed watchdog after filesystem access
        #ifdef ARCH_NRF52
        ::nrf52Loop();
        yield();  // Yield to allow other tasks to run
        #endif
        
        if (!extended_lfs) {
            LOG_ERROR("NodeDBVirtualBackend: Extended filesystem not available for directory scan");
            return false;
        }
        
        // Open /nodes directory
        lfs_dir_t dir;
        int dir_result = lfs_dir_open(extended_lfs, &dir, "/nodes");
        if (dir_result != LFS_ERR_OK) {
            LOG_WARN("NodeDBVirtualBackend: Failed to open /nodes directory (error: %d), falling back to slot-by-slot scan", dir_result);
            // Fallback to slow method - CRITICAL: Add watchdog feed to prevent reset
            constexpr uint32_t MAX_SLOT_ID = MAX_NUM_NODES - 1;
            constexpr uint32_t YIELD_INTERVAL = 50;  // Yield every 50 slots
            for (uint16_t slot_id = 0; slot_id <= MAX_SLOT_ID; slot_id++) {
                // CRITICAL: Yield periodically to prevent watchdog timeout
                if (slot_id % YIELD_INTERVAL == 0) {
                    yield();
                    #ifdef ARCH_NRF52
                    ::nrf52Loop();  // Feed watchdog and process SoftDevice events
                    #endif
                }
                
                if (NodeStorage::slotExists(slot_id)) {
                    meshtastic_NodeInfoLite node;
                    if (NodeStorage::readNodeFromSlot(slot_id, &node)) {
                        if (NodeIndex::addNode(node.num, slot_id)) {
                            slots_found++;
                        }
                    }
                }
            }
        } else {
            // Read directory entries
            // CRITICAL: Add periodic watchdog feed and yield to prevent reset during long operations
            struct lfs_info info;
            uint32_t entries_processed = 0;
            constexpr uint32_t YIELD_INTERVAL = 10;  // Yield every 10 entries
            
                while (true) {
                    int read_result = lfs_dir_read(extended_lfs, &dir, &info);
                    if (read_result <= 0) {
                        break;  // End of directory or error
                    }
                    
                    entries_processed++;
                    
                    // CRITICAL: Yield periodically to prevent watchdog timeout
                    if (entries_processed % YIELD_INTERVAL == 0) {
                        yield();
                        #ifdef ARCH_NRF52
                        ::nrf52Loop();  // Feed watchdog and process SoftDevice events (use global namespace)
                        #endif
                }
                
                // Check if this is a slot file (format: "slot_XXXX.bin")
                if (info.type == LFS_TYPE_REG && strncmp(info.name, "slot_", 5) == 0) {
                    // Extract slot ID from filename "slot_XXXX.bin"
                    uint16_t slot_id = 0;
                    if (sscanf(info.name, "slot_%hu.bin", &slot_id) == 1) {
                        // Read node from slot
                        meshtastic_NodeInfoLite node;
                        if (NodeStorage::readNodeFromSlot(slot_id, &node)) {
                            // Add to index
                            if (NodeIndex::addNode(node.num, slot_id)) {
                                slots_found++;
                                // Note: last_heard is NOT in flash (it's hot data, RAM-only)
                                // If index was loaded from flash, last_heard is already preserved
                                // If index is being rebuilt, last_heard will be 0 (lost on reboot)
                                // This is acceptable - it will be updated when packets are received
                            } else {
                                LOG_WARN("NodeDBVirtualBackend: Failed to add node 0x%x (slot %u) to index", 
                                         node.num, slot_id);
                            }
                        } else {
                            LOG_WARN("NodeDBVirtualBackend: Failed to read node from slot %u", slot_id);
                        }
                        
                        // CRITICAL: Feed watchdog after reading each slot (readNodeFromSlot can be slow)
                        #ifdef ARCH_NRF52
                        ::nrf52Loop();  // Feed watchdog and process SoftDevice events
                        #endif
                    }
                }
            }
            
            lfs_dir_close(extended_lfs, &dir);
        }
        
        LOG_INFO("NodeDBVirtualBackend: Rebuilt index from %u slot files", slots_found);
        
        // CRITICAL: Verify that index rebuild was successful
        // If no slots were found and index is still empty, this is OK (first boot, no nodes yet)
        // Don't treat this as an error - mark index as loaded so we don't retry infinitely
        uint32_t final_index_count = NodeIndex::getNodeCount();
        if (final_index_count == 0 && slots_found == 0) {
            LOG_INFO("NodeDBVirtualBackend: Index is empty (first boot, no nodes in flash yet) - this is normal");
            // Mark as loaded even though empty - this prevents infinite retry loop
            // Index will be populated as nodes are received from network
            index_loaded = true;
            return true;  // Success - empty index is valid state
        }
    } else {
        // Index already has entries - validate against slot files
        uint32_t slot_count = NodeStorage::getSlotCount();
        
        if (slot_count != index_count) {
            LOG_WARN("NodeDBVirtualBackend: Index mismatch (slots: %u, index: %u), rebuilding index", 
                     slot_count, index_count);
            // Rebuild index from slot files
            NodeIndex::clear();
            
            // OPTIMIZED: Scan directory instead of checking each slot individually
            // This is much faster (only reads existing files, not all 1234 slots)
            uint32_t slots_found = 0;
            
            LOG_INFO("NodeDBVirtualBackend: Scanning /nodes directory to rebuild index...");
            
            // CRITICAL: Feed watchdog before accessing filesystem
            // getExtendedFSForNodeDB() may initialize filesystem, which can take time
            #ifdef ARCH_NRF52
            ::nrf52Loop();
            yield();  // Yield to allow other tasks to run
            #endif
            
            // Use extended filesystem for directory scan
            // CRITICAL: This may trigger filesystem initialization if not already initialized
            lfs_t* extended_lfs = getExtendedFSForNodeDB();
            
            // CRITICAL: Feed watchdog after filesystem access
            #ifdef ARCH_NRF52
            ::nrf52Loop();
            yield();  // Yield to allow other tasks to run
            #endif
            
            if (!extended_lfs) {
                LOG_ERROR("NodeDBVirtualBackend: Extended filesystem not available for directory scan");
                return false;
            }
            
            // Open /nodes directory
            lfs_dir_t dir;
            int dir_result = lfs_dir_open(extended_lfs, &dir, "/nodes");
            if (dir_result != LFS_ERR_OK) {
                LOG_WARN("NodeDBVirtualBackend: Failed to open /nodes directory (error: %d), falling back to slot-by-slot scan", dir_result);
                // Fallback to slow method - CRITICAL: Add watchdog feed to prevent reset
                constexpr uint32_t MAX_SLOT_ID = MAX_NUM_NODES - 1;
                constexpr uint32_t YIELD_INTERVAL = 50;  // Yield every 50 slots
                for (uint16_t slot_id = 0; slot_id <= MAX_SLOT_ID; slot_id++) {
                    // CRITICAL: Yield periodically to prevent watchdog timeout
                    if (slot_id % YIELD_INTERVAL == 0) {
                        yield();
                        #ifdef ARCH_NRF52
                        ::nrf52Loop();  // Feed watchdog and process SoftDevice events
                        #endif
                    }
                    
                    if (NodeStorage::slotExists(slot_id)) {
                        meshtastic_NodeInfoLite node;
                        if (NodeStorage::readNodeFromSlot(slot_id, &node)) {
                            if (NodeIndex::addNode(node.num, slot_id)) {
                                slots_found++;
                            }
                        }
                        
                        // CRITICAL: Feed watchdog after reading each slot
                        #ifdef ARCH_NRF52
                        ::nrf52Loop();  // Feed watchdog and process SoftDevice events
                        #endif
                    }
                }
            } else {
                // Read directory entries
                // CRITICAL: Add periodic watchdog feed and yield to prevent reset during long operations
                struct lfs_info info;
                uint32_t entries_processed = 0;
                constexpr uint32_t YIELD_INTERVAL = 10;  // Yield every 10 entries
                
                while (true) {
                    int read_result = lfs_dir_read(extended_lfs, &dir, &info);
                    if (read_result <= 0) {
                        break;  // End of directory or error
                    }
                    
                    entries_processed++;
                    
                    // CRITICAL: Yield periodically to prevent watchdog timeout
                    if (entries_processed % YIELD_INTERVAL == 0) {
                        yield();
                        #ifdef ARCH_NRF52
                        ::nrf52Loop();  // Feed watchdog and process SoftDevice events (use global namespace)
                        #endif
                    }
                    
                    // Check if this is a slot file (format: "slot_XXXX.bin")
                    if (info.type == LFS_TYPE_REG && strncmp(info.name, "slot_", 5) == 0) {
                        // Extract slot ID from filename "slot_XXXX.bin"
                        uint16_t slot_id = 0;
                        if (sscanf(info.name, "slot_%hu.bin", &slot_id) == 1) {
                            // Read node from slot
                            meshtastic_NodeInfoLite node;
                            if (NodeStorage::readNodeFromSlot(slot_id, &node)) {
                                // Add to index
                                if (NodeIndex::addNode(node.num, slot_id)) {
                                    slots_found++;
                                    // Note: last_heard is NOT in flash (it's hot data, RAM-only)
                                    // If index was loaded from flash, last_heard is already preserved
                                    // If index is being rebuilt, last_heard will be 0 (lost on reboot)
                                    // This is acceptable - it will be updated when packets are received
                                }
                            }
                            
                            // CRITICAL: Feed watchdog after reading each slot (readNodeFromSlot can be slow)
                            #ifdef ARCH_NRF52
                            ::nrf52Loop();  // Feed watchdog and process SoftDevice events
                            #endif
                        }
                    }
                }
                
                lfs_dir_close(extended_lfs, &dir);
            }
            
            LOG_INFO("NodeDBVirtualBackend: Rebuilt index from %u slot files", slots_found);
            
            // CRITICAL: Verify that index rebuild was successful
            // If no slots were found and index is still empty, this is OK (first boot, no nodes yet)
            uint32_t final_index_count = NodeIndex::getNodeCount();
            if (final_index_count == 0 && slots_found == 0) {
                LOG_INFO("NodeDBVirtualBackend: Index is empty during validation (first boot, no nodes in flash yet) - this is normal");
                // Mark as loaded even though empty - this prevents infinite retry loop
                index_loaded = true;
                return true;  // Success - empty index is valid state
            }
        } else {
            LOG_INFO("NodeDBVirtualBackend: Index valid (%u nodes)", index_count);
        }
    }
    
    // ========================================================================
    // PRELOAD CACHE: Load important nodes into cache for fast access
    // ========================================================================
    // Strategy:
    // 1. Load all protected nodes (local node, routers, favorites) - always in cache
    // 2. Load N most recent nodes (by last_heard) until cache is 50-70% full
    // This provides fast access to important and active nodes immediately after reboot
    
    uint32_t total_nodes = NodeIndex::getNodeCount();
    if (total_nodes == 0) {
        LOG_INFO("NodeDBVirtualBackend: No nodes to preload");
        return true;
    }
    
    LOG_INFO("NodeDBVirtualBackend: Preloading cache (%u nodes in index)...", total_nodes);
    
    uint32_t preloaded_count = 0;
    uint32_t protected_count = 0;
    size_t max_cache_size = NodeCache::getMaxCacheSize();
    size_t target_preload = (max_cache_size * 60) / 100;  // Preload up to 60% of cache
    
    // Step 1: Load all protected nodes (local, routers, favorites)
    // Read directly from flash and add to cache via putNode() for better control
    for (uint32_t i = 0; i < total_nodes; i++) {
        // CRITICAL: Yield periodically to prevent watchdog timeout
        if (i % 10 == 0) {
            yield();
            #ifdef ARCH_NRF52
            ::nrf52Loop();  // Feed watchdog and process SoftDevice events
            #endif
        }
        
        NodeNum nodeNum = NodeIndex::getNodeNumByIndex(i);
        if (nodeNum == 0) {
            continue;  // Invalid node number
        }
        
        NodeIndexEntry* entry = NodeIndex::findNode(nodeNum);
        if (entry && (entry->flags & NodeIndexEntry::FLAG_PROTECTED)) {
            // Protected node - load from flash and add to cache
            meshtastic_NodeInfoLite node;
            if (NodeStorage::readNodeFromSlot(entry->slot_id, &node)) {
                if (NodeCache::putNode(nodeNum, &node)) {
                    // Mark as cached in index
                    NodeIndex::markCached(nodeNum, true);
                    protected_count++;
                    preloaded_count++;
                } else {
                    LOG_DEBUG("NodeDBVirtualBackend: Failed to add protected node 0x%x to cache", nodeNum);
                }
            } else {
                LOG_DEBUG("NodeDBVirtualBackend: Failed to read protected node 0x%x from slot %u", nodeNum, entry->slot_id);
            }
            
            // CRITICAL: Feed watchdog after reading each node (readNodeFromSlot can be slow)
            #ifdef ARCH_NRF52
            ::nrf52Loop();  // Feed watchdog and process SoftDevice events
            #endif
        }
    }
    
    LOG_INFO("NodeDBVirtualBackend: Preloaded %u protected nodes", protected_count);
    
    // If no protected nodes were found, load first N nodes anyway (they may not be marked as protected yet)
    if (protected_count == 0 && total_nodes > 0) {
        LOG_DEBUG("NodeDBVirtualBackend: No protected nodes found, loading first nodes from index...");
        // Load first nodes to ensure cache has some data (up to 50 or target_preload)
        uint32_t initial_load_count = (target_preload < 50) ? target_preload : 50;
        for (uint32_t i = 0; i < total_nodes && i < initial_load_count && preloaded_count < target_preload; i++) {
            // CRITICAL: Yield periodically to prevent watchdog timeout
            if (i % 5 == 0) {
                yield();
                #ifdef ARCH_NRF52
                ::nrf52Loop();  // Feed watchdog and process SoftDevice events
                #endif
            }
            
            NodeNum nodeNum = NodeIndex::getNodeNumByIndex(i);
            if (nodeNum == 0) {
                continue;
            }
            
            NodeIndexEntry* entry = NodeIndex::findNode(nodeNum);
            if (!entry) {
                continue;
            }
            
            meshtastic_NodeInfoLite node;
            if (NodeStorage::readNodeFromSlot(entry->slot_id, &node)) {
                // Validate node integrity before adding to cache
                if (node.num == 0 || node.num != nodeNum) {
                    LOG_WARN("NodeDBVirtualBackend: Node integrity check failed for slot %u (expected 0x%x, got 0x%x)", 
                             entry->slot_id, nodeNum, node.num);
                    continue;
                }
                
                if (NodeCache::putNode(nodeNum, &node)) {
                    NodeIndex::markCached(nodeNum, true);
                    preloaded_count++;
                    protected_count++;  // Count as initial load
                    LOG_DEBUG("NodeDBVirtualBackend: Preloaded node 0x%x from slot %u", nodeNum, entry->slot_id);
                } else {
                    LOG_WARN("NodeDBVirtualBackend: Failed to add node 0x%x to cache (cache may be full)", nodeNum);
                }
            } else {
                LOG_WARN("NodeDBVirtualBackend: Failed to read node from slot %u", entry->slot_id);
            }
            
            // CRITICAL: Feed watchdog after reading each node (readNodeFromSlot can be slow)
            #ifdef ARCH_NRF52
            ::nrf52Loop();  // Feed watchdog and process SoftDevice events
            #endif
        }
        LOG_INFO("NodeDBVirtualBackend: Loaded %u initial nodes", protected_count);
    }
    
    // Step 2: Load most recent nodes (by last_heard from NodeIndex) until target is reached
    // CRITICAL: Use last_heard from NodeIndex (persisted on flash), not from NodeInfoLite (always 0 on flash)
    uint32_t recent_count = 0;
    
    // Initialize incremental load state if starting fresh
    if (incremental_load_state.state == IncrementalLoadState::IDLE) {
        incremental_load_state.current_index = 0;
        incremental_load_state.total_nodes = total_nodes;
        incremental_load_state.preload_target = target_preload;
        incremental_load_state.preload_count = preloaded_count;
        incremental_load_state.sort_count = 0;
        incremental_load_state.sort_processed = 0;
        incremental_load_state.state = IncrementalLoadState::PRELOADING;
    }
    
    // Check if time is valid (not RTCQualityNone)
    // If time is not valid, getTime() returns time since boot, not Unix time
    // In this case, we'll use access_count and relative ordering instead
    RTCQuality time_quality = getRTCQuality();
    uint32_t now = getValidTime(RTCQualityDevice, false);  // Returns 0 if time not valid
    bool time_valid = (time_quality != RTCQualityNone && now > 0);
    
    // Store in state for incremental processing
    incremental_load_state.time_valid = time_valid;
    incremental_load_state.now = now;
    
    LOG_DEBUG("NodeDBVirtualBackend: Continuing to load nodes until cache target (%u/%u)...", preloaded_count, target_preload);
    LOG_DEBUG("NodeDBVirtualBackend: Time quality: %d, now: %u, time_valid: %s", 
              time_quality, now, time_valid ? "yes" : "no");
    
    uint32_t operation_start = millis();  // Track time for this chunk
    
    // Use static buffer from incremental_load_state (replaces stack allocation)
    // This prevents stack overflow and allows incremental processing
    using NodeWithPriority = IncrementalLoadState::NodeWithPriority;
    NodeWithPriority* sort_buffer = incremental_load_state.sort_buffer;
    constexpr uint32_t MAX_SORT_NODES = IncrementalLoadState::MAX_SORT_NODES;
    uint32_t& sort_count = incremental_load_state.sort_count;
    sort_count = 0;
    
    // Collect nodes with their priority from NodeIndex
    // Process in chunks to avoid blocking - start from where we left off
    uint32_t start_index = incremental_load_state.current_index;
    uint32_t chunk_size = 50;  // Process 50 nodes at a time
    uint32_t end_index = (start_index + chunk_size < total_nodes) ? (start_index + chunk_size) : total_nodes;
    
    for (uint32_t i = start_index; i < end_index && sort_count < MAX_SORT_NODES; i++) {
        NodeNum nodeNum = NodeIndex::getNodeNumByIndex(i);
        if (nodeNum == 0) {
            continue;
        }
        
        // Skip if already cached
        if (NodeCache::isCached(nodeNum)) {
            continue;
        }
        
        NodeIndexEntry* entry = NodeIndex::findNode(nodeNum);
        if (!entry) {
            continue;
        }
        
        // Get last_heard and access_count from NodeIndex
        uint32_t last_heard = entry->last_heard;
        uint32_t access_count = entry->access_count;
        
        // Calculate priority:
        // - If time is valid: use last_heard (most recent = highest priority)
        // - If time is not valid: use access_count (most accessed = highest priority)
        // - If both are 0: use index order (earlier = higher priority, so we load first N)
        uint32_t priority = 0;
        if (incremental_load_state.time_valid && last_heard > 0) {
            // Time is valid: use last_heard (most recent = highest)
            // But we need to handle case where time was updated after last_heard was saved
            // If last_heard is in the future (time updated), treat it as very old
            if (last_heard > incremental_load_state.now) {
                priority = 0;  // Future timestamp = very old (time was updated)
            } else {
                priority = last_heard;  // Most recent = highest
            }
        } else if (access_count > 0) {
            // Time not valid: use access_count (most accessed = highest)
            // Scale access_count to be comparable to timestamps (multiply by large number)
            priority = access_count * 1000000;  // Scale to make it comparable
        } else {
            // Both are 0: use index order (earlier nodes = higher priority)
            // This ensures we load first N nodes if no activity data is available
            priority = (total_nodes - i);  // Earlier = higher priority
        }
        
        sort_buffer[sort_count].num = nodeNum;
        sort_buffer[sort_count].priority = priority;
        sort_buffer[sort_count].last_heard = last_heard;
        sort_buffer[sort_count].slot_id = entry->slot_id;
        sort_count++;
    }
    
    // Sort by priority descending (highest priority first)
    // Simple bubble sort (OK for small arrays)
    for (uint32_t i = 0; i < sort_count - 1; i++) {
        for (uint32_t j = 0; j < sort_count - i - 1; j++) {
            if (sort_buffer[j].priority < sort_buffer[j + 1].priority) {
                NodeWithPriority temp = sort_buffer[j];
                sort_buffer[j] = sort_buffer[j + 1];
                sort_buffer[j + 1] = temp;
            }
        }
    }
    
    // Load nodes in order (most recent first)
    // Process incrementally - start from where we left off
    uint32_t start_sort = incremental_load_state.sort_processed;
    for (uint32_t i = start_sort; i < sort_count && incremental_load_state.preload_count < incremental_load_state.preload_target; i++) {
        // CRITICAL: Yield periodically to prevent watchdog timeout
        if (i % 5 == 0) {
            yield();
            #ifdef ARCH_NRF52
            ::nrf52Loop();  // Feed watchdog and process SoftDevice events
            #endif
        }
        
        NodeNum nodeNum = sort_buffer[i].num;
        uint16_t slot_id = sort_buffer[i].slot_id;
        uint32_t last_heard = sort_buffer[i].last_heard;
        
        // Read directly from flash and add to cache
        meshtastic_NodeInfoLite node;
        if (NodeStorage::readNodeFromSlot(slot_id, &node)) {
            // Validate node integrity
            if (node.num == 0 || node.num != nodeNum) {
                LOG_WARN("NodeDBVirtualBackend: Node integrity check failed for slot %u (expected 0x%x, got 0x%x)", 
                         slot_id, nodeNum, node.num);
                continue;
            }
            
            // Restore last_heard from NodeIndex (it's not in flash, but we have it in index)
            node.last_heard = last_heard;
            
            if (NodeCache::putNode(nodeNum, &node)) {
                // Mark as cached in index
                NodeIndex::markCached(nodeNum, true);
                incremental_load_state.preload_count++;
                recent_count++;
                incremental_load_state.sort_processed = i + 1;  // Update progress
                LOG_DEBUG("NodeDBVirtualBackend: Preloaded node 0x%x from slot %u (last_heard: %u, priority: %u)", 
                         nodeNum, slot_id, last_heard, sort_buffer[i].priority);
            } else {
                LOG_DEBUG("NodeDBVirtualBackend: Cache full, cannot preload more nodes");
                incremental_load_state.sort_processed = i;  // Save progress
                break;  // Cache is full, stop trying
            }
        } else {
            LOG_WARN("NodeDBVirtualBackend: Failed to read node from slot %u", slot_id);
            incremental_load_state.sort_processed = i + 1;  // Continue anyway
        }
        
        // CRITICAL: Feed watchdog after reading each node (readNodeFromSlot can be slow)
        #ifdef ARCH_NRF52
        ::nrf52Loop();  // Feed watchdog and process SoftDevice events
        #endif
        
        // CRITICAL: Check time limit - don't spend more than 100ms in one chunk
        if (millis() - operation_start > 100) {
            LOG_DEBUG("NodeDBVirtualBackend: Time limit reached in preload, yielding");
            incremental_load_state.current_index = i + 1;  // Save progress
            incremental_load_state.state = IncrementalLoadState::PRELOADING;
            yield();
            #ifdef ARCH_NRF52
            ::nrf52Loop();
            #endif
            return false;  // Indicate loading is in progress
        }
    }
    
    // Update state
    incremental_load_state.current_index = end_index;
    
    LOG_INFO("NodeDBVirtualBackend: Preloaded %u recent nodes (total: %u/%u in cache)", 
             recent_count, incremental_load_state.preload_count, max_cache_size);
    
    // Check if we've finished processing all nodes
    if (incremental_load_state.current_index >= incremental_load_state.total_nodes && 
        incremental_load_state.sort_processed >= incremental_load_state.sort_count) {
        // All done - mark as loaded
        uint32_t final_index_count = NodeIndex::getNodeCount();
        if (final_index_count == 0) {
            LOG_ERROR("NodeDBVirtualBackend: Index is empty after loadFromDisk() - cannot mark as loaded");
            incremental_load_state.state = IncrementalLoadState::IDLE;
            return false;
        }
        
        // Mark index as loaded (prevents repeated loading)
        index_loaded = true;
        incremental_load_state.state = IncrementalLoadState::IDLE;
        
        LOG_INFO("NodeDBVirtualBackend: Successfully loaded index (%u nodes)", final_index_count);
        return true;
    } else {
        // Still in progress
        incremental_load_state.state = IncrementalLoadState::PRELOADING;
        LOG_DEBUG("NodeDBVirtualBackend: Load in progress (%u/%u nodes processed, %u/%u preloaded)", 
                 incremental_load_state.current_index, incremental_load_state.total_nodes, 
                 incremental_load_state.preload_count, incremental_load_state.preload_target);
        return false;  // Indicate loading is still in progress
    }
}

/**
 * @brief Save dirty nodes to flash
 * NOTE: Does NOT initialize backend - must be initialized before calling this
 * This prevents blocking device startup with filesystem operations
 */
bool saveToDisk()
{
    // CRITICAL: Do NOT initialize here - this can be called from NodeDB constructor
    // during device startup. If backend is not initialized, return true (no-op)
    // to allow standard save method to be used instead.
    if (!isEnabled()) {
        return true;  // Backend not initialized - allow fallback to standard save
    }
    
    LOG_DEBUG("NodeDBVirtualBackend: saveToDisk() called");
    
    // Throttle saves to 1 minute
    uint32_t now = millis();
    if (now - last_save_time < SAVE_THROTTLE_MS) {
        LOG_DEBUG("NodeDBVirtualBackend: Save throttled (last save %u ms ago)", now - last_save_time);
        return true;  // Too soon, skip save
    }
    
    LOG_DEBUG("NodeDBVirtualBackend: Checking radio state...");
    // Check radio state before saving (for extended FS variants)
    // NOTE: During initialization, RadioLibInterface::instance may be nullptr
    // This is OK - we skip radio check if radio is not initialized yet
    if (!NodeDBFilesystemAdapter::shouldProceedWithSave(last_save_time)) {
        LOG_DEBUG("NodeDBVirtualBackend: Radio busy, deferring save");
        return true;  // Radio busy, defer save
    }
    
    LOG_DEBUG("NodeDBVirtualBackend: Flushing dirty nodes to flash...");
    // Flush dirty nodes to flash
    uint32_t written = NodeCache::flushDirtyNodes();
    
    if (written > 0) {
        LOG_INFO("NodeDBVirtualBackend: Saved %u dirty nodes to flash", written);
    } else {
        LOG_DEBUG("NodeDBVirtualBackend: No dirty nodes to save");
    }
    
    // CRITICAL: Save NodeIndex to flash (preserves last_heard timestamps)
    // This is essential for:
    // 1. Preloading cache with most active nodes on next boot
    // 2. Evicting oldest nodes when flash is full
    LOG_DEBUG("NodeDBVirtualBackend: Saving NodeIndex to flash...");
    if (NodeIndex::saveToFlash()) {
        LOG_DEBUG("NodeDBVirtualBackend: NodeIndex saved to flash successfully");
    } else {
        LOG_WARN("NodeDBVirtualBackend: Failed to save NodeIndex to flash");
        // Don't fail the entire save operation if index save fails
    }
    
    last_save_time = now;
    LOG_DEBUG("NodeDBVirtualBackend: saveToDisk() completed");
    return true;
}

/**
 * @brief Get node by index (for iteration)
 * 
 * CRITICAL: This function avoids stack overflow by using getNodeNumByIndex()
 * instead of allocating a large array (1234 * 4 = 4936 bytes) on the stack.
 */
meshtastic_NodeInfoLite* getNodeByIndex(size_t index)
{
    // CRITICAL: Do NOT initialize here - backend must be initialized before use
    if (!isEnabled()) {
        return nullptr;
    }
    
    // Get node number by index directly (avoids large stack allocation)
    NodeNum nodeNum = NodeIndex::getNodeNumByIndex(index);
    if (nodeNum == 0) {
        // Index out of range or node not found
        return nullptr;
    }
    
    // Get node by number (loads from flash if not cached)
    return getNode(nodeNum);
}

/**
 * @brief Mark a node as protected
 */
void markProtected(NodeNum nodeNum, bool isProtected)
{
    if (!isEnabled()) {
        return;
    }
    
    NodeIndex::markProtected(nodeNum, isProtected);
}

/**
 * @brief Validate index integrity
 */
bool validateIndexIntegrity()
{
    if (!isEnabled()) {
        return false;
    }
    
    LOG_DEBUG("NodeDBVirtualBackend: Validating index integrity...");
    
    uint32_t index_count = NodeIndex::getNodeCount();
    uint32_t slot_count = NodeStorage::getSlotCount();
    
    if (index_count != slot_count) {
        LOG_WARN("NodeDBVirtualBackend: Index mismatch (index: %u, slots: %u)", index_count, slot_count);
        return false;
    }
    
    // Check that all index entries point to valid slots
    for (uint32_t i = 0; i < index_count; i++) {
        NodeNum nodeNum = NodeIndex::getNodeNumByIndex(i);
        if (nodeNum == 0) {
            continue;
        }
        
        NodeIndexEntry* entry = NodeIndex::findNode(nodeNum);
        if (!entry || entry->slot_id == UINT16_MAX) {
            LOG_WARN("NodeDBVirtualBackend: Invalid index entry for node 0x%x", nodeNum);
            return false;
        }
        
        // Check if slot file exists
        if (!NodeStorage::slotExists(entry->slot_id)) {
            LOG_WARN("NodeDBVirtualBackend: Slot %u for node 0x%x does not exist", entry->slot_id, nodeNum);
            return false;
        }
    }
    
    LOG_DEBUG("NodeDBVirtualBackend: Index integrity check passed");
    return true;
}

/**
 * @brief Rebuild index from flash slots if needed
 */
bool rebuildIndexIfNeeded()
{
    if (!isEnabled()) {
        return false;
    }
    
    if (validateIndexIntegrity()) {
        return true;  // Index is valid, no rebuild needed
    }
    
    LOG_WARN("NodeDBVirtualBackend: Index integrity check failed, rebuilding...");
    
    // Clear current index
    NodeIndex::clear();
    
    // Rebuild by scanning slots (this is done in loadFromDisk() when index is empty)
    // For now, just mark index as not loaded so it will be rebuilt on next loadFromDisk()
    index_loaded = false;
    
    LOG_INFO("NodeDBVirtualBackend: Index marked for rebuild on next load");
    return true;
}

/**
 * @brief Recover from flash errors
 */
bool recoverFromError()
{
    if (!isEnabled()) {
        return false;
    }
    
    LOG_WARN("NodeDBVirtualBackend: Attempting error recovery...");
    
    // Step 1: Validate index integrity
    if (!validateIndexIntegrity()) {
        // Step 2: Rebuild index if corrupted
        if (!rebuildIndexIfNeeded()) {
            LOG_ERROR("NodeDBVirtualBackend: Failed to rebuild index during recovery");
            return false;
        }
    }
    
    // Step 3: Clear any corrupted cache entries
    // This will be handled automatically when nodes are accessed
    
    LOG_INFO("NodeDBVirtualBackend: Error recovery completed");
    return true;
}

} // namespace NodeDBVirtualBackend

#endif // USE_EXTENDED_FS_FOR_NODEDB

