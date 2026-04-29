# Recool - Agent Instructions

Single-file C implementation of a Wayland screen recorder for low-resource 24/7 capture.

## Build & Run

```bash
# Verify dependencies first
make check-deps

# Build
make

# Run directly
./recool

# Or build and run
make run

# Install to ~/.local/bin
make install
```

**Important**: The compiler is hardcoded to `clang` in the Makefile (line 5). Build will fail if clang is not
available.

## Dependencies

Required system packages (pkg-config names):
- libpipewire-0.3
- dbus-1
- libavformat, libavcodec, libavutil, libswscale (FFmpeg)

Also requires `clang` compiler. Use `make check-deps` to verify all are present.

## Architecture

**Single-file design**: All code lives in `recool.c` (1446 lines). No headers, no modules, all inline.

Pipeline:
1. D-Bus portal request → user permission dialog
2. PipeWire stream connection → native resolution capture
3. Software scaling (configurable factor, default 0.5x)
4. Hardware-accelerated encoding (VAAPI preferred) → MP4 output

## Configuration Constants

Top of `recool.c` (lines 10-39) contains all tunable parameters:
- `CAPTURE_INTERVAL_MS`: Screenshot frequency (default 1000ms = 1fps)
- `SCALE_FACTOR`: Resolution multiplier (default 0.5 = half resolution)
- `OUTPUT_BASE_DIR`: Relative to HOME (default "Videos/Recools")
- `ENCODER_PRIORITY`: CSV list (default "hevc_vaapi,h264_vaapi,libx265,libx264")
- `VIDEO_CRF`: Quality 0-51 (default 28)
- `VAAPI_DEVICE`: Hardware device (default NULL = auto-detect, or "/dev/dri/renderD128" to force specific)

Change these constants directly in the source; no runtime flags or config files exist.

## Code Conventions

When modifying C code:
- Use inline implementations (no separate headers)
- Configuration changes → edit constants at top of file
- Structures grouped by subsystem (Portal, PipeWire, Scaler, Encoder)
- Functions prefixed by subsystem (e.g., `portal_*`, `encoder_*`)
- Error messages use `[ERROR]`, info uses `[INFO]`, warnings use `[WARNING]`

## Output

Videos written to `~/Videos/Recools/YYYY-MM-DD-HH-MM-SS.mp4` by default. Directory created automatically if
missing.

## Testing

No automated tests. Manual verification:
1. Run `./recool`
2. Grant screen capture permission in dialog
3. Ctrl+C to stop
4. Verify MP4 created in output directory
5. Check video playback

## Common Issues

- **Build fails**: Check `make check-deps` output for missing packages
- **Permission dialog timeout**: Default 30s (line 31), may need increase on slow systems
- **No hardware encoder**: Falls back to software if VAAPI unavailable (unless `REQUIRE_HARDWARE_ACCEL` set)
- **Missing clang**: Makefile requires clang specifically, not gcc
