///                MentOS, The Mentoring Operating system project
/// @file ata.c
/// @brief
/// @copyright (c) 2014-2021 This file is distributed under the MIT License.
/// See LICENSE.md for details.
///! @cond Doxygen_Suppress

#include "drivers/ata.h"
#include "klib/spinlock.h"
#include "fcntl.h"
#include "mem/vmem_map.h"
#include "klib/list.h"
#include "stdio.h"
#include "descriptor_tables/isr.h"
#include "fs/vfs.h"
#include "devices/pci.h"
#include "misc/debug.h"
#include "mem/kheap.h"
#include "assert.h"
#include "string.h"
#include "kernel.h"
#include "hardware/pic8259.h"
#include "io/port_io.h"
#include "time.h"

// #define COMPLETE_SCHEDULER

static char ata_drive_char   = 'a';
static int cdrom_number      = 0;
static uint32_t ata_pci      = 0x00000000;
static int atapi_in_progress = 0;
static list_t *atapi_waiter  = NULL;

typedef union atapi_command_t {
    uint8_t command_bytes[12];
    uint16_t command_words[6];
} atapi_command_t;

/// @brief Physical Region Descriptor Tables (PRDT).
typedef struct prdt_t {
    uintptr_t offset;
    uint16_t bytes;
    uint16_t last;
} prdt_t;

/// @brief Stores information about an ATA device.
typedef struct ata_device_t {
    char name[256];
    int io_base;
    int control;
    int slave;
    int is_atapi;
    ata_identify_t identity;
    /// Physical Region Descriptor Table (PRDT).
    prdt_t *dma_prdt;
    /// Stores the physical address of the current PRDT in the Bus Master Register,
    ///  of the Bus Mastering ATA Disk Controller on the PCI bus.
    uintptr_t dma_prdt_phys;
    uint8_t *dma_start;
    uintptr_t dma_start_phys;
    uint32_t bar4;
    uint32_t atapi_lba;
    uint32_t atapi_sector_size;
    /// Device root file.
    vfs_file_t *fs_root;
} ata_device_t;

ata_device_t ata_primary_master   = { .io_base = 0x1F0, .control = 0x3F6, .slave = 0 };
ata_device_t ata_primary_slave    = { .io_base = 0x1F0, .control = 0x3F6, .slave = 1 };
ata_device_t ata_secondary_master = { .io_base = 0x170, .control = 0x376, .slave = 0 };
ata_device_t ata_secondary_slave  = { .io_base = 0x170, .control = 0x376, .slave = 1 };

spinlock_t ata_lock;

// TODO: support other sector sizes.
#define ATA_SECTOR_SIZE 512

/// @brief Waits for the
/// @param dev
void ata_io_wait(ata_device_t *dev)
{
    inportb(dev->io_base + ATA_REG_ALTSTATUS);
    inportb(dev->io_base + ATA_REG_ALTSTATUS);
    inportb(dev->io_base + ATA_REG_ALTSTATUS);
    inportb(dev->io_base + ATA_REG_ALTSTATUS);
    inportb(dev->io_base + ATA_REG_ALTSTATUS);
}

static void ata_device_read_sector(ata_device_t *, uint64_t, uint8_t *);
static void ata_device_read_sector_atapi(ata_device_t *, uint64_t, uint8_t *);
static void ata_device_write_sector_retry(ata_device_t *, uint64_t, uint8_t *);

static vfs_file_t *ata_open(const char *, int, mode_t);
static int ata_close(vfs_file_t *);
static ssize_t ata_read(vfs_file_t *, char *, off_t, size_t);
static ssize_t atapi_read(vfs_file_t *, char *, off_t, size_t);
static ssize_t ata_write(vfs_file_t *, const void *, off_t, size_t);
static int ata_fstat(vfs_file_t *file, stat_t *stat);
static int ata_stat(const char *path, stat_t *stat);

