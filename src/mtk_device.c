#include "mtk_device.h"
#include <string.h>

#include <libusb.h>
#include <stdio.h>

#include "flash_tool/util.h"
#include "src/util.h"

#ifdef _WIN32
#include <winsock2.h> // htonl, htons, ntohl, ntohs
#pragma comment(lib, "ws2_32.lib")

#define htobe16(x) htons(x)
#define htobe32(x) htonl(x)
#define be16toh(x) ntohs(x)
#define be32toh(x) ntohl(x)

static inline uint64_t htobe64(uint64_t x) { return ((uint64_t)htonl(x & 0xFFFFFFFF) << 32) | htonl(x >> 32); }

static inline uint64_t be64toh(uint64_t x) { return ((uint64_t)ntohl(x & 0xFFFFFFFF) << 32) | ntohl(x >> 32); }

#else
#include <endian.h>
#endif

bool verbose = false;

int mtk_device_open(mtk_device *device, libusb_device_handle *dev) {
    device->dev = dev;
    device->buffer_offset = 0;
    device->buffer_available = 0;

    int err;

#if !_WIN32
    verboseLog("detach kernel\n");
    if ((err = libusb_set_auto_detach_kernel_driver(dev, true)) < 0) {
        return err;
    }
#endif

    verboseLog("Claim interface\n");
    if ((err = libusb_claim_interface(dev, MTK_DEVICE_INTERFACE)) < 0) {
        return err;
    }

    verboseLog("Claim interface 2\n");
    if ((err = libusb_claim_interface(dev, MTK_DEVICE_INTERFACE)) < 0) {
        return err;
    }

    return 0;
}

static int hotplug_callback_fn(libusb_context *ctx, libusb_device *device, libusb_hotplug_event event, void *user_data) {
    (void)ctx;
    (void)event;

    libusb_device **dev = user_data;
    *dev = device;

    return true;
}

// taken from libusb, examples/xusb
void printInfo(libusb_device *dev) {
    struct libusb_device_descriptor dev_desc;
    libusb_get_device_descriptor(dev, &dev_desc);
    printf("            length: %d\n", dev_desc.bLength);
    printf("      device class: %d\n", dev_desc.bDeviceClass);
    printf("               S/N: %d\n", dev_desc.iSerialNumber);
    printf("           VID:PID: %04X:%04X\n", dev_desc.idVendor, dev_desc.idProduct);
    printf("         bcdDevice: %04X\n", dev_desc.bcdDevice);
    printf("   iMan:iProd:iSer: %d:%d:%d\n", dev_desc.iManufacturer, dev_desc.iProduct, dev_desc.iSerialNumber);
    printf("          nb confs: %d\n", dev_desc.bNumConfigurations);

    // Read IADs
    printf("\nReading interface association descriptors (IADs) for first configuration:\n");
    struct libusb_interface_association_descriptor_array *iad_array;
    int r = libusb_get_interface_association_descriptors(dev, 0, &iad_array);
    if (r == LIBUSB_SUCCESS) {
        printf("    nb IADs: %d\n", iad_array->length);
        for (int i = 0; i < iad_array->length; i++) {
            const struct libusb_interface_association_descriptor *iad = &iad_array->iad[i];
            printf("      IAD %d:\n", i);
            printf("            bFirstInterface: %u\n", iad->bFirstInterface);
            printf("            bInterfaceCount: %u\n", iad->bInterfaceCount);
            printf("             bFunctionClass: %02X\n", iad->bFunctionClass);
            printf("          bFunctionSubClass: %02X\n", iad->bFunctionSubClass);
            printf("          bFunctionProtocol: %02X\n", iad->bFunctionProtocol);
            if (iad->iFunction) {
                printf("                  iFunction: %u (libusb_get_string_descriptor_ascii failed!)\n", iad->iFunction);
            } else
                printf("                  iFunction: 0\n");
        }
        libusb_free_interface_association_descriptors(iad_array);
    }

    printf("\n");
}

int mtk_device_detect(mtk_device *device, libusb_context *ctx) {
    libusb_device *dev = NULL;

    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        errx(2, "libusb has no hotplug capabilities\n");
    }

    int err = libusb_hotplug_register_callback(
        ctx, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, LIBUSB_HOTPLUG_ENUMERATE, MTK_DEVICE_VID, MTK_DEVICE_PID, LIBUSB_CLASS_COMM, hotplug_callback_fn, &dev, NULL);

    if (err < 0) {
        return err;
    }

    verboseLog("Hotplug registered, handling events\n");
    while (dev == NULL) {
        err = libusb_handle_events(ctx);
        check_libusb(err, "wait failed");
    }
    printInfo(dev);

    verboseLog("Opening device\n");
    libusb_device_handle *devh;
    if ((err = libusb_open(dev, &devh)) < 0) {
        verboseLog("Open failed\n");
        return err;
    }

    return mtk_device_open(device, devh);
}

