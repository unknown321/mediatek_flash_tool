#include "args.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define open _open
#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define lseek _lseeki64
#else
#include <sys/stat.h>
#include <unistd.h>

#endif

static uint64_t parse_uint64_opt(const char *key, const char *str);
static void parse_operation(struct arguments *arguments, int key, const char *arg, bool flashing);
static void validate_arguments(struct arguments *arguments, const char *program_name);

void args_print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s [OPTIONS]...\n", program_name);
    fprintf(stderr, "MediaTek device communication tool for MT8590-based Walkmans\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -2, --da-stage2         Device is in DA Stage 2\n");
    fprintf(stderr, "  -P, --preloader         Device is in Preloader mode\n");
    fprintf(stderr, "  -d, --download-agent FILE\n");
    fprintf(stderr, "                          Path to MediaTek Download Agent binary\n");
    fprintf(stderr, "  -a, --address ADDRESS   EMMC address to read/write\n");
    fprintf(stderr, "  -l, --length LENGTH     Length of data to read/write\n");
    fprintf(stderr, "  -D, --dump FILE         Path to dump data to\n");
    fprintf(stderr, "  -F, --flash FILE        Path to flash data from\n");
    fprintf(stderr, "  -R, --reboot            Reboot device after completion\n");
    fprintf(stderr, "  -v, --verbose           Produce verbose output\n");
    fprintf(stderr, "  -n, --no-interactive    Don't prompt before exiting\n");
    fprintf(stderr, "  -h, --help              Show this help message\n");
}

void args_cleanup(struct arguments *arguments) {
    for (size_t i = 0; i < arguments->operations_count; i++) {
        if (arguments->operations[i].fd != -1) {
            close(arguments->operations[i].fd);
        }
    }
    if (arguments->download_agent_fd != -1) {
        close(arguments->download_agent_fd);
    }
}

void args_parse(int argc, char **argv, struct arguments *arguments) {
    // Initialize arguments
    arguments->state = DEVICE_STATE_NONE;
    arguments->download_agent = NULL;
    arguments->address = 0;
    arguments->length = 0;
    arguments->reboot = false;
    arguments->verbose = false;
    arguments->interactive = true;
    arguments->operations_count = 0;
    arguments->download_agent_fd = -1;

    for (int i = 0; i < MAX_OPERATIONS; i++) {
        arguments->operations[i].fd = -1;
    }

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            args_print_usage(argv[0]);
            exit(0);
        } else if (strcmp(arg, "-2") == 0 || strcmp(arg, "--da-stage2") == 0) {
            arguments->state = DEVICE_STATE_DA_STAGE2;
        } else if (strcmp(arg, "-P") == 0 || strcmp(arg, "--preloader") == 0) {
            arguments->state = DEVICE_STATE_PRELOADER;
        } else if (strcmp(arg, "-d") == 0 || strcmp(arg, "--download-agent") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: Missing argument for %s\n", arg);
                args_print_usage(argv[0]);
                exit(1);
            }
            arguments->download_agent = argv[i];
        } else if (strcmp(arg, "-a") == 0 || strcmp(arg, "--address") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: Missing argument for %s\n", arg);
                args_print_usage(argv[0]);
                exit(1);
            }
            arguments->address = parse_uint64_opt(arg, argv[i]);
        } else if (strcmp(arg, "-l") == 0 || strcmp(arg, "--length") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: Missing argument for %s\n", arg);
                args_print_usage(argv[0]);
                exit(1);
            }
            arguments->length = parse_uint64_opt(arg, argv[i]);
        } else if (strcmp(arg, "-D") == 0 || strcmp(arg, "--dump") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: Missing argument for %s\n", arg);
                args_print_usage(argv[0]);
                exit(1);
            }
            parse_operation(arguments, 'D', argv[i], false);
        } else if (strcmp(arg, "-F") == 0 || strcmp(arg, "--flash") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: Missing argument for %s\n", arg);
                args_print_usage(argv[0]);
                exit(1);
            }
            parse_operation(arguments, 'F', argv[i], true);
        } else if (strcmp(arg, "-R") == 0 || strcmp(arg, "--reboot") == 0) {
            arguments->reboot = true;
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            arguments->verbose = true;
        } else if (strcmp(arg, "-n") == 0 || strcmp(arg, "--no-interactive") == 0) {
            arguments->interactive = false;
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", arg);
            args_print_usage(argv[0]);
            exit(1);
        }
    }

    validate_arguments(arguments, argv[0]);
}