// == SUPPORT FUNCTIONS =======================================================
static uint8_t ata_status_wait(ata_device_t *dev, int timeout)
{
    uint8_t status;
    if (timeout > 0) {
        for (int i = 0; (status = inportb(dev->io_base + ATA_REG_STATUS)) & ATA_STAT_BUSY && (i < timeout); ++i) {}
    } else {
        while ((status = inportb(dev->io_base + ATA_REG_STATUS)) & ATA_STAT_BUSY) {}
    }
    return status;
}
static int ata_wait(ata_device_t *dev, bool_t advanced)
{
    ata_io_wait(dev);
    ata_status_wait(dev, 0);
    if (advanced) {
        uint8_t status = inportb(dev->io_base + ATA_REG_STATUS);
        if (status & ATA_STAT_ERR)
            return 1;
        if (status & ATA_STAT_FAULT)
            return 1;
        if (!(status & ATA_STAT_DRQ))
            return 1;
    }
    return 0;
}
static void ata_device_select(ata_device_t *dev)
{
    outportb(dev->io_base + 1, 1);
    outportb(dev->control, 0);
    outportb(dev->io_base + ATA_REG_HDDEVSEL, 0xA0 | dev->slave << 4);
    ata_io_wait(dev);
}
static uint64_t ata_max_offset(ata_device_t *dev)
{
    uint64_t sectors = dev->identity.sectors_48;
    if (!sectors) {
        // Fall back to sectors_28.
        sectors = dev->identity.sectors_28;
    }

    return sectors * ATA_SECTOR_SIZE;
}
static uint64_t atapi_max_offset(ata_device_t *dev)
{
    uint64_t max_sector = dev->atapi_lba;
    if (!max_sector) {
        return 0;
    }
    return (max_sector + 1) * dev->atapi_sector_size;
}
static int buffer_compare(uint32_t *ptr1, uint32_t *ptr2, size_t size)
{
    assert(!(size % 4));

    size_t i = 0;

    while (i < size) {
        if (*ptr1 != *ptr2) {
            return 1;
        }

        ptr1++;
        ptr2++;
        i += sizeof(uint32_t);
    }
    return 0;
}

// == VFS ENTRY GENERATION ====================================================
/// Filesystem general operations.
static vfs_sys_operations_t ata_sys_operations = {
    .mkdir_f = NULL,
    .rmdir_f = NULL,
    .stat_f  = ata_stat
};

/// ATA filesystem file operations.
static vfs_file_operations_t ata_fs_operations = {
    .open_f     = ata_open,
    .unlink_f   = NULL,
    .close_f    = ata_close,
    .read_f     = ata_read,
    .write_f    = ata_write,
    .lseek_f    = NULL,
    .stat_f     = ata_fstat,
    .ioctl_f    = NULL,
    .getdents_f = NULL
};

/// ATAPI filesystem file operations.
static vfs_file_operations_t atapi_fs_operations = {
    .open_f     = ata_open,
    .unlink_f   = NULL,
    .close_f    = ata_close,
    .read_f     = atapi_read,
    .write_f    = NULL,
    .lseek_f    = NULL,
    .stat_f     = ata_fstat,
    .ioctl_f    = NULL,
    .getdents_f = NULL
};

static vfs_file_t *atapi_device_create(ata_device_t *device)
{
    char path[PATH_MAX];
    sprintf(path, "/dev/%s", device->name);
    // Create the file.
    vfs_file_t *file = vfs_open(path, O_RDONLY | O_CREAT, 0);
    // Set the device.
    file->device = device;
    // Set the length
    file->length = atapi_max_offset(device);
    // Re-set the flags.
    file->flags = DT_BLK;
    // Change the operations.
    file->sys_operations = &ata_sys_operations;
    file->fs_operations  = &atapi_fs_operations;
    return file;
}
static vfs_file_t *ata_device_create(ata_device_t *device)
{
    char path[PATH_MAX];
    sprintf(path, "/dev/%s", device->name);
    // Create the file.
    vfs_file_t *file = vfs_open(path, O_RDWR | O_CREAT, 0);
    // Set the device.
    file->device = device;
    // Set the length
    file->length = ata_max_offset(device);
    // Re-set the flags.
    file->flags = DT_BLK;
    // Change the operations.
    file->sys_operations = &ata_sys_operations;
    file->fs_operations  = &ata_fs_operations;
    return file;
}

