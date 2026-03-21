#include <libdrm/drm.h>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <fstream>


int changeDeviceStatus(const char* connector)
{
	int file_d = open("/sys/class/drm/card1-DP-1/status", O_WRONLY);
	write(file_d, "on", 2);
	close(file_d);
	return 0;
}


int injectEDID(const char* connector, const char* edid_path)
{
	FILE *file = fopen(edid_path, "rb");
	
	if (!file)
	{perror("open edid"); return -1;}

	unsigned char edid[128];

	size_t n = fread(edid, 1, sizeof(edid), file);


	fclose(file);

	if (n != 128)
	{
		printf("EDID must be 128 bytes, got %zu\n", n);
		return -1;
	}

	char path[256];
	snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", connector);
	
	int fd = open(path, O_WRONLY);
	if (fd < 0)
	{
		perror("Opening edid sysfs failed");
		return -1;
	}
	size_t written = write(fd, edid, 128);
	close(fd);
	
	if (written != 128)
	{
		perror("writing edid failed");
		return -1;
	}
	return 0;
}


int triggerHotPlug(const char* connector)
{
	char path[256];
	snprintf(path, sizeof(path), "/sys/class/drm/%s/uevent", connector);
	
	int f = open(path, O_WRONLY);

	if (f < 0)
	{
		perror("opening uevent failed");
		return -1;
	}
	write(f, "change", 6);
	close(f);
	return 0;
}

int main(void) {
    const char *connector = "card1-DP-1";
    const char *edid_file = "./edids/1920x1080.bin";

    if (changeDeviceStatus(connector) < 0)   return 1;
    if (injectEDID(connector, edid_file) < 0) return 1;
    if (triggerHotPlug(connector) < 0)   return 1;

    printf("Virtual monitor set up on %s\n", connector);
    return 0;
}
