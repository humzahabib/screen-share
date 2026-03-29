/*
 * vmonitor.c — Virtual monitor setup tool
 *
 * Creates a virtual monitor on a disconnected connector by:
 *   1. Injecting a fake EDID via sysfs (works on amdgpu, i915)
 *   2. Falling back to kernel param injection via bootloader if sysfs fails
 *   3. Installing a udev rule for persistence across reboots
 *
 * Usage:
 *   sudo vmonitor [OPTIONS]
 *
 * Options:
 *   -c <connector>       Connector name, e.g. card1-DP-1 (auto-detect if omitted)
 *   -r <WxH@HZ>         Resolution, e.g. 1920x1080@60 (default: 1920x1080@60)
 *   --remove             Remove virtual monitor and udev rule
 *   --list               List available connectors
 *   --no-udev            Skip udev rule installation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "edid.h"

/* ------------------------------------------------------------------ */
/* Types                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    BOOTLOADER_GRUB,
    BOOTLOADER_SYSTEMD_BOOT,
    BOOTLOADER_UNKNOWN
} Bootloader;

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                    */
/* ------------------------------------------------------------------ */

static int write_file(const char *path, const void *data, size_t len) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) { perror(path); return -1; }
    ssize_t w = write(fd, data, len);
    close(fd);
    return (w == (ssize_t)len) ? 0 : -1;
}

static int read_file(const char *path, char *buf, size_t len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return 0;
}

/* Returns pointer into connector string past "cardN-", e.g.
   "card1-DP-1" → "DP-1" */
static const char *bare_connector(const char *connector) {
    const char *dash = strchr(connector, '-');
    return dash ? dash + 1 : connector;
}

/* ------------------------------------------------------------------ */
/* Connector discovery                                                  */
/* ------------------------------------------------------------------ */

/* Fill connectors[] with names of all DRM connectors.
   Returns count found. */
static int list_connectors(char connectors[][64], int max) {
    DIR *d = opendir("/sys/class/drm");
    if (!d) { perror("opendir /sys/class/drm"); return 0; }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max) {
        /* Must contain a hyphen (connector format: cardN-TYPE-N) */
        if (!strchr(ent->d_name, '-')) continue;
        /* Must not be just the card itself */
        if (strncmp(ent->d_name, "card", 4) != 0) continue;
        /* Confirm status file exists */
        char status_path[256];
        snprintf(status_path, sizeof(status_path),
                 "/sys/class/drm/%s/status", ent->d_name);
        struct stat st;
        if (stat(status_path, &st) != 0) continue;

        strncpy(connectors[count], ent->d_name, 63);
        connectors[count][63] = '\0';
        count++;
    }
    closedir(d);
    return count;
}

