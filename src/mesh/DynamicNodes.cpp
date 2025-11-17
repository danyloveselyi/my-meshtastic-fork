#include "configuration.h"  // Must be first to get variant.h overrides
#include "mesh-pb-constants.h"

// Default values for dynamic node management
// Variants can override these by defining them in variant.cpp with the same names

// Default maximum number of nodes (can be overridden by variants)
// Note: extern is needed for const to have external linkage with weak attribute
// Uses MAX_NUM_NODES from mesh-pb-constants.h which is architecture-specific:
// - ARCH_NRF52: 80
// - ARCH_STM32WL: 10
// - ESP32S3: 100-250 (depends on flash size)
// - Other: 100
extern const uint32_t DEFAULT_MAX_NODES __attribute__((weak)) = MAX_NUM_NODES;

// Current maximum nodes limit (can be changed at runtime, starts at DEFAULT_MAX_NODES)
uint32_t dynamic_max_nodes __attribute__((weak)) = MAX_NUM_NODES;

