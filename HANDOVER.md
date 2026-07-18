# Handover Documentation

## Changes Made
- Modified [snap/snapcraft.yaml](file:///home/berat/Documents/Projects/AetherTFTP/snap/snapcraft.yaml) to add the environment variable `QT_QPA_PLATFORM: "wayland;xcb"` under the `aethertftp` application block.
- This ensures that if the snap application runs on a Wayland-based host environment where native Wayland initialization (`wayland-egl`) fails, it gracefully falls back to X11 (via XWayland) instead of crashing.

## Verification
- Running the app with `QT_QPA_PLATFORM=xcb aethertftp --help` runs correctly without crashing.
- Once the snap is rebuilt with these configuration changes, the snap runner wrapper will automatically apply this variable.
