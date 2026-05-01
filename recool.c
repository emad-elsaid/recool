/*
 * Recool - Wayland Screen Recorder
 * Single-file implementation for 24/7 low-resource screen recording
 */

// ============================================================================
// CONFIGURATION CONSTANTS
// ============================================================================

// Capture settings
#define CAPTURE_INTERVAL_MS         1000        // Screenshot frequency in milliseconds
#define SCALE_FACTOR                1.0         // Video resolution scale (0.5-1.0, higher = better OCR but larger files)
                                                 // For better OCR: use 0.75 or 1.0
                                                 // For lower storage: use 0.5

// Output settings
#define OUTPUT_BASE_DIR             "Videos/Recools"           // Relative to HOME
#define OUTPUT_FILENAME_FORMAT      "%Y-%m-%d-%H-%M-%S"        // YYYY-MM-DD-HH-MM-SS
#define OUTPUT_FILE_EXTENSION       ".mp4"

// Database settings
#define DATABASE_DIR                ".local/share/recool"      // Relative to HOME
#define DATABASE_FILENAME           "recool.db"

// OCR settings
#define OCR_ENABLED                 1           // Enable background OCR processing
#define OCR_THREAD_PRIORITY         19          // Nice priority (0-19, higher = lower priority)
#define OCR_PROCESS_DELAY_MS        500         // Delay between processing frames
#define OCR_BATCH_SIZE              10          // Frames to process before checking stop signal
#define OCR_LANGUAGE                "eng"       // Tesseract language (eng, fra, deu, etc.)

// OCR accuracy settings
// PSM (Page Segmentation Mode):
//   3 = Fully automatic (default, assumes dense paragraphs)
//   6 = Uniform block of text (good for UI elements)
//  11 = Sparse text (best for screen captures with scattered text)
//  13 = Raw line (single line of text)
#define OCR_PAGE_SEG_MODE           11          // Use 11 for screen captures with sparse UI text

// Perceptual hashing settings
#define PHASH_ENABLED               1           // Enable duplicate detection
#define PHASH_THRESHOLD             8           // Hamming distance threshold (0-64, lower = stricter)

// Encoder settings
#define ENCODER_PRIORITY            "hevc_vaapi,h264_vaapi,libx265,libx264"
#define VIDEO_CRF                   28          // Quality (0=best, 51=worst)
                                                 // For better OCR: use 20-23 (higher quality, larger files)
                                                 // For lower storage: use 28-32
#define VIDEO_PRESET                "ultrafast" // Encoding speed
#define KEYFRAME_INTERVAL           300         // I-frame every 5 minutes

// Hardware acceleration
#define VAAPI_DEVICE                NULL        // Auto-detect (use "/dev/dri/renderD128" to force specific device)
#define REQUIRE_HARDWARE_ACCEL      0           // Exit if no GPU encoder found

// Portal configuration
#define RESTORE_TOKEN_FILE          ".local/share/recool/portal_token"  // Relative to HOME
#define DBUS_TIMEOUT_MS             30000       // Permission dialog timeout

// PipeWire reconnection
#define PIPEWIRE_RETRY_COUNT        3           // Retry attempts if disconnected
#define PIPEWIRE_RETRY_DELAY_SEC    5           // Delay between retries

// System settings
#define ENABLE_NICE_PRIORITY        1           // Run at low priority (nice +10)
#define SCALE_ALGORITHM             SWS_FAST_BILINEAR  // Scaling method

// ============================================================================
// INCLUDES
// ============================================================================

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/timerfd.h>
#include <sys/resource.h>
#include <poll.h>

// D-Bus
#include <dbus/dbus.h>

// PipeWire
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/types.h>

// FFmpeg
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>

// SQLite
#include <sqlite3.h>

// Tesseract OCR
#include <tesseract/capi.h>
#include <leptonica/allheaders.h>

// Threading
#include <pthread.h>

// ============================================================================
// GLOBAL STATE
// ============================================================================

static volatile sig_atomic_t g_running = 1;
static uint64_t g_frame_count = 0;
static time_t g_start_time = 0;

// ============================================================================
// STRUCTURES
// ============================================================================

typedef struct {
    char *token;
} RestoreToken;

typedef struct {
    DBusConnection *connection;
    char *session_handle;
    char *request_path;
    int pipewire_fd;
    char *restore_token;
    bool response_received;
    uint32_t response_code;
    uint32_t pipewire_node;
} PortalContext;

typedef struct {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    struct pw_context *context;
    struct pw_core *core;
    int width;
    int height;
    enum spa_video_format format;
    bool frame_available;
    void *frame_data;
    size_t frame_size;
    int retry_count;
} PipeWireContext;

typedef struct {
    struct SwsContext *sws_ctx;
    int src_width;
    int src_height;
    int dst_width;
    int dst_height;
    AVFrame *scaled_frame;
} ScalerContext;

typedef struct {
    const AVCodec *codec;
    AVCodecContext *codec_ctx;
    AVFormatContext *format_ctx;
    AVStream *stream;
    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ctx;
    AVFrame *hw_frame;
    int64_t pts;
    char *output_path;
} EncoderContext;

typedef struct {
    sqlite3 *db;
    char *db_path;
    int64_t current_recording_id;  // Current recording session ID
} DatabaseContext;

typedef struct {
    TessBaseAPI *api;              // Tesseract OCR handle
    pthread_t thread;              // Background worker thread
    volatile sig_atomic_t running; // Thread stop signal (atomic for safety)
    bool thread_created;           // Track if pthread_create succeeded
    DatabaseContext *db_ctx;       // Reference to database for queries
} OCRContext;

// ============================================================================
// SIGNAL HANDLING
// ============================================================================

static void signal_handler(int signum) {
    (void)signum;
    g_running = 0;
}

static void setup_signal_handlers(void) {
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

static char* expand_home_path(const char *path) {
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "[ERROR] HOME environment variable not set\n");
        return NULL;
    }

    size_t len = strlen(home) + strlen(path) + 2;
    char *expanded = malloc(len);
    if (!expanded) {
        fprintf(stderr, "[ERROR] Memory allocation failed\n");
        return NULL;
    }

    snprintf(expanded, len, "%s/%s", home, path);
    return expanded;
}

static bool create_directory_recursive(const char *path) {
    char *path_copy = strdup(path);
    if (!path_copy) return false;

    char *p = path_copy;
    if (*p == '/') p++;

    while (*p) {
        while (*p && *p != '/') p++;
        char tmp = *p;
        *p = '\0';

        if (mkdir(path_copy, 0755) != 0 && errno != EEXIST) {
            free(path_copy);
            return false;
        }

        *p = tmp;
        if (*p) p++;
    }

    free(path_copy);
    return true;
}

static char* get_output_filename(void) {
    char *base_dir = expand_home_path(OUTPUT_BASE_DIR);
    if (!base_dir) return NULL;

    if (!create_directory_recursive(base_dir)) {
        fprintf(stderr, "[ERROR] Cannot create output directory: %s\n", base_dir);
        free(base_dir);
        return NULL;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), OUTPUT_FILENAME_FORMAT, tm_info);

    size_t len = strlen(base_dir) + strlen(timestamp) + strlen(OUTPUT_FILE_EXTENSION) + 2;
    char *output_path = malloc(len);
    if (!output_path) {
        free(base_dir);
        return NULL;
    }

    snprintf(output_path, len, "%s/%s%s", base_dir, timestamp, OUTPUT_FILE_EXTENSION);
    free(base_dir);

    return output_path;
}

// ============================================================================
// RESTORE TOKEN MANAGEMENT
// ============================================================================

static char* restore_token_load(void) {
    char *token_path = expand_home_path(RESTORE_TOKEN_FILE);
    if (!token_path) return NULL;

    FILE *f = fopen(token_path, "r");
    if (!f) {
        free(token_path);
        return NULL;
    }

    char buffer[256] = {0};
    if (!fgets(buffer, sizeof(buffer), f)) {
        fclose(f);
        free(token_path);
        return NULL;
    }

    fclose(f);
    free(token_path);

    // Remove newline
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len-1] == '\n') {
        buffer[len-1] = '\0';
    }

    return strdup(buffer);
}

static bool restore_token_save(const char *token) {
    if (!token) return false;

    char *token_path = expand_home_path(RESTORE_TOKEN_FILE);
    if (!token_path) return false;

    // Create parent directory
    char *dir_path = strdup(token_path);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        create_directory_recursive(dir_path);
    }
    free(dir_path);

    FILE *f = fopen(token_path, "w");
    if (!f) {
        fprintf(stderr, "[WARNING] Cannot save restore token: %s\n", strerror(errno));
        free(token_path);
        return false;
    }

    fprintf(f, "%s\n", token);
    fclose(f);
    free(token_path);

    return true;
}

// ============================================================================
// DATABASE MANAGEMENT
// ============================================================================

