#include "../DPP/include/dpp/dpp.h" // Discord bot library - provides all Discord API functionality
#include <iostream> // Standard input/output - for console logging (std::cout, std::cerr)
#include <string> // String handling - for std::string class and std::to_string() function
#include <fstream> // File stream operations - for writing downloaded images to disk
#include <thread> // Threading support - for std::thread to send images asynchronously
#include <mutex> // Mutex support - for thread synchronization
#include <atomic> // Atomic support - for thread-safe boolean flags
#include <sqlite3.h> // SQLite database for FIFO queue
#include <condition_variable> // For efficient worker thread waiting
#include <fstream> // File stream operations - for saving BSZ files to disk
#include <termios.h> // Terminal I/O control - for hiding password input in getHiddenInput()
#include <unistd.h> // Unix standard definitions - provides STDIN_FILENO constant
#include <ctime> // Time functions - for std::time() to generate timestamps
#include <cstdlib> // C standard library - for setenv function
#include <cstdio> // C standard I/O - for snprintf function
#include <vector> // Dynamic arrays - for std::vector to hold image byte data
#include <chrono> // Time utilities - for std::chrono::seconds() delays
#include <algorithm> // Algorithm functions - for std::transform (string case conversion)
#include <tuple> // Tuple support - for std::tuple to hold job data with multiple fields
#include <cstring> // C string functions - for strcmp in database migration
#include <sqlite3.h> // SQLite3 database - for persistent work queue storage

// Locale support - for fixing locale issues with Bella Engine
#include <locale>

// Bella Engine SDK - for rendering and scene creation
#include "../bella_engine_sdk/src/bella_sdk/bella_engine.h"
#include "../bella_engine_sdk/src/dl_core/dl_main.inl"

// oomer's helper utility code
#include "../oom/oom_license.h"
#include "../oom/oom_bella_engine.h"
#include "../oom/oom_bella_misc.h"

/**
 * Structure representing a work item in the processing queue
 */
struct WorkItem {
    int64_t id;                    // Unique database ID
    std::string attachment_url;     // Discord attachment URL to download
    std::string original_filename;  // Original filename from Discord
    uint64_t channel_id;           // Discord channel ID for response
    uint64_t user_id;              // Discord user ID for mentions
    std::string username;          // Discord username for display
    std::string message_content;   // Discord message content for orbit/resolution parsing
    int64_t created_at;            // Unix timestamp when job was created
    int retry_count;               // Number of times this job has been retried
    
    WorkItem() : id(0), channel_id(0), user_id(0), created_at(0), retry_count(0) {}
};

/**
 * SQLite-backed FIFO work queue for managing .bsz file processing jobs
 * Provides persistence across system crashes and sequential processing
 */
class WorkQueue {
private:
    sqlite3* db;
    std::mutex queue_mutex;
    std::condition_variable queue_condition;
    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> cancel_current_job{false};
    std::atomic<int64_t> current_job_id{0};
    
public:
    WorkQueue() : db(nullptr) {}
    
    ~WorkQueue() {
        if (db) {
            sqlite3_close(db);
        }
    }
    
    /**
     * Initialize SQLite database and create work queue table
     */
    bool initialize(const std::string& db_path = "work_queue.db") {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        // Open SQLite database (creates file if it doesn't exist)
        int rc = sqlite3_open(db_path.c_str(), &db);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to open SQLite database: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        // Create work queue table if it doesn't exist
        const char* create_table_sql = R"(
            CREATE TABLE IF NOT EXISTS work_queue (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                attachment_url TEXT NOT NULL,
                original_filename TEXT NOT NULL,
                channel_id INTEGER NOT NULL,
                user_id INTEGER NOT NULL,
                created_at INTEGER NOT NULL,
                retry_count INTEGER DEFAULT 0,
                status TEXT DEFAULT 'pending',
                bella_start_time INTEGER DEFAULT 0,
                bella_end_time INTEGER DEFAULT 0,
                username TEXT DEFAULT '',
                message_content TEXT DEFAULT ''
            );
            
            CREATE INDEX IF NOT EXISTS idx_status_created 
            ON work_queue(status, created_at);
        )";
        
