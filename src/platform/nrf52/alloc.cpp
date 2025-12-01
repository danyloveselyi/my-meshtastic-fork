#include "configuration.h"
#include "rtos.h"
#include "memGet.h"
#include <assert.h>
#include <stdlib.h>

/**
 * Custom new/delete with graceful error handling
 * Returns nullptr instead of asserting to allow caller to handle errors
 */

// Global counter for memory allocation failures (for diagnostics)
static uint32_t memory_allocation_failures = 0;

uint32_t getAllocationFailureCount()
{
    return memory_allocation_failures;
}

void *operator new(size_t size)
{
    uint32_t freeHeap = memGet.getFreeHeap();
    uint32_t heapTotal = memGet.getHeapSize();
    auto p = rtos_malloc(size);
    
    if (!p) {
        memory_allocation_failures++;
        uint32_t usedHeap = (heapTotal > freeHeap) ? (heapTotal - freeHeap) : 0;
        LOG_ERROR("operator new FAILED: size=%u bytes, freeHeap=%u, heapTotal=%u, usedHeap=%u, failures=%u", 
                 size, freeHeap, heapTotal, usedHeap, memory_allocation_failures);
        // Return nullptr instead of assert to allow graceful error handling
        return nullptr;
    }
    
    return p;
}

void *operator new[](size_t size)
{
    uint32_t freeHeap = memGet.getFreeHeap();
    uint32_t heapTotal = memGet.getHeapSize();
    auto p = rtos_malloc(size);
    
    if (!p) {
        memory_allocation_failures++;
        uint32_t usedHeap = (heapTotal > freeHeap) ? (heapTotal - freeHeap) : 0;
        LOG_ERROR("operator new[] FAILED: size=%u bytes, freeHeap=%u, heapTotal=%u, usedHeap=%u, failures=%u", 
                 size, freeHeap, heapTotal, usedHeap, memory_allocation_failures);
        // Return nullptr instead of assert to allow graceful error handling
        return nullptr;
    }
    
    return p;
}

void operator delete(void *ptr)
{
    rtos_free(ptr);
}

void operator delete[](void *ptr)
{
    rtos_free(ptr);
}