static int database_get_version(DatabaseContext *ctx) {
    sqlite3_stmt *stmt = NULL;
    int version = 0;
    
    // Check if schema_migrations table exists
    const char *check_sql = "SELECT name FROM sqlite_master WHERE type='table' AND name='schema_migrations'";
    if (sqlite3_prepare_v2(ctx->db, check_sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    
    int exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    
    if (!exists) {
        return 0;
    }
    
    // Get latest version
    const char *version_sql = "SELECT MAX(version) FROM schema_migrations";
    if (sqlite3_prepare_v2(ctx->db, version_sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return version;
}

static int database_migration_001(DatabaseContext *ctx) {
    const char *sql = 
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "    version INTEGER PRIMARY KEY,"
        "    applied_at INTEGER NOT NULL"
        ");"
        
        "CREATE TABLE IF NOT EXISTS recordings ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    start_time INTEGER NOT NULL,"
        "    end_time INTEGER,"
        "    file_path TEXT NOT NULL,"
        "    resolution TEXT NOT NULL,"
        "    original_resolution TEXT NOT NULL,"
        "    duration_seconds INTEGER,"
        "    file_size_bytes INTEGER,"
        "    monitor_id INTEGER DEFAULT 0,"
        "    ocr_completed BOOLEAN DEFAULT 0,"
        "    created_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_recordings_time ON recordings(start_time, end_time);"
        "CREATE INDEX IF NOT EXISTS idx_recordings_ocr ON recordings(ocr_completed);";
    
    char *err_msg = NULL;
    if (sqlite3_exec(ctx->db, sql, NULL, NULL, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "[ERROR] Migration 001 failed: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }
    
    return 0;
}

static int database_migration_002(DatabaseContext *ctx) {
    const char *sql = 
        "CREATE TABLE IF NOT EXISTS frames ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    recording_id INTEGER NOT NULL,"
        "    timestamp INTEGER NOT NULL,"
        "    offset_ms INTEGER NOT NULL,"
        "    perceptual_hash TEXT,"
        "    is_duplicate BOOLEAN DEFAULT 0,"
        "    duplicate_of_frame_id INTEGER,"
        "    created_at INTEGER NOT NULL,"
        "    FOREIGN KEY(recording_id) REFERENCES recordings(id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_frames_recording ON frames(recording_id);"
        "CREATE INDEX IF NOT EXISTS idx_frames_timestamp ON frames(timestamp);"
        "CREATE INDEX IF NOT EXISTS idx_frames_hash ON frames(perceptual_hash);";
    
    char *err_msg = NULL;
    if (sqlite3_exec(ctx->db, sql, NULL, NULL, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "[ERROR] Migration 002 failed: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }
    
    return 0;
}

static int database_migration_003(DatabaseContext *ctx) {
    const char *sql = 
        "CREATE VIRTUAL TABLE IF NOT EXISTS frame_text USING fts5("
        "    frame_id UNINDEXED,"
        "    text_content,"
        "    content='',"
        "    tokenize='porter unicode61'"
        ");"
        
        "CREATE TABLE IF NOT EXISTS frame_text_metadata ("
        "    frame_id INTEGER PRIMARY KEY,"
        "    ocr_processed_at INTEGER,"
        "    ocr_language TEXT DEFAULT 'eng',"
        "    text_confidence REAL,"
        "    word_count INTEGER,"
        "    FOREIGN KEY(frame_id) REFERENCES frames(id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_ocr_processed ON frame_text_metadata(ocr_processed_at);";
    
    char *err_msg = NULL;
    if (sqlite3_exec(ctx->db, sql, NULL, NULL, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "[ERROR] Migration 003 failed: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }
    
    return 0;
}

static int database_migration_004(DatabaseContext *ctx) {
    // Migration 004: Fix FTS5 table to store content
    // The original migration 003 created a contentless FTS5 table (content='')
    // which only stores the index but not the actual text. We need to recreate it.
    
    const char *sql = 
        // Drop the old contentless FTS5 table
        "DROP TABLE IF EXISTS frame_text;"
        
        // Recreate with proper content storage
        "CREATE VIRTUAL TABLE frame_text USING fts5("
        "    frame_id UNINDEXED,"
        "    text_content,"
        "    tokenize='porter unicode61'"
        ");";
    
    char *err_msg = NULL;
    if (sqlite3_exec(ctx->db, sql, NULL, NULL, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "[ERROR] Migration 004 failed: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }
    
    return 0;
}

static int database_apply_migrations(DatabaseContext *ctx) {
    typedef struct {
        int version;
        const char *description;
        int (*apply)(DatabaseContext *);
    } Migration;
    
    Migration migrations[] = {
        {1, "Initial schema (recordings table)", database_migration_001},
        {2, "Frame metadata and tracking", database_migration_002},
        {3, "OCR full-text search and progress", database_migration_003},
        {4, "Fix FTS5 content storage", database_migration_004},
    };
    
    int current_version = database_get_version(ctx);
    int num_migrations = sizeof(migrations) / sizeof(migrations[0]);
    
    for (int i = 0; i < num_migrations; i++) {
        if (migrations[i].version > current_version) {
            fprintf(stderr, "[INFO] Applying migration %d: %s\n", 
                    migrations[i].version, migrations[i].description);
            
            char *err_msg = NULL;
            if (sqlite3_exec(ctx->db, "BEGIN TRANSACTION", NULL, NULL, &err_msg) != SQLITE_OK) {
                fprintf(stderr, "[ERROR] Failed to begin transaction: %s\n", err_msg);
                sqlite3_free(err_msg);
                return -1;
            }
            
            if (migrations[i].apply(ctx) < 0) {
                sqlite3_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
                return -1;
            }
            
            // Record migration
            sqlite3_stmt *stmt = NULL;
            const char *sql = "INSERT INTO schema_migrations (version, applied_at) VALUES (?, ?)";
            if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, migrations[i].version);
                sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            
            if (sqlite3_exec(ctx->db, "COMMIT", NULL, NULL, &err_msg) != SQLITE_OK) {
                fprintf(stderr, "[ERROR] Failed to commit transaction: %s\n", err_msg);
                sqlite3_free(err_msg);
                return -1;
            }
        }
    }
    
    return 0;
}

static int database_init(DatabaseContext *ctx) {
    // Expand database path
    char *db_dir = expand_home_path(DATABASE_DIR);
    if (!db_dir) {
        fprintf(stderr, "[ERROR] Failed to expand database directory path\n");
        return -1;
    }

    // Create database directory if it doesn't exist
    if (!create_directory_recursive(db_dir)) {
        fprintf(stderr, "[ERROR] Cannot create database directory: %s\n", db_dir);
        free(db_dir);
        return -1;
    }

    // Build full database path
    size_t len = strlen(db_dir) + strlen(DATABASE_FILENAME) + 2;
    ctx->db_path = malloc(len);
    if (!ctx->db_path) {
        fprintf(stderr, "[ERROR] Memory allocation failed for database path\n");
        free(db_dir);
        return -1;
    }
    snprintf(ctx->db_path, len, "%s/%s", db_dir, DATABASE_FILENAME);
    free(db_dir);

    // Open or create database
    int rc = sqlite3_open(ctx->db_path, &ctx->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[ERROR] Cannot open database: %s\n", sqlite3_errmsg(ctx->db));
        free(ctx->db_path);
        ctx->db_path = NULL;
        return -1;
    }

    fprintf(stderr, "[INFO] Database: %s\n", ctx->db_path);
    
    // Enable optimizations
    char *err_msg = NULL;
    const char *pragmas = 
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA cache_size = -64000;"
        "PRAGMA temp_store = MEMORY;"
        "PRAGMA foreign_keys = ON;";
    
    if (sqlite3_exec(ctx->db, pragmas, NULL, NULL, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "[WARNING] Failed to set PRAGMA settings: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
    
    // Apply migrations
    if (database_apply_migrations(ctx) < 0) {
        fprintf(stderr, "[ERROR] Database migrations failed\n");
        sqlite3_close(ctx->db);
        free(ctx->db_path);
        return -1;
    }
    
    ctx->current_recording_id = 0;
    
    return 0;
}

static int64_t database_start_recording(DatabaseContext *ctx, const char *file_path, 
                                         int orig_width, int orig_height,
                                         int scaled_width, int scaled_height) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = 
        "INSERT INTO recordings "
        "(start_time, file_path, resolution, original_resolution, monitor_id, created_at) "
        "VALUES (?, ?, ?, ?, 0, ?)";
    
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[ERROR] Failed to prepare recording insert: %s\n", 
                sqlite3_errmsg(ctx->db));
        return -1;
    }
    
    time_t now = time(NULL);
    char resolution[64];
    char orig_resolution[64];
    snprintf(resolution, sizeof(resolution), "%dx%d", scaled_width, scaled_height);
    snprintf(orig_resolution, sizeof(orig_resolution), "%dx%d", orig_width, orig_height);
    
    sqlite3_bind_int64(stmt, 1, (int64_t)now);
    sqlite3_bind_text(stmt, 2, file_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, resolution, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, orig_resolution, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, (int64_t)now);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[ERROR] Failed to insert recording: %s\n", sqlite3_errmsg(ctx->db));
        return -1;
    }
    
    ctx->current_recording_id = sqlite3_last_insert_rowid(ctx->db);
    fprintf(stderr, "[INFO] Recording session started (ID: %ld)\n", 
            (long)ctx->current_recording_id);
    
    return ctx->current_recording_id;
}

static void database_end_recording(DatabaseContext *ctx, int64_t duration_sec, int64_t file_size) {
    if (ctx->current_recording_id == 0) return;
    
    sqlite3_stmt *stmt = NULL;
    const char *sql = 
        "UPDATE recordings SET end_time = ?, duration_seconds = ?, file_size_bytes = ? "
        "WHERE id = ?";
    
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
        sqlite3_bind_int64(stmt, 2, duration_sec);
        sqlite3_bind_int64(stmt, 3, file_size);
        sqlite3_bind_int64(stmt, 4, ctx->current_recording_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    
    fprintf(stderr, "[INFO] Recording session ended (ID: %ld)\n", 
            (long)ctx->current_recording_id);
}

static void database_cleanup(DatabaseContext *ctx) {
    if (ctx->db) {
        sqlite3_close(ctx->db);
        ctx->db = NULL;
    }
    if (ctx->db_path) {
        free(ctx->db_path);
        ctx->db_path = NULL;
    }
}

// ============================================================================
// D-BUS PORTAL COMMUNICATION
// ============================================================================

static char* generate_request_token(void) {
    static uint32_t counter = 0;
    char token[64];
    snprintf(token, sizeof(token), "recool%u", counter++);
    return strdup(token);
}

static char* generate_session_token(void) {
    static uint32_t counter = 0;
    char token[64];
    snprintf(token, sizeof(token), "recoolsession%u", counter++);
    return strdup(token);
}

static DBusHandlerResult signal_filter(DBusConnection *connection, DBusMessage *message, void *user_data) {
    (void)connection;
    PortalContext *ctx = user_data;

    if (dbus_message_is_signal(message, "org.freedesktop.portal.Request", "Response")) {
        DBusMessageIter args, dict_iter, entry_iter, variant_iter;

        // Get response code
        if (!dbus_message_iter_init(message, &args)) {
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        uint32_t response;
        dbus_message_iter_get_basic(&args, &response);
        ctx->response_code = response;

        // Get results dictionary
        if (!dbus_message_iter_next(&args)) {
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        dbus_message_iter_recurse(&args, &dict_iter);

        // Extract session_handle, streams, and restore_token
        while (dbus_message_iter_get_arg_type(&dict_iter) == DBUS_TYPE_DICT_ENTRY) {
            dbus_message_iter_recurse(&dict_iter, &entry_iter);

            const char *key;
            dbus_message_iter_get_basic(&entry_iter, &key);
            dbus_message_iter_next(&entry_iter);
            dbus_message_iter_recurse(&entry_iter, &variant_iter);

            if (strcmp(key, "session_handle") == 0) {
                const char *session_handle;
                dbus_message_iter_get_basic(&variant_iter, &session_handle);
                if (ctx->session_handle) free(ctx->session_handle);
                ctx->session_handle = strdup(session_handle);
            } else if (strcmp(key, "streams") == 0) {
                // Extract PipeWire node ID from streams array
                if (dbus_message_iter_get_arg_type(&variant_iter) == DBUS_TYPE_ARRAY) {
                    DBusMessageIter streams_iter, stream_struct;
                    dbus_message_iter_recurse(&variant_iter, &streams_iter);

                    if (dbus_message_iter_get_arg_type(&streams_iter) == DBUS_TYPE_STRUCT) {
                        dbus_message_iter_recurse(&streams_iter, &stream_struct);
                        uint32_t node_id;
                        dbus_message_iter_get_basic(&stream_struct, &node_id);
                        ctx->pipewire_node = node_id;
                    }
                }
            } else if (strcmp(key, "restore_token") == 0) {
                const char *restore_token;
                dbus_message_iter_get_basic(&variant_iter, &restore_token);
                if (ctx->restore_token) free(ctx->restore_token);
                ctx->restore_token = strdup(restore_token);
            }

            dbus_message_iter_next(&dict_iter);
        }

        ctx->response_received = true;
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int wait_for_response(PortalContext *ctx, int timeout_ms) {
    ctx->response_received = false;
    ctx->response_code = 1;

    int elapsed = 0;
    while (!ctx->response_received && elapsed < timeout_ms) {
        dbus_connection_read_write_dispatch(ctx->connection, 100);
        elapsed += 100;
    }

    if (!ctx->response_received) {
        fprintf(stderr, "[ERROR] Timeout waiting for portal response\n");
        return -1;
    }

    if (ctx->response_code != 0) {
        fprintf(stderr, "[ERROR] Portal request denied (code: %u)\n", ctx->response_code);
        return -1;
    }

    return 0;
}

static int portal_create_session(PortalContext *ctx) {
    char *session_token = generate_session_token();
    char *request_token = generate_request_token();

    // Build expected request path
    const char *sender = dbus_bus_get_unique_name(ctx->connection);
    char *sender_clean = strdup(sender + 1);
    for (char *p = sender_clean; *p; p++) {
        if (*p == '.') *p = '_';
    }

    char request_path[512];
    snprintf(request_path, sizeof(request_path),
             "/org/freedesktop/portal/desktop/request/%s/%s",
             sender_clean, request_token);

    // Subscribe to Response signal for this request
    char match_rule[1024];
    snprintf(match_rule, sizeof(match_rule),
             "type='signal',sender='org.freedesktop.portal.Desktop',"
             "interface='org.freedesktop.portal.Request',member='Response',"
             "path='%s'", request_path);

    dbus_bus_add_match(ctx->connection, match_rule, NULL);
    dbus_connection_flush(ctx->connection);

    // Call CreateSession
    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        "CreateSession"
    );

    DBusMessageIter args, dict;
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);

    // Add session_handle_token
    {
        DBusMessageIter entry, variant;
        const char *key = "session_handle_token";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &session_token);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    // Add handle_token
    {
        DBusMessageIter entry, variant;
        const char *key = "handle_token";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &request_token);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    dbus_message_iter_close_container(&args, &dict);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        ctx->connection, msg, DBUS_TIMEOUT_MS, &error
    );

    dbus_message_unref(msg);

    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "[ERROR] CreateSession failed: %s\n", error.message);
        dbus_error_free(&error);
        free(session_token);
        free(request_token);
        free(sender_clean);
        return -1;
    }

    dbus_message_unref(reply);

    // Build expected session handle
    char session_handle[512];
    snprintf(session_handle, sizeof(session_handle),
             "/org/freedesktop/portal/desktop/session/%s/%s",
             sender_clean, session_token);
    ctx->session_handle = strdup(session_handle);

    free(session_token);
    free(request_token);
    free(sender_clean);

    // Wait for Response signal
    return wait_for_response(ctx, DBUS_TIMEOUT_MS);
}

static int portal_select_sources(PortalContext *ctx, const char *restore_token) {
    char *request_token = generate_request_token();

    // Build expected request path
    const char *sender = dbus_bus_get_unique_name(ctx->connection);
    char *sender_clean = strdup(sender + 1);
    for (char *p = sender_clean; *p; p++) {
        if (*p == '.') *p = '_';
    }

    char request_path[512];
    snprintf(request_path, sizeof(request_path),
             "/org/freedesktop/portal/desktop/request/%s/%s",
             sender_clean, request_token);

    // Subscribe to Response signal
    char match_rule[1024];
    snprintf(match_rule, sizeof(match_rule),
             "type='signal',sender='org.freedesktop.portal.Desktop',"
             "interface='org.freedesktop.portal.Request',member='Response',"
             "path='%s'", request_path);

    dbus_bus_add_match(ctx->connection, match_rule, NULL);
    dbus_connection_flush(ctx->connection);

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        "SelectSources"
    );

    DBusMessageIter args, dict;
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &ctx->session_handle);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);

    // Add types (monitor=1)
    {
        DBusMessageIter entry, variant;
        const char *key = "types";
        uint32_t types = 1; // Monitor
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &types);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    // Add multiple (true for all monitors)
    {
        DBusMessageIter entry, variant;
        const char *key = "multiple";
        dbus_bool_t multiple = TRUE;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &multiple);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    // Add restore_token if available
    if (restore_token && strlen(restore_token) > 0) {
        DBusMessageIter entry, variant;
        const char *key = "restore_token";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &restore_token);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    // Add persist_mode (2 = persist until explicitly revoked)
    {
        DBusMessageIter entry, variant;
        const char *key = "persist_mode";
        uint32_t persist_mode = 2;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &persist_mode);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    // Add handle_token
    {
        DBusMessageIter entry, variant;
        const char *key = "handle_token";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &request_token);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    dbus_message_iter_close_container(&args, &dict);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        ctx->connection, msg, DBUS_TIMEOUT_MS, &error
    );

    dbus_message_unref(msg);

    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "[ERROR] SelectSources failed: %s\n", error.message);
        dbus_error_free(&error);
        free(request_token);
        free(sender_clean);
        return -1;
    }

    dbus_message_unref(reply);
    free(request_token);
    free(sender_clean);

    return wait_for_response(ctx, DBUS_TIMEOUT_MS);
}

static int portal_start(PortalContext *ctx) {
    char *request_token = generate_request_token();

    // Build expected request path
    const char *sender = dbus_bus_get_unique_name(ctx->connection);
    char *sender_clean = strdup(sender + 1);
    for (char *p = sender_clean; *p; p++) {
        if (*p == '.') *p = '_';
    }

    char request_path[512];
    snprintf(request_path, sizeof(request_path),
             "/org/freedesktop/portal/desktop/request/%s/%s",
             sender_clean, request_token);

    // Subscribe to Response signal
    char match_rule[1024];
    snprintf(match_rule, sizeof(match_rule),
             "type='signal',sender='org.freedesktop.portal.Desktop',"
             "interface='org.freedesktop.portal.Request',member='Response',"
             "path='%s'", request_path);

    dbus_bus_add_match(ctx->connection, match_rule, NULL);
    dbus_connection_flush(ctx->connection);

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        "Start"
    );

    DBusMessageIter args, dict;
    const char *parent = "";
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &ctx->session_handle);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);

    // Add handle_token
    {
        DBusMessageIter entry, variant;
        const char *key = "handle_token";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &request_token);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    dbus_message_iter_close_container(&args, &dict);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        ctx->connection, msg, DBUS_TIMEOUT_MS, &error
    );

    dbus_message_unref(msg);

    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "[ERROR] Start failed: %s\n", error.message);
        dbus_error_free(&error);
        free(request_token);
        free(sender_clean);
        return -1;
    }

    dbus_message_unref(reply);
    free(request_token);
    free(sender_clean);

    fprintf(stderr, "[INFO] Waiting for screen capture permission dialog...\n");

    return wait_for_response(ctx, DBUS_TIMEOUT_MS);
}

