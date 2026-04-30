# Recool Development Roadmap

This document outlines planned features inspired by Microsoft Recall for the Recool screen recorder.

## Microsoft Recall Feature Inspiration

Microsoft Recall is a Windows 11 AI-powered feature that captures periodic screenshots (snapshots) and uses
semantic search to help users find past content. Below are features to consider implementing in Recool.

---

## Core Features

### 1. Semantic Search with OCR
**Description:**
- Implement optical character recognition (OCR) on captured frames
- Enable text-based search through historical recordings
- Support fuzzy/semantic search (e.g., "pizza recipe" finds "thin crust pizza")
- Index both text and visual content for retrieval
- Provide text and visual match results sorted by relevance
- **Resource-efficient approach:**
  - Process OCR only during system idle time (background priority)
  - Use perceptual hashing to skip duplicate frames
  - Downscale high-res frames before OCR
  - Cache OCR results to avoid reprocessing
  - Consider on-demand OCR (when user searches) vs pre-indexing

**Test Cases:**
- Search for exact text visible in recorded frames returns correct timestamp
- Partial text search finds relevant frames (e.g., "pizza" matches "thin crust pizza")
- Search with typos returns close matches
- Non-existent text returns empty results
- Search performance scales with database size (test with 1000+ frames)
- OCR accuracy tested on various fonts, sizes, and screen resolutions
- **Performance tests:**
  - OCR background process uses <10% CPU on average
  - Duplicate frame detection reduces OCR workload by >50%
  - System remains responsive during background indexing
  - Memory usage stays under 200MB during OCR processing

---

### 2. Timeline Navigation Interface
**Description:**
- Build explorable timeline view showing recording history
- Segment timeline by activity blocks (when content changed)
- Hover preview of frames without loading full video
- Jump to specific moments by clicking timeline segments
- Display date/time markers along timeline

**Test Cases:**
- Timeline accurately shows all recording sessions
- Hover preview loads within 500ms
- Click on timeline jumps to exact frame timestamp
- Timeline segments align with actual content changes
- Date/time labels are correctly positioned
- Timeline handles multi-day recordings correctly
- Empty periods (no recording) are visually distinct

---

### 3. Content Filtering and Privacy Controls
**Description:**
- Allow users to filter specific applications from being recorded
- Support website filtering for supported browsers (Edge, Firefox, Chrome, Opera)
- Detect and filter private browsing sessions automatically
- Filter sensitive information (passwords, credit cards) using pattern detection
- Provide system tray indicator when filtering is active
- Support bulk deletion of filtered content

**Test Cases:**
- Added apps do not appear in any recordings
- Filtered websites are not captured in foreground tabs
- Private/incognito browser windows are never recorded
- Password input fields trigger automatic filtering
- Credit card numbers are detected and frames skipped
- System tray shows filter badge when filter is active
- Bulk delete removes all frames containing specific app/website
- Filter rules persist across application restarts
- DRM-protected content (Netflix, etc.) is never captured

---

### 4. Snapshot Management and Storage Controls
**Description:**
- Configurable storage limits (e.g., 25GB, 50GB, 75GB, 150GB)
- Automatic deletion of oldest snapshots when limit reached
- Configurable retention period (30/60/90/180 days or unlimited)
- Manual deletion options: all snapshots, or by timeframe (hour/day/week/month)
- Storage usage statistics and visualization
- Per-snapshot deletion from search results

**Test Cases:**
- Storage limit enforced, oldest frames deleted first
- Retention period deletes frames older than configured age
- "Delete all" removes entire recording database
- Timeframe deletion (e.g., "past 24 hours") removes correct subset
- Storage usage display updates in real-time
- Individual snapshot deletion works from search results
- Deletion is immediate and frees disk space
- System notifies when approaching storage limit

---

