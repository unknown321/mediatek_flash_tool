#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <libusb.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock.h>
#else
#include <netinet/in.h>
#endif

#include "args.h"
#include "io_handler.h"
#include "util.h"
#include <memory.h>

#include "mtk_da.h"
#include "mtk_device.h"
#include "mtk_preloader.h"

static void handle_state_none(mtk_device *device);

static void handle_state_preloader(mtk_device *device, int download_agent_fd, const mtk_da_info *info);

static void handle_state_da_stage2(mtk_device *device, const struct operation *operations, size_t count, bool reboot);

int main(int argc, char **argv) {
    struct arguments arguments;
    args_parse(argc, argv, &arguments);

    int err;

    const mtk_da_info *info = NULL;

    if (arguments.state != DEVICE_STATE_DA_STAGE2) {
        err = mtk_da_info_load(arguments.download_agent_fd, &info);
        check_errnum(-err, "Unable to load Download Agent binary");

        printf("DA identifier:   %.*s\n", (int)sizeof(info->da_identifier), info->da_identifier);
        printf("DA description:  %.*s\n", (int)sizeof(info->da_description), info->da_description);
        printf("DA count:        %" PRIu32 "\n", info->da_count);
        printf("\n");
    }

    err = libusb_init(NULL);
    check_libusb(err, "libusb_init failed");

    int level = arguments.verbose ? LIBUSB_LOG_LEVEL_DEBUG : LIBUSB_LOG_LEVEL_INFO;
#if LIBUSB_API_VERSION >= 0x01000106
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, level);
#else
    libusb_set_debug(NULL, level);
#endif
    verbose = arguments.verbose;

    interactive = arguments.interactive;

    printf("Waiting for MediaTek device...\n");
    printf("1. Detach cable and turn off the device\n");
    printf("2. Hold Play and Volume Down buttons\n");
    printf("3. Insert cable\n");
    printf("4. Release the buttons after successful detection\n");

    mtk_device device;
    err = mtk_device_detect(&device, NULL);
    check_libusb(err, "Unable to detect MediaTek device");

    switch (arguments.state) {
    case DEVICE_STATE_NONE:
        handle_state_none(&device);
        /* fallthrough */
    case DEVICE_STATE_PRELOADER:
        handle_state_preloader(&device, arguments.download_agent_fd, info);
        /* fallthrough */
    case DEVICE_STATE_DA_STAGE2:
        handle_state_da_stage2(&device, arguments.operations, arguments.operations_count, arguments.reboot);
        break;
    }
    args_cleanup(&arguments);

    return 0;
}

static void handle_state_none(mtk_device *device) {
    printf("Syncing with MediaTek Preloader...\n");

    int err = mtk_preloader_start(device);
    check_libusb(err, "Unable to sync with MediaTek Preloader");
}

