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
- sqlite3
- tesseract (OCR engine)
- lept (Leptonica image processing)

Also requires `clang` compiler and `pthread` (POSIX threads). Use `make check-deps` to verify all are present.

## Architecture

**Single-file design**: All code lives in `recool.c` (~2400 lines). No headers, no modules, all inline.

**Pipeline:**
1. D-Bus portal request → user permission dialog
2. PipeWire stream connection → native resolution capture
3. Software scaling (configurable factor, default 0.5x)
4. Hardware-accelerated encoding (VAAPI preferred) → MP4 output
5. SQLite database tracking (metadata and indexing)
6. Background OCR thread → perceptual hashing → text extraction → FTS5 indexing

**Subsystems:**
- Portal: D-Bus communication for screen capture permission
- PipeWire: Frame capture from Wayland compositor
- Scaler: Software frame scaling (libswscale)
- Encoder: Video encoding (FFmpeg with VAAPI acceleration)
- Database: SQLite storage with migrations, metadata tracking, and FTS5 search
- Perceptual Hash: dHash duplicate detection (reduces OCR workload by >90%)
- OCR: Background worker thread for text extraction (Tesseract)

**Data Flow:**
```
Wayland → PipeWire → Scaler → Encoder → MP4 file
                        ↓
                    Database (metadata + frames)
                        ↓
                    OCR Worker Thread:
                      1. Extract frame from video
                      2. Compute perceptual hash (dHash)
                      3. Check for duplicates
                      4. Run Tesseract (unique frames only)
                      5. Store in FTS5 index
```

## Configuration Constants

Top of `recool.c` (lines 10-48) contains all tunable parameters:

**Capture settings:**
- `CAPTURE_INTERVAL_MS`: Screenshot frequency (default 1000ms = 1fps)
- `SCALE_FACTOR`: Resolution multiplier (default 0.5 = half resolution)

**Output settings:**
- `OUTPUT_BASE_DIR`: Relative to HOME (default "Videos/Recools")
- `OUTPUT_FILENAME_FORMAT`: Timestamp format (default "%Y-%m-%d-%H-%M-%S")
- `OUTPUT_FILE_EXTENSION`: File extension (default ".mp4")

**Database settings:**
- `DATABASE_DIR`: Relative to HOME (default ".local/share/recool")
- `DATABASE_FILENAME`: Database filename (default "recool.db")

**OCR settings:**
- `OCR_ENABLED`: Enable background OCR (default 1)
- `OCR_THREAD_PRIORITY`: Nice priority 0-19 (default 19 = lowest CPU priority)
- `OCR_PROCESS_DELAY_MS`: Delay between processing frames (default 500ms)
- `OCR_BATCH_SIZE`: Frames to process before checking stop signal (default 10)
- `OCR_LANGUAGE`: Tesseract language code (default "eng")

**Perceptual hashing settings:**
- `PHASH_ENABLED`: Enable duplicate detection (default 1)
- `PHASH_THRESHOLD`: Hamming distance threshold 0-64 (default 8, lower = stricter)

**Encoder settings:**
- `ENCODER_PRIORITY`: CSV list (default "hevc_vaapi,h264_vaapi,libx265,libx264")
- `VIDEO_CRF`: Quality 0-51 (default 28)
- `VIDEO_PRESET`: Encoding speed (default "ultrafast")
- `KEYFRAME_INTERVAL`: I-frame interval (default 300 frames = 5 minutes at 1fps)

**Hardware acceleration:**
- `VAAPI_DEVICE`: Hardware device (default NULL = auto-detect, or "/dev/dri/renderD128" to force)
- `REQUIRE_HARDWARE_ACCEL`: Exit if no GPU encoder (default 0)

**Portal configuration:**
- `RESTORE_TOKEN_FILE`: Token storage path (default ".local/share/recool/portal_token")
- `DBUS_TIMEOUT_MS`: Permission dialog timeout (default 30000ms)

**System settings:**
- `ENABLE_NICE_PRIORITY`: Run at low priority (default 1, nice +10)
- `SCALE_ALGORITHM`: Scaling method (default SWS_FAST_BILINEAR)