        char* error_msg = nullptr;
        rc = sqlite3_exec(db, create_table_sql, nullptr, nullptr, &error_msg);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to create work queue table: " << error_msg << std::endl;
            sqlite3_free(error_msg);
            return false;
        }
        
        std::cout << "✅ Work queue database initialized: " << db_path << std::endl;
        
        // Database migration: Add missing columns if they don't exist
        const char* check_column_sql = "PRAGMA table_info(work_queue);";
        bool has_bella_start_time = false;
        bool has_bella_end_time = false;
        bool has_username = false;
        bool has_message_content = false;
        
        sqlite3_stmt* pragma_stmt;
        rc = sqlite3_prepare_v2(db, check_column_sql, -1, &pragma_stmt, nullptr);
        if (rc == SQLITE_OK) {
            while (sqlite3_step(pragma_stmt) == SQLITE_ROW) {
                const char* column_name = (const char*)sqlite3_column_text(pragma_stmt, 1);
                if (column_name) {
                    if (strcmp(column_name, "bella_start_time") == 0) {
                        has_bella_start_time = true;
                    } else if (strcmp(column_name, "bella_end_time") == 0) {
                        has_bella_end_time = true;
                    } else if (strcmp(column_name, "username") == 0) {
                        has_username = true;
                    } else if (strcmp(column_name, "message_content") == 0) {
                        has_message_content = true;
                    }
                }
            }
            sqlite3_finalize(pragma_stmt);
        }
        
        if (!has_bella_start_time) {
            std::cout << "🔄 Migrating database: Adding bella_start_time column..." << std::endl;
            const char* add_column_sql = "ALTER TABLE work_queue ADD COLUMN bella_start_time INTEGER DEFAULT 0;";
            rc = sqlite3_exec(db, add_column_sql, nullptr, nullptr, &error_msg);
            if (rc != SQLITE_OK) {
                std::cerr << "❌ Failed to add bella_start_time column: " << error_msg << std::endl;
                sqlite3_free(error_msg);
            } else {
                std::cout << "✅ Database migration: bella_start_time column added" << std::endl;
            }
        }
        
        if (!has_bella_end_time) {
            std::cout << "🔄 Migrating database: Adding bella_end_time column..." << std::endl;
            const char* add_end_column_sql = "ALTER TABLE work_queue ADD COLUMN bella_end_time INTEGER DEFAULT 0;";
            rc = sqlite3_exec(db, add_end_column_sql, nullptr, nullptr, &error_msg);
            if (rc != SQLITE_OK) {
                std::cerr << "❌ Failed to add bella_end_time column: " << error_msg << std::endl;
                sqlite3_free(error_msg);
            } else {
                std::cout << "✅ Database migration: bella_end_time column added" << std::endl;
            }
        }
        
        if (!has_username) {
            std::cout << "🔄 Migrating database: Adding username column..." << std::endl;
            const char* add_username_column_sql = "ALTER TABLE work_queue ADD COLUMN username TEXT DEFAULT '';";
            rc = sqlite3_exec(db, add_username_column_sql, nullptr, nullptr, &error_msg);
            if (rc != SQLITE_OK) {
                std::cerr << "❌ Failed to add username column: " << error_msg << std::endl;
                sqlite3_free(error_msg);
            } else {
                std::cout << "✅ Database migration: username column added" << std::endl;
            }
        }
        
        if (!has_message_content) {
            std::cout << "🔄 Migrating database: Adding message_content column..." << std::endl;
            const char* add_message_content_column_sql = "ALTER TABLE work_queue ADD COLUMN message_content TEXT DEFAULT '';";
            rc = sqlite3_exec(db, add_message_content_column_sql, nullptr, nullptr, &error_msg);
            if (rc != SQLITE_OK) {
                std::cerr << "❌ Failed to add message_content column: " << error_msg << std::endl;
                sqlite3_free(error_msg);
            } else {
                std::cout << "✅ Database migration: message_content column added" << std::endl;
            }
        }
        
        // Reset any stuck 'processing' jobs back to 'pending' on startup
        // This handles cases where the bot was stopped while jobs were being processed
        const char* reset_processing_sql = "UPDATE work_queue SET status = 'pending' WHERE status = 'processing';";
        rc = sqlite3_exec(db, reset_processing_sql, nullptr, nullptr, &error_msg);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to reset stuck processing jobs: " << error_msg << std::endl;
            sqlite3_free(error_msg);
        } else {
            int reset_count = sqlite3_changes(db);
            if (reset_count > 0) {
                std::cout << "🔄 Reset " << reset_count << " stuck processing job(s) back to pending" << std::endl;
            }
        }
        
        // Log any existing pending jobs on startup
        int pending_count = getPendingJobCount();
        if (pending_count > 0) {
            std::cout << "📋 Found " << pending_count << " pending jobs from previous session" << std::endl;
        }
        
        return true;
    }
    
    /**
     * Add a new work item to the queue
     */
    bool enqueue(const WorkItem& item) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        const char* insert_sql = R"(
            INSERT INTO work_queue 
            (attachment_url, original_filename, channel_id, user_id, username, message_content, created_at, retry_count)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?);
        )";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare insert statement: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        // Bind parameters
        sqlite3_bind_text(stmt, 1, item.attachment_url.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, item.original_filename.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, item.channel_id);
        sqlite3_bind_int64(stmt, 4, item.user_id);
        sqlite3_bind_text(stmt, 5, item.username.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, item.message_content.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 7, item.created_at);
        sqlite3_bind_int(stmt, 8, item.retry_count);
        
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        if (rc != SQLITE_DONE) {
            std::cerr << "❌ Failed to insert work item: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        std::cout << "📥 Enqueued job: " << item.original_filename << " (ID: " << sqlite3_last_insert_rowid(db) << ")" << std::endl;
        
        // Notify worker thread that new work is available
        queue_condition.notify_one();
        return true;
    }
    
    /**
     * Get the next work item from the queue (FIFO order)
     * Blocks until work is available or shutdown is requested
     */
    bool dequeue(WorkItem& item) {
        std::unique_lock<std::mutex> lock(queue_mutex);
        
        while (!shutdown_requested) {
            const char* select_sql = R"(
                SELECT id, attachment_url, original_filename, channel_id, user_id, username, message_content, created_at, retry_count
                FROM work_queue 
                WHERE status = 'pending'
                ORDER BY created_at ASC
                LIMIT 1;
            )";
            
            sqlite3_stmt* stmt;
            int rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr);
            if (rc != SQLITE_OK) {
                std::cerr << "❌ Failed to prepare select statement: " << sqlite3_errmsg(db) << std::endl;
                return false;
            }
            
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                // Found a work item
                item.id = sqlite3_column_int64(stmt, 0);
                item.attachment_url = (const char*)sqlite3_column_text(stmt, 1);
                item.original_filename = (const char*)sqlite3_column_text(stmt, 2);
                item.channel_id = sqlite3_column_int64(stmt, 3);
                item.user_id = sqlite3_column_int64(stmt, 4);
                item.username = (const char*)sqlite3_column_text(stmt, 5);
                item.message_content = (const char*)sqlite3_column_text(stmt, 6);
                item.created_at = sqlite3_column_int64(stmt, 7);
                item.retry_count = sqlite3_column_int(stmt, 8);
                
                sqlite3_finalize(stmt);
                
                // Mark as processing
                markProcessing(item.id);
                
                std::cout << "📤 Dequeued job " << item.id << ": " << item.original_filename << std::endl;
                return true;
                
            } else if (rc == SQLITE_DONE) {
                // No work available, wait for notification
                sqlite3_finalize(stmt);
                queue_condition.wait(lock);
                
            } else {
                // Error occurred
                std::cerr << "❌ Failed to select work item: " << sqlite3_errmsg(db) << std::endl;
                sqlite3_finalize(stmt);
                return false;
            }
        }
        
        return false; // Shutdown requested
    }
    
    /**
     * Mark a work item as completed (keeps it in database for history)
     */
    bool markCompleted(int64_t item_id) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        const char* update_sql = "UPDATE work_queue SET status = 'completed', bella_end_time = ? WHERE id = ?;";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare completion update: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        sqlite3_bind_int64(stmt, 1, std::time(nullptr));
        sqlite3_bind_int64(stmt, 2, item_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        if (rc != SQLITE_DONE) {
            std::cerr << "❌ Failed to mark work item as completed: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        std::cout << "✅ Completed job " << item_id << std::endl;
        return true;
    }
    
    /**
     * Mark a work item as failed and update retry count
     */
    bool markFailed(int64_t item_id, int max_retries = 3) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        // First, get current retry count
        const char* select_sql = "SELECT retry_count FROM work_queue WHERE id = ?;";
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare retry count select: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        sqlite3_bind_int64(stmt, 1, item_id);
        rc = sqlite3_step(stmt);
        
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            std::cerr << "❌ Work item " << item_id << " not found for retry update" << std::endl;
            return false;
        }
        
        int current_retries = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        
        if (current_retries >= max_retries) {
            // Too many retries, remove from queue
            std::cout << "💀 Job " << item_id << " failed permanently after " << current_retries << " retries" << std::endl;
            const char* delete_sql = "DELETE FROM work_queue WHERE id = ?;";
            sqlite3_prepare_v2(db, delete_sql, -1, &stmt, nullptr);
            sqlite3_bind_int64(stmt, 1, item_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        } else {
            // Increment retry count and mark as pending for retry
            std::cout << "🔄 Job " << item_id << " failed, retry " << (current_retries + 1) << "/" << max_retries << std::endl;
            const char* update_sql = "UPDATE work_queue SET retry_count = ?, status = 'pending' WHERE id = ?;";
            sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, current_retries + 1);
            sqlite3_bind_int64(stmt, 2, item_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            
            // Notify worker thread to try again
            queue_condition.notify_one();
        }
        
        return true;
    }
    
    /**
     * Mark when bella starts rendering for a job
     */
    bool markBellaStarted(int64_t item_id) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        const char* update_sql = "UPDATE work_queue SET bella_start_time = ? WHERE id = ?;";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare bella start time update: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        sqlite3_bind_int64(stmt, 1, std::time(nullptr));
        sqlite3_bind_int64(stmt, 2, item_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        if (rc != SQLITE_DONE) {
            std::cerr << "❌ Failed to update bella start time: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        std::cout << "⏱️ Marked bella start time for job " << item_id << std::endl;
        return true;
    }
    
    /**
     * Get history of completed jobs for /history command
     * Returns a vector of tuples: (original_filename, username, bella_start_time, bella_end_time, created_at)
     */
    std::vector<std::tuple<std::string, std::string, int64_t, int64_t, int64_t>> getHistory(int limit = 10) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        std::vector<std::tuple<std::string, std::string, int64_t, int64_t, int64_t>> result;
        
        const char* history_sql = R"(
            SELECT original_filename, username, bella_start_time, bella_end_time, created_at
            FROM work_queue 
            WHERE status = 'completed' AND bella_start_time > 0 AND bella_end_time > 0
            ORDER BY bella_end_time DESC
            LIMIT ?;
        )";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, history_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare history query: " << sqlite3_errmsg(db) << std::endl;
            return result;
        }
        
        sqlite3_bind_int(stmt, 1, limit);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string filename = (const char*)sqlite3_column_text(stmt, 0);
            std::string username = (const char*)sqlite3_column_text(stmt, 1);
            int64_t bella_start_time = sqlite3_column_int64(stmt, 2);
            int64_t bella_end_time = sqlite3_column_int64(stmt, 3);
            int64_t created_at = sqlite3_column_int64(stmt, 4);
            result.emplace_back(filename, username, bella_start_time, bella_end_time, created_at);
        }
        
        sqlite3_finalize(stmt);
        return result;
    }
    
    /**
     * Signal the current job to be cancelled
     * Returns the filename of the job being cancelled, or empty string if none
     */
    std::string cancelCurrentJob() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        const char* select_sql = R"(
            SELECT id, original_filename FROM work_queue 
            WHERE status = 'processing'
            ORDER BY created_at ASC
            LIMIT 1;
        )";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare current job select: " << sqlite3_errmsg(db) << std::endl;
            return "";
        }
        
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            int64_t job_id = sqlite3_column_int64(stmt, 0);
            std::string filename = (const char*)sqlite3_column_text(stmt, 1);
            sqlite3_finalize(stmt);
            
            // Signal cancellation
            cancel_current_job = true;
            std::cout << "🛑 Admin requested cancellation of job " << job_id << ": " << filename << std::endl;
            return filename;
        } else {
            sqlite3_finalize(stmt);
        }
        
        return ""; // No job found
    }
    
    /**
     * Check if the current job should be cancelled
     */
    bool shouldCancelCurrentJob() {
        return cancel_current_job.load();
    }
    
    /**
     * Mark the current job as cancelled and remove it
     */
    void markCurrentJobCancelled() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        // Reset cancellation flag
        cancel_current_job = false;
        
        // Remove the cancelled job
        int64_t job_id = current_job_id.load();
        if (job_id > 0) {
            const char* delete_sql = "DELETE FROM work_queue WHERE id = ?;";
            sqlite3_stmt* stmt;
            int rc = sqlite3_prepare_v2(db, delete_sql, -1, &stmt, nullptr);
            if (rc == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, job_id);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
                std::cout << "🗑️ Cancelled job " << job_id << " removed from database" << std::endl;
            }
            current_job_id = 0;
        }
    }
    
    /**
     * Set the current job ID being processed
     */
    void setCurrentJobId(int64_t job_id) {
        current_job_id = job_id;
    }
    
    /**
     * Get the user ID of the current processing job owner
     * Returns 0 if no job is processing
     */
    uint64_t getCurrentJobOwnerId() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        const char* select_sql = R"(
            SELECT user_id FROM work_queue 
            WHERE status = 'processing'
            ORDER BY created_at ASC
            LIMIT 1;
        )";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return 0;
        }
        
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            uint64_t user_id = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            return user_id;
        }
        
        sqlite3_finalize(stmt);
        return 0; // No processing job found
    }
    
    /**
     * Request shutdown of the work queue
     */
    void requestShutdown() {
        shutdown_requested = true;
        queue_condition.notify_all();
    }
    
    /**
     * Get all jobs for queue display (processing + pending)
     * Returns a vector of tuples: (original_filename, username, is_processing, bella_start_time)
     */
    std::vector<std::tuple<std::string, std::string, bool, int64_t>> getQueueDisplay() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        std::vector<std::tuple<std::string, std::string, bool, int64_t>> result;
        
        // First get processing jobs (currently rendering)
        const char* processing_sql = R"(
            SELECT original_filename, username, bella_start_time
            FROM work_queue 
            WHERE status = 'processing'
            ORDER BY created_at ASC;
        )";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, processing_sql, -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string filename = (const char*)sqlite3_column_text(stmt, 0);
                std::string username = (const char*)sqlite3_column_text(stmt, 1);
                int64_t bella_start_time = sqlite3_column_int64(stmt, 2);
                result.emplace_back(filename, username, true, bella_start_time); // true = processing
            }
            sqlite3_finalize(stmt);
        }
        
        // Then get pending jobs
        const char* pending_sql = R"(
            SELECT original_filename, username
            FROM work_queue 
            WHERE status = 'pending'
            ORDER BY created_at ASC;
        )";
        
        rc = sqlite3_prepare_v2(db, pending_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare queue display query: " << sqlite3_errmsg(db) << std::endl;
            return result;
        }
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string filename = (const char*)sqlite3_column_text(stmt, 0);
            std::string username = (const char*)sqlite3_column_text(stmt, 1);
            result.emplace_back(filename, username, false, 0); // false = pending, 0 = no bella start time
        }
        
        sqlite3_finalize(stmt);
        return result;
    }
    