static int portal_open_pipewire_remote(PortalContext *ctx) {
    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        "OpenPipeWireRemote"
    );

    DBusMessageIter args, dict;
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &ctx->session_handle);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&args, &dict);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        ctx->connection, msg, DBUS_TIMEOUT_MS, &error
    );

    dbus_message_unref(msg);

    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "[ERROR] OpenPipeWireRemote failed: %s\n", error.message);
        dbus_error_free(&error);
        return -1;
    }

    // Extract file descriptor
    int fd = -1;
    DBusMessageIter iter;
    dbus_message_iter_init(reply, &iter);

    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UNIX_FD) {
        dbus_message_iter_get_basic(&iter, &fd);
    }

    dbus_message_unref(reply);

    if (fd < 0) {
        fprintf(stderr, "[ERROR] Failed to get PipeWire file descriptor\n");
        return -1;
    }

    ctx->pipewire_fd = fd;
    return 0;
}

static int portal_request_screencast(PortalContext *ctx, const char *restore_token) {
    DBusError error;
    dbus_error_init(&error);

    ctx->connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "[ERROR] Cannot connect to D-Bus session bus: %s\n", error.message);
        dbus_error_free(&error);
        return -1;
    }

    // Add signal filter for Response signals
    dbus_connection_add_filter(ctx->connection, signal_filter, ctx, NULL);

    if (restore_token) {
        fprintf(stderr, "[INFO] Using saved permission token...\n");
    } else {
        fprintf(stderr, "[INFO] Requesting screen capture permission...\n");
    }

    if (portal_create_session(ctx) < 0) {
        return -1;
    }

    if (portal_select_sources(ctx, restore_token) < 0) {
        return -1;
    }

    if (portal_start(ctx) < 0) {
        return -1;
    }

    if (portal_open_pipewire_remote(ctx) < 0) {
        return -1;
    }

    fprintf(stderr, "[INFO] Screen capture permission granted\n");
    fprintf(stderr, "[INFO] PipeWire node ID: %u\n", ctx->pipewire_node);

    return 0;
}

