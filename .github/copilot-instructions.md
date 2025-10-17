
# Meshtastic Firmware Copilot Instructions

## Architecture Overview

**Meshtastic** is a LoRa mesh networking firmware supporting ESP32, nRF52, RP2040/RP2350, and STM32 platforms. The architecture centers around:

- **Multi-platform support**: Platform-specific code lives in `arch/{esp32,nrf52,portduino,rp2xx0,stm32}/` with shared core in `src/`
- **Module system**: Protocol handlers in `src/modules/` inherit from `ProtobufModule` or `SinglePortModule`
- **Mesh networking**: Core mesh logic in `src/mesh/` with routing, crypto, and radio interfaces
- **Hardware variants**: Device-specific configs in `variants/*/` with pin definitions and hardware capabilities

**This Fork**: Adds `MemoryMonitorModule` for RAM/Flash usage tracking, extreme value logging, and low-memory warnings. Primary supported targets are `rak4631_eth_gw` and `rak4631`.

**⚠️ RAK4631 Focus**: This fork is specifically optimized for RAK4631 hardware. All development, testing, and feature implementations should prioritize RAK4631 compatibility. Other hardware variants may not function correctly or may be excluded from builds to reduce memory footprint for RAK4631 optimization.

## Key Development Patterns

### Build System (PlatformIO)
- **RAK4631 Primary builds**: `pio run -e rak4631` or `pio run -e rak4631_eth_gw` (default environment)
- Other hardware not guaranteed to work: Focus development on RAK4631 variants only
- Platform-specific builds: `bin/build-{esp32,nrf52,stm32,rpi2040}.sh`
- Architecture configs: `arch/*/esp32.ini`, `arch/*/nrf52.ini` etc. with platform-specific flags and dependencies
- Variant files: `variants/*/platformio.ini` override build settings per hardware variant
- Config inheritance: Root `platformio.ini` includes all configs via `extra_configs = arch/*/*.ini, variants/*/platformio.ini`

### Module Development
```cpp
// All modules inherit from ProtobufModule with standardized patterns:
class MyModule : public ProtobufModule<meshtastic_MyMessage>
{
public:
    MyModule() : ProtobufModule("mymodule", meshtastic_PortNum_MY_APP, &myMessage) {}
    
protected:
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;
};
```
- Register modules in `src/modules/Modules.cpp` with conditional compilation flags
- Use memory pools for packet allocation: `allocDataPacket()` and `service.sendToMesh()`

### Hardware Abstraction
- Pin definitions: `variants/*/variant.h` defines `PIN_SPI_*`, `LORA_*`, display pins
- Conditional compilation: Use `#ifdef VARIANT_NAME` and feature flags like `MESHTASTIC_EXCLUDE_*`
- Radio interfaces: Inherit from `RadioLibInterface` in `src/mesh/` (SX126x, SX128x, etc.)

### Configuration System
- Protobuf-based config: `meshtastic/*.proto` files generate C structures
- NodeDB: Persistent storage in `src/mesh/NodeDB.cpp` handles device and channel configs
- Build-time flags: Set in `platformio.ini` environment sections with `-D` flags

## Critical Development Workflows

### Adding New Hardware Variant
1. Create `variants/myboard/` with `variant.h`, `pins_arduino.h`, `platformio.ini`
2. Define pin mappings, radio type, and hardware capabilities
3. Add board definition to `boards/myboard.json` if custom
4. Test with `pio run -e myboard`

### Protocol Buffer Updates
- Protobufs live in separate `protobufs/` submodule
- Regenerate with `bin/regen-protos.sh` after protobuf changes
- Generated files appear in `src/mesh/generated/meshtastic/` (e.g., `mesh.pb.h`, `config.pb.h`)
- Use `nanopb` library for embedded-friendly protobuf implementation

### Debugging and Logging
- Use `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` macros (not printf)
- Enable with `#define DEBUG_PORT Serial` in variant files
- Memory debugging: `src/memGet.cpp` provides heap monitoring
- Static analysis: `bin/check-all.sh` runs cppcheck

### Testing Patterns
- Unit tests in `test/` use custom `TestUtil.h` framework
- Hardware-in-loop: Use `test-simulator.sh` for native builds
- Platform testing: `arch/portduino/` provides Linux simulation environment

## Integration Points

### Thread System
- `src/concurrency/OSThread.h`: Platform-agnostic threading
- Main threads: `PowerFSMThread`, `ButtonThread`, `AudioThread` in `src/`
- Module threads: Use `GenericThreadModule` for background processing

### Memory Management
- Fixed-size memory pools in `src/mesh/MemoryPool.h` prevent fragmentation
- Packet queues: `TypedQueue` and `PointerQueue` for thread-safe messaging
- RAM constraints: nRF52 variants especially memory-constrained

### Radio Layer
- `RadioInterface` abstraction supports multiple LoRa chips
- Frequency/region configs in `variant.h` with `HAS_RADIO` feature flags
- Mesh routing: `FloodingRouter` vs `ReliableRouter` algorithms in `src/mesh/`

### External Communication
- Serial API: `src/mesh/StreamAPI.cpp` for CLI and external app communication  
- Bluetooth: Platform-specific in `src/nimble/` (nRF52) and ESP32 Bluetooth Classic
- Network modes: WiFi (ESP32), Ethernet gateways, MQTT bridging

## Fork-Specific Features

### Memory Monitoring Module
- `MemoryMonitorModule` in `src/modules/MemoryMonitorModule.{cpp,h}` tracks RAM/Flash usage
- Inherits from `ProtobufModule<meshtastic_Telemetry>` following standard module patterns
- Provides continuous monitoring, extreme value logging, and low-memory warnings
- **Note**: Not yet registered in `src/modules/Modules.cpp` - requires manual integration

## Common Gotchas

- **Memory allocation**: Always use pools, never `malloc()` on embedded targets
- **Platform ifdefs**: Check `#ifdef ARCH_*` before using platform-specific APIs
- **Protobuf size limits**: Max 237 bytes for LoRa packets, use `pb_encode_*` helpers
- **Build flags**: Exclude unused features with `MESHTASTIC_EXCLUDE_*` to save flash/RAM
- **Variant inheritance**: Some variants extend others; check `extends = base_variant` in platformio.ini