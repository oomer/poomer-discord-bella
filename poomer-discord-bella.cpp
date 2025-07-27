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
#include <vector> // Dynamic arrays - for std::vector to hold image byte data
#include <chrono> // Time utilities - for std::chrono::seconds() delays
#include <algorithm> // Algorithm functions - for std::transform (string case conversion)
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
                status TEXT DEFAULT 'pending'
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
            (attachment_url, original_filename, channel_id, user_id, created_at, retry_count)
            VALUES (?, ?, ?, ?, ?, ?);
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
        sqlite3_bind_int64(stmt, 5, item.created_at);
        sqlite3_bind_int(stmt, 6, item.retry_count);
        
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
                SELECT id, attachment_url, original_filename, channel_id, user_id, created_at, retry_count
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
                item.created_at = sqlite3_column_int64(stmt, 5);
                item.retry_count = sqlite3_column_int(stmt, 6);
                
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
     * Mark a work item as completed and remove it from the queue
     */
    bool markCompleted(int64_t item_id) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        const char* delete_sql = "DELETE FROM work_queue WHERE id = ?;";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, delete_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare delete statement: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        sqlite3_bind_int64(stmt, 1, item_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        if (rc != SQLITE_DONE) {
            std::cerr << "❌ Failed to delete completed work item: " << sqlite3_errmsg(db) << std::endl;
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
     * Request shutdown of the work queue
     */
    void requestShutdown() {
        shutdown_requested = true;
        queue_condition.notify_all();
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
 * Function to process .bsz file with bella path tracer
 * 
 * @param engine - Reference to the bella engine
 * @param bsz_data - The .bsz file data as bytes
 * @param filename - The original filename
 * @return std::vector<uint8_t> - The rendered output data
 */
std::vector<uint8_t> processBszFile(dl::bella_sdk::Engine& engine, const std::vector<uint8_t>& bsz_data, const std::string& filename) {
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
        //belCamera["resolution"] = dl::Vec2{200, 200}; 
        
        std::cout << "✅ Loaded .bsz scene into bella engine" << std::endl;
        
        // Start rendering
        std::cout << "🎨 Starting bella render..." << std::endl;
        engine.start();
        
        // Wait for rendering to complete
        while(engine.rendering()) { 
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        std::cout << "✅ Bella render completed!" << std::endl;
        
        // TODO: Read the rendered output and return it
        // For now, return original data as placeholder
        return bsz_data;
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Error processing .bsz file: " << e.what() << std::endl;
        return bsz_data;
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
            // Convert response body to vector for processing
            std::vector<uint8_t> original_data(download_data.begin(), download_data.end());
            
            // Process the .bsz file with bella path tracer
            std::vector<uint8_t> processed_data = processBszFile(*engine, original_data, item.original_filename);
            
            // Extract base filename for the rendered JPEG
            std::string base_filename = item.original_filename;
            if (base_filename.length() >= 4 && base_filename.substr(base_filename.length() - 4) == ".bsz") {
                base_filename = base_filename.substr(0, base_filename.length() - 4);
            }
            std::string jpeg_filename = base_filename + ".jpg";
            
            // Variables for message creation
            std::vector<uint8_t> jpeg_data;
            dpp::message msg(item.channel_id, "");
            
            // Read the rendered JPEG file from disk
            std::ifstream jpeg_file(jpeg_filename, std::ios::binary);
            
            if (jpeg_file.is_open()) {
                // Get file size
                jpeg_file.seekg(0, std::ios::end);
                size_t file_size = jpeg_file.tellg();
                jpeg_file.seekg(0, std::ios::beg);
                
                // Read JPEG data
                jpeg_data.resize(file_size);
                jpeg_file.read(reinterpret_cast<char*>(jpeg_data.data()), file_size);
                jpeg_file.close();
                
                std::cout << "📷 Read rendered JPEG: " << jpeg_filename << " (" << jpeg_data.size() << " bytes)" << std::endl;
                
                // Create message with rendered JPEG
                msg.content = "🎨 Here's your rendered image! <@" + std::to_string(item.user_id) + ">";
                msg.add_file(jpeg_filename, std::string(jpeg_data.begin(), jpeg_data.end()));
            } else {
                std::cout << "❌ Could not read rendered JPEG file: " << jpeg_filename << std::endl;
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
                    if (jpeg_data.empty()) {
                        std::cout << "✅ Successfully sent error message for " << item.original_filename << std::endl;
                    } else {
                        std::cout << "✅ Successfully sent rendered " << jpeg_filename << "!" << std::endl;
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
            
            if (send_success && !jpeg_data.empty()) {
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
    // [&bot] captures the bot variable by reference so we can use it inside the lambda
    bot.on_slashcommand([&bot](const dpp::slashcommand_t& event) {
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
        
        // Check if this is the command we care about
        if (event.command.get_command_name() == "help") {
            // CRITICAL: Must acknowledge Discord interaction within 3 seconds
            // or Discord shows "The application did not respond" error
            std::cout << "Sending interaction acknowledgment for command #" << command_id << "..." << std::endl;
            event.reply("🚀 I am a Bella render bot, ready to render your scenes!\nNo need to mention me, just upload your scene(s) (.bsz) and I will render them.");
            
            // Generate unique thread ID for debugging async operations
            static int thread_counter = 0;  // Separate counter for threads
            int current_thread_id = ++thread_counter;
        } else {
            // Handle unexpected commands (probably old cached registrations)
            std::cout << "Status: UNEXPECTED COMMAND (probably cached registration) - Command #" << command_id << std::endl;
            std::cout << "========================" << std::endl;
            event.reply("⚠️ This command is no longer supported. Please use `/help` instead.");
        }
    });

    // Step 9: Set up bot ready event (called when bot successfully connects to Discord)
    bot.on_ready([&bot](const dpp::ready_t& event) {
        // run_once ensures this only happens on first connection, not reconnections
	    if (dpp::run_once<struct register_bot_commands>()) {
	        std::cout << "Bot is ready! Starting command registration..." << std::endl;
	        std::cout << "Bot user: " << bot.me.username << " (ID: " << bot.me.id << ")" << std::endl;
	        
	        // Clean up any old slash commands before registering new ones
	        std::cout << "Clearing old global commands..." << std::endl;
	        bot.global_commands_get([&bot](const dpp::confirmation_callback_t& callback) {
	            if (!callback.is_error()) {
	                // Get list of existing commands
	                auto commands = std::get<dpp::slashcommand_map>(callback.value);
	                std::cout << "Found " << commands.size() << " existing commands" << std::endl;
	                
	                // Delete each old command
	                for (auto& command : commands) {
	                    std::cout << "Deleting old command: " << command.second.name << std::endl;
	                    bot.global_command_delete(command.first, [](const dpp::confirmation_callback_t& del_callback) {
	                        if (del_callback.is_error()) {
	                            std::cout << "❌ Failed to delete command: " << del_callback.get_error().message << std::endl;
	                        } else {
	                            std::cout << "✅ Command deleted successfully" << std::endl;
	                        }
	                    });
	                }
	            } else {
	                std::cout << "❌ Failed to get existing commands: " << callback.get_error().message << std::endl;
	            }
	            
	            // Register our new slash command
	            std::cout << "Registering commands..." << std::endl;
	            // Parameters: command_name, description, bot_id
	            bot.global_command_create(dpp::slashcommand("help", "on other commands", bot.me.id), [](const dpp::confirmation_callback_t& reg_callback) {
	                if (reg_callback.is_error()) {
	                    std::cout << "❌ Failed to register command: " << reg_callback.get_error().message << std::endl;
	                } else {
	                    std::cout << "✅ Command registered successfully!" << std::endl;
	                }
	            });
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