static void portal_cleanup(PortalContext *ctx) {
    if (ctx->session_handle) {
        free(ctx->session_handle);
        ctx->session_handle = NULL;
    }
    if (ctx->restore_token) {
        free(ctx->restore_token);
        ctx->restore_token = NULL;
    }
    if (ctx->connection) {
        dbus_connection_unref(ctx->connection);
        ctx->connection = NULL;
    }
}

// ============================================================================
// PIPEWIRE STREAM
// ============================================================================

static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    PipeWireContext *ctx = data;

    if (param == NULL || id != SPA_PARAM_Format) {
        return;
    }

    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0) {
        return;
    }

    ctx->width = info.size.width;
    ctx->height = info.size.height;
    ctx->format = info.format;

    fprintf(stderr, "[INFO] Native resolution: %dx%d\n", ctx->width, ctx->height);
}

static void on_stream_process(void *data) {
    PipeWireContext *ctx = data;
    struct pw_buffer *buf;
    struct spa_buffer *spa_buf;

    buf = pw_stream_dequeue_buffer(ctx->stream);
    if (!buf) {
        return;
    }

    spa_buf = buf->buffer;
    if (spa_buf->datas[0].data == NULL) {
        pw_stream_queue_buffer(ctx->stream, buf);
        return;
    }

    // Copy frame data
    ctx->frame_data = spa_buf->datas[0].data;
    ctx->frame_size = spa_buf->datas[0].chunk->size;
    ctx->frame_available = true;

    pw_stream_queue_buffer(ctx->stream, buf);
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
                                     enum pw_stream_state state, const char *error) {
    (void)old;
    (void)data;

    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "[ERROR] PipeWire stream error: %s\n", error);
    } else if (state == PW_STREAM_STATE_STREAMING) {
        fprintf(stderr, "[INFO] PipeWire stream active\n");
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_stream_param_changed,
    .process = on_stream_process,
    .state_changed = on_stream_state_changed,
};

static int pipewire_init(PipeWireContext *ctx, int fd, uint32_t node_id) {
    pw_init(NULL, NULL);

    ctx->loop = pw_thread_loop_new("recool-loop", NULL);
    if (!ctx->loop) {
        fprintf(stderr, "[ERROR] Failed to create PipeWire loop\n");
        return -1;
    }

    ctx->context = pw_context_new(
        pw_thread_loop_get_loop(ctx->loop),
        NULL, 0
    );
    if (!ctx->context) {
        fprintf(stderr, "[ERROR] Failed to create PipeWire context\n");
        return -1;
    }

    if (pw_thread_loop_start(ctx->loop) < 0) {
        fprintf(stderr, "[ERROR] Failed to start PipeWire loop\n");
        return -1;
    }

    pw_thread_loop_lock(ctx->loop);

    ctx->core = pw_context_connect_fd(ctx->context, fd, NULL, 0);
    if (!ctx->core) {
        pw_thread_loop_unlock(ctx->loop);
        fprintf(stderr, "[ERROR] Failed to connect to PipeWire\n");
        return -1;
    }

    ctx->stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(ctx->loop),
        "recool-stream",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            NULL
        ),
        &stream_events,
        ctx
    );

    if (!ctx->stream) {
        pw_thread_loop_unlock(ctx->loop);
        fprintf(stderr, "[ERROR] Failed to create PipeWire stream\n");
        return -1;
    }

    // Connect to PipeWire node
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];

    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(4,
            SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_RGBA)
    );

    fprintf(stderr, "[INFO] Connecting to PipeWire node %u...\n", node_id);

    if (pw_stream_connect(ctx->stream,
                         PW_DIRECTION_INPUT,
                         node_id,
                         PW_STREAM_FLAG_AUTOCONNECT |
                         PW_STREAM_FLAG_MAP_BUFFERS,
                         params, 1) < 0) {
        pw_thread_loop_unlock(ctx->loop);
        fprintf(stderr, "[ERROR] Failed to connect PipeWire stream\n");
        return -1;
    }

    pw_thread_loop_unlock(ctx->loop);

    return 0;
}

static void* pipewire_get_frame(PipeWireContext *ctx) {
    if (!ctx->frame_available) {
        return NULL;
    }

    ctx->frame_available = false;
    return ctx->frame_data;
}

