// Anolis Runtime
// Config-based runtime with CLI argument parsing

#include <filesystem>
#include <iostream>
#include <string>

#include "logging/logger.hpp"
#include "runtime/config.hpp"
#include "runtime/runtime.hpp"
#include "runtime/signal_handler.hpp"

int main(int argc, char **argv) {
    // Parse CLI arguments
    std::string config_path = "anolis-runtime.yaml";  // Default
    bool check_config_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg.substr(0, 9) == "--config=") {
            config_path = arg.substr(9);
        } else if (arg == "--check-config" && i + 1 < argc) {
            config_path = argv[++i];
            check_config_only = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cerr << "Usage: anolis-runtime [OPTIONS]\n\n";
            std::cerr << "Options:\n";
            std::cerr << "  --config=PATH         Path to config file (default: anolis-runtime.yaml)\n";
            std::cerr << "  --check-config PATH   Validate config file and exit (0=ok, 1=error)\n";
            std::cerr << "  --help, -h            Show this help\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            std::cerr << "Use --help for usage information\n";
            return 1;
        }
    }

    // Check if config exists
    if (!std::filesystem::exists(config_path)) {
        // Using cerr here as logger might not be initialized/configured
        std::cerr << "ERROR: Config file not found: " << config_path << "\n";
        std::cerr << "\nCreate a config file or specify path with --config=PATH\n";
        return 1;
    }

    if (!check_config_only) {
        LOG_INFO("Anolis Core Runtime v0 starting...");
        LOG_INFO("Loading config: " + config_path);
    }

    // Load configuration
    anolis::runtime::RuntimeConfig config;
    std::string error;

    if (!anolis::runtime::load_config(config_path, config, error)) {
        if (check_config_only) {
            std::cerr << "ERROR: " << error << "\n";
        } else {
            LOG_ERROR("Failed to load config: " + error);
        }
        return 1;
    }

    if (check_config_only) {
        std::cout << "Config valid: " << config.runtime.name << ", " << config.providers.size() << " provider(s)"
                  << ", port " << config.http.port << "\n";
        return 0;
    }

    // Initialize logger level
    anolis::logging::Logger::set_level(anolis::logging::string_to_level(config.logging.level));

    // Create and initialize runtime
    anolis::runtime::Runtime runtime(config);

    if (!runtime.initialize(error)) {
        LOG_ERROR("Runtime initialization failed: " + error);
        return 1;
    }

    // Install signal handler for graceful shutdown
    anolis::runtime::SignalHandler::install();

    LOG_INFO("Runtime Ready");
    LOG_INFO("  Providers: " << config.providers.size());
    LOG_INFO("  Devices: " << runtime.get_registry().device_count());
    LOG_INFO("  Polling: " << config.polling.interval_ms << "ms");

    // Run main loop (blocking)
    runtime.run();

    LOG_INFO("Shutdown complete");
    return 0;
}
