# Extended Filesystem Configuration for RAK4631 Lite

This variant provides extended filesystem support (80 pages, 320 KB) for storing 500+ nodes.

## Features

- **Extended filesystem**: 80 pages (320 KB) instead of default 7 pages (28 KB)
- **500+ nodes support**: Sufficient space for large mesh networks
- **Automatic formatting**: Filesystem automatically formats when size changes
- **Bootloader safe**: Filesystem located at 0x80000-0xF4000, safely away from bootloader (0xF4000+)
- **Update safe**: Bootloader updates (DFU/USB/Bluetooth) only write to app area (0x27000-0x80000), filesystem data is preserved

## Memory Layout

```
0x00000 - 0x27000: SoftDevice (156 KB)
0x27000 - 0x80000: Application (356 KB) - linker script limited
0x80000 - 0xF4000: Filesystem (80 pages, 320 KB) - LittleFS
0xF4000 - 0xFFFFF: Bootloader (48 KB) - safe margin 144 KB maintained
```

## Files

- `main-nrf52-filesystem.cpp` - Extended preFSBegin() logic with automatic formatting
- `FSCommon-fallback.cpp` - Fallback logic for version file creation
- `nrf52840_s140_v7.ld` - Linker script limiting application to 0x80000
- `variant-filesystem-config.h` - Configuration constants (optional)

## Configuration

The variant is enabled by setting `RAK_4631_LITE_EXTENDED_FILESYSTEM` macro in `platformio.ini`.

All filesystem-related code is isolated in `variants/rak4631_lite/` directory, making firmware updates easier - only variant files need to be updated.

