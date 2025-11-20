/**
 * @file variant-filesystem-config.h
 * @brief Extended filesystem configuration for RAK4631 Lite variant
 * 
 * This header defines filesystem configuration constants for RAK4631 Lite variant.
 * It provides extended filesystem support (80 pages, 320 KB) for 500+ nodes.
 * 
 * Memory Layout:
 * - Application: 0x27000 - 0x80000 (356 KB)
 * - Filesystem: 0x80000 - 0xF4000 (80 pages, 320 KB)
 * - Bootloader: 0xF4000 - 0xFFFFF (48 KB)
 */

#ifndef _VARIANT_FILESYSTEM_CONFIG_H_
#define _VARIANT_FILESYSTEM_CONFIG_H_

#ifdef RAK_4631_LITE_EXTENDED_FILESYSTEM

// Filesystem size configuration
#define VARIANT_LFS_SIZE_VERSION 80  // Number of pages (320 KB - sufficient for 400+ nodes)
#define VARIANT_LFS_SIZE_VERSION_FILE "/.lfs_size_version"
#define VARIANT_LFS_FLASH_ADDR 0x80000  // After application (linker limited to 0x80000)
#define VARIANT_LFS_FLASH_TOTAL_SIZE (80 * 4096)  // 80 pages * 4KB = 327680 bytes = 320 KB

// Filesystem display size for /mem command
#define VARIANT_LFS_DISPLAY_TOTAL (80 * 4096)  // 320 KB

#endif // RAK_4631_LITE_EXTENDED_FILESYSTEM

#endif // _VARIANT_FILESYSTEM_CONFIG_H_