// CLEANUP PATTERN: Reference implementation for proper resource management
// This function demonstrates the correct cleanup sequence to prevent crashes:
//   1. Stop threads/event loops FIRST (prevents callbacks on destroyed objects)
//   2. Destroy child objects before parents (reverse order of creation)
//   3. Disconnect before destroy (for network/connection objects)
//   4. Always NULL-check before destroying (idempotent cleanup)
//   5. Set pointers to NULL after destruction (prevents double-free)
//
// Creation order (pipewire_init):
//   loop → context → core → stream
// Destruction order (REVERSE):
//   stream → core → context → loop
//
// CRITICAL: ALL library objects MUST be stored in the context structure.
// Never create orphaned objects - they WILL cause segfaults during cleanup.
static void pipewire_cleanup(PipeWireContext *ctx) {
    // Stop the thread loop first to prevent any callbacks
    if (ctx->loop) {
        pw_thread_loop_stop(ctx->loop);
    }

    // Now safely destroy objects in reverse order of creation
    if (ctx->stream) {
        pw_stream_destroy(ctx->stream);
        ctx->stream = NULL;
    }

    if (ctx->core) {
        pw_core_disconnect(ctx->core);
        ctx->core = NULL;
    }

    if (ctx->context) {
        pw_context_destroy(ctx->context);
        ctx->context = NULL;
    }

    if (ctx->loop) {
        pw_thread_loop_destroy(ctx->loop);
        ctx->loop = NULL;
    }

    pw_deinit();
}

// ============================================================================
// FRAME SCALING
// ============================================================================

static int scaler_init(ScalerContext *ctx, int src_width, int src_height, enum AVPixelFormat dst_format) {
    ctx->src_width = src_width;
    ctx->src_height = src_height;
    ctx->dst_width = (int)(src_width * SCALE_FACTOR);
    ctx->dst_height = (int)(src_height * SCALE_FACTOR);

    fprintf(stderr, "[INFO] Encoded resolution: %dx%d\n", ctx->dst_width, ctx->dst_height);

    ctx->sws_ctx = sws_getContext(
        src_width, src_height, AV_PIX_FMT_BGRA,
        ctx->dst_width, ctx->dst_height, dst_format,
        SCALE_ALGORITHM, NULL, NULL, NULL
    );

    if (!ctx->sws_ctx) {
        fprintf(stderr, "[ERROR] Failed to create scaler context\n");
        return -1;
    }

    ctx->scaled_frame = av_frame_alloc();
    if (!ctx->scaled_frame) {
        fprintf(stderr, "[ERROR] Failed to allocate scaled frame\n");
        return -1;
    }

    ctx->scaled_frame->format = dst_format;
    ctx->scaled_frame->width = ctx->dst_width;
    ctx->scaled_frame->height = ctx->dst_height;

    if (av_frame_get_buffer(ctx->scaled_frame, 32) < 0) {
        fprintf(stderr, "[ERROR] Failed to allocate scaled frame buffer\n");
        return -1;
    }

    return 0;
}

static AVFrame* scaler_process(ScalerContext *ctx, void *src_data) {
    const uint8_t *src_slices[1] = { src_data };
    int src_strides[1] = { ctx->src_width * 4 };

    sws_scale(ctx->sws_ctx,
              src_slices, src_strides,
              0, ctx->src_height,
              ctx->scaled_frame->data,
              ctx->scaled_frame->linesize);

    return ctx->scaled_frame;
}

static void scaler_cleanup(ScalerContext *ctx) {
    if (ctx->scaled_frame) {
        av_frame_free(&ctx->scaled_frame);
    }
    if (ctx->sws_ctx) {
        sws_freeContext(ctx->sws_ctx);
        ctx->sws_ctx = NULL;
    }
}

// ============================================================================
// FFMPEG ENCODER
// ============================================================================

static const AVCodec* encoder_probe(AVBufferRef **hw_device_ctx) {
    // Parse ENCODER_PRIORITY into array
    char *priority_copy = strdup(ENCODER_PRIORITY);
    const char *encoders[16];
    int encoder_count = 0;

    char *token = strtok(priority_copy, ",");
    while (token && encoder_count < 15) {
        encoders[encoder_count++] = token;
        token = strtok(NULL, ",");
    }
    encoders[encoder_count] = NULL;

    for (int i = 0; encoders[i]; i++) {
        const AVCodec *codec = avcodec_find_encoder_by_name(encoders[i]);
        if (!codec) continue;

        // Try to initialize hardware device
        AVBufferRef *hw_ctx = NULL;
        enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;

        if (strstr(encoders[i], "vaapi")) {
            hw_type = AV_HWDEVICE_TYPE_VAAPI;
        } else if (strstr(encoders[i], "vulkan")) {
            hw_type = AV_HWDEVICE_TYPE_VULKAN;
        }

        if (hw_type != AV_HWDEVICE_TYPE_NONE) {
            if (av_hwdevice_ctx_create(&hw_ctx, hw_type, VAAPI_DEVICE, NULL, 0) < 0) {
                continue;
            }
        }

        fprintf(stderr, "[INFO] Hardware encoder: %s\n", encoders[i]);
        *hw_device_ctx = hw_ctx;
        free(priority_copy);
        return codec;
    }

    free(priority_copy);
    return NULL;
}

static int encoder_init(EncoderContext *ctx, int width, int height, const char *output_path) {
    ctx->output_path = strdup(output_path);

    ctx->codec = encoder_probe(&ctx->hw_device_ctx);
    if (!ctx->codec) {
        if (REQUIRE_HARDWARE_ACCEL) {
            fprintf(stderr, "[ERROR] No hardware encoder found. Install VA-API or Vulkan drivers.\n");
            return -1;
        }
    }

    ctx->codec_ctx = avcodec_alloc_context3(ctx->codec);
    if (!ctx->codec_ctx) {
        fprintf(stderr, "[ERROR] Failed to allocate codec context\n");
        return -1;
    }

    ctx->codec_ctx->width = width;
    ctx->codec_ctx->height = height;
    ctx->codec_ctx->time_base = (AVRational){1, 1};
    ctx->codec_ctx->framerate = (AVRational){1, 1};
    ctx->codec_ctx->gop_size = KEYFRAME_INTERVAL;

    // Set pixel format and hardware context
    if (ctx->hw_device_ctx) {
        // Create hardware frames context for uploading sw frames
        AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(ctx->hw_device_ctx);
        if (!hw_frames_ref) {
            fprintf(stderr, "[ERROR] Failed to create hardware frames context\n");
            return -1;
        }

        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)hw_frames_ref->data;
        frames_ctx->format = AV_PIX_FMT_VAAPI;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        frames_ctx->width = width;
        frames_ctx->height = height;
        frames_ctx->initial_pool_size = 20;

        if (av_hwframe_ctx_init(hw_frames_ref) < 0) {
            fprintf(stderr, "[ERROR] Failed to initialize hardware frames context\n");
            av_buffer_unref(&hw_frames_ref);
            return -1;
        }

        ctx->hw_frames_ctx = hw_frames_ref;
        ctx->codec_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
        ctx->codec_ctx->pix_fmt = AV_PIX_FMT_VAAPI;

        // Allocate hardware frame for uploads
        ctx->hw_frame = av_frame_alloc();
        if (!ctx->hw_frame) {
            fprintf(stderr, "[ERROR] Failed to allocate hardware frame\n");
            return -1;
        }

        if (av_hwframe_get_buffer(hw_frames_ref, ctx->hw_frame, 0) < 0) {
            fprintf(stderr, "[ERROR] Failed to allocate hardware frame buffer\n");
            return -1;
        }
    } else {
        ctx->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    av_opt_set(ctx->codec_ctx->priv_data, "preset", VIDEO_PRESET, 0);

    // CRF not supported by VAAPI, use global_quality instead
    if (ctx->hw_device_ctx && strstr(ctx->codec->name, "vaapi")) {
        ctx->codec_ctx->global_quality = VIDEO_CRF;
    } else {
        av_opt_set_int(ctx->codec_ctx->priv_data, "crf", VIDEO_CRF, 0);
    }

    if (avcodec_open2(ctx->codec_ctx, ctx->codec, NULL) < 0) {
        fprintf(stderr, "[ERROR] Failed to open codec\n");
        return -1;
    }

    // Create output format context
    if (avformat_alloc_output_context2(&ctx->format_ctx, NULL, NULL, output_path) < 0) {
        fprintf(stderr, "[ERROR] Failed to create output context\n");
        return -1;
    }

    ctx->stream = avformat_new_stream(ctx->format_ctx, NULL);
    if (!ctx->stream) {
        fprintf(stderr, "[ERROR] Failed to create output stream\n");
        return -1;
    }

    if (avcodec_parameters_from_context(ctx->stream->codecpar, ctx->codec_ctx) < 0) {
        fprintf(stderr, "[ERROR] Failed to copy codec parameters\n");
        return -1;
    }

    ctx->stream->time_base = ctx->codec_ctx->time_base;

    if (!(ctx->format_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ctx->format_ctx->pb, output_path, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "[ERROR] Failed to open output file: %s\n", output_path);
            return -1;
        }
    }

    if (avformat_write_header(ctx->format_ctx, NULL) < 0) {
        fprintf(stderr, "[ERROR] Failed to write output header\n");
        return -1;
    }

    ctx->pts = 0;

    fprintf(stderr, "[INFO] Output: %s\n", output_path);
    fprintf(stderr, "[INFO] Recording started. Press Ctrl+C to stop.\n");

    return 0;
}

