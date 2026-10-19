#include <Library/BaseLib.h>
#include <Library/PlatformMemoryMapLib.h>

/*
 * Samsung Galaxy A14 4G (SM-A145F), Exynos 850 / s5e3830.
 *
 * Derived from the device tree the stock bootloader hands to the kernel
 * (uniLoader: live-dtb/stock_live.dtb). Only RAM that is known to be free is
 * handed to UEFI; everything TrustZone or the bootloader owns is left out:
 *
 *   0x8fe00000 +1M   ramoops            0x8ffff000 +4K   kaslr seed
 *   0x90000000 +300K ect_binary         0x91200000 +2M   sec_debug_next
 *   0xbba00000-0xbfffffff               TrustZone protected (not in DTB RAM)
 *   0xbff80000 +512K el3mon            0xc3000000 +512K seclog_mem
 *   0xd0000000 +136M cp_rmem (modem)   0xdb900000 +4M    camera_uncached
 *   0xdc000000 +64M  reserved (seen in /proc/iomem on SM-A145F A15 firmware)
 *
 * The rest of the 4 GB is listed below as well. The live DTB has seven RAM
 * nodes (4013 MiB): 0x80000000 (954M), 0xc0000000 (512M), 0xe0d00000 (499M)
 * and 2 GiB ABOVE 4 GiB at 0x880000000. Above 0xe0d00000 only ranges that
 * /proc/iomem does not list as reserved are handed out (rounded inwards to
 * 1 MiB, pieces < 4 MiB dropped). The reserved blocks there (camera pools at
 * 0xe1900000-0xe3c9ffff, gnss at 0xee000000, debug snapshot at 0xf0000000,
 * log_bcm 0xfc000000, debug_kinfo 0xfcfff000, 0xff000000-0xffffffff) stay out.
 * In the 2 GiB bank the only fixed carve-out is mfc_fw_rmem (0x8a2400000, 4M);
 * the other "reserved" entries there are Android kernel allocations.
 */
static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {
/*                                                    EFI_RESOURCE_ EFI_RESOURCE_ATTRIBUTE_ EFI_MEMORY_TYPE ARM_REGION_ATTRIBUTE_
     MemLabel(32 Char.),  MemBase,    MemSize, BuildHob, ResourceType, ResourceAttribute, MemoryType, CacheAttributes
--------------------- Register ---------------------*/
    {"Periphs",           0x00000000, 0x20000000,  AddMem, MEM_RES, UNCACHEABLE,  RtCode,   NS_DEVICE},

//--------------------- DDR --------------------- */

    /* 0x80000000-0x80c00000: uniLoader payload entry (0x80200000), DTB, ramdisk */
    {"HLOS 0",            0x80000000, 0x00C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN},
    {"UEFI Stack",        0x80C00000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP,  BsData, WRITE_BACK},
    {"CPU Vectors",       0x80C40000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP,  BsCode, WRITE_BACK},
    /* ends at 0x8fe00000 (ramoops) */
    {"HLOS 0 Split",      0x80C50000, 0x0F1B0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN},
    /* 0x8fe00000-0x91400000 reserved: ramoops, kaslr, ect_binary, sec_debug_next */
    {"HLOS 0 Split 2",    0x91400000, 0x0EC00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN},
    /* uniLoader image incl. the DTB it passes in x0 (0xa070e000): UEFI only stores the pointer and reads
       the tree later, so this must never be handed out as free memory */
    {"uniLoader + DTB",   0xA0000000, 0x01000000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
    {"UEFI FD",           0xA1000000, 0x00700000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},
    /* up to 0xbb9fffff, the end of the first RAM bank in the live DTB */
    {"HLOS 0 Split 3",    0xA1700000, 0x1A300000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    {"HLOS 1",            0xC0000000, 0x03000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    {"HLOS 1 Split",      0xC3080000, 0x0CF80000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    {"HLOS 2",            0xD8800000, 0x03100000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    {"HLOS 2 Split",      0xDBD00000, 0x00300000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    /* 0xdc000000-0xdfffffff reserved (abox pools); 0xe0000000-0xe0cfffff is not RAM */
    {"HLOS 3",           0xE0D00000, 0x00C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    /* 0xe1900000-0xe3cfffff camera pools reserved */
    {"HLOS 3 Split",     0xE3D00000, 0x04200000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    {"HLOS 3 Split 2",   0xE8000000, 0x04400000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    /* 0xec400000-0xee5fffff reserved (gnss etc.) */
    {"HLOS 3 Split 3",   0xEE600000, 0x00A00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    /* 0xef000000-0xefbfffff and 0xeff00000-0xf08fffff reserved (itmon, debug snapshot) */
    {"HLOS 3 Split 4",   0xF0900000, 0x09700000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    /* bootloader framebuffer 13 MiB; 20 MiB mapped because PlatformInitialize clears 0x1400000 bytes */
    {"Display Reserved",  0xFA000000, 0x01400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
    {"HLOS 4",           0xFB400000, 0x00C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    /* 0xfc000000-0xfc0fffff log_bcm, 0xfcfff000 debug_kinfo reserved */
    {"HLOS 4 Split",     0xFC100000, 0x00E00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    /* 0xff000000-0xffffffff reserved */
    {"HLOS 4 Split 2",   0xFD000000, 0x02000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},

    /* 2 GiB of RAM above 4 GiB (live DTB: memory@880000000..memory@8e0000000, 4 x 512 MiB).
       Needs a >= 36-bit physical address space; the MMU table is built from this list. */
    {"HLOS 5",           0x880000000ULL, 0x22400000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
    /* 0x8a2400000-0x8a27fffff mfc_fw_rmem (fixed reserved-memory node) */
    {"HLOS 5 Split",     0x8A2800000ULL, 0x5D800000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},

//------------------- Terminator for MMU ---------------------
{"Terminator", 0, 0, 0, 0, 0, 0, 0}};

ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap()
{
  return gDeviceMemoryDescriptorEx;
}