private:
    /**
     * Mark a work item as being processed
     */
    bool markProcessing(int64_t item_id) {
        const char* update_sql = "UPDATE work_queue SET status = 'processing' WHERE id = ?;";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return false;
        }
        
        sqlite3_bind_int64(stmt, 1, item_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        return rc == SQLITE_DONE;
    }
    
    /**
     * Get count of pending jobs in the queue
     */
    int getPendingJobCount() {
        const char* count_sql = "SELECT COUNT(*) FROM work_queue WHERE status = 'pending';";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, count_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return 0;
        }
        
        rc = sqlite3_step(stmt);
        int count = (rc == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
        sqlite3_finalize(stmt);
        
        return count;
    }
};

/**
 * Function to securely input text without displaying it on screen (like password input)
 * This is used for Discord bot tokens to keep them private in the terminal
 * 
 * @param prompt - The text to display before asking for input
 * @return std::string - The hidden text that was typed
 */
std::string getHiddenInput(const std::string& prompt) {
    // Display the prompt to the user
    std::cout << prompt;
    std::cout.flush();  // Force immediate output (don't wait for newline)
    
    // Save current terminal settings so we can restore them later
    termios oldt;
    tcgetattr(STDIN_FILENO, &oldt);  // Get current terminal attributes
    
    // Create new terminal settings that disable echo (hide typed characters)
    termios newt = oldt;             // Copy current settings
    newt.c_lflag &= ~ECHO;          // Remove the ECHO flag (disables character display)
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);  // Apply new settings immediately
    
    // Read the hidden input
    std::string input;
    std::getline(std::cin, input);   // Read entire line (including spaces)
    
    // Restore original terminal settings (re-enable echo)
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    std::cout << std::endl;          // Add newline since user's Enter wasn't displayed
    
    return input;
}

/**
 * Function to parse resolution from Discord message content
 * Supports format like "resolution=100x100" or "resolution=1920x1080"
 * 
 * @param message_content - The Discord message content
 * @param width - Reference to store parsed width (0 if not found)
 * @param height - Reference to store parsed height (0 if not found)
 * @return bool - True if resolution was successfully parsed
 */
