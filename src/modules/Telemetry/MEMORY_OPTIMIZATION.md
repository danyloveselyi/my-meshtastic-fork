# Telemetry Memory Optimization Documentation

## Overview

This document explains the memory optimization strategy used in Meshtastic telemetry modules to reduce Flash memory usage on resource-constrained devices like nRF52840.

## Strategy: Protobuf Reuse

Instead of creating custom protobuf definitions for specialized data (memory statistics, node counts, etc.), we reuse existing protobuf structures. This saves significant Flash memory by avoiding additional protobuf code generation.

## Field Mapping Examples

### DeviceTelemetry::getMemoryStatsAsEnvironmentTelemetry()

**Reuses:** `meshtastic_EnvironmentMetrics` protobuf structure  
**Purpose:** Send memory and node statistics without custom protobuf definitions

| Environment Field | Actual Data | Description |
|-------------------|-------------|-------------|
| `gas_resistance` | Flash Total (KB) | Total Flash memory available |
| `relative_humidity` | Flash Free (KB) | Free Flash memory available |
| `iaq` | Heap Total (KB) | Total RAM heap size |
| `lux` | Heap Free (KB) | Free RAM heap available |
| `white_lux` | Online Nodes | Count of active mesh nodes |
| `barometric_pressure` | Total Nodes | Total nodes in database |
| `current` | Max Nodes | Maximum node limit configured |

### Standard Protobuf Usage

Other telemetry modules use protobuf structures in their intended way:

- **PowerTelemetry**: `meshtastic_PowerMetrics` for actual power sensor data
- **HealthTelemetry**: `meshtastic_HealthMetrics` for health sensor data  
- **EnvironmentTelemetry**: `meshtastic_EnvironmentMetrics` for environment sensors

## Memory Savings

This optimization provides:
- **Reduced Flash usage**: No additional protobuf code generation
- **Protocol compatibility**: Existing client applications work unchanged
- **Flexibility**: Same protobuf structure can serve multiple purposes
- **Maintainability**: Single protobuf definition for multiple data types

## Implementation Notes

### Code Comments
All optimized functions include detailed comments explaining:
- Field mapping strategies
- Memory optimization rationale  
- Protocol compatibility considerations
- Debug logging for field verification

### Debug Logging
Memory statistics telemetry includes dual logging:
```cpp
LOG_INFO("MemoryStats as Environment: Flash(gas_resistance/relative_humidity)=%.1f/%.1fKB, ...");
LOG_INFO("Raw memory values: FlashTotal=%u, FlashUsed=%u, FlashFree=%u, ...");
```

### Client Compatibility
Clients can distinguish data types by:
- Source node identification
- Telemetry packet timing/context
- Field value patterns (e.g., memory values vs. sensor readings)

## Usage in Optimized Builds

For memory-constrained builds (like `rak4631_eth_gw`), this strategy enables:
- Memory monitoring without custom protocols
- Node statistics transmission  
- Flash/heap usage tracking
- Network health monitoring

All while maintaining the aggressive memory optimization requirements of embedded LoRa devices.