#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <libdrm/drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

using namespace std;

int main() 
{
    int fd = open("/dev/dri/card1", O_RDONLY | O_CLOEXEC);

    if (fd < 0) 
    {
        cerr << "Cannot open Device " << endl;
        return 1;
    }

    drmVersionPtr v = drmGetVersion(fd);

    if (!v)
    {
        cerr << "drmGetVersion failed\n";
        close(fd);
        return 1;
    }

    cout << "Driver: " << v -> name << "Kernel: " << v -> version_major << "." <<
    v -> version_minor << "." << v -> version_patchlevel << ", Date:  "<< v -> date << endl;

    cout << "Now Checking Resources\n===========================================" << endl;


    drmModeResPtr resources = drmModeGetResources(fd);


    cout << "Connectors: " << resources -> count_connectors << endl
        << "Encoders: " << resources -> count_encoders << endl
        << "CRTC: " << resources -> count_crtcs << endl
        << "Frame Buffers: " << resources -> count_fbs << endl;



    drmFreeVersion(v);
    close(fd);
    return 0;

}