bool parseResolution(const std::string& message_content, int& width, int& height) {
    width = 0;
    height = 0;
    
    // Look for "resolution=" pattern (case insensitive)
    std::string content_lower = message_content;
    std::transform(content_lower.begin(), content_lower.end(), content_lower.begin(), ::tolower);
    
    size_t pos = content_lower.find("resolution=");
    if (pos == std::string::npos) {
        return false; // Pattern not found
    }
    
    // Extract the resolution part after "resolution="
    size_t start = pos + 11; // Length of "resolution="
    if (start >= message_content.length()) {
        return false; // Nothing after "resolution="
    }
    
    // Find the end of the resolution specification (space, newline, or end of string)
    size_t end = message_content.find_first_of(" \t\n\r", start);
    if (end == std::string::npos) {
        end = message_content.length();
    }
    
    std::string resolution_str = message_content.substr(start, end - start);
    
    // Parse "WIDTHxHEIGHT" format
    size_t x_pos = resolution_str.find('x');
    if (x_pos == std::string::npos) {
        x_pos = resolution_str.find('X'); // Try uppercase X
    }
    
    if (x_pos == std::string::npos || x_pos == 0 || x_pos == resolution_str.length() - 1) {
        return false; // No 'x' found or 'x' at beginning/end
    }
    
    try {
        std::string width_str = resolution_str.substr(0, x_pos);
        std::string height_str = resolution_str.substr(x_pos + 1);
        
        width = std::stoi(width_str);
        height = std::stoi(height_str);
        
        // Validate reasonable resolution bounds
        if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
            std::cout << "⚠️ Invalid resolution values: " << width << "x" << height << " (must be 1-8192)" << std::endl;
            width = 0;
            height = 0;
            return false;
        }
        
        std::cout << "✅ Parsed resolution override: " << width << "x" << height << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cout << "⚠️ Failed to parse resolution numbers: " << e.what() << std::endl;
        return false;
    }
}

/**
 * Function to parse orbit from Discord message content
 * Supports format like "orbit=10" for 10 frames
 * 
 * @param message_content - The Discord message content
 * @return int - Number of frames to render (0 if not found)
 */
int parseOrbit(const std::string& message_content) {
    // Look for "orbit=" pattern (case insensitive)
    std::string content_lower = message_content;
    std::transform(content_lower.begin(), content_lower.end(), content_lower.begin(), ::tolower);
    
    size_t pos = content_lower.find("orbit=");
    if (pos == std::string::npos) {
        return 0; // Pattern not found
    }
    
    // Extract the number part after "orbit="
    size_t start = pos + 6; // Length of "orbit="
    if (start >= message_content.length()) {
        return 0; // Nothing after "orbit="
    }
    
    // Find the end of the number specification (space, newline, or end of string)
    size_t end = message_content.find_first_of(" \t\n\r", start);
    if (end == std::string::npos) {
        end = message_content.length();
    }
    
    std::string frames_str = message_content.substr(start, end - start);
    
    try {
        int frames = std::stoi(frames_str);
        
        // Validate reasonable frame count bounds
        if (frames <= 0 || frames > 300) {
            std::cout << "⚠️ Invalid orbit frame count: " << frames << " (must be 1-300)" << std::endl;
            return 0;
        }
        
        std::cout << "✅ Parsed orbit frames: " << frames << std::endl;
        return frames;
        
    } catch (const std::exception& e) {
        std::cout << "⚠️ Failed to parse orbit frame count: " << e.what() << std::endl;
        return 0;
    }
}

/**
 * Function to process .bsz file with bella path tracer
 * 
 * @param engine - Reference to the bella engine
 * @param bsz_data - The .bsz file data as bytes
 * @param filename - The original filename
 * @param message_content - The Discord message content for orbit parsing
 * @param work_queue - Reference to work queue for tracking bella start time
 * @param item_id - ID of the work item being processed
 * @return std::string - The output filename (JPEG or MP4)
 */
