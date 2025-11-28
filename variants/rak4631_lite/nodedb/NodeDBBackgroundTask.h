/**
 * @file NodeDBBackgroundTask.h
 * @brief Background task for asynchronous NodeDB operations
 * 
 * This module provides a background OSThread task that processes flash operations
 * asynchronously to avoid blocking the main loop. It handles:
 * - Write queue processing (deferred flash writes)
 * - Incremental index loading
 * - Eviction queue processing
 * 
 * Key features:
 * - Non-blocking flash operations
 * - Throttled writes (max 1-2 per cycle)
 * - Watchdog-friendly (yields regularly)
 * - Priority-based processing
 */

#pragma once

#include "configuration.h"
#include "concurrency/OSThread.h"

#ifdef USE_EXTENDED_FS_FOR_NODEDB

namespace concurrency {
    class OSThread;
}

namespace NodeDBBackgroundTask {

/**
 * @brief Background task class for NodeDB operations
 */
class NodeDBBackgroundTaskThread : public concurrency::OSThread {
public:
    NodeDBBackgroundTaskThread();
    virtual ~NodeDBBackgroundTaskThread() = default;

protected:
    /**
     * @brief Main task loop - called periodically by OSThread
     * @return Desired period for next invocation (in ms), or RUN_SAME
     */
    virtual int32_t runOnce() override;

private:
    /**
     * @brief Process write queue (deferred flash writes)
     * @param maxWrites Maximum number of writes to process this cycle
     * @return Number of writes processed
     */
    uint32_t processWriteQueue(uint32_t maxWrites = 2);
    
    /**
     * @brief Process incremental index loading
     * @return true if loading is in progress, false if complete
     */
    bool processIncrementalLoad();
    
    /**
     * @brief Check if we should proceed with flash operations
     * @return true if safe to proceed
     */
    bool shouldProceedWithFlashOps();
    
    uint32_t last_operation_time;  // Track time of last operation for watchdog
    bool load_in_progress;          // Track if incremental load is in progress
};

/**
 * @brief Initialize and start the background task
 * @return true if initialization was successful
 */
bool initialize();

/**
 * @brief Check if background task is initialized
 * @return true if initialized
 */
bool isInitialized();

/**
 * @brief Get the background task instance
 * @return Pointer to task instance, or nullptr if not initialized
 */
NodeDBBackgroundTaskThread* getInstance();

} // namespace NodeDBBackgroundTask

#endif // USE_EXTENDED_FS_FOR_NODEDB