Change these constants directly in the source; no runtime flags or config files exist.

## Code Conventions

When modifying C code:
- Use inline implementations (no separate headers)
- Configuration changes → edit constants at top of file
- Structures grouped by subsystem (Portal, PipeWire, Scaler, Encoder, Database, Perceptual Hash, OCR)
- Functions prefixed by subsystem (e.g., `portal_*`, `encoder_*`, `database_*`, `ocr_*`)
- Error messages use `[ERROR]`, info uses `[INFO]`, warnings use `[WARNING]`

## Resource Management Rules

**CRITICAL: Every allocated resource MUST be tracked and cleaned up properly.**

### Mandatory Practices

1. **Store ALL created objects in context structures**
   - NEVER create objects (malloc, open files, library objects) without storing the handle
   - If a library function returns a pointer/handle, add it to the relevant context struct
   - Example: `pw_context_connect_fd()` returns `pw_core*` → MUST be stored in `PipeWireContext.core`

2. **Initialize context structs to zero**
   - Use `= {0}` or `memset` when declaring context structures
   - Ensures NULL pointers are safe to check in cleanup functions
   - Example: `PipeWireContext pipewire = {0};`

3. **Cleanup functions MUST be symmetric with init functions**
   - For every `*_init()`, write corresponding `*_cleanup()` immediately
   - Destroy resources in REVERSE order of creation
   - Always check for NULL before destroying (idempotent cleanup)

4. **Cleanup order matters**
   - Stop threads/loops FIRST (prevents callbacks on destroyed objects)
   - Destroy child objects before parent objects
   - Disconnect before destroy
   - Example PipeWire cleanup order:
     1. Stop thread loop
     2. Destroy stream
     3. Disconnect core
     4. Destroy context
     5. Destroy loop
     6. Deinitialize library

5. **Test cleanup paths**
   - Run `make test-cleanup` after any resource management changes
   - Verify clean exit with Ctrl+C (SIGINT)
   - Check for segfaults, memory leaks, file descriptor leaks
   - Use valgrind for thorough validation

### Checklist for New Subsystems

When adding a new subsystem with resource allocation:

```
[ ] Context structure defined with ALL resource handles
[ ] Init function creates resources in correct order
[ ] Cleanup function destroys resources in REVERSE order
[ ] Cleanup function checks NULL before each destroy
[ ] Context structure initialized to zero in main()
[ ] Cleanup function called in main() error paths
[ ] Cleanup function called in main() normal exit
[ ] Tested with Ctrl+C (SIGINT)
[ ] Tested with `make test-cleanup`
[ ] No segfaults, no memory leaks, no FD leaks
```

### Common Pitfalls

- **Orphaned objects**: Creating library objects without storing handles → crash on cleanup
- **Wrong cleanup order**: Destroying parent before children → use-after-free crash
- **Missing NULL checks**: Calling destroy on uninitialized pointers → segfault
- **Active callbacks**: Destroying objects while threads still active → race condition crash
- **Circular dependencies**: Not stopping event loops before destroying objects → deadlock

## Output

**Video files:** `~/Videos/Recools/YYYY-MM-DD-HH-MM-SS.mp4` by default. Directory created automatically if
missing.

**Database:** `~/.local/share/recool/recool.db` - SQLite database for metadata and search index. Created
automatically on first run.

## Testing

No automated tests. Manual verification:
1. Run `./recool`
2. Grant screen capture permission in dialog
3. Ctrl+C to stop
4. Verify MP4 created in output directory
5. Check video playback

## Development Strategy

**CRITICAL: Feature-by-Feature Incremental Development**

Recool follows a strict incremental development approach to maintain stability and minimize regressions:

1. **Consult ROADMAP.md** for planned features and priorities
2. **Implement ONE feature at a time** - complete, test, commit before moving to next
3. **Track progress in IMPLEMENTED.md** - update status after each feature
4. **Never mix features** in a single implementation cycle
5. **Test thoroughly** after each feature before proceeding