static int encoder_send_frame(EncoderContext *ctx, AVFrame *frame) {
    AVFrame *encode_frame = frame;

    // Upload to hardware frame if using hardware encoding
    if (ctx->hw_frame) {
        if (av_hwframe_transfer_data(ctx->hw_frame, frame, 0) < 0) {
            fprintf(stderr, "[ERROR] Failed to transfer frame to hardware\n");
            return -1;
        }

        if (av_frame_copy_props(ctx->hw_frame, frame) < 0) {
            fprintf(stderr, "[ERROR] Failed to copy frame properties\n");
            return -1;
        }

        encode_frame = ctx->hw_frame;
    }

    encode_frame->pts = ctx->pts++;

    if (avcodec_send_frame(ctx->codec_ctx, encode_frame) < 0) {
        fprintf(stderr, "[ERROR] Failed to send frame to encoder\n");
        return -1;
    }

    while (1) {
        AVPacket *pkt = av_packet_alloc();
        int ret = avcodec_receive_packet(ctx->codec_ctx, pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            break;
        }

        if (ret < 0) {
            av_packet_free(&pkt);
            fprintf(stderr, "[ERROR] Failed to receive packet from encoder\n");
            return -1;
        }

        pkt->stream_index = ctx->stream->index;
        av_packet_rescale_ts(pkt, ctx->codec_ctx->time_base, ctx->stream->time_base);

        if (av_interleaved_write_frame(ctx->format_ctx, pkt) < 0) {
            av_packet_free(&pkt);
            fprintf(stderr, "[ERROR] Failed to write frame\n");
            return -1;
        }

        av_packet_free(&pkt);
    }

    return 0;
}

static void encoder_flush(EncoderContext *ctx) {
    avcodec_send_frame(ctx->codec_ctx, NULL);

    while (1) {
        AVPacket *pkt = av_packet_alloc();
        int ret = avcodec_receive_packet(ctx->codec_ctx, pkt);

        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            av_packet_free(&pkt);
            break;
        }

        if (ret >= 0) {
            pkt->stream_index = ctx->stream->index;
            av_packet_rescale_ts(pkt, ctx->codec_ctx->time_base, ctx->stream->time_base);
            av_interleaved_write_frame(ctx->format_ctx, pkt);
        }

        av_packet_free(&pkt);
    }

    av_write_trailer(ctx->format_ctx);
}

static void encoder_cleanup(EncoderContext *ctx) {
    if (ctx->format_ctx) {
        if (!(ctx->format_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ctx->format_ctx->pb);
        }
        avformat_free_context(ctx->format_ctx);
        ctx->format_ctx = NULL;
    }

    if (ctx->hw_frame) {
        av_frame_free(&ctx->hw_frame);
    }

    if (ctx->codec_ctx) {
        avcodec_free_context(&ctx->codec_ctx);
    }

    if (ctx->hw_frames_ctx) {
        av_buffer_unref(&ctx->hw_frames_ctx);
    }

    if (ctx->hw_device_ctx) {
        av_buffer_unref(&ctx->hw_device_ctx);
    }

    if (ctx->output_path) {
        free(ctx->output_path);
        ctx->output_path = NULL;
    }
}

// ============================================================================
// PERCEPTUAL HASHING SUBSYSTEM (Duplicate Detection)
// ============================================================================

// Compute difference hash (dHash) for an image
// Returns a 64-bit hash where each bit represents a gradient comparison
static uint64_t compute_dhash(PIX *image) {
    if (!image) return 0;
    
    // Convert to grayscale if needed
    PIX *gray = NULL;
    if (pixGetDepth(image) > 8) {
        gray = pixConvertTo8(image, 0);
    } else {
        gray = pixClone(image);
    }
    
    if (!gray) return 0;
    
    // Resize to 9x8 (we need 9 columns to compare 8 adjacent pairs)
    PIX *small = pixScale(gray, 9.0 / pixGetWidth(gray), 8.0 / pixGetHeight(gray));
    pixDestroy(&gray);
    
    if (!small) return 0;
    
    // Compute hash by comparing adjacent horizontal pixels
    uint64_t hash = 0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            uint32_t left_pixel, right_pixel;
            pixGetPixel(small, x, y, &left_pixel);
            pixGetPixel(small, x + 1, y, &right_pixel);
            
            // Set bit if left pixel is brighter than right
            if (left_pixel > right_pixel) {
                hash |= (1ULL << (y * 8 + x));
            }
        }
    }
    
    pixDestroy(&small);
    return hash;
}

// Convert 64-bit hash to hex string (16 characters + null terminator)
static void dhash_to_string(uint64_t hash, char *output) {
    snprintf(output, 17, "%016lx", hash);
}

// Parse hex string back to 64-bit hash
static uint64_t dhash_from_string(const char *hash_str) {
    if (!hash_str) return 0;
    return strtoull(hash_str, NULL, 16);
}

// Compute Hamming distance between two hashes (number of differing bits)
static int hamming_distance(uint64_t hash1, uint64_t hash2) {
    uint64_t diff = hash1 ^ hash2;
    int distance = 0;
    
    // Count set bits
    while (diff) {
        distance += diff & 1;
        diff >>= 1;
    }
    
    return distance;
}

// Check if two hashes represent duplicate/similar images
static int are_hashes_similar(uint64_t hash1, uint64_t hash2, int threshold) {
    return hamming_distance(hash1, hash2) <= threshold;
}

// Find duplicate frame in the same recording based on perceptual hash
// Returns frame_id of duplicate, or 0 if no duplicate found
static int64_t find_duplicate_frame(DatabaseContext *db_ctx, int64_t recording_id, 
                                     uint64_t hash, int threshold) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = 
        "SELECT id, perceptual_hash FROM frames "
        "WHERE recording_id = ? AND perceptual_hash IS NOT NULL "
        "ORDER BY id DESC LIMIT 10";  // Check last 10 frames only (efficiency)
    
    if (sqlite3_prepare_v2(db_ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    
    sqlite3_bind_int64(stmt, 1, recording_id);
    
    int64_t duplicate_id = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t frame_id = sqlite3_column_int64(stmt, 0);
        const char *hash_str = (const char*)sqlite3_column_text(stmt, 1);
        
        if (hash_str) {
            uint64_t stored_hash = dhash_from_string(hash_str);
            if (are_hashes_similar(hash, stored_hash, threshold)) {
                duplicate_id = frame_id;
                break;
            }
        }
    }
    
    sqlite3_finalize(stmt);
    return duplicate_id;
}

// ============================================================================
// OCR PROCESSING SUBSYSTEM
// ============================================================================

static int ocr_process_frame(OCRContext *ctx, int64_t frame_id, const char *video_path, 
                             int64_t offset_ms, int64_t recording_id) {
    // Extract frame from video using FFmpeg
    char temp_image[512];
    snprintf(temp_image, sizeof(temp_image), "/tmp/recool_ocr_%ld.png", (long)frame_id);
    
    // Use FFmpeg to extract frame at specific timestamp
    // TODO: Consider getting actual PTS from encoder instead of calculated offset
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), 
             "ffmpeg -ss %.3f -i \"%s\" -frames:v 1 -y \"%s\" 2>/dev/null",
             offset_ms / 1000.0, video_path, temp_image);
    
    if (system(cmd) != 0) {
        return -1;
    }
    
    // Load image with Leptonica
    PIX *image = pixRead(temp_image);
    if (!image) {
        unlink(temp_image);
        return -1;
    }
    
    // Compute perceptual hash for duplicate detection
    uint64_t phash = 0;
    char phash_str[17] = {0};
    int64_t duplicate_of_id = 0;
    int is_duplicate = 0;
    
#if PHASH_ENABLED
    phash = compute_dhash(image);
    dhash_to_string(phash, phash_str);
    
    // Check if this frame is a duplicate
    duplicate_of_id = find_duplicate_frame(ctx->db_ctx, recording_id, phash, PHASH_THRESHOLD);
    if (duplicate_of_id > 0) {
        is_duplicate = 1;
    }