/* Find first disconnected external connector (skips eDP = laptop panel) */
static int auto_detect_connector(char *out, size_t len) {
    char connectors[32][64];
    int count = list_connectors(connectors, 32);

    for (int i = 0; i < count; i++) {
        /* Skip built-in laptop panel */
        if (strstr(connectors[i], "eDP")) continue;

        char status_path[256];
        snprintf(status_path, sizeof(status_path),
                 "/sys/class/drm/%s/status", connectors[i]);

        char status[32];
        if (read_file(status_path, status, sizeof(status)) != 0) continue;

        if (strcmp(status, "disconnected") == 0) {
            strncpy(out, connectors[i], len - 1);
            out[len - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

static void cmd_list_connectors(void) {
    char connectors[32][64];
    int count = list_connectors(connectors, 32);

    printf("%-30s %s\n", "CONNECTOR", "STATUS");
    printf("%-30s %s\n", "---------", "------");

    for (int i = 0; i < count; i++) {
        char status_path[256];
        snprintf(status_path, sizeof(status_path),
                 "/sys/class/drm/%s/status", connectors[i]);
        char status[32] = "unknown";
        read_file(status_path, status, sizeof(status));
        printf("%-30s %s\n", connectors[i], status);
    }
}

/* ------------------------------------------------------------------ */
/* EDID firmware installation                                           */
/* ------------------------------------------------------------------ */

static const char *FIRMWARE_DIRS[] = {
    "/lib/firmware/edid",
    "/usr/lib/firmware/edid",
    "/usr/local/lib/firmware/edid",
    NULL
};

static const char *find_or_create_firmware_dir(void) {
    for (int i = 0; FIRMWARE_DIRS[i]; i++) {
        struct stat st;
        if (stat(FIRMWARE_DIRS[i], &st) == 0)
            return FIRMWARE_DIRS[i];
        if (mkdir(FIRMWARE_DIRS[i], 0755) == 0)
            return FIRMWARE_DIRS[i];
    }
    return NULL;
}

static int install_edid_to_firmware(const unsigned char *edid_data) {
    const char *fw_dir = find_or_create_firmware_dir();
    if (!fw_dir) {
        fprintf(stderr, "Could not find or create firmware directory\n");
        return -1;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/virtual-monitor.bin", fw_dir);

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    fwrite(edid_data, 1, 128, f);
    fclose(f);

    printf("EDID installed to %s\n", path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Bootloader detection & patching                                      */
/* ------------------------------------------------------------------ */

static Bootloader detect_bootloader(void) {
    struct stat st;
    if (stat("/boot/grub/grub.cfg",  &st) == 0 ||
        stat("/boot/grub2/grub.cfg", &st) == 0)
        return BOOTLOADER_GRUB;
    if (stat("/boot/loader/loader.conf", &st) == 0)
        return BOOTLOADER_SYSTEMD_BOOT;
    return BOOTLOADER_UNKNOWN;
}

/* Read entire file into a malloc'd buffer. Caller must free(). */
static char *slurp_file(const char *path, long *out_size) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = (char *)malloc(size + 512);  /* extra headroom for our insertion */
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    if (out_size) *out_size = size;
    return buf;
}

static int patch_grub(const char *connector) {
    const char *grub_paths[] = {
        "/etc/default/grub",
        "/etc/default/grub2",
        NULL
    };

    const char *grub_file = NULL;
    struct stat st;
    for (int i = 0; grub_paths[i]; i++) {
        if (stat(grub_paths[i], &st) == 0) {
            grub_file = grub_paths[i];
            break;
        }
    }
    if (!grub_file) {
        fprintf(stderr, "Could not find /etc/default/grub\n");
        return -1;
    }

    char param[128];
    snprintf(param, sizeof(param),
             "drm.edid_firmware=%s:edid/virtual-monitor.bin",
             bare_connector(connector));

    char *contents = slurp_file(grub_file, NULL);
    if (!contents) { perror(grub_file); return -1; }

    if (strstr(contents, param)) {
        printf("GRUB already contains kernel param, skipping patch\n");
        free(contents);
        return 0;
    }

    /* Find GRUB_CMDLINE_LINUX_DEFAULT="..." */
    char *line = strstr(contents, "GRUB_CMDLINE_LINUX_DEFAULT=\"");
    if (!line) {
        fprintf(stderr, "GRUB_CMDLINE_LINUX_DEFAULT not found in %s\n",
                grub_file);
        free(contents);
        return -1;
    }

    char *closing = strchr(line + strlen("GRUB_CMDLINE_LINUX_DEFAULT=\""), '"');
    if (!closing) {
        fprintf(stderr, "Malformed GRUB_CMDLINE_LINUX_DEFAULT\n");
        free(contents);
        return -1;
    }

    /* Build new file contents */
    char *new_contents = (char *)malloc(strlen(contents) + 256);
    if (!new_contents) { free(contents); return -1; }

    size_t prefix_len = (size_t)(closing - contents);
    strncpy(new_contents, contents, prefix_len);
    new_contents[prefix_len] = '\0';
    strcat(new_contents, " ");
    strcat(new_contents, param);
    strcat(new_contents, closing);

    FILE *f = fopen(grub_file, "w");
    if (!f) {
        perror(grub_file);
        free(contents); free(new_contents);
        return -1;
    }
    fputs(new_contents, f);
    fclose(f);
    free(contents);
    free(new_contents);

    printf("Patched %s\n", grub_file);

    /* Rebuild grub.cfg */
    printf("Rebuilding GRUB config...\n");
    if (system("grub-mkconfig -o /boot/grub/grub.cfg 2>/dev/null") != 0)
        system("grub2-mkconfig -o /boot/grub2/grub.cfg 2>/dev/null");

    printf("GRUB updated. Reboot to apply.\n");
    return 0;
}

static int patch_systemd_boot(const char *connector) {
    char param[128];
    snprintf(param, sizeof(param),
             "drm.edid_firmware=%s:edid/virtual-monitor.bin",
             bare_connector(connector));

    DIR *d = opendir("/boot/loader/entries");
    if (!d) { perror("/boot/loader/entries"); return -1; }

    int patched = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strstr(ent->d_name, ".conf")) continue;

        char entry_path[256];
        snprintf(entry_path, sizeof(entry_path),
                 "/boot/loader/entries/%s", ent->d_name);

        char *contents = slurp_file(entry_path, NULL);
        if (!contents) continue;

        if (strstr(contents, param)) {
            free(contents);
            continue;
        }

        /* Find the "options" line */
        char *options = strstr(contents, "\noptions ");
        if (!options) { free(contents); continue; }

        char *eol = strchr(options + 1, '\n');
        char *new_contents = (char *)malloc(strlen(contents) + 256);
        if (!new_contents) { free(contents); continue; }

        if (eol) {
            size_t prefix = (size_t)(eol - contents);
            strncpy(new_contents, contents, prefix);
            new_contents[prefix] = '\0';
            strcat(new_contents, " ");
            strcat(new_contents, param);
            strcat(new_contents, eol);
        } else {
            strcpy(new_contents, contents);
            strcat(new_contents, " ");
            strcat(new_contents, param);
        }

        FILE *f = fopen(entry_path, "w");
        if (f) {
            fputs(new_contents, f);
            fclose(f);
            printf("Patched %s\n", entry_path);
            patched++;
        }
        free(contents);
        free(new_contents);
    }
    closedir(d);

    if (patched > 0)
        printf("systemd-boot updated. Reboot to apply.\n");
    else
        printf("No entry files found to patch in /boot/loader/entries/\n");

    return patched > 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* udev rule                                                            */
/* ------------------------------------------------------------------ */

#define UDEV_RULE_PATH "/etc/udev/rules.d/99-virtual-monitor.rules"
#define SETUP_BINARY   "/usr/local/bin/vmonitor-apply"

static int install_udev_rule(const char *connector) {
    /* Install a copy of ourselves as the apply binary */
    char self_path[256];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len < 0) { perror("readlink /proc/self/exe"); return -1; }
    self_path[len] = '\0';

    /* Copy binary to /usr/local/bin/vmonitor-apply */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s' && chmod +x '%s'",
             self_path, SETUP_BINARY, SETUP_BINARY);
    if (system(cmd) != 0) {
        fprintf(stderr, "Failed to install %s\n", SETUP_BINARY);
        return -1;
    }

    /* Write the udev rule */
    FILE *f = fopen(UDEV_RULE_PATH, "w");
    if (!f) { perror(UDEV_RULE_PATH); return -1; }

    /* Match any DP or HDMI connector that isn't eDP (built-in panel).
       %k passes the kernel name (e.g. card1-DP-1) to our binary. */
    fprintf(f,
        "# Virtual monitor — auto-inject EDID on disconnected connectors\n"
        "ACTION==\"add\", SUBSYSTEM==\"drm\", "
        "KERNEL!=\"card*-eDP*\", KERNEL==\"card*-DP-*\", "
        "RUN+=\"%s --apply %%k\"\n"
        "ACTION==\"add\", SUBSYSTEM==\"drm\", "
        "KERNEL!=\"card*-eDP*\", KERNEL==\"card*-HDMI-*\", "
        "RUN+=\"%s --apply %%k\"\n",
        SETUP_BINARY, SETUP_BINARY);

    fclose(f);
    printf("udev rule installed at %s\n", UDEV_RULE_PATH);

    system("udevadm control --reload-rules");
    system("udevadm trigger");
    return 0;
}

static int remove_udev_rule(void) {
    if (remove(UDEV_RULE_PATH) == 0)
        printf("Removed %s\n", UDEV_RULE_PATH);
    else
        perror(UDEV_RULE_PATH);

    if (remove(SETUP_BINARY) == 0)
        printf("Removed %s\n", SETUP_BINARY);
    else
        perror(SETUP_BINARY);

    system("udevadm control --reload-rules");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Core setup                                                           */
/* ------------------------------------------------------------------ */

static int setup_virtual_monitor(const char *connector,
                                  const EDIDPreset *preset,
                                  int install_udev) {
    char path[256];
    struct stat st;

    /* Verify connector exists */
    snprintf(path, sizeof(path), "/sys/class/drm/%s", connector);
    if (stat(path, &st) != 0) {
        fprintf(stderr, "Connector not found: %s\n", connector);
        return -1;
    }

    /* Skip if a real monitor is already connected */
    char status[32];
    snprintf(path, sizeof(path), "/sys/class/drm/%s/status", connector);
    if (read_file(path, status, sizeof(status)) == 0) {
        if (strcmp(status, "connected") == 0) {
            printf("%s already has a real monitor connected, skipping\n",
                   connector);
            return 0;
        }
    }

    printf("Setting up virtual monitor on %s (%s)\n",
           connector, preset->name);

    /* 1. Install EDID to firmware dir (needed for kernel param fallback) */
    install_edid_to_firmware(preset->data);

    /* 2. Force connector status to "on" */
    snprintf(path, sizeof(path), "/sys/class/drm/%s/status", connector);
    if (write_file(path, "on", 2) < 0)
        fprintf(stderr, "Warning: could not force connector status\n");
    else
        printf("Connector status forced to 'on'\n");

    /* 3. Try sysfs EDID injection */
    snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", connector);
    int fd = open(path, O_WRONLY);
    int sysfs_ok = 0;

    if (fd >= 0) {
        ssize_t written = write(fd, preset->data, 128);
        close(fd);
        if (written == 128) {
            printf("EDID injected via sysfs\n");
            sysfs_ok = 1;
        } else {
            fprintf(stderr, "sysfs EDID write incomplete\n");
        }
    } else {
        fprintf(stderr, "sysfs EDID write not supported by driver\n");
    }

    /* 4. If sysfs failed, patch the bootloader */
    if (!sysfs_ok) {
        printf("Falling back to kernel parameter injection...\n");

        switch (detect_bootloader()) {
            case BOOTLOADER_GRUB:
                printf("Detected GRUB\n");
                patch_grub(connector);
                break;
            case BOOTLOADER_SYSTEMD_BOOT:
                printf("Detected systemd-boot\n");
                patch_systemd_boot(connector);
                break;
            case BOOTLOADER_UNKNOWN:
                fprintf(stderr,
                    "Unknown bootloader. Add this kernel param manually:\n"
                    "  drm.edid_firmware=%s:edid/virtual-monitor.bin\n\n",
                    bare_connector(connector));
                break;
        }
    }

    /* 5. Trigger hotplug so compositor notices */
    snprintf(path, sizeof(path), "/sys/class/drm/%s/uevent", connector);
    write_file(path, "change", 6);
    printf("Hotplug event triggered\n");

    /* 6. Install udev rule for persistence */
    if (install_udev) {
        install_udev_rule(connector);
    }

    printf("\nDone. If your compositor doesn't detect the monitor,\n");
    printf("add this to your compositor config:\n");

    if (strstr(connector, "DP"))
        printf("  monitor = DP-1, %ux%u@%u, auto, 1\n",
               preset->width, preset->height, preset->refresh);
    else
        printf("  monitor = HDMI-A-1, %ux%u@%u, auto, 1\n",
               preset->width, preset->height, preset->refresh);

    return 0;
}

static int remove_virtual_monitor(const char *connector) {
    char path[256];

    /* Reset connector status */
    snprintf(path, sizeof(path), "/sys/class/drm/%s/status", connector);
    write_file(path, "detect", 6);

    /* Clear sysfs EDID */
    snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", connector);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, "", 0);
        close(fd);
    }

    /* Trigger hotplug */
    snprintf(path, sizeof(path), "/sys/class/drm/%s/uevent", connector);
    write_file(path, "change", 6);

    /* Remove udev rule */
    remove_udev_rule();

    printf("Virtual monitor removed from %s\n", connector);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

static void usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  -c <connector>    Connector, e.g. card1-DP-1 (auto-detect if omitted)\n");
    printf("  -r <WxH@HZ>       Resolution (default: 1920x1080@60)\n");
    printf("  --remove          Remove virtual monitor\n");
    printf("  --list            List all connectors and their status\n");
    printf("  --no-udev         Skip udev rule installation\n");
    printf("  --apply <conn>    Internal: called by udev rule\n");
    printf("\nAvailable resolutions:\n");
    for (size_t i = 0; i < EDID_PRESET_COUNT; i++)
        printf("  %s\n", EDID_PRESETS[i].name);
}

int main(int argc, char *argv[]) {
    char connector[64] = {0};
    unsigned int width = 1920, height = 1080, refresh = 60;
    int do_remove   = 0;
    int do_list     = 0;
    int install_udev = 1;
    int is_udev_apply = 0;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            do_list = 1;
        } else if (strcmp(argv[i], "--remove") == 0) {
            do_remove = 1;
        } else if (strcmp(argv[i], "--no-udev") == 0) {
            install_udev = 0;
        } else if (strcmp(argv[i], "--apply") == 0) {
            /* Called internally by udev — next arg is connector */
            is_udev_apply = 1;
            if (i + 1 < argc)
                strncpy(connector, argv[++i], sizeof(connector) - 1);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            strncpy(connector, argv[++i], sizeof(connector) - 1);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            /* Parse WxH@HZ */
            sscanf(argv[++i], "%ux%u@%u", &width, &height, &refresh);
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    if (do_list) {
        cmd_list_connectors();
        return 0;
    }

    /* Must be root for sysfs writes */
    if (geteuid() != 0) {
        fprintf(stderr, "This tool must be run as root (sudo %s)\n", argv[0]);
        return 1;
    }

    /* Auto-detect connector if not specified */
    if (connector[0] == '\0') {
        if (auto_detect_connector(connector, sizeof(connector)) != 0) {
            fprintf(stderr,
                "No disconnected external connector found.\n"
                "Use --list to see available connectors.\n");
            return 1;
        }
        printf("Auto-detected connector: %s\n", connector);
    }

    if (do_remove) {
        return remove_virtual_monitor(connector);
    }

    /* Find EDID preset */
    const EDIDPreset *preset = find_edid(width, height, refresh);
    if (!preset) {
        fprintf(stderr, "No EDID preset for %ux%u@%u\n",
                width, height, refresh);
        fprintf(stderr, "Available presets:\n");
        for (size_t i = 0; i < EDID_PRESET_COUNT; i++)
            fprintf(stderr, "  %s\n", EDID_PRESETS[i].name);
        return 1;
    }

    /* Validate checksum */
    if (!validate_edid(preset->data)) {
        fprintf(stderr, "EDID checksum invalid for preset %s\n", preset->name);
        return 1;
    }

    /* When called from udev, skip udev rule installation (already installed) */
    if (is_udev_apply) install_udev = 0;

    return setup_virtual_monitor(connector, preset, install_udev);
}