# Recool

Wayland screen recorder designed for 24/7 low-resource capture. Single-file C implementation with hardware-accelerated encoding.

## Features

- **Low resource usage**: 1 FPS capture at 50% native resolution by default
- **Hardware acceleration**: VAAPI/Vulkan encoding support with software fallback
- **24/7 operation**: Optimized for continuous long-running capture
- **Zero configuration**: Sensible defaults, runs out of the box
- **Single binary**: No external config files or dependencies at runtime
- **SQLite database**: Metadata tracking and frame indexing
- **Background OCR**: Automatic text extraction with low priority processing
- **Perceptual hashing**: Duplicate detection reduces OCR workload by >90%
- **Progress tracking**: OCR resumes from where it stopped on restart
- **Full-text search**: FTS5-powered search through all captured text

## Quick Start

```bash
# Install dependencies (Arch Linux)
sudo pacman -S pipewire dbus ffmpeg sqlite clang tesseract tesseract-data-eng leptonica

# Build and run
make
./recool
```

Grant screen capture permission in the system dialog, then press Ctrl+C to stop recording.

Videos are saved to `~/Videos/Recools/YYYY-MM-DD-HH-MM-SS.mp4`

Database stored at `~/.local/share/recool/recool.db`

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
- sqlite3
- tesseract (OCR engine)
- leptonica (image processing)

**Optional (for hardware encoding):**
- VA-API drivers (intel-media-driver, libva-mesa-driver, etc.)
- Vulkan support

## Configuration

Edit constants at the top of `recool.c` (lines 10-48):

```c
// Capture settings
#define CAPTURE_INTERVAL_MS         1000        // Screenshot frequency (1 FPS)
#define SCALE_FACTOR                0.5         // 50% of native resolution

// Output settings
#define OUTPUT_BASE_DIR             "Videos/Recools"
#define OUTPUT_FILENAME_FORMAT      "%Y-%m-%d-%H-%M-%S"

// Database settings
#define DATABASE_DIR                ".local/share/recool"
#define DATABASE_FILENAME           "recool.db"

// OCR settings
#define OCR_ENABLED                 1           // Enable background OCR
#define OCR_THREAD_PRIORITY         19          // Nice priority (0-19, higher = lower)
#define OCR_PROCESS_DELAY_MS        500         // Delay between frames
#define OCR_LANGUAGE                "eng"       // Tesseract language

// Perceptual hashing settings
#define PHASH_ENABLED               1           // Enable duplicate detection
#define PHASH_THRESHOLD             8           // Hamming distance (0-64, lower = stricter)

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
6. SQLite database → metadata tracking and frame indexing
7. Background OCR thread → text extraction with duplicate detection (low priority)
   - Perceptual hashing (dHash) → identify duplicate frames
   - Tesseract OCR → extract text from unique frames only
   - FTS5 indexing → full-text search capability

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
- **Capture CPU**: <5% on modern processors
- **OCR CPU**: <10% (background thread, nice priority 19)
- **OCR efficiency**: >90% workload reduction via duplicate detection
- **Memory**: ~80MB (capture) + ~100MB (OCR processing)
- **Disk**: ~10-20MB per hour (video) + ~1MB per 1000 frames (database)

## Troubleshooting

**Build fails:**
```bash
# Check missing dependencies
make check-deps

# Install missing packages (Arch)
sudo pacman -S pipewire dbus ffmpeg sqlite clang tesseract tesseract-data-eng leptonica
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

**OCR not working:**
- Install tesseract language data: `sudo pacman -S tesseract-data-eng`
- For other languages, edit `OCR_LANGUAGE` constant and install corresponding package
- Check available languages: `tesseract --list-langs`
- Disable OCR if not needed: set `OCR_ENABLED` to `0` in recool.c

## Technical Details

- **Language**: C11 with POSIX extensions
- **Build system**: Make
- **Lines of code**: ~2400 (single file)
- **Binary size**: 80K compiled
- **Wayland protocols**: XDG Desktop Portal ScreenCast
- **Video format**: MP4 container, HEVC/H.264 codec
- **Color space**: BGRx/BGRA input → NV12/YUV420P encoding
- **Database**: SQLite 3.35+ with FTS5 for full-text search
- **OCR Engine**: Tesseract 5+ with Leptonica image processing
- **Perceptual hashing**: dHash algorithm (64-bit, Hamming distance comparison)
- **Threading**: POSIX threads for background OCR processing

## License

See source file header for license information.

## Contributing

This is a minimal single-file implementation. When contributing:
- Keep all code in `recool.c` (no separate headers)
- Maintain subsystem prefixes (`portal_*`, `pipewire_*`, `encoder_*`, `scaler_*`, `database_*`, `ocr_*`)
- Use `[INFO]`, `[WARNING]`, `[ERROR]` log prefixes
- Test with both hardware and software encoding paths
- Verify resource constraints: <5% CPU capture, <10% OCR, <200MB RAM
- Follow incremental development strategy (see AGENTS.md)

## See Also

- `AGENTS.md` - Additional context for automated agents and contributors