int mtk_device_read(mtk_device *device, uint8_t *buffer, size_t size) {
    size_t offset = 0;

    while (offset < size) {
        if (device->buffer_available == 0) {
            int transferred;

            int err;
            if ((err = libusb_bulk_transfer(device->dev, MTK_DEVICE_EPIN, device->buffer, MTK_DEVICE_PKTSIZE, &transferred, MTK_DEVICE_TMOUT)) < 0) {
                return err;
            }

            device->buffer_offset = 0;
            device->buffer_available = transferred;
        }

        size_t n = MIN(size - offset, device->buffer_available);
        if (buffer != NULL) {
            memcpy(buffer + offset, device->buffer + device->buffer_offset, n);
        }

        offset += n;
        device->buffer_offset += n;
        device->buffer_available -= n;
    }

    if (buffer != NULL) {
        verboseLog("RX:");
        if (verbose) {
            if (size < 63) {
                for (int i = 0; i < size; i++) {
                    printf("%02x", buffer[i]);
                }
            } else {
                printf("%zu", size);
            }
            printf("\n");
        }
    }

    return 0;
}

int mtk_device_write(mtk_device *device, const uint8_t *buffer, size_t size) {
    size_t offset = 0;

    verboseLog("TX:");
    while (offset < size) {
        if (verbose) {
            if (size < 63) {
                for (int i = 0; i < size; i++) {
                    printf("%02x", buffer[i]);
                }
            } else {
                printf("%zu", size);
            }
            printf("\n");
        }
        int transferred;

        int err = libusb_bulk_transfer(device->dev, MTK_DEVICE_EPOUT, (uint8_t *)buffer + offset, size - offset, &transferred, MTK_DEVICE_TMOUT);
        if (err < 0) {
            return err;
        }

        offset += transferred;
    }

    return 0;
}

int mtk_device_read8(mtk_device *device, uint8_t *data) { return mtk_device_read(device, data, sizeof(*data)); }

int mtk_device_read16(mtk_device *device, uint16_t *data) {
    int err = mtk_device_read(device, (uint8_t *)data, sizeof(*data));
    if (err < 0) {
        return err;
    }
    if (data != NULL) {
        *data = be16toh(*data);
    }
    return 0;
}

int mtk_device_read32(mtk_device *device, uint32_t *data) {
    int err = mtk_device_read(device, (uint8_t *)data, sizeof(*data));
    if (err < 0) {
        return err;
    }
    if (data != NULL) {
        *data = be32toh(*data);
    }
    return 0;
}

int mtk_device_read64(mtk_device *device, uint64_t *data) {
    int err = mtk_device_read(device, (uint8_t *)data, sizeof(*data));
    if (err < 0) {
        return err;
    }
    if (data != NULL) {
        *data = be64toh(*data);
    }
    return 0;
}

int mtk_device_write8(mtk_device *device, uint8_t data) { return mtk_device_write(device, &data, sizeof(data)); }

int mtk_device_write16(mtk_device *device, uint16_t data) {
    data = htobe16(data);
    return mtk_device_write(device, (uint8_t *)&data, sizeof(data));
}

int mtk_device_write32(mtk_device *device, uint32_t data) {
    data = htobe32(data);
    return mtk_device_write(device, (uint8_t *)&data, sizeof(data));
}

int mtk_device_write64(mtk_device *device, uint64_t data) {
    data = htobe64(data);
    return mtk_device_write(device, (uint8_t *)&data, sizeof(data));
}

int mtk_device_echo8(mtk_device *device, uint8_t data) {
    int err;

    if ((err = mtk_device_write8(device, data)) < 0) {
        return err;
    }

    uint8_t reply;
    if ((err = mtk_device_read8(device, &reply)) < 0) {
        return err;
    }
    if (reply != data) {
        return LIBUSB_ERROR_OTHER;
    }

    return 0;
}

int mtk_device_echo16(mtk_device *device, uint16_t data) {
    int err;

    if ((err = mtk_device_write16(device, data)) < 0) {
        return err;
    }

    uint16_t reply;
    if ((err = mtk_device_read16(device, &reply)) < 0) {
        return err;
    }
    if (reply != data) {
        return LIBUSB_ERROR_OTHER;
    }

    return 0;
}

int mtk_device_echo32(mtk_device *device, uint32_t data) {
    int err;

    if ((err = mtk_device_write32(device, data)) < 0) {
        return err;
    }

    uint32_t reply;
    if ((err = mtk_device_read32(device, &reply)) < 0) {
        return err;
    }
    if (reply != data) {
        return LIBUSB_ERROR_OTHER;
    }

    return 0;
}

int mtk_device_echo64(mtk_device *device, uint64_t data) {
    int err;

    if ((err = mtk_device_write64(device, data)) < 0) {
        return err;
    }

    uint64_t reply;
    if ((err = mtk_device_read64(device, &reply)) < 0) {
        return err;
    }
    if (reply != data) {
        return LIBUSB_ERROR_OTHER;
    }

    return 0;
}