static void handle_state_preloader(mtk_device *device, int download_agent_fd, const mtk_da_info *info) {
    int err;
    uint16_t status;
    struct file_info fi;

    uint16_t hw_code;
    err = mtk_preloader_get_hw_code(device, &hw_code, &status);
    check_libusb(err, "Unable to get chip code");
    check_mtk_preloader(status, "GET_HW_CODE");

    printf("\nHW code:     0x%04" PRIx16 "\n", hw_code);

    uint16_t hw_subcode, hw_ver, sw_ver;
    err = mtk_preloader_get_hw_sw_ver(device, &hw_subcode, &hw_ver, &sw_ver, &status);
    check_libusb(err, "Unable to get hardware/software version");
    check_mtk_preloader(status, "GET_HW_SW_VER");

    printf("HW subcode:  0x%04" PRIx16 "\n", hw_subcode);
    printf("HW version:  0x%04" PRIx16 "\n", hw_ver);
    printf("SW version:  0x%04" PRIx16 "\n", sw_ver);

    uint32_t tgt_config;
    err = mtk_preloader_get_tgt_config(device, &tgt_config, &status);
    check_libusb(err, "Unable to get target config");
    check_mtk_preloader(status, "GET_TARGET_CONFIG");

    printf("\nTarget config:  0x%08" PRIx32 "\n", tgt_config);

    const mtk_da_entry *entry = NULL;
    for (size_t i = 0; i < info->da_count; i++) {
        if (info->DA[i].magic != MTK_DA_ENTRY_MAGIC) {
            errx(1, "DA entry has invalid magic");
        }
        verboseLog(
            "code 0x%x, hw 0x%x, sw 0x%x, addr 0x%x\n", info->DA[i].hw_code, info->DA[i].hw_ver, info->DA[i].sw_ver, info->DA[i].load_regions[0].start_addr);
        if (info->DA[i].hw_code == hw_code && info->DA[i].hw_ver <= hw_ver && info->DA[i].sw_ver <= sw_ver) {
            entry = &info->DA[i];
            verboseLog("found\n");
            break;
        }
    }
    if (entry == NULL) {
        errx(1, "Unable to find DA entry for HW code");
    }
    if (entry->load_regions_count > MTK_DA_ENTRY_LOAD_REGIONS) {
        errx(1, "Invalid load regions count in DA entry");
    }
    if (entry->entry_region_index >= entry->load_regions_count) {
        errx(1, "Invalid entry region index");
    }

    const mtk_da_load_region *da_stage1 = NULL;
    for (size_t i = entry->entry_region_index; i + 1 < entry->load_regions_count; i++) {
        if (entry->load_regions[i].sig_len > 0) {
            da_stage1 = &entry->load_regions[i];
            break;
        }
    }
    if (da_stage1 == NULL) {
        errx(1, "Unable to find valid load region for DA entry");
    }
    if (da_stage1->sig_offset + da_stage1->sig_len != da_stage1->len) {
        errx(1, "DA Stage 1 signature is not at end of load region");
    }

    const mtk_da_load_region *da_stage2 = da_stage1 + 1;
    if (da_stage2->sig_offset + da_stage2->sig_len != da_stage2->len) {
        errx(1, "DA Stage 2 signature is not at end of load region");
    }

    printf("\nDisabling watchdog timer...\n");
    err = mtk_preloader_disable_wdt(device, &status);
    check_libusb(err, "Unable to disable WDT");
    check_mtk_preloader(status, "WRITE32");

    // target config
    mtk_device_echo8(device, 0xd8);
    uint8_t targetConf[6];
    mtk_device_read(device, targetConf, 6);

    verboseLog("Getting BLver\n");
    uint8_t blver;
    uint8_t getBLver = 0xfe;
    mtk_device_write(device, &getBLver, 1);
    mtk_device_read(device, &blver, 1);

    verboseLog("Getting BROMver\n");
    uint8_t bromver;
    uint8_t getver = 0xff;
    mtk_device_write(device, &getver, 1);
    mtk_device_read(device, &bromver, 1);

    verboseLog("Getting HW,SW\n");
    uint8_t hwsw;
    uint8_t getHWSWver = 0xfc;
    mtk_device_write(device, &getHWSWver, 1);
    mtk_device_read(device, &hwsw, 1);
    usleep(50 * 1000);
    mtk_device_read(device, &bromver, 8);

    verboseLog("Getting meID\n");
    getBLver = 0xfe;
    mtk_device_write(device, &getBLver, 1);
    mtk_device_read(device, &blver, 1);
    //    uint8_t getmeid = 0xE1;
    //    mtk_device_write(device, &getmeid, 1);
    //    mtk_device_read(device, &meidCMD, 1);
    //    if (meidCMD == getmeid) {
    //        uint8_t length[4];
    //        mtk_device_read(device, length, 4);
    //        uint8_t meid[2];
    //        mtk_device_read(device, meid, 2);
    //    }

    //    printf("socid\n");
    //    uint8_t socidCMD;
    //    getBLver = 0xfe ;
    //    mtk_device_write(device, &getBLver, 1);
    //    mtk_device_read(device, &blver, 1);
    //    uint8_t getsocid = 0xE7;
    //    mtk_device_write(device, &getsocid, 1);
    //    mtk_device_read(device, &socidCMD, 1);
    //    if (socidCMD == 0xE1) {
    //        uint8_t length[4];
    //        mtk_device_read(device, length, 4);
    //        uint8_t socid[2];
    //        mtk_device_read(device, socid, 2);
    //    }

    fi.fd = download_agent_fd;
    fi.offset = da_stage1->offset;

    printf("Sending DA Stage 1...\n");
    err = mtk_preloader_send_da(device, da_stage1->start_addr, da_stage1->len, da_stage1->sig_len, &status, io_handler, &fi);
    check_libusb(err, "Unable to send DA");
    check_mtk_preloader(status, "SEND_DA");

    printf("\nJumping to DA Stage 1... (0x%x)\n", da_stage1->start_addr);
    err = mtk_preloader_jump_da(device, da_stage1->start_addr, &status);
    check_libusb(err, "Unable to jump to DA");
    check_mtk_preloader(status, "JUMP_DA");

    uint32_t nand_ret, emmc_ret, emmc_id[4];
    uint8_t da_major_ver, da_minor_ver;

    err = mtk_da_sync(device, &nand_ret, &emmc_ret, emmc_id, &da_major_ver, &da_minor_ver);
    check_libusb(err, "Unable to sync with DA Stage 1");
    if (nand_ret != MTK_DA_NAND_NOT_FOUND) {
        errx(2, "NAND controller did not return NAND_NOT_FOUND: 0x%x", nand_ret);
    }
    if (emmc_ret != 0) {
        errx(2, "EMMC controller returned error: 0x%x", emmc_ret);
    }

    printf("EMMC ID:     %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32 "\n", emmc_id[0], emmc_id[1], emmc_id[2], emmc_id[3]);
    printf("DA version:  DA_v%" PRIu8 ".%" PRIu8 "\n", da_major_ver, da_minor_ver);

    fi.fd = download_agent_fd;
    fi.offset = da_stage2->offset;

    printf("\nSending DA Stage 2...\n");
    verboseLog("DA stage 2 offset: 0x%zx\n", fi.offset);
    uint8_t retval;
    err = mtk_da_send_da(device, da_stage2->start_addr, da_stage2->len, &retval, io_handler, &fi);
    verboseLog("Send DA stage 2, err 0x%x\n", err);
    check_libusb(err, "Unable to send DA");
    check_mtk_da_ack(retval);
    printf("Successfully uploaded stage 2\n");

    verboseLog("Reading flash info\n");
    uint32_t reports[7] = {0x1c, 0x11, 0xE, 0x9, 0x5c, 0x1c, 0x26};
    for (int i = 0; i < 7; i++) {
        verboseLog("Reading 0x%02x\n", reports[i]);
        uint8_t buf[reports[i]];
        err = mtk_device_read(device, buf, reports[i]);
        check_libusb(err, "Unable to read DA report");
    }

    uint8_t buf[0xA];
    err = mtk_device_read(device, buf, 0xA);
    check_libusb(err, "Unable to read DA return value");
    struct passinfo pi;
    memcpy(&pi, buf, sizeof pi);
    pi.download_status = htonl(pi.download_status);
    pi.boot_style = htonl(pi.boot_style);

    if (pi.ack == MTK_DA_ACK) {
        verboseLog("%s, ack ok\n", __FUNCTION__);
        return;
    }
    verboseLog("PI status: ack: 0x%x, download_status: 0x%x, boot_style: 0x%x, soc_ok: 0x%x\n", pi.ack, pi.download_status, pi.boot_style, pi.soc_ok);

    if (pi.download_status == MTK_DA_ACK) {
        verboseLog("Get DA return value\n");
        err = mtk_device_read8(device, &retval);
        check_libusb(err, "Unable to read DA return value");
        err = mtk_device_read8(device, &retval);
        check_libusb(err, "Unable to read DA return value");
        err = mtk_device_read8(device, &retval);
        check_libusb(err, "Unable to read DA return value");
        err = mtk_device_read8(device, &retval);
        check_libusb(err, "Unable to read DA return value");
        verboseLog("Check SOC\n");
        check_mtk_da_soc_ok(retval);
    }

    verboseLog("%s done\n", __FUNCTION__);
}

static void handle_state_da_stage2(mtk_device *device, const struct operation *operations, size_t count, bool reboot) {
    int err;
    uint8_t retval;

    verboseLog("%s\n", __FUNCTION__);

    uint8_t usb_status;
    err = mtk_da_usb_check_status(device, &usb_status, &retval);
    check_libusb(err, "Unable to check USB status");
    check_mtk_da_ack(retval);
    if (usb_status != 1) {
        errx(2, "DA did not return valid USB status: %02" PRIx8, usb_status);
    }

    printf("\n");
    for (size_t i = 0; i < count; i++) {
        const struct operation *operation = &operations[i];
        printf("Address:  0x%016" PRIx64 "\n", operation->address);
        printf("Length:   0x%016" PRIx64 "\n", operation->length);

        verboseLog("switchpart\n");
        err = mtk_da_sdmmc_switch_part(device, MTK_DA_EMMC_PART_USER, &retval);
        check_libusb(err, "Unable to switch partition to EMMC_USER");
        check_mtk_da_ack(retval);

        struct file_info fi = {
            .fd = operation->fd,
            .offset = 0,
        };

        verboseLog("operation\n");
        switch (operation->key) {
        case 'D':
            err = mtk_da_read(device, MTK_DA_STORAGE_SDMMC, operation->address, operation->length, &retval, io_handler, &fi);
            check_libusb(err, "Unable to perform dump operation");
            check_mtk_da_ack(retval);
            break;

        case 'F':
            err = mtk_da_sdmmc_write_data(device, MTK_DA_STORAGE_SDMMC, MTK_DA_EMMC_PART_USER, operation->address, operation->length, &retval, io_handler, &fi);
            check_libusb(err, "Unable to perform flash operation");
            check_mtk_da_cont_char(retval);
            break;
        }

        printf("\n");
    }

    if (reboot) {
        printf("Enabling WDT to reboot device...\n");
        err = mtk_da_enable_watchdog(device, 0, false, false, false, true, &retval);
        check_libusb(err, "Unable to enable WDT");
        check_mtk_da_ack(retval);
    }
}
