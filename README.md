# Virtual Display Project Progress

A system level user space program meant to use connected mobile/tablet devices as secondary displays.

## System Setup
- Hardware: AMD Radeon 680M + NVIDIA RTX 2050
- OS: Arch Linux + Hyprland
- Primary GPU: AMD card1 (/dev/dri/card1)
- Permissions: In video group, can access DRM devices

## Completed Challenges
1. List files in /dev/dri/
2. Open DRM device file
3. First DRM API call (drmGetVersion)
4. Get DRM resources count
5. Listing connector details
6. Made a Virtual Monitor by manipulating files in /sys/class/drm

## Key Learnings
- DRM concepts: CRTC, Encoder, Connector
- AMD GPU has 9 connectors
- Need O_RDWR for DRM operations
- Working with libdrm library

## Next Steps
- Analyze connector details
- Identify virtual display candidates
- Create first virtual display
