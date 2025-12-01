/**
 * @file variant-filesystem-config.h
 * @brief Extended filesystem configuration for RAK4631 Lite variant
 * 
 * This header defines filesystem configuration constants for RAK4631 Lite variant.
 * It provides extended filesystem support (68 pages, 272 KB) for NodeDB storage.
 * 
 * Memory Layout:
 * - Application: 0x27000 - 0xA8000 (~516 KB) - limited by linker script to make room for Extended FS
 * - Extended FS: 0xA8000 - 0xEBFFF (272 KB, 68 pages) - after application, before Main FS
 * - Gap: 0xEC000 - 0xECFFF (4 KB, 1 page) - safety buffer before Main FS
 * - Main FS: 0xED000 - 0xF4000 (28 KB, 7 pages) - standard Meshtastic filesystem
 * - Bootloader: 0xF8000 - 0xFFFFF (32 KB) - Adafruit bootloader standard address
 * Note: Extended FS is 68 pages (272 KB) to provide sufficient space for 2,000-2,400 nodes
 * (realistic capacity based on actual node slot sizes: 69-82 bytes data + ~20-50 bytes LittleFS overhead = ~100-130 bytes per node)
 */

#ifndef _VARIANT_FILESYSTEM_CONFIG_H_
#define _VARIANT_FILESYSTEM_CONFIG_H_

#ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM

// Filesystem size configuration
#define VARIANT_LFS_SIZE_VERSION 68  // Number of pages (272 KB - sufficient for 2,000-2,400 nodes, ends before Main FS at 0xED000)
#define VARIANT_LFS_SIZE_VERSION_FILE "/.lfs_size_version"
#define VARIANT_LFS_FLASH_ADDR 0xA8000  // After application, limited by linker script
#define VARIANT_LFS_FLASH_TOTAL_SIZE (68 * 4096)  // 68 pages * 4KB = 278528 bytes = 272 KB

// Filesystem display size for /mem command
#define VARIANT_LFS_DISPLAY_TOTAL (68 * 4096)  // 272 KB

#endif // RAK_4631_LITE_EXTENDED_FILESYSTEM

#endif // _VARIANT_FILESYSTEM_CONFIG_H_