### 5. Interactive Content Tools (Click to Do)
**Description:**
- Enable interaction with static frames/screenshots
- Copy text directly from recorded frames
- Extract images from frames for editing
- Open URLs detected in frames
- Copy detected content to clipboard
- Integration with system text editors and image tools

**Test Cases:**
- Text selection from frame matches original content
- Copied text preserves formatting where possible
- URL detection identifies all visible links
- Opening URL launches correct application
- Image extraction preserves original quality
- Integration with external apps (Notepad, GIMP, etc.) works
- Copy operation completes within 200ms

---

### 6. Windows Hello / Biometric Authentication
**Description:**
- Require authentication before viewing recordings
- Just-in-time decryption protected by biometric auth
- Encrypt recording database and search index
- Support fingerprint, face recognition, or PIN fallback
- Re-authentication required after timeout period

**Test Cases:**
- Application launches only after successful authentication
- Failed authentication blocks access completely
- Encrypted database cannot be read without authentication
- Re-authentication prompt appears after 15 minutes idle
- Biometric failure falls back to PIN correctly
- Database remains encrypted at rest
- Multiple failed attempts lock application temporarily

---

### 7. Snapshot Export Functionality
**Description:**
- Export selected snapshots to external directory
- Export in multiple formats (PNG, MP4 video compilation)
- Preserve metadata (timestamp, application context)
- Batch export capabilities
- Export with or without sensitive content filtering

**Test Cases:**
- Single snapshot exports to correct format
- Batch export of 100+ snapshots completes successfully
- Exported files include timestamp metadata
- Video compilation maintains chronological order
- Export respects filtering rules (no sensitive data in export)
- Export progress indicator updates correctly
- Exported files are readable by standard viewers

---

## Advanced Features

### 8. Smart Contextual Homepage
**Description:**
- Personalized homepage showing recent activity
- Quick access to frequently accessed recordings
- Suggestions based on usage patterns
- Application usage statistics
- Optional: disable for privacy (show search bar only)

**Test Cases:**
- Homepage shows recordings from past 24 hours
- Most-accessed recordings appear at top
- Usage statistics accurately reflect recording time per app
- Privacy mode shows only search bar
- Homepage loads within 1 second
- Clicking suggestion navigates to correct recording

---

### 9. Multi-Monitor Support
**Description:**
- Detect and record all connected displays
- Per-monitor filtering rules
- Timeline shows which monitor had activity
- Search across all monitors or filter by specific display

**Test Cases:**
- All monitors are recorded simultaneously
- Per-monitor filtering works independently
- Timeline indicates monitor source correctly
- Search can be scoped to specific monitor
- Hotplug events (monitor added/removed) handled gracefully
- Recording continues if one monitor disconnects

---

### 10. Keyboard Shortcuts
**Description:**
- Global hotkey to open application (default: Win+J)
- Quick pause/resume recording
- Quick search activation
- Navigate timeline with keyboard
- Copy/delete selected frame shortcuts

**Test Cases:**
- Win+J opens application from any context
- Pause hotkey stops recording immediately
- Search hotkey focuses search bar
- Arrow keys navigate timeline correctly
- Ctrl+C copies selected frame
- Delete key removes selected frame after confirmation
- Hotkeys don't conflict with system shortcuts

---

### 11. Remote Desktop Session Filtering
**Description:**
- Automatically detect and skip remote desktop sessions
- Support for RDP, VNC, VMConnect, Azure Virtual Desktop
- Detect RAIL (Remote Application) windows
- Configurable: allow or block remote session recording

**Test Cases:**
- RDP sessions are never recorded by default
- VNC sessions filtered correctly
- RAIL windows detected and skipped
- Configuration option allows recording when explicitly enabled
- Remote session detection doesn't break local recording
- Works with screen capture protection enabled

---