// == VFS CALLBACKS ===========================================================
static vfs_file_t *ata_open(const char *path, int flags, mode_t mode)
{
    pr_default("ata_open(%s, %d, %d)\n", path, flags, mode);
    if (ata_primary_master.fs_root && (strcmp(path, ata_primary_master.fs_root->name) == 0)) {
        ++ata_primary_master.fs_root->count;
        return ata_primary_master.fs_root;
    }
    if (ata_primary_slave.fs_root && (strcmp(path, ata_primary_slave.fs_root->name) == 0)) {
        ++ata_primary_slave.fs_root->count;
        return ata_primary_slave.fs_root;
    }
    if (ata_secondary_master.fs_root && (strcmp(path, ata_secondary_master.fs_root->name) == 0)) {
        ++ata_secondary_master.fs_root->count;
        return ata_secondary_master.fs_root;
    }
    if (ata_secondary_slave.fs_root && (strcmp(path, ata_secondary_slave.fs_root->name) == 0)) {
        ++ata_secondary_slave.fs_root->count;
        return ata_secondary_slave.fs_root;
    }
    return NULL;
}
static int ata_close(vfs_file_t *file)
{
    pr_default("ata_close(%p)\n", file);
    if (ata_primary_master.fs_root == file) {
        --ata_primary_master.fs_root->count;
    }
    if (ata_primary_slave.fs_root == file) {
        --ata_primary_slave.fs_root->count;
    }
    if (ata_secondary_master.fs_root == file) {
        --ata_secondary_master.fs_root->count;
    }
    if (ata_secondary_slave.fs_root == file) {
        --ata_secondary_slave.fs_root->count;
    }
    return 0;
}
static ssize_t ata_read(vfs_file_t *file, char *buffer, off_t offset, size_t size)
{
    pr_default("ata_read(%p, %p, %d, %d)\n", file, buffer, offset, size);
    ata_device_t *dev = (ata_device_t *)file->device;
    assert(dev);

    unsigned int start_block = offset / ATA_SECTOR_SIZE;
    unsigned int end_block   = (offset + size - 1) / ATA_SECTOR_SIZE;

    unsigned int x_offset = 0;

    if (offset > ata_max_offset(dev)) {
        return 0;
    }

    if (offset + size > ata_max_offset(dev)) {
        unsigned int i = ata_max_offset(dev) - offset;
        size           = i;
    }

    if (offset % ATA_SECTOR_SIZE) {
        unsigned int prefix_size = (ATA_SECTOR_SIZE - (offset % ATA_SECTOR_SIZE));
        char *tmp                = kmalloc(ATA_SECTOR_SIZE);
        ata_device_read_sector(dev, start_block, (uint8_t *)tmp);
        memcpy(buffer, (void *)((uintptr_t)tmp + (offset % ATA_SECTOR_SIZE)), prefix_size);
        kfree(tmp);
        x_offset += prefix_size;
        start_block++;
    }
    if ((offset + size) % ATA_SECTOR_SIZE && start_block <= end_block) {
        unsigned int postfix_size = (offset + size) % ATA_SECTOR_SIZE;
        char *tmp                 = kmalloc(ATA_SECTOR_SIZE);
        ata_device_read_sector(dev, end_block, (uint8_t *)tmp);
        memcpy((void *)((uintptr_t)buffer + size - postfix_size), tmp, postfix_size);
        kfree(tmp);
        end_block--;
    }
    while (start_block <= end_block) {
        ata_device_read_sector(dev, start_block, (uint8_t *)((uintptr_t)buffer + x_offset));
        x_offset += ATA_SECTOR_SIZE;
        start_block++;
    }
    return size;
}
static ssize_t atapi_read(vfs_file_t *file, char *buffer, off_t offset, size_t size)
{
    pr_default("atapi_read(%p, %p, %d, %d)\n", file, buffer, offset, size);
    ata_device_t *dev        = (ata_device_t *)file->device;
    unsigned int start_block = offset / dev->atapi_sector_size;
    unsigned int end_block   = (offset + size - 1) / dev->atapi_sector_size;
    unsigned int x_offset    = 0;
    if (offset > atapi_max_offset(dev)) {
        return 0;
    }
    if (offset + size > atapi_max_offset(dev)) {
        unsigned int i = atapi_max_offset(dev) - offset;
        size           = i;
    }
    if (offset % dev->atapi_sector_size) {
        unsigned int prefix_size = (dev->atapi_sector_size - (offset % dev->atapi_sector_size));
        char *tmp                = kmalloc(dev->atapi_sector_size);
        ata_device_read_sector_atapi(dev, start_block, (uint8_t *)tmp);
        memcpy(buffer, (void *)((uintptr_t)tmp + (offset % dev->atapi_sector_size)), prefix_size);
        kfree(tmp);
        x_offset += prefix_size;
        start_block++;
    }
    if ((offset + size) % dev->atapi_sector_size && start_block <= end_block) {
        unsigned int postfix_size = (offset + size) % dev->atapi_sector_size;
        char *tmp                 = kmalloc(dev->atapi_sector_size);
        ata_device_read_sector_atapi(dev, end_block, (uint8_t *)tmp);
        memcpy((void *)((uintptr_t)buffer + size - postfix_size), tmp, postfix_size);
        kfree(tmp);
        end_block--;
    }
    while (start_block <= end_block) {
        ata_device_read_sector_atapi(dev, start_block, (uint8_t *)((uintptr_t)buffer + x_offset));
        x_offset += dev->atapi_sector_size;
        start_block++;
    }
    return size;
}
static ssize_t ata_write(vfs_file_t *file, const void *buffer, off_t offset, size_t size)
{
    ata_device_t *dev        = (ata_device_t *)file->device;
    unsigned int start_block = offset / ATA_SECTOR_SIZE;
    unsigned int end_block   = (offset + size - 1) / ATA_SECTOR_SIZE;
    unsigned int x_offset    = 0;
    if (offset > ata_max_offset(dev)) {
        return 0;
    }
    if (offset + size > ata_max_offset(dev)) {
        unsigned int i = ata_max_offset(dev) - offset;
        size           = i;
    }
    if (offset % ATA_SECTOR_SIZE) {
        unsigned int prefix_size = (ATA_SECTOR_SIZE - (offset % ATA_SECTOR_SIZE));
        char *tmp                = kmalloc(ATA_SECTOR_SIZE);
        ata_device_read_sector(dev, start_block, (uint8_t *)tmp);
        pr_default("Writing first block");
        memcpy((void *)((uintptr_t)tmp + (offset % ATA_SECTOR_SIZE)), buffer, prefix_size);
        ata_device_write_sector_retry(dev, start_block, (uint8_t *)tmp);
        kfree(tmp);
        x_offset += prefix_size;
        start_block++;
    }
    if ((offset + size) % ATA_SECTOR_SIZE && start_block <= end_block) {
        unsigned int postfix_size = (offset + size) % ATA_SECTOR_SIZE;
        char *tmp                 = kmalloc(ATA_SECTOR_SIZE);
        ata_device_read_sector(dev, end_block, (uint8_t *)tmp);
        pr_default("Writing last block");
        memcpy(tmp, (void *)((uintptr_t)buffer + size - postfix_size), postfix_size);
        ata_device_write_sector_retry(dev, end_block, (uint8_t *)tmp);
        kfree(tmp);
        end_block--;
    }
    while (start_block <= end_block) {
        ata_device_write_sector_retry(dev, start_block, (uint8_t *)((uintptr_t)buffer + x_offset));
        x_offset += ATA_SECTOR_SIZE;
        start_block++;
    }
    return size;
}
static int _ata_stat(const ata_device_t *device, stat_t *stat)
{
    if (device) {
        stat->st_dev   = 0;
        stat->st_ino   = 0;
        stat->st_mode  = 0;
        stat->st_uid   = 0;
        stat->st_gid   = 0;
        stat->st_atime = sys_time(NULL);
        stat->st_mtime = sys_time(NULL);
        stat->st_ctime = sys_time(NULL);
        stat->st_size  = 0;
    }
    return 0;
}

