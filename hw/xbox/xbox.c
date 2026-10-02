/*
 * QEMU Xbox System Emulator
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/option.h"
#include "qemu/datadir.h"
#include "hw/hw.h"
#include "hw/loader.h"
#include "hw/i386/pc.h"
#include "hw/i386/kvm/clock.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/usb.h"
#include "net/net.h"
#include "hw/boards.h"
#include "hw/ide/pci.h"
#include "ui/xemu-settings.h"
#include "system/system.h"
#include "system/kvm.h"
#include "kvm/kvm_i386.h"
#include "hw/dma/i8257.h"

#include "hw/sysbus.h"
#include "system/arch_init.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "cpu.h"

#include "qapi/error.h"
#include "qemu/error-report.h"

#include "hw/timer/i8254.h"
#include "hw/audio/pcspk.h"
#include "hw/rtc/mc146818rtc.h"

#include "hw/xbox/xbox_pci.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/smbus_eeprom.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/mcpx/apu/apu.h"

#include "hw/xbox/xbox.h"
#include "smbus.h"
#include "chihiro/chihiro.h"
#include "chihiro/chihiro-fatx-builder.h"
#include "chihiro/chihiro-firmware.h"

#define MAX_IDE_BUS 2

/* FIXME: Clean this up and propagate errors to UI */
static void xbox_flash_init(MachineState *ms, MemoryRegion *rom_memory)
{
    const uint32_t rom_start = 0xFF000000;
    const char *bios_name;

    /* Locate BIOS ROM image */
    bios_name = ms->firmware ?: "bios.bin";

    int failed_to_load_bios = 1;
    char *filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    uint32_t bios_size = 256 * 1024;

    if (filename != NULL) {
        int bios_file_size = get_image_size(filename, NULL);
        if ((bios_file_size > 0) && ((bios_file_size % 65536) == 0)) {
            failed_to_load_bios = 0;
            bios_size = bios_file_size;
        }
    }

    char *bios_data = g_malloc(bios_size);
    assert(bios_data != NULL);

    if (!failed_to_load_bios && (filename != NULL)) {
        /* Read BIOS ROM into memory */
        failed_to_load_bios = 1;
        int fd = qemu_open(filename, O_RDONLY | O_BINARY, NULL);
        if (fd >= 0) {
            int rc = read(fd, bios_data, bios_size);
            if (rc == bios_size) {
                failed_to_load_bios = 0;
            }
            close(fd);
        }
    }

    if (failed_to_load_bios) {
        fprintf(stderr, "Failed to load BIOS '%s'\n", filename ? filename : "(null)");
        memset(bios_data, 0xff, bios_size);
    }
    if (filename != NULL) {
        g_free(filename);
    }

    /* Create BIOS region */
    MemoryRegion *bios;
    bios = g_malloc(sizeof(*bios));
    assert(bios != NULL);
    memory_region_init_rom(bios, NULL, "xbox.bios", bios_size, &error_fatal);
    rom_add_blob_fixed("xbox.bios", bios_data, bios_size, rom_start);

    /* Mirror ROM from 0xff000000 - 0xffffffff */
    uint32_t map_loc;
    for (map_loc = rom_start; map_loc >= rom_start; map_loc += bios_size) {
        MemoryRegion *map_bios = g_malloc(sizeof(*map_bios));
        memory_region_init_alias(map_bios, NULL, "pci-bios", bios, 0,
                                 bios_size);
        memory_region_add_subregion(rom_memory, map_loc, map_bios);
        memory_region_set_readonly(map_bios, true);
    }

    /* Create MCPX Boot ROM memory region
     *
     * For performance reasons, the overlay region should be page-aligned.
     * To do this, we simply make the memory region size equal to the size
     * of the BIOS image, and then overlay the boot ROM contents on top.
     *
     * Additionally, retail 1.1+ kernels have a quirk in very early boot stage
     * that depends on physical CPU WB caching behavior to briefly store a
     * computed value to a location in ROM and read it back in the next
     * instruction. Because we cannot emulate this cache behavior accurately,
     * work around this quirk by making this MCPX ROM region writable. When the
     * ROM is disabled during boot, any apparent writes to the region will be
     * discarded.
     *
     * Offending code which writes to ROM:
     *   sub ds:0FFFFD52Ch, eax
     *   mov eax, ds:0FFFFD52Ch
     */
    const char *bootrom_file =
        object_property_get_str(qdev_get_machine(), "bootrom", NULL);

    if ((bootrom_file != NULL) && *bootrom_file) {
        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bootrom_file);
        assert(filename);

        int bootrom_size = get_image_size(filename, NULL);
        if (bootrom_size != 512) {
            fprintf(stderr, "MCPX bootrom should be 512 bytes, got %d\n",
                    bootrom_size);
            exit(1);
            return;
        }

        if (xbox_is_chihiro()) {
            /* The Chihiro BIOS carries its own MCPX-compatible boot code at
             * the end of the image, keyed differently from the retail MCPX
             * ROM. Overlaying the retail MCPX would overwrite it and 2BL
             * decryption would fail, so leave the image alone.
             *
             * This must key off the machine type, not the image size: retail
             * BIOSes are 256 KiB, 512 KiB and 1 MiB, so a size test skips the
             * overlay for ordinary Xbox BIOSes too and leaves the reset
             * vector pointing at BIOS data instead of the MCPX boot code. */
        } else {
            /* Standard Xbox BIOS: overlay retail MCPX ROM */
            int fd = qemu_open(filename, O_RDONLY | O_BINARY, NULL);
            assert(fd >= 0);
            int rc = read(fd, bios_data + bios_size - bootrom_size,
                          bootrom_size);
            assert(rc == bootrom_size);
            close(fd);
        }
        g_free(filename);
    }

    // Leave last BIOS image overlay writeable to satisfy cache dependency
    MemoryRegion *mcpx = g_malloc(sizeof(MemoryRegion));
    memory_region_init_ram(mcpx, NULL, "xbox.mcpx", bios_size, &error_fatal);
    rom_add_blob_fixed("xbox.mcpx", bios_data, bios_size, -bios_size);
    memory_region_add_subregion_overlap(rom_memory, -bios_size, mcpx, 1);

    g_free(bios_data); /* duplicated by `rom_add_blob_fixed` */
}

