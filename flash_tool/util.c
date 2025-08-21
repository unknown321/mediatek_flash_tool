#include "util.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <libusb.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>

#include "mtk_da.h"

void errx(int status, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    printf("\nPress enter to exit\n");
    getchar();
    exit(status);
}

void check_errnum(int errnum, const char *s) {
    if (errnum != 0) {
        errx(1, "%s: %s", s, strerror(errnum));
    }
}

void check_libusb(int errnum, const char *s) {
    if (errnum < 0) {
        errx(1, "%s: %s", s, libusb_strerror(errnum));
    }
}

void check_mtk_preloader(uint16_t status, const char *cmd) {
    if (status != 0) {
        errx(2, "%s failed: 0x%04" PRIx16, cmd, status);
    }
}

void check_mtk_da_ack(uint8_t retval) {
    if (retval != MTK_DA_ACK) {
        errx(2, "DA did not ACK: 0x%02" PRIx8, retval);
    }
}

void check_mtk_da_cont_char(uint8_t retval) {
    if (retval != MTK_DA_CONT_CHAR) {
        errx(2, "DA did not return continuation character: 0x%02" PRIx8, retval);
    }
}

void check_mtk_da_soc_ok(uint8_t retval) {
    if (retval != MTK_DA_SOC_OK) {
        errx(2, "SOC DA did not return OK: 0x%02" PRIx8, retval);
    }
}

void verboseLog(const char* format, ...) {
    if (verbose) {
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
    }
}