/// @brief      Retrieves information concerning the file at the given position.
/// @param fid  The file struct.
/// @param stat The structure where the information are stored.
/// @return     0 if success.
static int ata_fstat(vfs_file_t *file, stat_t *stat)
{
    return _ata_stat(file->device, stat);
}

/// @brief      Retrieves information concerning the file at the given position.
/// @param path The path where the file resides.
/// @param stat The structure where the information are stored.
/// @return     0 if success.
static int ata_stat(const char *path, stat_t *stat)
{
    super_block_t *sb = vfs_get_superblock(path);
    if (sb && sb->root) {
        return _ata_stat(sb->root->device, stat);
    }
    return -1;
}

// == ATA DEVICE MANAGEMENT ===================================================
static bool_t ata_device_init(ata_device_t *dev)
{
    pr_default("Detected IDE device on bus 0x%3x\n", dev->io_base);
    pr_default("Device name: %s\n", dev->name);
    ata_device_select(dev);
    outportb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_IDENT);
    ata_io_wait(dev);
    uint8_t status = inportb(dev->io_base + ATA_REG_COMMAND);
    pr_default("Device status: %d\n", status);

    ata_wait(dev, false);

    uint16_t *buf = (uint16_t *)&dev->identity;
    for (int i = 0; i < 256; ++i) {
        buf[i] = inports(dev->io_base);
    }

    uint8_t *ptr = (uint8_t *)&dev->identity.model;
    for (int i = 0; i < 39; i += 2) {
        char tmp   = ptr[i + 1];
        ptr[i + 1] = ptr[i];
        ptr[i]     = tmp;
    }
    ptr[39] = 0;

    pr_default("Device Model: %s\n", dev->identity.model);
    pr_default("Sectors (48): %d\n", (uint32_t)dev->identity.sectors_48);
    pr_default("Sectors (24): %d\n", dev->identity.sectors_28);

    pr_default("Setting up DMA...\n");
    // dev->dma_prdt = kmalloc_p(sizeof(prdt_t) * 1, &dev->dma_prdt_phys);
    // dev->dma_start = kmalloc_p(4096, &dev->dma_start_phys);
    // TODO: Check correctness.
    {
        uint32_t order     = find_nearest_order_greater(0, sizeof(prdt_t));
        page_t *page       = _alloc_pages(GFP_KERNEL, order);
        dev->dma_prdt      = (void *)get_lowmem_address_from_page(page);
        dev->dma_prdt_phys = get_physical_address_from_page(page);
    }
    {
        uint32_t order      = find_nearest_order_greater(0, 4096);
        page_t *page        = _alloc_pages(GFP_KERNEL, order);
        dev->dma_start      = (void *)get_lowmem_address_from_page(page);
        dev->dma_start_phys = get_physical_address_from_page(page);
    }
    pr_default("Putting prdt    at 0x%x (0x%x phys)\n", dev->dma_prdt, dev->dma_prdt_phys);
    pr_default("Putting prdt[0] at 0x%x (0x%x phys)\n", dev->dma_start, dev->dma_start_phys);

    dev->dma_prdt[0].offset = dev->dma_start_phys;
    dev->dma_prdt[0].bytes  = 512;
    dev->dma_prdt[0].last   = 0x8000;

    pr_default("ATA PCI device ID: 0x%x\n", ata_pci);

    uint16_t command_reg = pci_read_field(ata_pci, PCI_COMMAND, 4);
    pr_default("COMMAND register before: 0x%4x\n", command_reg);
    if (command_reg & (1U << 2U)) {
        pr_default("Bus mastering already enabled.\n");
    } else {
        // bit 2.
        command_reg |= (1U << 2U);
        pr_default("Enabling bus mastering...\n");
        pci_write_field(ata_pci, PCI_COMMAND, 4, command_reg);
        command_reg = pci_read_field(ata_pci, PCI_COMMAND, 4);
        pr_default("COMMAND register after: 0x%4x\n", command_reg);
    }

    dev->bar4 = pci_read_field(ata_pci, PCI_BASE_ADDRESS_4, 4);
    pr_default("BAR4: 0x%x\n", dev->bar4);

    if (dev->bar4 & 0x00000001U) {
        dev->bar4 = dev->bar4 & 0xFFFFFFFC;
    } else {
        pr_default("? ATA bus master registers are 'usually' I/O ports.\n");
        // No DMA because we're not sure what to do here-
        return 1;
    }
    pci_write_field(ata_pci, PCI_INTERRUPT_LINE, 1, 0xFE);
    if (pci_read_field(ata_pci, PCI_INTERRUPT_LINE, 1) == 0xFE) {
        // Needs assignment.
        pci_write_field(ata_pci, PCI_INTERRUPT_LINE, 1, 14);
    }
    return 0;
}
static int atapi_device_init(ata_device_t *dev)
{
    pr_default("Detected ATAPI device at io-base 0x%3x, ctrl 0x%3x, slave %d\n",
               dev->io_base, dev->control, dev->slave);
    pr_default("Device name: %s\n", dev->name);
    ata_device_select(dev);
    outportb(dev->io_base + ATA_REG_COMMAND, ATAPI_CMD_ID_PCKT);
    ata_io_wait(dev);
    uint8_t status = inportb(dev->io_base + ATA_REG_COMMAND);
    pr_default("Device status: %d\n", status);

    ata_wait(dev, false);

    uint16_t *buf = (uint16_t *)&dev->identity;

    for (int i = 0; i < 256; ++i) {
        buf[i] = inports(dev->io_base);
    }

    uint8_t *ptr = (uint8_t *)&dev->identity.model;
    for (int i = 0; i < 39; i += 2) {
        char tmp   = ptr[i + 1];
        ptr[i + 1] = ptr[i];
        ptr[i]     = tmp;
    }
    ptr[39] = 0;

    pr_default("Device Model: %s\n", dev->identity.model);

    // Detect medium.
    atapi_command_t command;
    command.command_bytes[0] = 0x25;
    command.command_bytes[1] = 0;
    command.command_bytes[2] = 0;
    command.command_bytes[3] = 0;
    command.command_bytes[4] = 0;
    command.command_bytes[5] = 0;
    command.command_bytes[6] = 0;
    command.command_bytes[7] = 0;
    // Bit 0 = PMI (0, last sector).
    command.command_bytes[8] = 0;
    // Control.
    command.command_bytes[9]  = 0;
    command.command_bytes[10] = 0;
    command.command_bytes[11] = 0;

    uint16_t bus = dev->io_base;

    outportb(bus + ATA_REG_FEATURES, 0x00);
    outportb(bus + ATA_REG_LBA1, 0x08);
    outportb(bus + ATA_REG_LBA2, 0x08);
    outportb(bus + ATA_REG_COMMAND, ATAPI_CMD_PACKET);

    // Poll.
    while (1) {
        status = inportb(dev->io_base + ATA_REG_STATUS);
        if ((status & ATA_STAT_ERR)) {
            goto atapi_error;
        }
        if (!(status & ATA_STAT_BUSY) && (status & ATA_STAT_READY)) {
            break;
        }
    }

    for (int i = 0; i < 6; ++i)
        outports(bus, command.command_words[i]);

    // Poll.
    while (1) {
        status = inportb(dev->io_base + ATA_REG_STATUS);
        if ((status & ATA_STAT_ERR)) {
            goto atapi_error_read;
        }
        if (!(status & ATA_STAT_BUSY) && (status & ATA_STAT_READY)) {
            break;
        }
        if ((status & ATA_STAT_DRQ)) {
            break;
        }
    }

    uint16_t data[4];

    for (int i = 0; i < 4; ++i) {
        data[i] = inports(bus);
    }

#define htonl(l)                                                        \
    ((((l)&0xFF) << 24) | (((l)&0xFF00) << 8) | (((l)&0xFF0000) >> 8) | \
     (((l)&0xFF000000) >> 24))
    uint32_t lba, blocks;

    memcpy(&lba, &data[0], sizeof(uint32_t));

    lba = htonl(lba);

    memcpy(&blocks, &data[2], sizeof(uint32_t));

    blocks = htonl(blocks);

    dev->atapi_lba         = lba;
    dev->atapi_sector_size = blocks;

    if (!lba) {
        return false;
    }

    pr_default("Finished! LBA = %x; block length = %x\n", lba, blocks);
    return true;

atapi_error_read:
    pr_default("ATAPI error; no medium?\n");
    return false;

atapi_error:
    pr_default("ATAPI early error; unsure\n");
    return false;
}
static void ata_soft_reset(ata_device_t *dev)
{
    outportb(dev->control, 0x04);
    ata_io_wait(dev);
    outportb(dev->control, 0x00);
}
static int ata_device_detect(ata_device_t *dev)
{
    ata_soft_reset(dev);
    ata_io_wait(dev);
    outportb(dev->io_base + ATA_REG_HDDEVSEL, 0xA0 | dev->slave << 4);
    ata_io_wait(dev);
    ata_status_wait(dev, 10000);

    pr_default("Probing cylinder registers...\n");
    uint8_t cl = inportb(dev->io_base + ATA_REG_LBA1);
    uint8_t ch = inportb(dev->io_base + ATA_REG_LBA2);
    if ((cl == 0xFF) && (ch == 0xFF)) {
        pr_default("No drive(s) present\n");
        return 1;
    }

    pr_default("Waiting while busy...\n");
    uint8_t status = ata_status_wait(dev, 5000);
    if (status & ATA_STAT_BUSY) {
        pr_default("No drive(s) present\n");
        return 1;
    }

    pr_default("Device detected: 0x%2x 0x%2x\n", cl, ch);
    if ((cl == 0x00 && ch == 0x00) || (cl == 0x3C && ch == 0xC3)) {
        // The device is not an ATAPI.
        dev->is_atapi = false;
        // Parallel ATA device, or emulated SATA
        sprintf(dev->name, "hd%c", ata_drive_char);

        dev->fs_root = ata_device_create(dev);
        if (!dev->fs_root) {
            pr_default("Failed to create ata device!\n");
            return 1;
        }
        if (!vfs_mount(dev->fs_root->name, dev->fs_root)) {
            pr_default("Failed to mount ata device!\n");
            return 1;
        }
        if (ata_device_init(dev)) {
            pr_default("Failed to initialize ata device!\n");
            return 1;
        }
        ++ata_drive_char;
    } else if ((cl == 0x14 && ch == 0xEB) || (cl == 0x69 && ch == 0x96)) {
        // The device is an ATAPI.
        dev->is_atapi = true;
        sprintf(dev->name, "cdrom%d", cdrom_number);

        dev->fs_root = atapi_device_create(dev);
        if (!dev->fs_root) {
            pr_default("Failed to create atapi device!\n");
            return 1;
        }
        if (!vfs_mount(dev->fs_root->name, dev->fs_root)) {
            pr_default("Failed to mount atapi device!\n");
            return 1;
        }
        if (atapi_device_init(dev)) {
            pr_default("Failed to initialize atapi device!\n");
            return 1;
        }
        ++cdrom_number;
    }
    pr_default("\n");
    return 0;
}