### 12. Sensitive Information AI Filtering
**Description:**
- On-device ML model to detect sensitive data patterns
- Pattern matching for: passwords, credit cards, SSN, API keys
- Visual detection of password fields and masked inputs
- No data leaves the device (local processing only)
- Configurable sensitivity levels
- **Lightweight implementation:**
  - Prioritize regex patterns over ML models (zero overhead)
  - Use Hyperscan for multi-pattern matching (fast, low memory)
  - ML model only as fallback, must be <10MB
  - Consider simple heuristics: password fields via accessibility APIs
  - Skip ML entirely if pattern matching sufficient

**Test Cases:**
- Password input fields trigger frame skip
- Credit card number format (####-####-####-####) detected
- API key patterns (40-character hex strings) recognized
- Social security numbers (###-##-####) filtered
- False positive rate below 5% on normal content
- Detection latency under 50ms per frame
- Model runs entirely offline
- **Performance tests:**
  - Pattern matching adds <1% CPU overhead
  - ML model (if used) inference <20ms per frame
  - Memory footprint increase <30MB when active

---

## Implementation Priority

**Note:** All phases must maintain <5% CPU and <200MB RAM constraints during operation.

1. **Phase 1 (MVP - Resource Efficiency Focus):**
   - OCR and text search (Feature 1) - background processing only
   - Timeline navigation (Feature 2) - memory-mapped files
   - Basic storage management (Feature 4)
   - Implement perceptual hashing for duplicate detection
   - Process separation: capture, indexer, GUI

2. **Phase 2 (Privacy - Minimal Overhead):**
   - Content filtering (Feature 3) - simple window title matching
   - Authentication/encryption (Feature 6) - hardware AES-NI
   - Sensitive info filtering (Feature 12) - regex patterns first, ML optional

3. **Phase 3 (Usability - On-Demand Features):**
   - Interactive content tools (Feature 5) - only when GUI active
   - Snapshot export (Feature 7) - background job
   - Keyboard shortcuts (Feature 10) - zero overhead

4. **Phase 4 (Advanced - Optional, Feature-Gated):**
   - Smart homepage (Feature 8) - compile-time optional
   - Multi-monitor support (Feature 9) - linear scaling per monitor
   - Remote desktop filtering (Feature 11) - simple process name check

---

## Technical Considerations

### Critical Constraints: Minimal CPU & Memory Usage

**Design Philosophy:**
Recool is designed for 24/7 operation with near-zero system impact. All features must respect these constraints.

**CPU Usage Targets:**
- **Idle recording**: <1% CPU usage on modern systems
- **Peak (during capture)**: <5% CPU usage
- **OCR processing**: Background only, throttled to <10% CPU
- **Search operations**: <20% CPU burst, max 2 seconds

**Memory Usage Targets:**
- **Base footprint**: <50MB RAM
- **During recording**: <100MB RAM
- **OCR/indexing**: <200MB peak (background process)
- **Search/UI**: <150MB additional when GUI active

**Optimization Strategies:**

1. **Lazy Processing Architecture:**
   - Capture frames at 1fps (already minimal)
   - Queue OCR jobs to background worker process
   - Process OCR only during system idle time
   - Use nice/ionice to deprioritize background tasks

2. **Incremental Indexing:**
   - Index new frames in batches (e.g., every 100 frames)
   - Avoid reprocessing entire database
   - Use SQLite WAL mode for concurrent reads during indexing
   - Pause indexing during high system load

3. **Memory-Mapped Storage:**
   - Use mmap() for video files to avoid loading in RAM
   - Stream frames directly from disk during playback
   - Keep only thumbnail cache in memory (<10MB)
   - LRU eviction for preview thumbnails

4. **Efficient OCR:**
   - Downscale frames to 720p before OCR (if higher resolution)
   - Use lightweight OCR: Tesseract with fast config
   - Alternative: ppocr-mobile (smaller model, faster)
   - Process only changed regions (diff detection)
   - Skip OCR on duplicate frames (perceptual hash check)

5. **Smart Capture Logic:**
   - Use perceptual hashing to detect duplicate frames
   - Skip encoding identical frames (store reference only)
   - Reduce capture rate during idle periods (screen unchanged)
   - Pause recording during screensaver/lock

6. **Hardware Acceleration:**
   - Leverage VAAPI for encoding (already implemented)
   - Use GPU for OCR if available (OpenCL/CUDA for Tesseract)
   - Hardware-accelerated image scaling (VA-API scale)
   - Offload crypto to AES-NI when available

7. **Deferred Processing:**
   - OCR only when user searches (on-demand)
   - Build index incrementally in background
   - Cache OCR results to avoid reprocessing
   - Prioritize recent frames for indexing

8. **Lightweight Dependencies:**
   - Avoid heavy ML frameworks for sensitive data detection
   - Use regex patterns instead where possible
   - Consider Hyperscan for fast pattern matching
   - Keep dependencies minimal (no Electron, no Python runtime)

9. **Process Separation:**
   - Main capture process: minimal, always running
   - Indexer process: background, low priority
   - GUI process: on-demand, separate from capture
   - IPC via shared memory or DBus (low overhead)

10. **Resource Monitoring:**
    - Self-throttle if system load >80%
    - Pause indexing during high CPU usage
    - Reduce capture quality if storage I/O saturated
    - Emit warnings if memory usage exceeds targets

### Database Schema & Migrations

**SQLite as Central Data Store:**
All captured information, metadata, and indexes stored in a single SQLite database for simplicity and
efficiency.

**Database File Location:**
- `~/.local/share/recool/recool.db`
- WAL mode enabled for concurrent reads during indexing
- Auto-vacuum for efficient space management

**Schema Migration System:**
Migrations tracked in `schema_migrations` table with version numbers. Each migration is atomic and
idempotent.

**Migration List:**

```sql
-- Migration 001: Initial schema
CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at INTEGER NOT NULL  -- Unix timestamp
);

CREATE TABLE IF NOT EXISTS recordings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    start_time INTEGER NOT NULL,      -- Unix timestamp
    end_time INTEGER,                  -- NULL if still recording
    file_path TEXT NOT NULL,           -- Path to MP4 file
    resolution TEXT NOT NULL,          -- e.g., "1920x1080"
    original_resolution TEXT NOT NULL, -- Before scaling
    duration_seconds INTEGER,
    file_size_bytes INTEGER,
    monitor_id INTEGER DEFAULT 0,      -- For multi-monitor support
    created_at INTEGER NOT NULL
);
CREATE INDEX idx_recordings_time ON recordings(start_time, end_time);

-- Migration 002: Frame metadata
CREATE TABLE IF NOT EXISTS frames (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    recording_id INTEGER NOT NULL,
    timestamp INTEGER NOT NULL,        -- Unix timestamp of capture
    offset_ms INTEGER NOT NULL,        -- Millisecond offset in video
    perceptual_hash TEXT,              -- For duplicate detection
    is_duplicate BOOLEAN DEFAULT 0,    -- References previous frame
    duplicate_of_frame_id INTEGER,     -- FK to frames.id if duplicate
    window_title TEXT,                 -- Active window title
    application_name TEXT,             -- Process name
    application_path TEXT,             -- Full executable path
    url TEXT,                          -- Detected browser URL (if applicable)
    created_at INTEGER NOT NULL,
    FOREIGN KEY(recording_id) REFERENCES recordings(id) ON DELETE CASCADE
);
CREATE INDEX idx_frames_recording ON frames(recording_id);
CREATE INDEX idx_frames_timestamp ON frames(timestamp);
CREATE INDEX idx_frames_app ON frames(application_name);
CREATE INDEX idx_frames_hash ON frames(perceptual_hash);

-- Migration 003: OCR full-text search
CREATE VIRTUAL TABLE IF NOT EXISTS frame_text USING fts5(
    frame_id UNINDEXED,
    text_content,
    content='',                        -- External content table
    tokenize='porter unicode61'       -- Better multilingual support
);
CREATE TABLE IF NOT EXISTS frame_text_metadata (
    frame_id INTEGER PRIMARY KEY,
    ocr_processed_at INTEGER,          -- Unix timestamp
    ocr_language TEXT DEFAULT 'eng',   -- Tesseract language code
    text_confidence REAL,              -- Average OCR confidence (0-100)
    word_count INTEGER,
    FOREIGN KEY(frame_id) REFERENCES frames(id) ON DELETE CASCADE
);
CREATE INDEX idx_ocr_processed ON frame_text_metadata(ocr_processed_at);

-- Migration 004: Privacy filters
CREATE TABLE IF NOT EXISTS filter_rules (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    rule_type TEXT NOT NULL,           -- 'app', 'website', 'window_title_pattern'
    pattern TEXT NOT NULL,             -- App name, URL pattern, or regex
    filter_mode TEXT NOT NULL,         -- 'skip_recording', 'blur', 'delete_after'
    is_enabled BOOLEAN DEFAULT 1,
    created_at INTEGER NOT NULL
);
CREATE INDEX idx_filter_type ON filter_rules(rule_type, is_enabled);

CREATE TABLE IF NOT EXISTS filtered_frames (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    frame_id INTEGER NOT NULL,
    filter_rule_id INTEGER NOT NULL,
    action_taken TEXT NOT NULL,        -- 'skipped', 'blurred', 'deleted'
    detected_at INTEGER NOT NULL,
    FOREIGN KEY(frame_id) REFERENCES frames(id) ON DELETE CASCADE,
    FOREIGN KEY(filter_rule_id) REFERENCES filter_rules(id) ON DELETE CASCADE
);
CREATE INDEX idx_filtered_frames ON filtered_frames(frame_id);

-- Migration 005: Sensitive information detections
CREATE TABLE IF NOT EXISTS sensitive_detections (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    frame_id INTEGER NOT NULL,
    detection_type TEXT NOT NULL,      -- 'password_field', 'credit_card', 'ssn', 'api_key'
    confidence REAL NOT NULL,          -- 0.0 - 1.0
    bounding_box TEXT,                 -- JSON: {"x":0,"y":0,"w":100,"h":20}
    action_taken TEXT NOT NULL,        -- 'frame_skipped', 'region_redacted'
    detected_at INTEGER NOT NULL,
    FOREIGN KEY(frame_id) REFERENCES frames(id) ON DELETE CASCADE
);
CREATE INDEX idx_sensitive_type ON sensitive_detections(detection_type);

-- Migration 006: Storage management
CREATE TABLE IF NOT EXISTS storage_stats (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    computed_at INTEGER NOT NULL,
    total_recordings INTEGER NOT NULL,
    total_frames INTEGER NOT NULL,
    total_size_bytes INTEGER NOT NULL,
    oldest_recording_time INTEGER,
    newest_recording_time INTEGER
);

-- Migration 007: User preferences
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at INTEGER NOT NULL
);
-- Default settings
INSERT OR IGNORE INTO settings VALUES 
    ('max_storage_bytes', '26843545600', strftime('%s', 'now')),  -- 25GB default
    ('max_retention_days', '90', strftime('%s', 'now')),
    ('ocr_enabled', 'true', strftime('%s', 'now')),
    ('ocr_mode', 'background', strftime('%s', 'now')),  -- 'background', 'on_demand', 'disabled'
    ('sensitive_filter_enabled', 'true', strftime('%s', 'now')),
    ('capture_interval_ms', '1000', strftime('%s', 'now'));

-- Migration 008: Search history & analytics
CREATE TABLE IF NOT EXISTS search_history (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    query TEXT NOT NULL,
    result_count INTEGER NOT NULL,
    search_duration_ms INTEGER NOT NULL,
    searched_at INTEGER NOT NULL
);
CREATE INDEX idx_search_time ON search_history(searched_at);

CREATE TABLE IF NOT EXISTS frame_access_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    frame_id INTEGER NOT NULL,
    access_type TEXT NOT NULL,         -- 'viewed', 'copied', 'exported', 'deleted'
    accessed_at INTEGER NOT NULL,
    FOREIGN KEY(frame_id) REFERENCES frames(id) ON DELETE CASCADE
);
CREATE INDEX idx_access_log ON frame_access_log(frame_id, accessed_at);

-- Migration 009: Export tracking
CREATE TABLE IF NOT EXISTS exports (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    export_format TEXT NOT NULL,       -- 'png', 'mp4', 'zip'
    frame_count INTEGER NOT NULL,
    destination_path TEXT NOT NULL,
    file_size_bytes INTEGER,
    started_at INTEGER NOT NULL,
    completed_at INTEGER,
    status TEXT NOT NULL               -- 'in_progress', 'completed', 'failed'
);

CREATE TABLE IF NOT EXISTS export_frames (
    export_id INTEGER NOT NULL,
    frame_id INTEGER NOT NULL,
    PRIMARY KEY(export_id, frame_id),
    FOREIGN KEY(export_id) REFERENCES exports(id) ON DELETE CASCADE,
    FOREIGN KEY(frame_id) REFERENCES frames(id)
);

-- Migration 010: Authentication & encryption metadata
CREATE TABLE IF NOT EXISTS auth_config (
    id INTEGER PRIMARY KEY CHECK (id = 1), -- Single row
    encryption_enabled BOOLEAN DEFAULT 0,
    auth_method TEXT,                  -- 'pin', 'biometric', 'none'
    encryption_salt BLOB,              -- For key derivation
    encryption_iv BLOB,                -- Initialization vector
    last_auth_at INTEGER,
    failed_attempts INTEGER DEFAULT 0,
    locked_until INTEGER               -- NULL or Unix timestamp
);

-- Migration 011: Thumbnail cache
CREATE TABLE IF NOT EXISTS thumbnails (
    frame_id INTEGER PRIMARY KEY,
    thumbnail_data BLOB NOT NULL,      -- JPEG compressed, 160x90
    created_at INTEGER NOT NULL,
    FOREIGN KEY(frame_id) REFERENCES frames(id) ON DELETE CASCADE
);

-- Migration 012: Performance indexes
CREATE INDEX idx_frames_duplicate ON frames(is_duplicate, duplicate_of_frame_id);
CREATE INDEX idx_frames_composite ON frames(recording_id, timestamp, application_name);
CREATE INDEX idx_recordings_monitor ON recordings(monitor_id, start_time);
```

**Migration Application Code (C):**
```c
// Migration function signature
typedef int (*migration_fn)(sqlite3 *db);

typedef struct {
    int version;
    const char *description;
    migration_fn apply;
} migration_t;

int apply_migrations(sqlite3 *db) {
    // Get current version
    int current_version = get_schema_version(db);
    
    // Apply each migration in order
    for (int i = 0; i < num_migrations; i++) {
        if (migrations[i].version > current_version) {
            fprintf(stderr, "[INFO] Applying migration %d: %s\n", 
                    migrations[i].version, migrations[i].description);
            
            sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);
            
            int rc = migrations[i].apply(db);
            if (rc != SQLITE_OK) {
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
                fprintf(stderr, "[ERROR] Migration %d failed\n", migrations[i].version);
                return rc;
            }
            
            // Update version
            sqlite3_stmt *stmt;
            sqlite3_prepare_v2(db, "INSERT INTO schema_migrations VALUES (?, ?)", 
                             -1, &stmt, NULL);
            sqlite3_bind_int(stmt, 1, migrations[i].version);
            sqlite3_bind_int64(stmt, 2, time(NULL));
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            
            sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        }
    }
    return SQLITE_OK;
}
```

**Database Optimization Settings:**
```sql
-- Applied on connection open
PRAGMA journal_mode = WAL;           -- Write-Ahead Logging for concurrency
PRAGMA synchronous = NORMAL;         -- Balance safety/performance
PRAGMA cache_size = -64000;          -- 64MB cache
PRAGMA temp_store = MEMORY;          -- Temp tables in RAM
PRAGMA mmap_size = 268435456;        -- 256MB memory-mapped I/O
PRAGMA page_size = 4096;             -- Match filesystem block size
PRAGMA auto_vacuum = INCREMENTAL;    -- Efficient space reclamation
```

**Benefits of SQLite-Centric Design:**
- Single file deployment (easy backup)
- ACID transactions (data integrity)
- FTS5 built-in (no external search engine)
- <50KB RAM overhead
- Zero-configuration
- Concurrent reads via WAL mode
- Efficient indexes for fast queries

**Database Migration Test Cases:**
- Fresh installation creates all tables at latest version
- Migrations apply in correct order (1→2→3...→12)
- Failed migration rolls back transaction (atomic)
- Re-running migrations is idempotent (no duplicates)
- Schema version tracked correctly in schema_migrations table
- Database remains valid after power loss during migration (WAL recovery)
- Upgrade from version N to N+5 applies all intermediate migrations
- Downgrade protection: error if database is newer than binary version

**Database Performance Test Cases:**
- Insert 10,000 frames: <5 seconds
- Full-text search across 100,000 frames: <500ms
- Perceptual hash lookup (duplicate detection): <10ms
- Timeline query (date range): <100ms
- Storage stats computation: <1 second
- Database file size: <1MB per 1,000 frames (metadata only)
- WAL checkpoint doesn't block readers
- Concurrent OCR indexing + user search: no lock contention

**Data Integrity Test Cases:**
- Cascade delete: deleting recording removes all child frames
- Cascade delete: deleting frame removes OCR data, thumbnails, detections
- Foreign key constraints enforced
- No orphaned frames after recording deletion
- Transaction rollback on error (no partial data)
- Settings table prevents invalid values (e.g., negative storage limit)
- Duplicate detection: perceptual_hash uniqueness enforced per recording

### Dependencies to Add

**Core (required):**
- SQLite 3.35+ (FTS5 support, already in most systems)
- OCR engine: Tesseract 5+ (lightweight mode) or ppocr-mobile
- Image hashing: libpHash or simple DCT hash (duplicate detection)

**Optional (feature-gated):**
- GUI framework: GTK4 (native to Linux) or imgui (ultra-lightweight)
- Pattern matching: Hyperscan (for sensitive data detection)
- ML framework: ONNX Runtime (only if AI features enabled)

**Explicitly Avoided:**
- Python runtime (memory overhead)
- Electron/Chromium (massive footprint)
- TensorFlow/PyTorch (too heavy)
- External search engines (Elasticsearch, etc.)

### Performance Targets
- **Capture CPU**: <1% average, <5% peak
- **Capture RAM**: <100MB
- **OCR processing**: <100ms per frame (background only)
- **Search query response**: <500ms for 10,000+ frames
- **Timeline rendering**: 60fps smooth scrolling
- **Storage overhead**: <10% increase vs raw video
- **Startup time**: <500ms (excluding GUI)
- **Database size**: <1MB per 1000 frames (metadata + index)

### Privacy Guarantees
- All data stored locally only
- No telemetry or cloud uploads
- Encryption at rest using user credentials
- Secure deletion (overwrite deleted frames)

---

## References
- Microsoft Recall Documentation: https://support.microsoft.com/windows/retrace-your-steps-with-recall-aa03f8a0-a78b-4b3e-b0a1-2eb8ac48701c
- Privacy Controls: https://support.microsoft.com/windows/privacy-and-control-over-your-recall-experience-d404f672-7647-41e5-886c-a3c59680af15