static void parse_operation(struct arguments *arguments, int key, const char *arg, bool flashing) {
    if (arguments->operations_count == MAX_OPERATIONS) {
        fprintf(stderr, "Error: Too many operations\n");
        exit(1);
    }
    if (arguments->length == 0) {
        fprintf(stderr, "Error: Cannot perform zero-length operation\n");
        exit(1);
    }

    struct operation *operation = &arguments->operations[arguments->operations_count++];
    operation->key = key;
    operation->address = arguments->address;
    operation->length = arguments->length;

    int flags;
    const char *verb;

    if (flashing) {
        flags = O_RDONLY;
#if _WIN32
        flags |= O_BINARY;
#endif
        verb = "flashing";
    } else {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        verb = "dumping";
    }

    if ((operation->fd = open(arg, flags, 0666)) < 0) {
        fprintf(stderr, "Error: Unable to open file for %s: %s (%s)\n", verb, arg, strerror(errno));
        exit(1);
    }

    if (flashing) {
        off_t maxlength;
        if ((maxlength = lseek(operation->fd, 0, SEEK_END)) < 0) {
            fprintf(stderr, "Error: Unable to seek file descriptor: %s (%s)\n", arg, strerror(errno));
            exit(1);
        }
        if ((uint64_t)maxlength < arguments->length) {
            fprintf(stderr, "Error: Write length is greater than file size: %s\n", arg);
            exit(1);
        }
    }
}

static void validate_arguments(struct arguments *arguments, const char *program_name) {
    if (arguments->state != DEVICE_STATE_DA_STAGE2) {
        if (arguments->download_agent == NULL) {
            fprintf(stderr, "Error: MediaTek Download Agent binary is mandatory, unless device is in DA Stage 2\n");
            args_print_usage(program_name);
            exit(1);
        }

        int flag = O_RDONLY;
#if _WIN32
        flag |= O_BINARY;
#endif
        if ((arguments->download_agent_fd = open(arguments->download_agent, flag)) < 0) {
            fprintf(stderr, "Error: Unable to open Download Agent binary: %s (%s)\n", arguments->download_agent, strerror(errno));
            exit(1);
        }
    }

    if (arguments->operations_count == 0) {
        fprintf(stderr, "Error: No operations specified (use -D or -F)\n");
        args_print_usage(program_name);
        exit(1);
    }
}

static uint64_t parse_uint64_opt(const char *key, const char *str) {
    if (str == NULL || *str == '\0') {
        fprintf(stderr, "Error: Empty value for option %s\n", key);
        exit(1);
    }

    // Check if all characters are digits (with optional 0x prefix)
    const char *ptr = str;
    if (strncmp(ptr, "0x", 2) == 0) {
        ptr += 2;
    }

    while (*ptr) {
        if (!isxdigit((unsigned char)*ptr)) {
            fprintf(stderr, "Error: Option %s requires an integer -- '%s'\n", key, str);
            exit(1);
        }
        ptr++;
    }

    char *endptr = NULL;
    errno = 0;
    unsigned long long value = strtoull(str, &endptr, 0);

    if (errno != 0 || *endptr != '\0') {
        fprintf(stderr, "Error: Invalid integer for option %s: %s\n", key, str);
        exit(1);
    }

    return (uint64_t)value;
}