// == ATA SECTOR READ/WRITE FUNCTIONS =========================================
static void ata_device_read_sector(ata_device_t *dev, uint32_t lba, uint8_t *buffer)
{
    pr_default("ata_device_read_sector(%p, %d, %p)\n", dev, lba, buffer);

    uint16_t bus = dev->io_base;

    uint8_t slave = dev->slave;

    if (dev->is_atapi) {
        return;
    }

    spinlock_lock(&ata_lock);

#if 1
    int errors = 0;
try_again:
#endif

    pr_default("ata_wait\n");
    ata_wait(dev, false);

    // Stop.
    outportb(dev->bar4, 0x00);

    pr_default("Set the PRDT.\n");
    // Set the PRDT.
    outportl(dev->bar4 + 0x04, dev->dma_prdt_phys);

    pr_default("Enable error, irq status.\n");
    // Enable error, irq status.
    outportb(dev->bar4 + 0x2, inportb(dev->bar4 + 0x02) | 0x04 | 0x02);

    // Set read.
    outportb(dev->bar4, 0x08);

    pr_default("irq_enable...\n");
    //sti();

    while (1) {
        pr_default("Wait busy...\n");
        uint8_t status = inportb(dev->io_base + ATA_REG_STATUS);
        if (!(status & ATA_STAT_BUSY)) {
            break;
        }
    }

    pr_default("Read.\n");
    outportb(bus + ATA_REG_CONTROL, 0x00);
    outportb(bus + ATA_REG_HDDEVSEL, 0xe0 | slave << 4 | (lba & 0x0f000000) >> 24);
    ata_io_wait(dev);
    outportb(bus + ATA_REG_FEATURES, 0x00);
    outportb(bus + ATA_REG_SECCOUNT0, 1);
    outportb(bus + ATA_REG_LBA0, (lba & 0x000000ff) >> 0);
    outportb(bus + ATA_REG_LBA1, (lba & 0x0000ff00) >> 8);
    outportb(bus + ATA_REG_LBA2, (lba & 0x00ff0000) >> 16);
    // outportb(bus + ATA_REG_COMMAND, ATA_CMD_READ);
    while (1) {
        uint8_t status = inportb(dev->io_base + ATA_REG_STATUS);
        if (!(status & ATA_STAT_BUSY) && (status & ATA_STAT_READY)) {
            break;
        }
    }
    outportb(bus + ATA_REG_COMMAND, ATA_CMD_RD_DMA);

    ata_io_wait(dev);

    outportb(dev->bar4, 0x08 | 0x01);

    while (1) {
        int status  = inportb(dev->bar4 + 0x02);
        int dstatus = inportb(dev->io_base + ATA_REG_STATUS);
        if (!(status & 0x04)) {
            continue;
        }
        if (!(dstatus & ATA_STAT_BUSY)) {
            break;
        }
    }
    //cli();

#if 1
    if (ata_wait(dev, true)) {
        pr_default("Error during ATA read of lba block %d\n", lba);
        errors++;
        if (errors > 4) {
            pr_default("-- Too many errors trying to read this block. Bailing.\n");
            spinlock_unlock(&ata_lock);
            return;
        }
        goto try_again;
    }
#endif

    pr_default("Copy from DMA buffer to output buffer.\n");
    // Copy from DMA buffer to output buffer.
    memcpy(buffer, dev->dma_start, 512);

    // Inform device we are done.
    outportb(dev->bar4 + 0x2, inportb(dev->bar4 + 0x02) | 0x04 | 0x02);

#if 0
    int size = 256;
    inportsm(bus,buf,size);
    ata_wait(dev, false);
    outportb(bus + ATA_REG_CONTROL, 0x02);
#endif
    spinlock_unlock(&ata_lock);
}
static void ata_device_read_sector_atapi(ata_device_t *dev, uint32_t lba, uint8_t *buffer)
{
    if (!dev->is_atapi) {
        return;
    }

    uint16_t bus = dev->io_base;
    spinlock_lock(&ata_lock);

    outportb(dev->io_base + ATA_REG_HDDEVSEL, 0xA0 | dev->slave << 4);
    ata_io_wait(dev);

    outportb(bus + ATA_REG_FEATURES, 0x00);
    outportb(bus + ATA_REG_LBA1, dev->atapi_sector_size & 0xFF);
    outportb(bus + ATA_REG_LBA2, dev->atapi_sector_size >> 8);
    outportb(bus + ATA_REG_COMMAND, ATAPI_CMD_PACKET);

    // Poll.
    while (1) {
        uint8_t status = inportb(dev->io_base + ATA_REG_STATUS);
        if ((status & ATA_STAT_ERR)) {
            goto atapi_error_on_read_setup;
        }
        if (!(status & ATA_STAT_BUSY) && (status & ATA_STAT_DRQ)) {
            break;
        }
    }

    atapi_in_progress = true;

    atapi_command_t command;
    command.command_bytes[0] = 0xA8;
    command.command_bytes[1] = 0;
    command.command_bytes[2] = (lba >> 0x18) & 0xFF;
    command.command_bytes[3] = (lba >> 0x10) & 0xFF;
    command.command_bytes[4] = (lba >> 0x08) & 0xFF;
    command.command_bytes[5] = (lba >> 0x00) & 0xFF;
    command.command_bytes[6] = 0;
    command.command_bytes[7] = 0;
    // Bit 0 = PMI (0, last sector).
    command.command_bytes[8] = 0;
    // Control.
    command.command_bytes[9]  = 1;
    command.command_bytes[10] = 0;
    command.command_bytes[11] = 0;

    for (int i = 0; i < 6; ++i) {
        outports(bus, command.command_words[i]);
    }

    // Wait.
#ifdef COMPLETE_SCHEDULER
    sleep_on(atapi_waiter);
#endif

    atapi_in_progress = false;

    while (1) {
        uint8_t status = inportb(dev->io_base + ATA_REG_STATUS);
        if ((status & ATA_STAT_ERR)) {
            goto atapi_error_on_read_setup;
        }
        if (!(status & ATA_STAT_BUSY) && (status & ATA_STAT_DRQ)) {
            break;
        }
    }

    uint16_t size_to_read = inportb(bus + ATA_REG_LBA2) << 8;
    size_to_read          = size_to_read | inportb(bus + ATA_REG_LBA1);

    inportsm(bus, buffer, size_to_read / 2);

    while (1) {
        uint8_t status = inportb(dev->io_base + ATA_REG_STATUS);
        if ((status & ATA_STAT_ERR)) {
            goto atapi_error_on_read_setup;
        }
        if (!(status & ATA_STAT_BUSY) && (status & ATA_STAT_READY)) {
            break;
        }
    }

atapi_error_on_read_setup:;
    spinlock_unlock(&ata_lock);
}
static void ata_device_write_sector(ata_device_t *dev, uint32_t lba, uint8_t *buffer)
{
    uint16_t bus  = dev->io_base;
    uint8_t slave = dev->slave;

    spinlock_lock(&ata_lock);

    outportb(bus + ATA_REG_CONTROL, 0x02);

    ata_wait(dev, false);
    outportb(bus + ATA_REG_HDDEVSEL, 0xe0 | slave << 4 | (lba & 0x0f000000) >> 24);
    ata_wait(dev, false);

    outportb(bus + ATA_REG_FEATURES, 0x00);
    outportb(bus + ATA_REG_SECCOUNT0, 0x01);
    outportb(bus + ATA_REG_LBA0, (lba & 0x000000ff) >> 0);
    outportb(bus + ATA_REG_LBA1, (lba & 0x0000ff00) >> 8);
    outportb(bus + ATA_REG_LBA2, (lba & 0x00ff0000) >> 16);
    outportb(bus + ATA_REG_COMMAND, ATA_CMD_WRITE);
    ata_wait(dev, false);
    int size = ATA_SECTOR_SIZE / 2;
    outportsm(bus, buffer, size);
    outportb(bus + 0x07, ATA_CMD_CH_FLSH);
    ata_wait(dev, false);
    spinlock_unlock(&ata_lock);
}
static void ata_device_write_sector_retry(ata_device_t *dev, uint32_t lba, uint8_t *buffer)
{
    uint8_t *read_buf = kmalloc(ATA_SECTOR_SIZE);
    do {
        ata_device_write_sector(dev, lba, buffer);
        ata_device_read_sector(dev, lba, read_buf);
    } while (
        buffer_compare((uint32_t *)buffer, (uint32_t *)read_buf, ATA_SECTOR_SIZE));
    kfree(read_buf);
}