static void xbox_memory_init(PCMachineState *pcms,
                             MemoryRegion *system_memory,
                             MemoryRegion *rom_memory,
                             MemoryRegion **ram_memory)
{
    // int linux_boot, i;
    MemoryRegion *ram;//, *option_rom_mr;
    // FWCfgState *fw_cfg;
    MachineState *machine = MACHINE(pcms);
    // PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);

    // linux_boot = (machine->kernel_filename != NULL);

    /* Allocate RAM.  We allocate it as a single memory region and use
     * aliases to address portions of it, mostly for backwards compatibility
     * with older qemus that used qemu_ram_alloc().
     */
    ram = g_malloc(sizeof(*ram));
    memory_region_init_ram(ram, NULL, "xbox.ram",
                           machine->ram_size, &error_fatal);

    *ram_memory = ram;
    memory_region_add_subregion(system_memory, 0, ram);

    xbox_flash_init(machine, rom_memory);
    pc_system_flash_cleanup_unused(pcms);
}

/* PC hardware initialisation */
static void xbox_init(MachineState *machine)
{
    xbox_init_common(machine, NULL, NULL);
}

void xbox_init_common(MachineState *machine,
                      PCIBus **pci_bus_out,
                      ISABus **isa_bus_out)
{
    PCMachineState *pcms = PC_MACHINE(machine);
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
    X86MachineState *x86ms = X86_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    // MemoryRegion *system_io = get_system_io();

    PCIBus *pci_bus;
    ISABus *isa_bus;

    // qemu_irq *i8259;
    // qemu_irq smi_irq; // XBOX_TODO: SMM support?

    GSIState *gsi_state;

    // DriveInfo *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    // BusState *idebus[MAX_IDE_BUS];
    MC146818RtcState *rtc_state;
    ISADevice *pit = NULL;
    int pit_isa_irq = 0;
    qemu_irq pit_alt_irq = NULL;
    // ISADevice *pit;

    MemoryRegion *ram_memory;
    MemoryRegion *pci_memory;
    MemoryRegion *rom_memory;

    I2CBus *smbus;
    PCIBus *agp_bus;

    x86_cpus_init(x86ms, pcmc->default_cpu_version);

    if (kvm_enabled()) {
        kvmclock_create(pcmc->kvmclock_create_always);
    }

    pci_memory = g_new(MemoryRegion, 1);
    memory_region_init(pci_memory, NULL, "pci", UINT64_MAX);
    rom_memory = pci_memory;

    // pc_guest_info_init(pcms);

    /* allocate ram and load rom/bios */
    xbox_memory_init(pcms, system_memory, rom_memory, &ram_memory);

    gsi_state = pc_gsi_create(&x86ms->gsi, pcmc->pci_enabled);

    xbox_pci_init(x86ms->gsi,
                  get_system_memory(), get_system_io(),
                  pci_memory, ram_memory, rom_memory,
                  &pci_bus,
                  &isa_bus,
                  &smbus,
                  &agp_bus);

    pcms->pcibus = pci_bus;

    isa_bus_register_input_irqs(isa_bus, x86ms->gsi);

    pc_i8259_create(isa_bus, gsi_state->i8259_irq);

    if (tcg_enabled()) {
        x86_register_ferr_irq(x86ms->gsi[13]);
    }

    /* init basic PC hardware */
    rtc_state = mc146818_rtc_init(isa_bus, 2000, NULL);
    x86ms->rtc = ISA_DEVICE(rtc_state);

    if (kvm_pit_in_kernel()) {
        pit = kvm_pit_init(isa_bus, 0x40);
    } else {
        pit = i8254_pit_init(isa_bus, 0x40, pit_isa_irq, pit_alt_irq);
    }

    i8257_dma_init(OBJECT(machine), isa_bus, 0);

    object_property_set_link(OBJECT(pcms->pcspk), "pit",
                             OBJECT(pit), &error_fatal);
    isa_realize_and_unref(pcms->pcspk, isa_bus, &error_fatal);

    if (xbox_is_chihiro()) {
        chihiro_ide_interface_init();
    }

    PCIDevice *dev = pci_create_simple(pci_bus, PCI_DEVFN(9, 0), "piix3-ide");
    pci_ide_create_devs(dev);
    // idebus[0] = qdev_get_child_bus(&dev->qdev, "ide.0");
    // idebus[1] = qdev_get_child_bus(&dev->qdev, "ide.1");

    /* smbus devices */
    /* Chihiro: SMC default 0x00 maps to VGA in arcade kernel.
     * No avpack override needed — the kernel handles the mapping. */
    smbus_xbox_smc_init(smbus, 0x10);

    const char *video_encoder =
        object_property_get_str(qdev_get_machine(), "video-encoder", NULL);

    if (strcmp(video_encoder, "xcalibur") != 0) {
        if (!strcmp(video_encoder, "conexant")) {
            smbus_cx25871_init(smbus, 0x45);
        } else if (!strcmp(video_encoder, "focus")) {
            smbus_fs454_init(smbus, 0x6A);
        }
        smbus_adm1032_init(smbus, 0x4C);
    } else {
        smbus_xcalibur_init(smbus, 0x70);
    }

    /* USB */
    PCIDevice *usb1 = pci_new(PCI_DEVFN(3, 0), "pci-ohci");
    qdev_prop_set_uint32(&usb1->qdev, "num-ports", 4);
    pci_realize_and_unref(usb1, pci_bus, &error_fatal);

    PCIDevice *usb0 = pci_new(PCI_DEVFN(2, 0), "pci-ohci");
    qdev_prop_set_uint32(&usb0->qdev, "num-ports", 4);
    pci_realize_and_unref(usb0, pci_bus, &error_fatal);

    /* Ethernet! */
    PCIDevice *nvnet = pci_new(PCI_DEVFN(4, 0), "nvnet");
    qemu_configure_nic_device(DEVICE(nvnet), true, "nvnet");
    pci_realize_and_unref(nvnet, pci_bus, &error_fatal);

    /* APU! */
    mcpx_apu_init(pci_bus, PCI_DEVFN(5, 0), ram_memory);

    /* ACI! */
    pci_create_simple(pci_bus, PCI_DEVFN(6, 0), "mcpx-aci");

    /* GPU! */
    nv2a_init(agp_bus, PCI_DEVFN(0, 0), ram_memory);

    /* FIXME: Stub the memory controller */
    pci_create_simple(pci_bus, PCI_DEVFN(0, 3), "pci-testdev");

    if (pci_bus_out) {
        *pci_bus_out = pci_bus;
    }
    if (isa_bus_out) {
        *isa_bus_out = isa_bus;
    }

    /* A real Chihiro is an Xbox with add-on boards, so the baseboard is
     * layered on top of the machine that has just been built. The mediaboard
     * LPC I/O device supplies the XBAM identification string SEGABOOT checks
     * for. Chihiro also requires 128 MiB, which the caller has arranged. */
    if (xbox_is_chihiro()) {
        gint64 init_t0 = g_get_monotonic_time();
        printf("Chihiro: enabling the media board\n");

        chihiro_freeplay_setting = g_config.chihiro.settings.freeplay;

        /* The media board flash (SEGABOOT) and the baseboard EEPROMs: the
         * paths set for Chihiro, else the files beside the BIOS. */
        const char *bios = g_config.sys.files.flashrom_path;
        chihiro_load_flash_rom(bios);
        chihiro_load_eeproms(bios);
        if (!chihiro_flash_rom_loaded()) {
            fprintf(stderr, "Chihiro: ERROR — media board flash not found "
                    "(fpr21042_m29w160et.bin)\n");
        }
        /* The QC/SC firmware EEPROMs and the baseboard config are not
         * required to be on disk: without them the House of the Dead 3 dumps
         * built in here are used, as this port always did. */
        if (!chihiro_ic10_data) {
            chihiro_ic10_data = g_memdup2(hotd3_ic10_g24lc64,
                                          sizeof(hotd3_ic10_g24lc64));
            chihiro_ic10_size = sizeof(hotd3_ic10_g24lc64);
            printf("Chihiro: ic10 EEPROM not on disk, using the built-in dump\n");
        }
        if (!chihiro_pc20_data) {
            chihiro_pc20_data = g_memdup2(hotd3_pc20_g24lc64,
                                          sizeof(hotd3_pc20_g24lc64));
            chihiro_pc20_size = sizeof(hotd3_pc20_g24lc64);
            printf("Chihiro: pc20 EEPROM not on disk, using the built-in dump\n");
        }
        if (!chihiro_ic11_data) {
            chihiro_ic11_data = g_memdup2(hotd3_ic11_24lc024,
                                          sizeof(hotd3_ic11_24lc024));
            chihiro_ic11_size = sizeof(hotd3_ic11_24lc024);
            printf("Chihiro: ic11 EEPROM not on disk, using the built-in dump\n");
        }

        isa_create_simple(isa_bus, "chihiro-lpc");

        /* Chihiro southbridge has revision >= 0xB4. This clears bit 0 of
         * XboxHardwareInfo in the kernel, selecting PATH_B for USB topology
         * (direct port mapping instead of hub-based). Without this, the
         * kernel uses PATH_A which requires device table state=1 that LLE
         * USB entries never reach. */
        PCIDevice *lpc = pci_find_device(pci_bus, 0, PCI_DEVFN(1, 0));
        if (lpc) {
            pci_config_set_revision(lpc->config, 0xB4);
            printf("Chihiro: LPC bridge revision set to 0xB4 (PATH_B)\n");
        }

        chihiro_ide_load_rom();

        /* The game into the DIMM: a netboot FATX image is loaded as is; a
         * game folder (what the Android library hands over after unpacking
         * or copying a game) is built into a FATX filesystem in place. */
        {
            const char *dvd = xemu_chihiro_image();
            uint32_t fs_size = 0;
            uint8_t *fs_buf = chihiro_fatx_get_buffer(&fs_size);
            struct stat st;
            /* stat() cannot see an Android /dev/fdset/N path; qemu_open can */
            int img_fd = -1;
            bool have = dvd && dvd[0] && fs_buf && stat(dvd, &st) == 0;
            if (!have && dvd && dvd[0] && fs_buf) {
                img_fd = qemu_open(dvd, O_RDONLY | O_BINARY, NULL);
                have = img_fd >= 0 && fstat(img_fd, &st) == 0;
            }
            if (have) {
                if (S_ISDIR(st.st_mode)) {
                    uint32_t built = 0;
                    /* mbfs: the partition below the system area */
                    uint32_t mbfs_sectors =
                        (0x40000u << chihiro_dimm_factor()) - 0x8000u;
                    gint64 t0 = g_get_monotonic_time();
                    if (chihiro_fatx_build_into(dvd, fs_buf, fs_size,
                                                mbfs_sectors, &built)) {
                        printf("Chihiro: FATX built from '%s' (%u MB, %lld ms)\n",
                               dvd, built / (1024 * 1024),
                               (long long)(g_get_monotonic_time() - t0) / 1000);
                    }
                    snprintf(chihiro_game_dir, sizeof(chihiro_game_dir), "%s",
                             dvd);
                } else {
                    FILE *f = img_fd >= 0 ? fdopen(img_fd, "rb")
                                          : qemu_fopen(dvd, "rb");
                    if (f) {
                        img_fd = -1; /* owned by f now */
                    }
                    uint8_t magic[4];
                    if (f && fread(magic, 1, 4, f) == 4 &&
                        memcmp(magic, "FATX", 4) == 0) {
                        rewind(f);
                        gint64 t0 = g_get_monotonic_time();
                        uint32_t file_size = (uint32_t)MIN((uint64_t)st.st_size,
                                                           fs_size);
                        size_t nread = fread(fs_buf, 1, file_size, f);
                        printf("Chihiro: FATX image loaded '%s' (%zu bytes, "
                               "%lld ms)\n", dvd, nread,
                               (long long)(g_get_monotonic_time() - t0) / 1000);

                        /* The game executable, from the image's boot.id */
                        for (uint32_t off = 0;
                             off + CHIHIRO_BOOTID_LEN <= file_size; off++) {
                            char name[64];
                            if (memcmp(fs_buf + off, "BTID", 4) == 0 &&
                                chihiro_bootid_executable(fs_buf + off, name,
                                                          sizeof(name))) {
                                chihiro_set_game_executable(name);
                                break;
                            }
                        }
                    } else {
                        fprintf(stderr, "Chihiro: '%s' is not a FATX image "
                                "or a game folder\n", dvd);
                    }
                    if (f) {
                        fclose(f);
                    }
                    /* The image's folder, where a boot.id file may sit */
                    snprintf(chihiro_game_dir, sizeof(chihiro_game_dir), "%s",
                             dvd);
                    char *slash = strrchr(chihiro_game_dir, '/');
                    if (!slash) slash = strrchr(chihiro_game_dir, '\\');
                    if (slash) *slash = '\0';
                }
            } else if (dvd && dvd[0]) {
                fprintf(stderr, "Chihiro: cannot open game '%s'\n", dvd);
            }
            if (img_fd >= 0) {
                qemu_close(img_fd);
            }
        }

        /* The Chihiro BIOS jamtable writes to SMBus device 0x6A (Focus
         * FS454 video encoder) during early boot. Without this device,
         * the SMBus transaction never completes and boot hangs. */
        smbus_fs454_init(smbus, 0x6A);

        /* Chihiro baseboard USB: AN2131 QC + SC, created unattached and
         * hot-plugged after each OHCI bus start (chihiro_on_ohci_bus_start),
         * as the real chips load their firmware after power-up. */
        USBBus *usb0_bus = NULL;
        for (int i = 0; i < 4 && !usb0_bus; i++) {
            char bn[32]; snprintf(bn, sizeof(bn), "usb-bus.%d", i);
            BusState *bs = qdev_get_child_bus(DEVICE(usb0), bn);
            if (bs) usb0_bus = USB_BUS(bs);
        }
        if (usb0_bus) {
            USBDevice *qc = usb_create_simple(usb0_bus, "chihiro-an2131qc");
            USBDevice *sc = usb_create_simple(usb0_bus, "chihiro-an2131sc");
            chihiro_usb_set_devices(qc, sc);

            /* Load per-game saves (ic11 + extmem) and register exit flusher */
            chihiro_save_init();
        } else {
            printf("Chihiro: WARNING — could not find USB bus on OHCI\n");
        }
        printf("Chihiro: init complete (%lld ms)\n",
               (long long)(g_get_monotonic_time() - init_t0) / 1000);
    }
}

