/**
 * @file NodeDBBackgroundTask.cpp
 * @brief Background task implementation
 */

#include "NodeDBBackgroundTask.h"
#include "NodeDBWriteQueue.h"
#include "NodeDBVirtualBackend.h"
#include "NodeStorage.h"
#include "NodeCache.h"
#include "NodeIndex.h"
#include "../../../src/memGet.h"

#ifdef ARCH_NRF52
// Forward declaration for nrf52Loop()
extern void nrf52Loop();
#include "rtos.h"  // For rtos_malloc
#include <new>     // For placement new
#include "NRF52Bluetooth.h"  // For checking BLE connection state
#include "../../../src/BluetoothStatus.h"  // For checking BLE pairing state
extern NRF52Bluetooth *nrf52Bluetooth;  // Global BLE instance
extern meshtastic::BluetoothStatus *bluetoothStatus;  // Global BLE status
#endif
#include <Arduino.h>  // For yield(), millis(), delay()

#ifdef USE_EXTENDED_FS_FOR_NODEDB

namespace NodeDBBackgroundTask {

// Global instance
static NodeDBBackgroundTaskThread* task_instance = nullptr;
static bool task_initialized = false;

/**
 * @brief Constructor
 */
NodeDBBackgroundTaskThread::NodeDBBackgroundTaskThread()
    : concurrency::OSThread("NodeDBBg", 200)  // Run every 200ms by default
    , last_operation_time(0)
    , load_in_progress(false)
{
}

/**
 * @brief Main task loop
 */
int32_t NodeDBBackgroundTaskThread::runOnce()
{
    // CRITICAL: Feed watchdog at start of each cycle
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    #endif
    yield();
    
    // Check if we should proceed (radio not busy, etc.)
    if (!shouldProceedWithFlashOps()) {
        return RUN_SAME;  // Keep same interval, try again next time
    }
    
    uint32_t cycle_start = millis();
    uint32_t operations_done = 0;
    
    // Process write queue (max 2 writes per cycle to avoid blocking)
    operations_done += processWriteQueue(2);
    
    // Process incremental load if in progress
    if (processIncrementalLoad()) {
        load_in_progress = true;
    } else {
        load_in_progress = false;
    }
    
    // CRITICAL: Feed watchdog after operations
    #ifdef ARCH_NRF52
    ::nrf52Loop();
    #endif
    yield();
    
    // If we did work, check if we need a longer delay
    if (operations_done > 0) {
        uint32_t cycle_time = millis() - cycle_start;
        if (cycle_time > 100) {
            // Operations took a while, use longer interval
            return 500;  // Wait 500ms before next cycle
        }
    }
    
    // Default: run every 200ms
    return RUN_SAME;
}

/**
 * @brief Process write queue
 */
uint32_t NodeDBBackgroundTaskThread::processWriteQueue(uint32_t maxWrites)
{
    if (!NodeDBWriteQueue::isInitialized()) {
        return 0;
    }
    
    if (NodeDBWriteQueue::isEmpty()) {
        return 0;
    }
    
    uint32_t writes_processed = 0;
    uint32_t operation_start = millis();
    
    while (writes_processed < maxWrites && !NodeDBWriteQueue::isEmpty()) {
        // CRITICAL: Check time - don't spend more than 5 seconds in one go
        if (millis() - operation_start > 5000) {
            LOG_DEBUG("NodeDBBackgroundTask: Time limit reached, yielding");
            yield();
            #ifdef ARCH_NRF52
            ::nrf52Loop();
            #endif
            delay(50);  // Give system time to process events
            operation_start = millis();  // Reset timer
        }
        
        // Get next entry from queue
        NodeDBWriteQueue::WriteQueueEntry entry;
        if (!NodeDBWriteQueue::dequeue(entry)) {
            break;  // Queue empty
        }
        
        // Get node from cache
        meshtastic_NodeInfoLite* node = NodeCache::getNode(entry.nodeNum);
        if (!node) {
            LOG_WARN("NodeDBBackgroundTask: Node 0x%x not in cache, skipping write", entry.nodeNum);
            continue;
        }
        
        // CRITICAL: Feed watchdog before flash write
        #ifdef ARCH_NRF52
        ::nrf52Loop();
        #endif
        yield();
        
        // Write to flash
        uint32_t write_start = millis();
        bool write_success = NodeStorage::writeNodeToSlot(entry.slotId, node);
        uint32_t write_time = millis() - write_start;
        
        // CRITICAL: Feed watchdog after flash write
        #ifdef ARCH_NRF52
        ::nrf52Loop();
        #endif
        yield();
        
        if (write_success) {
            // Mark as clean in cache
            NodeCache::clearDirty(entry.nodeNum);
            writes_processed++;
            
            LOG_DEBUG("NodeDBBackgroundTask: Wrote node 0x%x to slot %u (took %u ms)", 
                     entry.nodeNum, entry.slotId, write_time);
        } else {
            LOG_ERROR("NodeDBBackgroundTask: Failed to write node 0x%x to slot %u", 
                     entry.nodeNum, entry.slotId);
            // Re-queue with lower priority? Or just skip?
            // For now, just skip - it will be marked dirty again if accessed
        }
        
        // Small delay between writes to allow other tasks to run
        if (writes_processed < maxWrites && !NodeDBWriteQueue::isEmpty()) {
            delay(10);  // 10ms delay between writes
        }
    }
    
    if (writes_processed > 0) {
        LOG_DEBUG("NodeDBBackgroundTask: Processed %u writes from queue (%u remaining)", 
                 writes_processed, NodeDBWriteQueue::getSize());
    }
    
    return writes_processed;
}

/**
 * @brief Process incremental index loading
 */
bool NodeDBBackgroundTaskThread::processIncrementalLoad()
{
    // Call loadFromDisk() which now supports incremental loading
    // It will process a chunk and return false if still in progress
    if (NodeDBVirtualBackend::loadFromDisk()) {
        // Loading complete
        load_in_progress = false;
        return false;
    } else {
        // Still in progress
        load_in_progress = true;
        return true;
    }
}

/**
 * @brief Check if we should proceed with flash operations
 */
bool NodeDBBackgroundTaskThread::shouldProceedWithFlashOps()
{
    // Check memory pressure
    uint32_t freeHeap = memGet.getFreeHeap();
    if (freeHeap < 20480) {  // Less than 20 KB free
        LOG_DEBUG("NodeDBBackgroundTask: Low memory (%u bytes), deferring flash ops", freeHeap);
        return false;
    }
    
    // CRITICAL: Check if BLE is actively pairing or just connected
    // Flash operations block radio and CPU, which can interfere with BLE bonding
    // Strategy: Defer operations during pairing and for a short time after connection to allow bonding to complete
    // After that, allow operations (BLE can handle flash operations when not actively pairing)
    #ifdef ARCH_NRF52
    static uint32_t last_ble_connect_time = 0;
    static bool ble_was_connected = false;
    static bool ble_was_pairing = false;
    
    bool ble_connected = (nrf52Bluetooth && nrf52Bluetooth->isConnected());
    bool ble_pairing = false;
    
    // Check if BLE is actively pairing (more reliable than just checking connection time)
    if (bluetoothStatus) {
        auto state = bluetoothStatus->getConnectionState();
        ble_pairing = (state == meshtastic::BluetoothStatus::ConnectionState::PAIRING);
    }
    
    uint32_t now = millis();
    
    // CRITICAL: Always defer flash operations during active pairing
    if (ble_pairing) {
        if (!ble_was_pairing) {
            LOG_DEBUG("NodeDBBackgroundTask: BLE pairing in progress, deferring flash ops");
            ble_was_pairing = true;
        }
        return false;  // Never allow flash operations during pairing
    }
    
    // Reset pairing flag when pairing completes
    if (ble_was_pairing && !ble_pairing) {
        ble_was_pairing = false;
        // CRITICAL: After pairing completes, wait longer to ensure bonding storage is fully written
        // Bonding storage write can take time, and flash operations during this can corrupt it
        if (ble_connected) {
            last_ble_connect_time = now;
            ble_was_connected = true;
            LOG_DEBUG("NodeDBBackgroundTask: Pairing completed, deferring flash ops for 30 seconds to ensure bonding completes");
        }
    }
    
    if (ble_connected) {
        // Track when BLE connection state changes
        if (!ble_was_connected) {
            // BLE just connected - mark time and defer operations
            last_ble_connect_time = now;
            ble_was_connected = true;
            LOG_DEBUG("NodeDBBackgroundTask: BLE connected, deferring flash ops for 30 seconds to allow bonding");
            return false;
        }
        
        // CRITICAL: Increased to 30 seconds after pairing/connection to ensure bonding storage is fully written
        // Bonding storage write happens after pairing completes, and flash operations during this can:
        // 1. Corrupt bonding storage (causing "binding failed : null")
        // 2. Cause device reboot
        // 3. Corrupt device name buffer
        if ((now - last_ble_connect_time) < 30000) {
            // Still in bonding window - defer
            return false;
        }
        
        // BLE connected but bonding window passed - allow operations
        // Flash operations won't interfere with idle BLE connection
    } else {
        // BLE not connected - reset tracking
        if (ble_was_connected) {
            ble_was_connected = false;
            last_ble_connect_time = 0;
        }
    }
    #endif
    
    return true;
}

/**
 * @brief Initialize and start the background task
 */
bool initialize()
{
    if (task_initialized) {
        return true;
    }
    
    // Initialize write queue first
    if (!NodeDBWriteQueue::initialize(100)) {
        LOG_ERROR("NodeDBBackgroundTask: Initialized write queue failed, continuing without async writes");
        // Don't fail - we can work synchronously
    }
    
    // Check available memory before creating task instance
    uint32_t freeHeap = memGet.getFreeHeap();
    uint32_t taskSize = sizeof(NodeDBBackgroundTaskThread);
    uint32_t safetyMargin = 10240; // 10 KB safety margin
    
    if (freeHeap < (taskSize + safetyMargin)) {
        LOG_WARN("NodeDBBackgroundTask: Not enough memory to create task (free: %u, required: %u + %u margin)", 
                 freeHeap, taskSize, safetyMargin);
        // Don't fail - we can work synchronously without background task
        return false;
    }
    
    // Create task instance using rtos_malloc to avoid assert in operator new
    #ifdef ARCH_NRF52
    task_instance = (NodeDBBackgroundTaskThread*)rtos_malloc(sizeof(NodeDBBackgroundTaskThread));
    if (!task_instance) {
        LOG_WARN("NodeDBBackgroundTask: rtos_malloc failed (size: %u bytes, free heap: %u)", 
                 sizeof(NodeDBBackgroundTaskThread), freeHeap);
        // Don't fail - we can work synchronously without background task
        return false;
    }
    // Use placement new to construct the object in the allocated memory
    new (task_instance) NodeDBBackgroundTaskThread();
    #else
    // For non-NRF52 platforms, use regular new (but check memory first)
    task_instance = new NodeDBBackgroundTaskThread();
    if (!task_instance) {
        LOG_WARN("NodeDBBackgroundTask: Failed to create task instance");
        return false;
    }
    #endif
    
    task_initialized = true;
    LOG_INFO("NodeDBBackgroundTask: Initialized and started");
    
    return true;
}

/**
 * @brief Check if background task is initialized
 */
bool isInitialized()
{
    return task_initialized && task_instance != nullptr;
}

/**
 * @brief Get the background task instance
 */
NodeDBBackgroundTaskThread* getInstance()
{
    return task_instance;
}

} // namespace NodeDBBackgroundTask

#endif // USE_EXTENDED_FS_FOR_NODEDB