### Implementation Workflow

For each feature from ROADMAP.md:

```
1. Read ROADMAP.md → Select next feature by priority
2. Plan implementation approach (discuss with user if complex)
3. Implement feature (modify recool.c or add new files as needed)
4. Test manually (document test results)
5. Update documentation to reflect current state:
   - IMPLEMENTED.md: Feature status, test results, deviations
   - AGENTS.md: Architecture, dependencies, configuration if changed
   - README.md: User-facing features, usage examples if applicable
6. Commit changes
7. Return to step 1 for next feature
```

**Documentation Update Requirements:**

After implementing each feature, ensure all documentation is current:

- **IMPLEMENTED.md**: 
  - Add feature entry with status, test results, implementation notes
  - Update "Current Architecture" section if structure changed
  - Update "Next Steps" to reflect what's actually next

- **AGENTS.md**:
  - Update Architecture section if subsystems added/changed
  - Update Configuration Constants if new settings added
  - Update Code Conventions if new patterns introduced
  - Update Priority Order with progress indicators (✅/🔄)
  - Update line count estimates if significantly changed

- **README.md**:
  - Update Features list if user-visible capabilities added
  - Update Dependencies if new runtime libraries required
  - Update Configuration examples if new constants added
  - Update Architecture/Pipeline if data flow changed
  - Update Performance metrics if resource usage changed
  - Update Usage Examples if new commands/flags added
  - Update Technical Details (line count, features)

**Documentation consistency is critical** - outdated docs mislead users and future development.

### Feature Status Tracking

**IMPLEMENTED.md Format:**
```markdown
# Recool - Implemented Features

## [Feature Name] - [Status]
**Implementation Date:** YYYY-MM-DD
**Roadmap Reference:** ROADMAP.md Section X.Y
**Status:** Completed | In Progress | Blocked
**Files Modified:** recool.c lines X-Y, new_file.c

**Description:**
Brief description of what was implemented.

**Test Results:**
- Test case 1: Pass/Fail
- Test case 2: Pass/Fail
- Performance metrics (CPU/RAM)

**Deviations from Roadmap:**
Any changes from original plan.

**Notes:**
Implementation notes, gotchas, future improvements.

---
```

### Rules for Agents

When implementing features:

- **ALWAYS check ROADMAP.md first** to understand the feature scope
- **ALWAYS update IMPLEMENTED.md** after completing a feature
- **NEVER implement multiple features simultaneously**
- **NEVER skip testing** - validate each feature works before moving on
- **MAINTAIN resource constraints**: <5% CPU, <200MB RAM
- **PRESERVE single-file architecture** unless roadmap explicitly calls for modularization
- **FOLLOW migration order** for database schema changes (001, 002, 003...)
- **ASK USER** if feature requirements are ambiguous or conflict with constraints

### Priority Order (from ROADMAP.md)

**Phase 1 (Current Focus):**
1. ✅ SQLite database integration (basic open/close)
2. ✅ Database migrations system (001-003)
3. ✅ Basic OCR indexing (background thread with progress tracking)
4. ✅ Perceptual hashing for duplicate detection (dHash algorithm, >90% OCR reduction)
5. Simple timeline query interface

**Phase 2:**
Privacy features (filters, sensitive data detection)

**Phase 3:**
Usability features (export, keyboard shortcuts)

**Phase 4:**
Advanced features (multi-monitor, smart homepage)
Advanced features (multi-monitor, smart homepage)

### Before Starting ANY Feature

1. Verify current state by reading IMPLEMENTED.md
2. Identify next feature from ROADMAP.md Phase 1
3. Check if prerequisites are complete
4. Confirm resource constraints can be met
5. Get user approval for implementation approach

## Common Issues

- **Build fails**: Check `make check-deps` output for missing packages
- **Permission dialog timeout**: Default 30s (line 31), may need increase on slow systems
- **No hardware encoder**: Falls back to software if VAAPI unavailable (unless `REQUIRE_HARDWARE_ACCEL` set)
- **Missing clang**: Makefile requires clang specifically, not gcc