static char *machine_get_bootrom(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    return g_strdup(ms->bootrom);
}

static void machine_set_bootrom(Object *obj, const char *value, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    g_free(ms->bootrom);
    ms->bootrom = g_strdup(value);
}

/* CHIHIRO (not upstream): read by devices that ask the machine. On this
 * port the machine is chosen by sys.chihiro (xbox_is_chihiro), so the
 * property only reports it. */
static bool machine_get_chihiro(Object *obj, Error **errp)
{
    return xbox_is_chihiro();
}

static char *machine_get_avpack(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    return g_strdup(ms->avpack);
}

static void machine_set_avpack(Object *obj, const char *value, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    if (!xbox_smc_avpack_to_reg(value, NULL)) {
        error_setg(errp, "-machine avpack=%s: unsupported option", value);
        xbox_smc_append_avpack_hint(errp);
        return;
    }

    g_free(ms->avpack);
    ms->avpack = g_strdup(value);
}

static void machine_set_short_animation(Object *obj, bool value, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    ms->short_animation = value;
}

static bool machine_get_short_animation(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);
    return ms->short_animation;
}

static char *machine_get_smc_version(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    return g_strdup(ms->smc_version);
}

static void machine_set_smc_version(Object *obj, const char *value,
                                    Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    if (strlen(value) != 3) {
        error_setg(errp, "-machine smc-version=%s: unsupported option", value);
        xbox_smc_append_smc_version_hint(errp);
        return;
    }

    g_free(ms->smc_version);
    ms->smc_version = g_strdup(value);
}