#endif
    
    // Only perform OCR if this is NOT a duplicate
    char *text = NULL;
    int confidence = 0;
    int word_count = 0;
    
    if (!is_duplicate) {
        // Perform OCR
        TessBaseAPISetImage2(ctx->api, image);
        text = TessBaseAPIGetUTF8Text(ctx->api);
        confidence = TessBaseAPIMeanTextConf(ctx->api);
        
        if (!text) {
            pixDestroy(&image);
            unlink(temp_image);
            return -1;
        }
        
        // Count words
        char *p = text;
        while (*p) {
            if (isspace(*p)) {
                p++;
            } else {
                word_count++;
                while (*p && !isspace(*p)) p++;
            }
        }
    }
    
    // Insert the frame record with perceptual hash and duplicate information
    sqlite3_stmt *stmt = NULL;
    const char *frame_sql = 
        "INSERT INTO frames (recording_id, timestamp, offset_ms, perceptual_hash, "
        "is_duplicate, duplicate_of_frame_id, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)";

    
    if (sqlite3_prepare_v2(ctx->db_ctx->db, frame_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, recording_id);
        sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
        sqlite3_bind_int64(stmt, 3, offset_ms);
#if PHASH_ENABLED
        sqlite3_bind_text(stmt, 4, phash_str, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, is_duplicate);
        if (duplicate_of_id > 0) {
            sqlite3_bind_int64(stmt, 6, duplicate_of_id);
        } else {
            sqlite3_bind_null(stmt, 6);
        }
        sqlite3_bind_int64(stmt, 7, (int64_t)time(NULL));
#else
        sqlite3_bind_null(stmt, 4);
        sqlite3_bind_int(stmt, 5, 0);
        sqlite3_bind_null(stmt, 6);
        sqlite3_bind_int64(stmt, 7, (int64_t)time(NULL));
#endif
        
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            if (text) TessDeleteText(text);
            pixDestroy(&image);
            unlink(temp_image);
            return -1;
        }
        
        sqlite3_finalize(stmt);
        // Get the auto-generated frame_id
        frame_id = sqlite3_last_insert_rowid(ctx->db_ctx->db);
    } else {
        if (text) TessDeleteText(text);
        pixDestroy(&image);
        unlink(temp_image);
        return -1;
    }
    
    // Only insert OCR data if this is NOT a duplicate
    if (!is_duplicate && text) {
        // Insert into FTS5 table
        const char *fts_sql = "INSERT INTO frame_text (frame_id, text_content) VALUES (?, ?)";
        if (sqlite3_prepare_v2(ctx->db_ctx->db, fts_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, frame_id);
            sqlite3_bind_text(stmt, 2, text, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        
        // Insert metadata
        const char *meta_sql = 
            "INSERT INTO frame_text_metadata "
            "(frame_id, ocr_processed_at, ocr_language, text_confidence, word_count) "
            "VALUES (?, ?, ?, ?, ?)";
        
        if (sqlite3_prepare_v2(ctx->db_ctx->db, meta_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, frame_id);
            sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
            sqlite3_bind_text(stmt, 3, OCR_LANGUAGE, -1, SQLITE_STATIC);
            sqlite3_bind_double(stmt, 4, (double)confidence);
            sqlite3_bind_int(stmt, 5, word_count);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    
    // Cleanup
    if (text) TessDeleteText(text);
    pixDestroy(&image);
    unlink(temp_image);
    
    return 0;
}

static int ocr_probe_video_duration(const char *video_path) {
    // Use ffprobe to get video duration in seconds
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -show_entries format=duration "
             "-of default=noprint_wrappers=1:nokey=1 \"%s\" 2>/dev/null",
             video_path);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    
    float duration = 0;
    if (fscanf(fp, "%f", &duration) != 1) {
        pclose(fp);
        return -1;
    }
    
    pclose(fp);
    return (int)(duration * 1000); // Convert to milliseconds
}

static void* ocr_worker_thread(void *arg) {
    OCRContext *ctx = (OCRContext*)arg;
    
    // Set thread priority to lowest
    setpriority(PRIO_PROCESS, 0, OCR_THREAD_PRIORITY);
    
    fprintf(stderr, "[INFO] OCR worker thread started (priority: %d)\n", OCR_THREAD_PRIORITY);
    
    while (ctx->running) {
        // Query for recordings that need OCR processing
        sqlite3_stmt *stmt = NULL;
        const char *sql = 
            "SELECT id, file_path, start_time "
            "FROM recordings "
            "WHERE (ocr_completed = 0 OR ocr_completed IS NULL) "
            "  AND end_time IS NOT NULL "
            "ORDER BY start_time ASC "
            "LIMIT 1";
        
        if (sqlite3_prepare_v2(ctx->db_ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            sleep(5);
            continue;
        }
        
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            // No recordings to process
            sqlite3_finalize(stmt);
            sleep(5);
            continue;
        }
        
        int64_t recording_id = sqlite3_column_int64(stmt, 0);
        const char *video_path = (const char*)sqlite3_column_text(stmt, 1);
        
        // Make a copy of video_path since it's only valid during statement lifetime
        char *video_path_copy = strdup(video_path);
        sqlite3_finalize(stmt);
        
        if (!video_path_copy) {
            sleep(5);
            continue;
        }
        
        fprintf(stderr, "[INFO] OCR processing recording ID %ld: %s\n", 
                (long)recording_id, video_path_copy);
        
        // Probe video to get duration
        int duration_ms = ocr_probe_video_duration(video_path_copy);
        if (duration_ms < 0) {
            fprintf(stderr, "[WARNING] Failed to probe video duration: %s\n", video_path_copy);
            fprintf(stderr, "[WARNING] Marking recording ID %ld as completed (failed)\n", (long)recording_id);
            
            // Mark as completed to avoid infinite retry loop on corrupted files
            sqlite3_stmt *fail_stmt = NULL;
            const char *fail_sql = "UPDATE recordings SET ocr_completed = 1 WHERE id = ?";
            if (sqlite3_prepare_v2(ctx->db_ctx->db, fail_sql, -1, &fail_stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(fail_stmt, 1, recording_id);
                sqlite3_step(fail_stmt);
                sqlite3_finalize(fail_stmt);
            }
            
            free(video_path_copy);
            sleep(1);  // Brief pause before moving to next recording
            continue;
        }
        
        // Process frames at CAPTURE_INTERVAL_MS intervals
        int processed = 0;
        for (int offset_ms = 0; offset_ms < duration_ms && ctx->running; 
             offset_ms += CAPTURE_INTERVAL_MS) {
            
            // Frame ID will be auto-generated by database
            if (ocr_process_frame(ctx, 0, video_path_copy, offset_ms, recording_id) == 0) {
                processed++;
                
                // Throttle processing
                if (processed % OCR_BATCH_SIZE == 0) {
                    usleep(OCR_PROCESS_DELAY_MS * 1000);
                }
            }
        }
        
        // Mark recording as OCR complete
        stmt = NULL;
        const char *update_sql = "UPDATE recordings SET ocr_completed = 1 WHERE id = ?";
        if (sqlite3_prepare_v2(ctx->db_ctx->db, update_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, recording_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        
        // Get duplicate statistics
        int unique_count = 0, duplicate_count = 0;
        const char *stats_sql = 
            "SELECT "
            "  SUM(CASE WHEN is_duplicate = 0 THEN 1 ELSE 0 END) as unique_frames, "
            "  SUM(CASE WHEN is_duplicate = 1 THEN 1 ELSE 0 END) as duplicates "
            "FROM frames WHERE recording_id = ?";
        
        if (sqlite3_prepare_v2(ctx->db_ctx->db, stats_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, recording_id);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                unique_count = sqlite3_column_int(stmt, 0);
                duplicate_count = sqlite3_column_int(stmt, 1);
            }
            sqlite3_finalize(stmt);
        }
        
        fprintf(stderr, "[INFO] OCR completed recording ID %ld (%d frames: %d unique, %d duplicates, %.1f%% saved)\n", 
                (long)recording_id, processed, unique_count, duplicate_count, 
                processed > 0 ? (duplicate_count * 100.0 / processed) : 0.0);
        
        free(video_path_copy);
    }
    
    fprintf(stderr, "[INFO] OCR worker thread stopped\n");
    return NULL;
}

static int ocr_init(OCRContext *ctx, DatabaseContext *db_ctx) {
    if (!OCR_ENABLED) {
        return 0;
    }
    
    ctx->db_ctx = db_ctx;
    ctx->running = 1;
    
    // Initialize Tesseract
    ctx->api = TessBaseAPICreate();
    if (TessBaseAPIInit3(ctx->api, NULL, OCR_LANGUAGE) != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize Tesseract (language: %s)\n", OCR_LANGUAGE);
        fprintf(stderr, "[ERROR] Install tesseract-data-%s package\n", OCR_LANGUAGE);
        TessBaseAPIDelete(ctx->api);
        return -1;
    }
    
    // Configure Tesseract for screen capture OCR
    // PSM 11 (sparse text) works best for UI elements scattered across the screen
    TessBaseAPISetPageSegMode(ctx->api, OCR_PAGE_SEG_MODE);
    
    fprintf(stderr, "[INFO] OCR initialized (language: %s)\n", OCR_LANGUAGE);
    
    // Start background worker thread
    if (pthread_create(&ctx->thread, NULL, ocr_worker_thread, ctx) != 0) {
        fprintf(stderr, "[ERROR] Failed to create OCR worker thread\n");
        TessBaseAPIEnd(ctx->api);
        TessBaseAPIDelete(ctx->api);
        ctx->api = NULL;
        return -1;
    }
    
    ctx->thread_created = true;
    return 0;
}

static void ocr_cleanup(OCRContext *ctx) {
    if (!OCR_ENABLED || !ctx->api) {
        return;
    }
    
    // Stop worker thread
    ctx->running = 0;
    
    // Only join if thread was actually created
    if (ctx->thread_created) {
        pthread_join(ctx->thread, NULL);
    }
    
    // Cleanup Tesseract
    TessBaseAPIEnd(ctx->api);
    TessBaseAPIDelete(ctx->api);
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char **argv) {
    setup_signal_handlers();

    // Parse command-line arguments
    bool reprocess_mode = false;
    char *reprocess_path = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--reprocess") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[ERROR] --reprocess requires a file path\n");
                fprintf(stderr, "Usage: %s [--reprocess <video_file.mp4>]\n", argv[0]);
                return 1;
            }
            reprocess_mode = true;
            reprocess_path = argv[i + 1];
            i++; // Skip next arg
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Recool - Wayland Screen Recorder with OCR\n\n");
            printf("Usage: %s [OPTIONS]\n\n", argv[0]);
            printf("Options:\n");
            printf("  --reprocess <file>   Reprocess video file for OCR indexing\n");
            printf("  --help, -h           Show this help message\n\n");
            printf("Examples:\n");
            printf("  %s                                  # Start recording\n", argv[0]);
            printf("  %s --reprocess ~/Videos/Recools/2026-05-01.mp4\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "[ERROR] Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Use --help for usage information\n");
            return 1;
        }
    }

    if (ENABLE_NICE_PRIORITY) {
        setpriority(PRIO_PROCESS, 0, 10);
    }

    // Initialize database
    DatabaseContext database = {0};
    if (database_init(&database) < 0) {
        return 1;
    }

    // Reprocess mode: add recording to database and exit
    if (reprocess_mode) {
        if (!OCR_ENABLED) {
            fprintf(stderr, "[ERROR] OCR is disabled (OCR_ENABLED = 0)\n");
            database_cleanup(&database);
            return 1;
        }
        
        // Check if file exists
        struct stat st;
        if (stat(reprocess_path, &st) != 0) {
            fprintf(stderr, "[ERROR] File not found: %s\n", reprocess_path);
            database_cleanup(&database);
            return 1;
        }
        
        fprintf(stderr, "[INFO] Reprocessing: %s\n", reprocess_path);
        
        // Insert recording into database with ocr_completed = 0
        // Use dummy resolution values - OCR doesn't need them
        if (database_start_recording(&database, reprocess_path, 0, 0, 0, 0) < 0) {
            fprintf(stderr, "[ERROR] Failed to add recording to database\n");
            database_cleanup(&database);
            return 1;
        }
        
        // Mark as ended immediately
        database_end_recording(&database, 0, st.st_size);
        
        fprintf(stderr, "[INFO] Added to OCR queue. Run recool normally to process.\n");
        database_cleanup(&database);
        return 0;
    }

    // Load restore token
    char *restore_token = restore_token_load();

    // Request screencast
    PortalContext portal = {0};
    if (portal_request_screencast(&portal, restore_token) < 0) {
        if (restore_token) free(restore_token);
        database_cleanup(&database);
        return 1;
    }

    // Save new restore token if portal returned one
    if (portal.restore_token) {
        restore_token_save(portal.restore_token);
        fprintf(stderr, "[INFO] Saved permission token for future runs\n");
    }

    if (restore_token) free(restore_token);

    // Initialize PipeWire
    PipeWireContext pipewire = {0};
    if (pipewire_init(&pipewire, portal.pipewire_fd, portal.pipewire_node) < 0) {
        portal_cleanup(&portal);
        database_cleanup(&database);
        return 1;
    }

    // Wait for first frame to get resolution
    fprintf(stderr, "[INFO] Waiting for first frame...\n");
    while (pipewire.width == 0 && g_running) {
        usleep(100000);
    }

    if (!g_running || pipewire.width == 0) {
        pipewire_cleanup(&pipewire);
        portal_cleanup(&portal);
        database_cleanup(&database);
        return 1;
    }

    // Get output filename
    char *output_path = get_output_filename();
    if (!output_path) {
        pipewire_cleanup(&pipewire);
        portal_cleanup(&portal);
        database_cleanup(&database);
        return 1;
    }

    // Calculate scaled dimensions
    int scaled_width = (int)(pipewire.width * SCALE_FACTOR);
    int scaled_height = (int)(pipewire.height * SCALE_FACTOR);

    // Initialize encoder first to determine pixel format
    EncoderContext encoder = {0};
    if (encoder_init(&encoder, scaled_width, scaled_height, output_path) < 0) {
        free(output_path);
        pipewire_cleanup(&pipewire);
        portal_cleanup(&portal);
        database_cleanup(&database);
        return 1;
    }
    free(output_path);

    // Initialize scaler with format needed by encoder
    enum AVPixelFormat scaler_format = encoder.hw_device_ctx ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    ScalerContext scaler = {0};
    if (scaler_init(&scaler, pipewire.width, pipewire.height, scaler_format) < 0) {
        encoder_cleanup(&encoder);
        pipewire_cleanup(&pipewire);
        portal_cleanup(&portal);
        database_cleanup(&database);
        return 1;
    }

    // Create timer
    int timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec timer_spec = {
        .it_interval = {CAPTURE_INTERVAL_MS / 1000, (CAPTURE_INTERVAL_MS % 1000) * 1000000},
        .it_value = {1, 0}
    };
    timerfd_settime(timerfd, 0, &timer_spec, NULL);

    g_start_time = time(NULL);
    
    // Start recording session in database
    if (database_start_recording(&database, encoder.output_path, 
                                  pipewire.width, pipewire.height,
                                  scaled_width, scaled_height) < 0) {
        fprintf(stderr, "[WARNING] Failed to record session to database\n");
    }
    
    // Initialize OCR background processor
    OCRContext ocr = {0};
    if (OCR_ENABLED) {
        if (ocr_init(&ocr, &database) < 0) {
            fprintf(stderr, "[WARNING] OCR initialization failed, continuing without OCR\n");
        }
    }

    // Main loop
    while (g_running) {
        struct pollfd pfd = {.fd = timerfd, .events = POLLIN};
        if (poll(&pfd, 1, 1000) <= 0) continue;

        uint64_t expirations;
        read(timerfd, &expirations, sizeof(expirations));

        void *frame_data = pipewire_get_frame(&pipewire);
        if (!frame_data) {
            usleep(10000);
            continue;
        }

        AVFrame *scaled_frame = scaler_process(&scaler, frame_data);
        if (encoder_send_frame(&encoder, scaled_frame) == 0) {
            g_frame_count++;
        }
    }

    // Cleanup
    close(timerfd);

    fprintf(stderr, "\n[INFO] Shutting down gracefully...\n");
    
    // Stop OCR worker
    ocr_cleanup(&ocr);
    
    fprintf(stderr, "[INFO] Flushing encoder...\n");

    encoder_flush(&encoder);

    time_t duration = time(NULL) - g_start_time;
    int hours = duration / 3600;
    int minutes = (duration % 3600) / 60;
    int seconds = duration % 60;

    struct stat st;
    long file_size = 0;
    if (stat(encoder.output_path, &st) == 0) {
        file_size = st.st_size / (1024 * 1024);
    }
    
    // Update recording end time in database
    database_end_recording(&database, duration, st.st_size);

    fprintf(stderr, "[INFO] Video saved: %s\n", encoder.output_path);
    fprintf(stderr, "[INFO] Duration: %dh %dm %ds (%lu frames)\n",
            hours, minutes, seconds, g_frame_count);
    fprintf(stderr, "[INFO] File size: %ld MB\n", file_size);

    fprintf(stderr, "[DEBUG] Cleaning up encoder...\n");
    encoder_cleanup(&encoder);
    fprintf(stderr, "[DEBUG] Cleaning up scaler...\n");
    scaler_cleanup(&scaler);
    fprintf(stderr, "[DEBUG] Cleaning up pipewire...\n");
    pipewire_cleanup(&pipewire);
    fprintf(stderr, "[DEBUG] Cleaning up portal...\n");
    portal_cleanup(&portal);
    fprintf(stderr, "[DEBUG] Cleaning up database...\n");
    database_cleanup(&database);
    fprintf(stderr, "[DEBUG] All cleanup complete\n");

    return 0;
}
