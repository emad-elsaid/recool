# Recool

Wayland screen recorder designed for 24/7 low-resource capture. Single-file C implementation with hardware-accelerated encoding.

## Features

- **Low resource usage**: 1 FPS capture at 50% native resolution by default
- **Hardware acceleration**: VAAPI/Vulkan encoding support with software fallback
- **24/7 operation**: Optimized for continuous long-running capture
- **Zero configuration**: Sensible defaults, runs out of the box
- **Single binary**: No external config files or dependencies at runtime

## Quick Start

```bash
# Install dependencies (Arch Linux)
sudo pacman -S pipewire dbus ffmpeg clang

# Build and run
make
./recool
```

Grant screen capture permission in the system dialog, then press Ctrl+C to stop recording.

Videos are saved to `~/Videos/Recools/YYYY-MM-DD-HH-MM-SS.mp4`

## Installation

```bash
# Verify all dependencies are present
make check-deps

# Build
make

# Install to ~/.local/bin
make install
```

## Dependencies

**Build time:**
- clang compiler
- pkg-config

**Runtime libraries:**
- libpipewire-0.3
- dbus-1
- FFmpeg (libavformat, libavcodec, libavutil, libswscale)

**Optional (for hardware encoding):**
- VA-API drivers (intel-media-driver, libva-mesa-driver, etc.)
- Vulkan support

## Configuration

Edit constants at the top of `recool.c` (lines 10-39):

```c
// Capture settings
#define CAPTURE_INTERVAL_MS         1000        // Screenshot frequency (1 FPS)
#define SCALE_FACTOR                0.5         // 50% of native resolution

// Output settings
#define OUTPUT_BASE_DIR             "Videos/Recools"
#define OUTPUT_FILENAME_FORMAT      "%Y-%m-%d-%H-%M-%S"

// Encoder settings
#define ENCODER_PRIORITY            "hevc_vaapi,h264_vaapi,libx265,libx264"
#define VIDEO_CRF                   28          // Quality (0=best, 51=worst)
#define VAAPI_DEVICE                NULL        // Auto-detect (or "/dev/dri/renderD128")
```

Rebuild after changes: `make clean && make`

## Architecture

**Pipeline:**
1. D-Bus XDG Desktop Portal → user permission dialog
2. PipeWire stream → native resolution screen capture
3. libswscale → software downscaling
4. FFmpeg encoder → hardware-accelerated encoding (VAAPI/Vulkan) or software fallback
5. MP4 muxer → output file

**Encoder priority:**
1. HEVC VAAPI (best compression, requires hardware)
2. H.264 VAAPI (good compression, widely supported)
3. libx265 (software HEVC, CPU-intensive)
4. libx264 (software H.264, fallback)

## Usage Examples

**Standard capture:**
```bash
./recool
```

**Long-running capture:**
```bash
# Run in background with systemd or tmux
tmux new-session -d -s recool './recool'
```

**Check output:**
```bash
ls -lh ~/Videos/Recools/
```

## Performance

Typical resource usage (1920x1080 @ 50% scale, 1 FPS, VAAPI encoding):
- CPU: <5% on modern processors
- Memory: ~50MB
- Disk: ~10-20MB per hour (depends on screen content)

## Troubleshooting

**Build fails:**
```bash
# Check missing dependencies
make check-deps

# Install missing packages (Arch)
sudo pacman -S pipewire dbus ffmpeg clang
```

**Permission dialog doesn't appear:**
- Ensure you're running under Wayland (not X11)
- Check xdg-desktop-portal is running: `systemctl --user status xdg-desktop-portal`

**No hardware encoder found:**
- Install VA-API drivers: `sudo pacman -S intel-media-driver` (Intel) or `libva-mesa-driver` (AMD)
- Check available devices: `ls -l /dev/dri/render*`
- Software encoding will be used automatically as fallback
- To force a specific GPU in multi-GPU systems, edit `VAAPI_DEVICE` constant

**Permission denied on subsequent runs:**
- Remove saved token: `rm ~/.local/share/recool/portal_token`
- Grant permission again when prompted

## Technical Details

- **Language**: C11 with POSIX extensions
- **Build system**: Make
- **Lines of code**: ~1450 (single file)
- **Wayland protocols**: XDG Desktop Portal ScreenCast
- **Video format**: MP4 container, HEVC/H.264 codec
- **Color space**: BGRx/BGRA input → NV12/YUV420P encoding

## License

See source file header for license information.

## Contributing

This is a minimal single-file implementation. When contributing:
- Keep all code in `recool.c` (no separate headers)
- Maintain subsystem prefixes (`portal_*`, `pipewire_*`, `encoder_*`, `scaler_*`)
- Use `[INFO]`, `[WARNING]`, `[ERROR]` log prefixes
- Test with both hardware and software encoding paths

## See Also

- `AGENTS.md` - Additional context for automated agents and contributors