// == IRQ HANDLERS ============================================================
/// @param f The interrupt stack frame.
static void ata_irq_handler_master(pt_regs *f)
{
    inportb(ata_primary_master.io_base + ATA_REG_STATUS);

    if (atapi_in_progress) {
        // wakeup_queue(atapi_waiter);
    }

    // irq_ack(14);
    pic8259_send_eoi(14);
}

/// @param f The interrupt stack frame.
static void ata_irq_handler_slave(pt_regs *f)
{
    inportb(ata_secondary_master.io_base + ATA_REG_STATUS);

    if (atapi_in_progress) {
        // wakeup_queue(atapi_waiter);
    }

    // irq_ack(15);
    pic8259_send_eoi(15);
}

// == PCI FUNCTIONS ===========================================================
static void pci_find_ata(uint32_t dev, uint16_t vid, uint16_t did, void *extra)
{
    if ((vid == 0x8086) && (did == 0x7010 || did == 0x7111)) {
        *((uint32_t *)extra) = dev;
    }
}

// == INITIALIZE/FINALIZE ATA =================================================
int ata_initialize()
{
    spinlock_init(&ata_lock);

    // Detect drives and mount them.
    // Locate ATA device via PCI.
    pci_scan(&pci_find_ata, -1, &ata_pci);

    //irq_install_handler(14, ata_irq_handler_master, "ide master");
    //irq_install_handler(15, ata_irq_handler_slave, "ide slave");

    //	atapi_waiter = list_create();

    pr_default("Detecteing devices...\n");
    pr_default("Detecteing Primary Master...\n");
    ata_device_detect(&ata_primary_master);
    pr_default("\n");
    pr_default("Detecteing Primary Slave...\n");
    ata_device_detect(&ata_primary_slave);
    pr_default("\n");
    pr_default("Detecteing Secondary Master...\n");
    ata_device_detect(&ata_secondary_master);
    pr_default("\n");
    pr_default("Detecteing Secondary Slave...\n");
    ata_device_detect(&ata_secondary_slave);
    pr_default("\n");
    pr_default("Done\n");

    return 0;
}

int ata_finalize()
{
    return 0;
}

///! @endcond