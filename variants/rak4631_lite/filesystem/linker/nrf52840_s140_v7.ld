/* Linker script to configure memory regions for RAK4631 Lite variant. */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  /* Application flash: 0x27000 - 0xA8000 (~516 KB) - limited to make room for enlarged Main FS
   * Main FS: 0xA8000 - 0xF4000 (304 KB, 76 pages) - enlarged Meshtastic filesystem for NodeDB
   * Bootloader: 0xF8000 - 0xFFFFF (32 KB) - Adafruit bootloader standard address
   * Note: Main FS is 76 pages (304 KB) to provide sufficient space for 1200+ nodes
   */
  FLASH (rx)     : ORIGIN = 0x27000, LENGTH = 0xA8000 - 0x27000

  /* SRAM required by Softdevice depend on
   * - Attribute Table Size (Number of Services and Characteristics)
   * - Vendor UUID count
   * - Max ATT MTU
   * - Concurrent connection peripheral + central + secure links
   * - Event Len, HVN queue, Write CMD queue
   */ 
  RAM (rwx) :  ORIGIN = 0x20006000, LENGTH = 0x20040000 - 0x20006000
}

SECTIONS
{
  . = ALIGN(4);
  .svc_data :
  {
    PROVIDE(__start_svc_data = .);
    KEEP(*(.svc_data))
    PROVIDE(__stop_svc_data = .);
  } > RAM
  
  .fs_data :
  {
    PROVIDE(__start_fs_data = .);
    KEEP(*(.fs_data))
    PROVIDE(__stop_fs_data = .);
  } > RAM
} INSERT AFTER .data;

INCLUDE "nrf52_common.ld"