static char *machine_get_video_encoder(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    return g_strdup(ms->video_encoder);
}

static void machine_set_video_encoder(Object *obj, const char *value,
                                      Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    if (strcmp(value, "conexant") != 0 && strcmp(value, "focus") != 0 &&
        strcmp(value, "xcalibur") != 0) {
        error_setg(errp, "-machine video_encoder=%s: unsupported option",
                   value);
        error_append_hint(
            errp, "Valid options are: conexant (default), focus, xcalibur\n");
        return;
    }

    g_free(ms->video_encoder);
    ms->video_encoder = g_strdup(value);
}

static void xbox_machine_options(MachineClass *m)
{
    ObjectClass *oc = OBJECT_CLASS(m);

    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);
    m->desc              = "Microsoft Xbox";
    m->max_cpus          = 1;
    m->option_rom_has_mr = true;
    m->rom_file_has_mr   = false;
    m->no_floppy         = 1,
    m->no_cdrom          = 1,
    m->default_cpu_type  = X86_CPU_TYPE_NAME("pentium3");
    m->is_default        = true;
    m->default_nic       = "nvnet";

    pcmc->pci_enabled         = true;
    pcmc->has_acpi_build      = false;
    pcmc->smbios_defaults     = false;
    pcmc->gigabyte_align      = false;
    pcmc->smbios_legacy_mode  = true;
    pcmc->has_reserved_memory = false;

    object_class_property_add_str(oc, "bootrom", machine_get_bootrom,
                                  machine_set_bootrom);
    object_class_property_set_description(oc, "bootrom", "Xbox bootrom file");

    object_class_property_add_bool(oc, "chihiro", machine_get_chihiro, NULL);
    object_class_property_set_description(
        oc, "chihiro", "Sega Chihiro: media board, baseboard and 128 MiB");

    object_class_property_add_str(oc, "avpack", machine_get_avpack,
                                  machine_set_avpack);
    object_class_property_set_description(
        oc, "avpack",
        "Xbox video connector: composite, scart, svideo, vga, rfu, hdtv "
        "(default), none");

    object_class_property_add_bool(oc, "short-animation",
                                   machine_get_short_animation,
                                   machine_set_short_animation);
    object_class_property_set_description(oc, "short-animation",
                                          "Skip Xbox boot animation");

    object_class_property_add_str(oc, "smc-version", machine_get_smc_version,
                                  machine_set_smc_version);
    object_class_property_set_description(
        oc, "smc-version", "Set the SMC version number, default is P01");

    object_class_property_add_str(oc, "video-encoder",
                                  machine_get_video_encoder,
                                  machine_set_video_encoder);
    object_class_property_set_description(
        oc, "video-encoder",
        "Set the encoder presented to the OS: conexant (default), focus, "
        "xcalibur");
}

static inline void xbox_machine_initfn(Object *obj)
{
    object_property_set_str(obj, "avpack", "hdtv", &error_fatal);
    object_property_set_bool(obj, "short-animation", false, &error_fatal);
    object_property_set_str(obj, "smc-version", "P01", &error_fatal);
    object_property_set_str(obj, "video-encoder", "conexant", &error_fatal);
}

static void xbox_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    xbox_machine_options(mc);
    mc->init = xbox_init;
}

static const TypeInfo pc_machine_type_xbox = {
    .name = TYPE_XBOX_MACHINE,
    .parent = TYPE_PC_MACHINE,
    .abstract = false,
    .instance_size = sizeof(XboxMachineState),
    .instance_init = xbox_machine_initfn,
    .class_size = sizeof(XboxMachineClass),
    .class_init = xbox_machine_class_init,
    .interfaces = (InterfaceInfo[]) {
         // { TYPE_HOTPLUG_HANDLER },
         // { TYPE_NMI },
         { }
    },
};

static void pc_machine_init_xbox(void)
{
    type_register_static(&pc_machine_type_xbox);
}

type_init(pc_machine_init_xbox)