std::string processBszFile(dl::bella_sdk::Engine& engine, const std::vector<uint8_t>& bsz_data, const std::string& filename, const std::string& message_content, WorkQueue* work_queue, int64_t item_id) {
    std::cout << "🔄 Processing .bsz file (" << bsz_data.size() << " bytes)..." << std::endl;
    
    // Fix locale issues that can cause Bella Engine to fail
    try {
        std::locale::global(std::locale("C"));
        std::cout << "✅ Set locale to 'C' for Bella Engine compatibility" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "⚠️ Could not set locale: " << e.what() << std::endl;
    }
    
    // Save .bsz data to working file (FIFO processing uses single file)
    std::string temp_filename = "poomer-discord-bella.bsz";
    std::ofstream temp_file(temp_filename, std::ios::binary);
    temp_file.write(reinterpret_cast<const char*>(bsz_data.data()), bsz_data.size());
    temp_file.close();
    
    std::cout << "💾 Saved .bsz to working file: " << temp_filename << std::endl;
    
    try {
        // Load the .bsz file into bella engine
        auto belScene = engine.scene();
        std::cout << "📖 Loading .bsz scene into bella engine..." << std::endl;
        belScene.read(temp_filename.c_str());
       
        // Extract base filename without .bsz extension for output
        std::string base_filename = filename;
        if (base_filename.length() >= 4 && base_filename.substr(base_filename.length() - 4) == ".bsz") {
            base_filename = base_filename.substr(0, base_filename.length() - 4);
        }
         
        std::cout << "📷 Setting output filename to: " << base_filename << ".jpg" << std::endl;
         
        belScene.beautyPass()["outputExt"] = ".jpg";
        belScene.beautyPass()["outputName"] = base_filename.c_str();

        auto imgOutputPath = engine.scene().createNode("outputImagePath", "oomerOutputPath");
        imgOutputPath["ext"] = ".jpg";
        imgOutputPath["dir"] = ".";
        //belScene.beautyPass()["targetNoise"] = dl::Int(10);
        belScene.beautyPass()["saveImage"] = dl::Int(0);
        belScene.beautyPass()["overridePath"] = imgOutputPath;
        auto belCamera = belScene.camera();
        auto belCameraPath = belScene.cameraPath(); // Since camera can be instanced, we get the full path of th one currently define din scene settings
        
        // Check for orbit animation
        int orbit_frames = parseOrbit(message_content);
        
        // Check for resolution override (applies to both single frame and orbit)
        int override_width, override_height;
        bool has_resolution_override = parseResolution(message_content, override_width, override_height);
        
        std::cout << "✅ Loaded .bsz scene into bella engine" << std::endl;
        
        // Mark bella start time for queue tracking
        if (work_queue) {
            work_queue->markBellaStarted(item_id);
        }
        
        if (orbit_frames > 0) {
            // Orbit camera animation rendering
            std::cout << "🎨 Starting orbit animation with " << orbit_frames << " frames..." << std::endl;
            
            // Set resolution for orbit rendering
            if (has_resolution_override) {
                std::cout << "🖼️ Applying resolution override for orbit: " << override_width << "x" << override_height << std::endl;
                belCamera["resolution"] = dl::Vec2{override_width, override_height};
            } else {
                std::cout << "🖼️ Using default orbit resolution: 100x100" << std::endl;
                //belCamera["resolution"] = dl::Vec2{100, 100}; // Default smaller for faster processing
            }
            
            for (int i = 0; i < orbit_frames; i++) {
                // Check for cancellation before each frame
                if (work_queue && work_queue->shouldCancelCurrentJob()) {
                    std::cout << "🛑 Cancelling orbit render for job " << item_id << " at frame " << i << std::endl;
                    work_queue->markCurrentJobCancelled();
                    return ""; // Return empty string for cancelled job
                }
                
                std::cout << "📹 Rendering frame " << (i + 1) << "/" << orbit_frames << std::endl;
                
                // Apply orbit offset for this frame
                auto offset = dl::Vec2{i * 0.25, 0.0};
                dl::bella_sdk::orbitCamera(engine.scene().cameraPath(), offset);
                
                // Set output filename for this frame
                auto belBeautyPass = belScene.beautyPass();
                belBeautyPass["outputName"] = dl::String::format("frame_%04d", i);
                
                // Render this frame
                engine.start();
                while(engine.rendering()) { 
                    // Check for cancellation during frame rendering
                    if (work_queue && work_queue->shouldCancelCurrentJob()) {
                        std::cout << "🛑 Cancelling orbit render during frame " << (i + 1) << std::endl;
                        engine.stop();
                        work_queue->markCurrentJobCancelled();
                        return ""; // Return empty string for cancelled job
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                
                std::cout << "✅ Frame " << (i + 1) << " completed" << std::endl;
            }
            
            std::cout << "🎬 All frames rendered, creating MP4 with ffmpeg..." << std::endl;
            
            // Create MP4 using ffmpeg
            std::string output_mp4 = base_filename + ".mp4";
            std::string command = "ffmpeg -y -loglevel error -framerate 30 -i frame_%04d.jpg -c:v libx264 -pix_fmt yuv420p " + output_mp4;
            
            std::cout << "Executing FFmpeg command: " << command << std::endl;
            int result = std::system(command.c_str());
            
            if (result == 0) {
                std::cout << "✅ MP4 conversion successful: " << output_mp4 << std::endl;
                
                // Clean up individual frame files
                for (int i = 0; i < orbit_frames; i++) {
                    char frame_file[32];
                    snprintf(frame_file, sizeof(frame_file), "frame_%04d.jpg", i);
                    std::remove(frame_file);
                }
                std::cout << "🧹 Cleaned up individual frame files" << std::endl;
                
                return output_mp4;
            } else {
                std::cout << "❌ FFmpeg conversion failed with error code: " << result << std::endl;
                return ""; // Return empty string for failed conversion
            }
            
        } else {
            // Single frame rendering
            std::cout << "🎨 Starting single frame bella render..." << std::endl;
            
            // Apply resolution override for single frame if specified
            if (has_resolution_override) {
                std::cout << "🖼️ Applying resolution override: " << override_width << "x" << override_height << std::endl;
                belCamera["resolution"] = dl::Vec2{override_width, override_height};
            } else {
                std::cout << "🖼️ Using scene's default resolution (no override specified)" << std::endl;
                // Keep the scene's original resolution - no change needed
            }
            
            engine.start();
            
            // Wait for rendering to complete, checking for cancellation
            bool was_cancelled = false;
            while(engine.rendering()) { 
                // Check for cancellation every 500ms
                if (work_queue && work_queue->shouldCancelCurrentJob()) {
                    std::cout << "🛑 Cancelling bella render for job " << item_id << std::endl;
                    engine.stop(); // Stop the Bella render
                    was_cancelled = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            
            if (was_cancelled) {
                std::cout << "🛑 Bella render cancelled successfully" << std::endl;
                work_queue->markCurrentJobCancelled();
                return ""; // Return empty string for cancelled job
            }
            
            std::cout << "✅ Single frame render completed!" << std::endl;
            return base_filename + ".jpg";
        }
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Error processing .bsz file: " << e.what() << std::endl;
        return ""; // Return empty string for error
    }
}

/**
 * Worker thread function that processes the work queue sequentially
 * This replaces the previous approach of creating one thread per file
 */
void workerThread(dpp::cluster* bot, WorkQueue* work_queue, dl::bella_sdk::Engine* engine) {
    std::cout << "🔧 Worker thread started" << std::endl;
    
    WorkItem item;
    while (work_queue->dequeue(item)) {
        std::cout << "\n--- PROCESSING BSZ FILE (Job " << item.id << ") ---" << std::endl;
        std::cout << "Downloading: " << item.original_filename << std::endl;
        std::cout << "From URL: " << item.attachment_url << std::endl;
        
        // Download the .bsz file using DPP's HTTP client
        std::cout << "🌐 Starting .bsz file download..." << std::endl;
        
        // Use a promise/future to make the async download synchronous for this worker
        std::mutex download_mutex;
        std::condition_variable download_cv;
        bool download_complete = false;
        bool download_success = false;
        std::string download_data;
        
        bot->request(item.attachment_url, dpp::m_get, [&](const dpp::http_request_completion_t& response) {
            std::lock_guard<std::mutex> lock(download_mutex);
            
            if (response.status == 200) {
                std::cout << "✅ Downloaded .bsz file (" << response.body.size() << " bytes)" << std::endl;
                download_data = response.body;
                download_success = true;
            } else {
                std::cout << "❌ Failed to download .bsz file. Status: " << response.status << std::endl;
                download_success = false;
            }
            
            download_complete = true;
            download_cv.notify_one();
        });
        
        // Wait for download to complete
        {
            std::unique_lock<std::mutex> lock(download_mutex);
            download_cv.wait(lock, [&]{ return download_complete; });
        }
        
        if (download_success) {
            // Set current job ID for cancellation tracking
            work_queue->setCurrentJobId(item.id);
            
            // Convert response body to vector for processing
            std::vector<uint8_t> original_data(download_data.begin(), download_data.end());
            
            // Check for cancellation before processing
            if (work_queue->shouldCancelCurrentJob()) {
                std::cout << "🛑 Job " << item.id << " cancelled before processing" << std::endl;
                work_queue->markCurrentJobCancelled();
                continue;
            }
            
            // Process the .bsz file with bella path tracer
            std::string output_filename = processBszFile(*engine, original_data, item.original_filename, item.message_content, work_queue, item.id);
            
            // Check if job was cancelled during processing
            if (work_queue->shouldCancelCurrentJob() || output_filename.empty()) {
                std::cout << "🛑 Job " << item.id << " was cancelled or failed during processing, skipping result handling" << std::endl;
                if (work_queue->shouldCancelCurrentJob()) {
                    work_queue->markCurrentJobCancelled();
                }
                continue; // Skip to next job
            }
            
            // Variables for message creation
            std::vector<uint8_t> file_data;
            dpp::message msg(item.channel_id, "");
            
            // Read the output file from disk (JPEG or MP4)
            std::ifstream output_file(output_filename, std::ios::binary);
            
            if (output_file.is_open()) {
                // Get file size
                output_file.seekg(0, std::ios::end);
                size_t file_size = output_file.tellg();
                output_file.seekg(0, std::ios::beg);
                
                // Read file data
                file_data.resize(file_size);
                output_file.read(reinterpret_cast<char*>(file_data.data()), file_size);
                output_file.close();
                
                std::cout << "📁 Read output file: " << output_filename << " (" << file_data.size() << " bytes)" << std::endl;
                
                // Check if it's an MP4 (orbit animation) or JPEG (single frame)
                bool is_mp4 = (output_filename.length() >= 4 && output_filename.substr(output_filename.length() - 4) == ".mp4");
                
                if (is_mp4) {
                    msg.content = "🎬 Here's your orbit animation! <@" + std::to_string(item.user_id) + ">";
                } else {
                    msg.content = "🎨 Here's your rendered image! <@" + std::to_string(item.user_id) + ">";
                }
                msg.add_file(output_filename, std::string(file_data.begin(), file_data.end()));
            } else {
                std::cout << "❌ Could not read output file: " << output_filename << std::endl;
                // Fallback: send error message
                msg.content = "❌ Rendering completed but could not read output file. <@" + std::to_string(item.user_id) + ">";
            }
            
            // Send the message (either JPEG or error message)
            std::mutex send_mutex;
            std::condition_variable send_cv;
            bool send_complete = false;
            bool send_success = false;
            
            bot->message_create(msg, [&](const dpp::confirmation_callback_t& callback) {
                std::lock_guard<std::mutex> lock(send_mutex);
                
                if (callback.is_error()) {
                    std::cout << "❌ Failed to send message: " << callback.get_error().message << std::endl;
                    send_success = false;
                } else {
                    if (file_data.empty()) {
                        std::cout << "✅ Successfully sent error message for " << item.original_filename << std::endl;
                    } else {
                        std::cout << "✅ Successfully sent " << output_filename << "!" << std::endl;
                    }
                    send_success = true;
                }
                
                send_complete = true;
                send_cv.notify_one();
            });
            
            // Wait for send to complete
            {
                std::unique_lock<std::mutex> lock(send_mutex);
                send_cv.wait(lock, [&]{ return send_complete; });
            }
            
            if (send_success && !file_data.empty()) {
                work_queue->markCompleted(item.id);
            } else {
                work_queue->markFailed(item.id);
            }
            
        } else {
            // Download failed
            bot->message_create(dpp::message(item.channel_id, "❌ Failed to download .bsz file for processing."));
            work_queue->markFailed(item.id);
        }
        
        // Small delay between processing jobs to prevent overwhelming the system
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    std::cout << "🔧 Worker thread shutting down" << std::endl;
}

/**
 * MAIN FUNCTION - Entry point of the Discord bot program using Diffuse Logic framework
 * Sets up the bot, registers commands, and starts the event loop
 */
int DL_main(dl::Args& args) {
    // Fix locale issues early to prevent Bella Engine problems
    try {
        std::locale::global(std::locale("C"));
        setenv("LC_ALL", "C", 1);
        setenv("LANG", "C", 1);
        std::cout << "✅ Set system locale to 'C' for compatibility" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "⚠️ Could not set initial locale: " << e.what() << std::endl;
    }

    // Setup Bella logging callbacks
    int s_oomBellaLogContext = 0; 
    dl::subscribeLog(&s_oomBellaLogContext, oom::bella::log);
    dl::flushStartupMessages();

    // Add command line arguments
    args.add("tp", "thirdparty",    "",   "prints third party licenses");
    args.add("li", "licenseinfo",   "",   "prints license info");
    args.add("t",  "token",         "",   "Discord bot token");

    if (args.helpRequested()) {
        std::cout << args.help("poomer-discord-bella (C) 2025 Harvey Fong","", "1.0") << std::endl;
        return 0;
    }
    
    if (args.have("--licenseinfo")) {
        std::cout << "poomer-discord-bella (C) 2025 Harvey Fong" << std::endl;
        std::cout << oom::license::printLicense() << std::endl;
        return 0;
    }
 
    if (args.have("--thirdparty")) {
        std::cout << oom::license::printBellaSDK() << "\n====\n" << std::endl;
        return 0;
    }

    // Step 1: Initialize Bella Engine
    std::cout << "=== Discord Bot Startup ===" << std::endl;
    std::cout << "🎨 Initializing Bella Engine..." << std::endl;
    
    dl::bella_sdk::Engine engine;
    engine.scene().loadDefs();
    
    // Create an engine observer to catch Engine event callbacks
    oom::bella::MyEngineObserver engineObserver;
    engine.subscribe(&engineObserver);
    
    std::cout << "✅ Bella Engine initialized" << std::endl;

    // Step 2: Initialize work queue database
    std::cout << "🗄️ Initializing work queue database..." << std::endl;
    
    WorkQueue work_queue;
    if (!work_queue.initialize()) {
        std::cerr << "❌ Failed to initialize work queue database" << std::endl;
        return 1;
    }
    
    // Step 3: Get Discord bot token from command line or user input
    std::string BOT_TOKEN;
    if (args.have("--token")) {
        BOT_TOKEN = args.value("--token").buf();
        std::cout << "✅ Using token from command line" << std::endl;
    } else {
        BOT_TOKEN = getHiddenInput("Enter Discord Bot Token: ");
    }
    
    // Validate that a token was provided
    if (BOT_TOKEN.empty()) {
        std::cerr << "Error: Bot token cannot be empty!" << std::endl;
        return 1;  // Exit with error code
    }

    // Step 4: Create Discord bot instance
    // dpp::cluster is the main bot class that handles all Discord connections
    // i_default_intents gives basic permissions, i_message_content allows reading message text
	dpp::cluster bot(BOT_TOKEN, dpp::i_default_intents | dpp::i_message_content);

    // Step 5: Enable logging to see what the bot is doing
    bot.on_log(dpp::utility::cout_logger());
    
    // Step 6: Start worker thread for processing the queue
    std::cout << "🔧 Starting worker thread..." << std::endl;
    std::thread worker(workerThread, &bot, &work_queue, &engine);

    // Step 7: Set up event handler for file uploads
    // Lambda function that gets called whenever a message with attachments is posted
    bot.on_message_create([&work_queue](const dpp::message_create_t& event) {
        
        // Ignore messages sent by bots (including our own) to prevent loops
        if (event.msg.author.is_bot()) {
            return;  // Early exit - don't process bot messages
        }
        
        // Check if this message has any file attachments
        if (!event.msg.attachments.empty()) {
            // Debug output to console (only you see this, not Discord users)
            std::cout << "\n=== FILE UPLOAD DETECTED ===" << std::endl;
            std::cout << "User: " << event.msg.author.username << "#" << event.msg.author.discriminator << std::endl;
            std::cout << "Channel ID: " << event.msg.channel_id << std::endl;
            std::cout << "Message ID: " << event.msg.id << std::endl;
            std::cout << "Attachments: " << event.msg.attachments.size() << std::endl;
            
            // Flag to track if we found any .bsz files
            bool found_bsz = false;
            std::vector<dpp::attachment> bsz_attachments;  // Store .bsz attachments for processing
            
            // Loop through each attached file
            for (const auto& attachment : event.msg.attachments) {
                // Print file details to console
                std::cout << "  - File: " << attachment.filename << std::endl;
                std::cout << "    Size: " << attachment.size << " bytes" << std::endl;
                std::cout << "    URL: " << attachment.url << std::endl;
                
                // Check if filename ends with ".bsz" (case-insensitive)
                std::string filename_lower = attachment.filename;
                // Convert entire filename to lowercase for easier comparison
                std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);
                
                // Check if file ends with .bsz (4 chars)
                // substr(length - N) gets the last N characters of the string
                if (filename_lower.length() >= 4 && filename_lower.substr(filename_lower.length() - 4) == ".bsz") {
                    std::cout << "    ✅ BSZ FILE DETECTED!" << std::endl;
                    found_bsz = true;
                    bsz_attachments.push_back(attachment);  // Store for processing
                }
            }
            
            // If we found .bsz files, enqueue them for processing
            if (found_bsz) {
                std::cout << "\n🎯 ACTION: Enqueueing .bsz files for processing" << std::endl;
                
                // Add emoji reaction to the message (like clicking a reaction in Discord)
                event.reply("🎨 Bella scene(s) detected! Adding to render queue...");
                
                // Enqueue each .bsz file for sequential processing
                for (const auto& bsz_attachment : bsz_attachments) {
                    WorkItem item;
                    item.attachment_url = bsz_attachment.url;
                    item.original_filename = bsz_attachment.filename;
                    item.channel_id = event.msg.channel_id;
                    item.user_id = event.msg.author.id;
                    item.username = event.msg.author.username;
                    item.message_content = event.msg.content; // Store message content for orbit parsing
                    item.created_at = std::time(nullptr);
                    item.retry_count = 0;
                    
                    if (work_queue.enqueue(item)) {
                        std::cout << "✅ Enqueued: " << bsz_attachment.filename << std::endl;
                    } else {
                        std::cout << "❌ Failed to enqueue: " << bsz_attachment.filename << std::endl;
                    }
                }
            }
            std::cout << "============================" << std::endl;
        }
    });

    // Step 8: Set up slash command handler (commands that start with /)
    // [&bot, &work_queue] captures variables by reference so we can use them inside the lambda
    bot.on_slashcommand([&bot, &work_queue](const dpp::slashcommand_t& event) {
        // Generate unique ID for this command execution (for debugging)
        static int command_counter = 0;  // static = keeps value between function calls (like a global variable)
        int command_id = ++command_counter;  // Pre-increment: add 1 then use value (so first command = 1)
        
        // Print detailed debug information about the command
        std::cout << "\n=== COMMAND RECEIVED #" << command_id << " ===" << std::endl;
        std::cout << "Command: " << event.command.get_command_name() << std::endl;
        std::cout << "User: " << event.command.get_issuing_user().username << "#" << event.command.get_issuing_user().discriminator << std::endl;
        std::cout << "User ID: " << event.command.get_issuing_user().id << std::endl;
        std::cout << "Guild ID: " << event.command.guild_id << std::endl;
        std::cout << "Channel ID: " << event.command.channel_id << std::endl;
        std::cout << "Timestamp: " << std::time(nullptr) << std::endl;  // Unix timestamp
        std::cout << "Interaction ID: " << event.command.id << std::endl;
        
        // Check which command was invoked
        if (event.command.get_command_name() == "help") {
            // CRITICAL: Must acknowledge Discord interaction within 3 seconds
            // or Discord shows "The application did not respond" error
            std::cout << "Sending interaction acknowledgment for command #" << command_id << "..." << std::endl;
            // Standard help message (admin commands are discoverable but user-protected)
            std::string help_message = "🚀 I am a Bella render bot, ready to render your scenes!\n\n**Commands:**\n• Upload .bsz files - I'll automatically render them\n• `/queue` - See current render queue\n• `/history` - View recently completed renders\n• `/remove` - Cancel current rendering job (your own jobs or admin)";
            
            event.reply(help_message);
            
            // Generate unique thread ID for debugging async operations
            static int thread_counter = 0;  // Separate counter for threads
            int current_thread_id = ++thread_counter;
            
        } else if (event.command.get_command_name() == "queue") {
            std::cout << "Processing queue command #" << command_id << "..." << std::endl;
            
            // Get all jobs (processing + pending) from the work queue
            auto queue_jobs = work_queue.getQueueDisplay();
            
            // Count processing and pending jobs separately
            size_t processing_count = 0;
            size_t pending_count = 0;
            for (const auto& job : queue_jobs) {
                if (std::get<2>(job)) { // is_processing
                    processing_count++;
                } else {
                    pending_count++;
                }
            }
            
            if (queue_jobs.empty()) {
                // No jobs in queue at all
                event.reply("🎉 Congrats, there are no queued jobs, any .bsz file you send me will be processed immediately!");
            } else {
                // Build queue display message
                std::string queue_message = "";
                
                size_t pending_position = 1;
                
                for (const auto& job : queue_jobs) {
                    const std::string& filename = std::get<0>(job);
                    const std::string& username = std::get<1>(job);
                    bool is_processing = std::get<2>(job);
                    int64_t bella_start_time = std::get<3>(job);
                    
                    if (is_processing) {
                        std::string render_time_text = "";
                        if (bella_start_time > 0) {
                            int64_t elapsed_seconds = std::time(nullptr) - bella_start_time;
                            int minutes = elapsed_seconds / 60;
                            int seconds = elapsed_seconds % 60;
                            
                            if (minutes > 0) {
                                render_time_text = " (" + std::to_string(minutes) + "m " + std::to_string(seconds) + "s)";
                            } else {
                                render_time_text = " (" + std::to_string(seconds) + "s)";
                            }
                        }
                        
                        queue_message += "**Rendering:** `" + filename + "` - " + username + render_time_text + "\n";
                    } else {
                        queue_message += std::to_string(pending_position) + ". `" + filename + "` - " + username + "\n";
                        pending_position++;
                    }
                }
                
                //queue_message += "\n*Jobs are processed in FIFO (first-in, first-out) order.*";
                event.reply(queue_message);
            }
            
        } else if (event.command.get_command_name() == "history") {
            std::cout << "Processing history command #" << command_id << "..." << std::endl;
            
            // Get recent completed jobs from the work queue
            auto history_jobs = work_queue.getHistory(10); // Get last 10 completed jobs
            
            if (history_jobs.empty()) {
                event.reply("📜 No completed renders found in history.");
            } else {
                std::string history_message = "📜 **Recent Completed Renders:**\n\n";
                
                for (const auto& job : history_jobs) {
                    const std::string& filename = std::get<0>(job);
                    const std::string& username = std::get<1>(job);
                    int64_t bella_start_time = std::get<2>(job);
                    int64_t bella_end_time = std::get<3>(job);
                    int64_t created_at = std::get<4>(job);
                    
                    // Calculate render time
                    int64_t render_seconds = bella_end_time - bella_start_time;
                    std::string render_time_text;
                    
                    if (render_seconds > 0) {
                        int minutes = render_seconds / 60;
                        int seconds = render_seconds % 60;
                        
                        if (minutes > 0) {
                            render_time_text = " ⏱️ " + std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
                        } else {
                            render_time_text = " ⏱️ " + std::to_string(seconds) + "s";
                        }
                    } else {
                        render_time_text = " ⏱️ timing data incomplete";
                    }
                    
                    history_message += "`" + filename + "` - " + username + render_time_text + "\n";
                }
                
                event.reply(history_message);
            }
            
        } else if (event.command.get_command_name() == "remove") {
            std::cout << "Processing remove command #" << command_id << "..." << std::endl;
            
            // List of admin user IDs who can cancel any job
            const std::vector<uint64_t> ADMIN_USER_IDS = {
                780541438022254624ULL  // harvey
                // Add more admin user IDs here, separated by commas
                // 123456789012345678ULL,  // another admin
            };
            
            uint64_t requesting_user_id = event.command.get_issuing_user().id;
            bool is_admin = std::find(ADMIN_USER_IDS.begin(), ADMIN_USER_IDS.end(), requesting_user_id) != ADMIN_USER_IDS.end();
            
            // Check if there's a current job and get its submitter
            bool is_job_owner = false;
            std::string current_job_filename = "";
            
            // Get current processing job info to check ownership
            auto current_jobs = work_queue.getQueueDisplay();
            for (const auto& job : current_jobs) {
                bool is_processing = std::get<2>(job); // is_processing flag
                if (is_processing) {
                    current_job_filename = std::get<0>(job); // filename
                    // We need to get the user_id from database since getQueueDisplay returns username
                    break;
                }
            }
            
            // Get the actual user_id of the job owner from database
            if (!current_job_filename.empty()) {
                auto job_owner_id = work_queue.getCurrentJobOwnerId();
                if (job_owner_id == requesting_user_id) {
                    is_job_owner = true;
                }
            }
            
            // Allow access if user is admin OR owns the current job
            if (!is_admin && !is_job_owner) {
                if (current_job_filename.empty()) {
                    event.reply("ℹ️ No job is currently being processed.");
                } else {
                    std::cout << "🚫 Unauthorized remove command attempt from user: " << event.command.get_issuing_user().username 
                             << " (ID: " << requesting_user_id << ") - not admin and not job owner" << std::endl;
                    event.reply("🚫 Access denied. You can only cancel your own jobs (or be an admin).");
                }
                return;
            }
            
            std::string user_type = is_admin ? "admin" : "job owner";
            std::cout << "✅ Remove command authorized for " << user_type << ": " << event.command.get_issuing_user().username << std::endl;
            
            // User is authorized (admin or job owner), proceed with cancellation
            std::string cancelled_filename = work_queue.cancelCurrentJob();
            
            if (!cancelled_filename.empty()) {
                event.reply("🛑 **Cancelling Bella render:** `" + cancelled_filename + "`\n\nImage interrupted before reaching target noise level.");
            } else {
                event.reply("ℹ️ No job is currently being processed.");
            }
            
        } else {
            // Handle unexpected commands (probably old cached registrations)
            std::cout << "Status: UNEXPECTED COMMAND (probably cached registration) - Command #" << command_id << std::endl;
            std::cout << "========================" << std::endl;
            event.reply("⚠️ This command is no longer supported. Please use `/help`, `/queue`, `/history`, or `/remove`.");
        }
    });

    // Helper function to register all slash commands
    auto register_all_commands = [&bot]() {
        std::cout << "Registering commands..." << std::endl;
        
        // Register help command
        bot.global_command_create(dpp::slashcommand("help", "Show information about available commands", bot.me.id), [](const dpp::confirmation_callback_t& reg_callback) {
            if (reg_callback.is_error()) {
                std::cout << "❌ Failed to register help command: " << reg_callback.get_error().message << std::endl;
            } else {
                std::cout << "✅ Help command registered successfully!" << std::endl;
            }
        });
        
        // Register queue command
        bot.global_command_create(dpp::slashcommand("queue", "Show current render queue status", bot.me.id), [](const dpp::confirmation_callback_t& reg_callback) {
            if (reg_callback.is_error()) {
                std::cout << "❌ Failed to register queue command: " << reg_callback.get_error().message << std::endl;
            } else {
                std::cout << "✅ Queue command registered successfully!" << std::endl;
            }
        });
        
        // Register history command
        bot.global_command_create(dpp::slashcommand("history", "Show recently completed renders with timing", bot.me.id), [](const dpp::confirmation_callback_t& reg_callback) {
            if (reg_callback.is_error()) {
                std::cout << "❌ Failed to register history command: " << reg_callback.get_error().message << std::endl;
            } else {
                std::cout << "✅ History command registered successfully!" << std::endl;
            }
        });
        
        // Register admin remove command
        bot.global_command_create(dpp::slashcommand("remove", "Cancel current processing job (admin or job owner)", bot.me.id), [](const dpp::confirmation_callback_t& reg_callback) {
            if (reg_callback.is_error()) {
                std::cout << "❌ Failed to register remove command: " << reg_callback.get_error().message << std::endl;
            } else {
                std::cout << "✅ Remove command registered successfully!" << std::endl;
            }
        });
    };

    // Step 9: Set up bot ready event (called when bot successfully connects to Discord)
    bot.on_ready([&bot, register_all_commands](const dpp::ready_t& event) {
        // run_once ensures this only happens on first connection, not reconnections
	    if (dpp::run_once<struct register_bot_commands>()) {
	        std::cout << "Bot is ready! Starting command registration..." << std::endl;
	        std::cout << "Bot user: " << bot.me.username << " (ID: " << bot.me.id << ")" << std::endl;
	        
	        // Clean up any old slash commands before registering new ones
	        std::cout << "Clearing old global commands..." << std::endl;
	        bot.global_commands_get([&bot, &register_all_commands](const dpp::confirmation_callback_t& callback) {
	            if (!callback.is_error()) {
	                // Get list of existing commands
	                auto commands = std::get<dpp::slashcommand_map>(callback.value);
	                std::cout << "Found " << commands.size() << " existing commands" << std::endl;
	                
	                if (commands.empty()) {
	                    // No commands to delete, proceed directly to registration
	                    std::cout << "No existing commands to delete, proceeding to registration..." << std::endl;
	                    register_all_commands();
	                } else {
	                    // Delete each old command and count completions
	                    auto* deletion_counter = new std::atomic<int>(0);
	                    auto* total_commands = new int(commands.size());
	                    
	                    for (auto& command : commands) {
	                        std::cout << "Deleting old command: " << command.second.name << std::endl;
	                        bot.global_command_delete(command.first, [&bot, &register_all_commands, deletion_counter, total_commands](const dpp::confirmation_callback_t& del_callback) {
	                            if (del_callback.is_error()) {
	                                std::cout << "❌ Failed to delete command: " << del_callback.get_error().message << std::endl;
	                            } else {
	                                std::cout << "✅ Command deleted successfully" << std::endl;
	                            }
	                            
	                            // Check if all deletions are complete
	                            int completed = deletion_counter->fetch_add(1) + 1;
	                            if (completed >= *total_commands) {
	                                std::cout << "All " << *total_commands << " commands deleted. Starting registration..." << std::endl;
	                                
	                                // Register new commands after all deletions complete
	                                register_all_commands();
	                                
	                                // Clean up allocated memory
	                                delete deletion_counter;
	                                delete total_commands;
	                            }
	                        });
	                    }
	                }
	            } else {
	                std::cout << "❌ Failed to get existing commands: " << callback.get_error().message << std::endl;
	                // Still register commands even if we couldn't get existing ones
	                register_all_commands();
	            }
	        });
	    }
    });

    // Step 10: Set up graceful shutdown handler
    std::cout << "Starting bot event loop..." << std::endl;
    
    // Start the bot in a separate thread so we can handle shutdown gracefully
    std::thread bot_thread([&bot]() {
        bot.start(dpp::st_wait);
    });
    
    // Wait for the bot thread to finish (which should be never unless there's an error)
    bot_thread.join();
    
    // If we get here, the bot has shut down, so clean up the worker thread
    std::cout << "Bot shutting down, stopping worker thread..." << std::endl;
    work_queue.requestShutdown();
    worker.join();
    
    // This line should never be reached unless the bot shuts down
    return 0;
}
