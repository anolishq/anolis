#include "runtime/config.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace anolis::runtime;

class ConfigTest : public ::testing::Test {
protected:
    fs::path temp_dir;

    void SetUp() override {
        // Create temporary test directory
        temp_dir = fs::temp_directory_path() / "anolis_config_test";
        fs::create_directories(temp_dir);
    }

    void TearDown() override {
        // Clean up temporary files
        if (fs::exists(temp_dir)) {
            fs::remove_all(temp_dir);
        }
    }

    std::string create_config_file(const std::string& name, const std::string& content) {
        fs::path config_path = temp_dir / name;
        std::ofstream file(config_path);
        file << content;
        file.close();
        return config_path.string();
    }
};

TEST_F(ConfigTest, ValidMinimalConfig) {
    std::string config_content = R"(
runtime:

http:
  enabled: true
  port: 8080

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("minimal.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_EQ(config.http.port, 8080);
    EXPECT_EQ(config.providers.size(), 1);
    EXPECT_EQ(config.providers[0].id, "test");
}

TEST_F(ConfigTest, RuntimeModeRejected) {
    std::string config_content = R"(
runtime:
  mode: MANUAL

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("mode_rejected.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("no longer configurable"), std::string::npos);
}

TEST_F(ConfigTest, InvalidLogLevel) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: INVALID_LEVEL
)";

    std::string config_path = create_config_file("invalid_log.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("log level"), std::string::npos);
}

TEST_F(ConfigTest, NestedTelemetryStructure) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info

telemetry:
  enabled: true
  influxdb:
    url: http://localhost:8086
    org: testorg
    bucket: testbucket
    token: testtoken
    batch_size: 50
    flush_interval_ms: 500
)";

    std::string config_path = create_config_file("nested_telemetry.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_TRUE(config.telemetry.enabled);
    EXPECT_EQ(config.telemetry.influx_url, "http://localhost:8086");
    EXPECT_EQ(config.telemetry.influx_org, "testorg");
    EXPECT_EQ(config.telemetry.influx_bucket, "testbucket");
    EXPECT_EQ(config.telemetry.batch_size, 50);
    EXPECT_EQ(config.telemetry.flush_interval_ms, 500);
}

TEST_F(ConfigTest, UnknownKeysDoNotFailLoad) {
    // Unknown keys should generate warnings but not prevent loading
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info

unknown_top_level_key: some_value
another_unknown: 123
)";

    std::string config_path = create_config_file("unknown_keys.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    // Should load despite unknown keys
    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_EQ(config.providers.size(), 1);
}

TEST_F(ConfigTest, MissingProvidersSection) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

logging:
  level: info
)";

    std::string config_path = create_config_file("no_providers.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    // Should load but validation might fail
    bool loaded = load_config(config_path, config, error);
    if (!loaded) {
        // If load fails, it should mention providers
        EXPECT_NE(error.find("provider"), std::string::npos);
    } else {
        // If load succeeds, providers should be empty
        EXPECT_TRUE(config.providers.empty());
    }
}

TEST_F(ConfigTest, HTTPBindAddressConfiguration) {
    std::string config_content = R"(
runtime:

http:
  enabled: true
  bind: 0.0.0.0
  port: 9090

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("http_bind.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_EQ(config.http.bind, "0.0.0.0");
    EXPECT_EQ(config.http.port, 9090);
}

TEST_F(ConfigTest, AutomationConfiguration) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info

automation:
  enabled: true
  behavior_tree: /path/to/tree.xml
  tick_rate_hz: 20
  manual_gating_policy: BLOCK
  parameters:
    - name: test_param
      type: double
      default: 10.5
      min: 0.0
      max: 100.0
)";

    std::string config_path = create_config_file("automation.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_TRUE(config.automation.enabled);
    EXPECT_EQ(config.automation.behavior_tree, "/path/to/tree.xml");
    EXPECT_EQ(config.automation.tick_rate_hz, 20);
    EXPECT_EQ(config.automation.parameters.size(), 1);
    EXPECT_EQ(config.automation.parameters[0].name, "test_param");
}

TEST_F(ConfigTest, AutomationBehaviorTreePathAliasAccepted) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info

automation:
  enabled: true
  behavior_tree_path: /path/to/tree.xml
)";

    std::string config_path = create_config_file("automation_alias.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_TRUE(config.automation.enabled);
    EXPECT_EQ(config.automation.behavior_tree, "/path/to/tree.xml");
}

TEST_F(ConfigTest, ValidLogLevels) {
    const std::vector<std::string> valid_levels = {"debug", "info", "warn", "error"};

    for (const auto& level : valid_levels) {
        std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: )" + level + R"(
)";

        std::string config_path = create_config_file("log_" + level + ".yaml", config_content);
        RuntimeConfig config;
        std::string error;

        EXPECT_TRUE(load_config(config_path, config, error)) << "Level: " << level << ", Error: " << error;
    }
}

TEST_F(ConfigTest, FileNotFound) {
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config("/nonexistent/path/config.yaml", config, error));
    EXPECT_FALSE(error.empty());
}

// ===== CORS Configuration Tests =====

TEST_F(ConfigTest, CorsWildcardWithCredentialsRejected) {
    std::string config_content = R"(
runtime:

http:
  enabled: true
  port: 8080
  cors_allowed_origins: ["*"]
  cors_allow_credentials: true

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("cors_wildcard_credentials.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    // load_config calls validate_config internally
    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_NE(error.find("wildcard"), std::string::npos) << "Error: " << error;
    EXPECT_NE(error.find("credentials"), std::string::npos) << "Error: " << error;
}

TEST_F(ConfigTest, CorsWildcardWithoutCredentialsAllowed) {
    std::string config_content = R"(
runtime:

http:
  enabled: true
  port: 8080
  cors_allowed_origins: ["*"]
  cors_allow_credentials: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("cors_wildcard_no_credentials.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_EQ(config.http.cors_allowed_origins.size(), 1);
    EXPECT_EQ(config.http.cors_allowed_origins[0], "*");
    EXPECT_FALSE(config.http.cors_allow_credentials);
}

TEST_F(ConfigTest, CorsSpecificOriginWithCredentialsAllowed) {
    std::string config_content = R"(
runtime:

http:
  enabled: true
  port: 8080
  cors_allowed_origins: ["https://example.com", "https://app.example.com"]
  cors_allow_credentials: true

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("cors_specific_credentials.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_EQ(config.http.cors_allowed_origins.size(), 2);
    EXPECT_TRUE(config.http.cors_allow_credentials);
}

// ===== Restart Policy Tests =====

TEST_F(ConfigTest, RestartPolicyEnabled) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider
    restart_policy:
      enabled: true
      max_attempts: 3
      backoff_ms: [100, 1000, 5000]
      timeout_ms: 30000
      success_reset_ms: 1200

logging:
  level: info
)";

    std::string config_path = create_config_file("restart_policy_enabled.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    ASSERT_EQ(config.providers.size(), 1);

    const auto& rp = config.providers[0].restart_policy;
    EXPECT_TRUE(rp.enabled);
    EXPECT_EQ(rp.max_attempts, 3);
    ASSERT_EQ(rp.backoff_ms.size(), 3);
    EXPECT_EQ(rp.backoff_ms[0], 100);
    EXPECT_EQ(rp.backoff_ms[1], 1000);
    EXPECT_EQ(rp.backoff_ms[2], 5000);
    EXPECT_EQ(rp.timeout_ms, 30000);
    EXPECT_EQ(rp.success_reset_ms, 1200);
}

TEST_F(ConfigTest, RestartPolicyDefaults) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("restart_policy_defaults.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    ASSERT_EQ(config.providers.size(), 1);

    const auto& rp = config.providers[0].restart_policy;
    EXPECT_FALSE(rp.enabled);  // Default disabled
    EXPECT_EQ(rp.max_attempts, 3);
    ASSERT_EQ(rp.backoff_ms.size(), 3);
    EXPECT_EQ(rp.timeout_ms, 30000);
    EXPECT_EQ(rp.success_reset_ms, 1000);
}

TEST_F(ConfigTest, RestartPolicyBackoffMismatch) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider
    restart_policy:
      enabled: true
      max_attempts: 3
      backoff_ms: [100, 1000]
      timeout_ms: 30000

logging:
  level: info
)";

    std::string config_path = create_config_file("restart_policy_mismatch.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("backoff_ms array length"), std::string::npos);
    EXPECT_NE(error.find("must match max_attempts"), std::string::npos);
}

TEST_F(ConfigTest, RestartPolicyNegativeBackoff) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider
    restart_policy:
      enabled: true
      max_attempts: 2
      backoff_ms: [100, -500]
      timeout_ms: 30000

logging:
  level: info
)";

    std::string config_path = create_config_file("restart_policy_negative.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("backoff_ms"), std::string::npos);
    EXPECT_NE(error.find("must be >= 0"), std::string::npos);
}

TEST_F(ConfigTest, RestartPolicyInvalidMaxAttempts) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider
    restart_policy:
      enabled: true
      max_attempts: 0
      backoff_ms: []
      timeout_ms: 30000

logging:
  level: info
)";

    std::string config_path = create_config_file("restart_policy_invalid_attempts.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("max_attempts must be >= 1"), std::string::npos);
}

TEST_F(ConfigTest, ProvidersSectionMustBeSequence) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  id: test_provider
  command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("providers_not_sequence.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_EQ(error, "'providers' must be a sequence");
}

TEST_F(ConfigTest, ProviderArgsMustBeSequence) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider
    args: --bad

logging:
  level: info
)";

    std::string config_path = create_config_file("provider_args_not_sequence.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_EQ(error, "'providers[0].args' must be a sequence");
}

TEST_F(ConfigTest, RestartPolicyMustBeMap) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider
    restart_policy: true

logging:
  level: info
)";

    std::string config_path = create_config_file("restart_policy_not_map.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_EQ(error, "'providers[0].restart_policy' must be a map");
}

TEST_F(ConfigTest, RestartPolicyBackoffMustBeSequence) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider
    restart_policy:
      enabled: true
      max_attempts: 1
      backoff_ms: 100
      timeout_ms: 30000

logging:
  level: info
)";

    std::string config_path = create_config_file("restart_policy_backoff_not_sequence.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_EQ(error, "'providers[0].restart_policy.backoff_ms' must be a sequence");
}

TEST_F(ConfigTest, TelemetryInfluxdbMustBeMap) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider

logging:
  level: info

telemetry:
  enabled: true
  influxdb: http://localhost:8086
)";

    std::string config_path = create_config_file("telemetry_influxdb_not_map.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_EQ(error, "'telemetry.influxdb' must be a map");
}

TEST_F(ConfigTest, AutomationParametersMustBeSequence) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider

logging:
  level: info

automation:
  enabled: true
  behavior_tree: /path/to/tree.xml
  parameters: bad
)";

    std::string config_path = create_config_file("automation_parameters_not_sequence.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_EQ(error, "'automation.parameters' must be a sequence");
}

TEST_F(ConfigTest, AutomationAllowedValuesMustBeSequence) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider

logging:
  level: info

automation:
  enabled: true
  behavior_tree: /path/to/tree.xml
  parameters:
    - name: mode
      type: string
      default: "normal"
      allowed_values: normal
)";

    std::string config_path = create_config_file("automation_allowed_values_not_sequence.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_EQ(error, "'automation.parameters[0].allowed_values' must be a sequence");
}
// Sprint 2.1.3: Negative configuration tests for idempotency and strict validation

TEST_F(ConfigTest, InvalidRuntimeMode) {
    std::string config_content = R"(
runtime:
  mode: MANUAL

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("invalid_mode.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("no longer configurable"), std::string::npos);
}

TEST_F(ConfigTest, InvalidRuntimeModeCaseSensitive) {
    std::string config_content = R"(
runtime:
  mode: manual

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("invalid_mode_lowercase.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("no longer configurable"), std::string::npos);
}
TEST_F(ConfigTest, DuplicateProviderIDs) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: duplicate_id
    command: /path/to/provider1
  - id: duplicate_id
    command: /path/to/provider2

logging:
  level: info
)";

    std::string config_path = create_config_file("duplicate_provider_ids.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("Duplicate provider ID"), std::string::npos);
}
TEST_F(ConfigTest, IdempotentParsing) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: provider1
    command: /path/to/provider1
  - id: provider2
    command: /path/to/provider2

automation:
  enabled: true
  behavior_tree: test.xml
  parameters:
    - name: param1
      type: double
      default: 1.0
    - name: param2
      type: int64
      default: 100

logging:
  level: info
)";

    std::string config_path = create_config_file("idempotent.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    // First load
    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_EQ(config.providers.size(), 2);
    EXPECT_EQ(config.automation.parameters.size(), 2);

    // Second load should replace, not accumulate
    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_EQ(config.providers.size(), 2) << "Providers should be replaced, not accumulated";
    EXPECT_EQ(config.automation.parameters.size(), 2) << "Parameters should be replaced, not accumulated";

    // Third load with different content
    std::string config_content2 = R"(
runtime:

http:
  enabled: false

providers:
  - id: provider_new
    command: /path/to/new_provider

automation:
  enabled: true
  behavior_tree: test.xml
  parameters:
    - name: param_new
      type: string
      default: "test"

logging:
  level: info
)";

    std::string config_path2 = create_config_file("idempotent2.yaml", config_content2);
    ASSERT_TRUE(load_config(config_path2, config, error)) << "Error: " << error;
    EXPECT_EQ(config.providers.size(), 1) << "Old providers should be cleared";
    EXPECT_EQ(config.providers[0].id, "provider_new");
    EXPECT_EQ(config.automation.parameters.size(), 1) << "Old parameters should be cleared";
    EXPECT_EQ(config.automation.parameters[0].name, "param_new");
}

TEST_F(ConfigTest, OmittedSectionsResetToDefaultsOnReload) {
    std::string config_content = R"(
runtime:
  name: first

http:
  enabled: true
  port: 9090

providers:
  - id: provider1
    command: /path/to/provider1

telemetry:
  enabled: true
  influxdb:
    url: http://localhost:8086
    org: anolis
    bucket: anolis
    token: secret

automation:
  enabled: true
  behavior_tree: test.xml

logging:
  level: debug
)";

    std::string config_path = create_config_file("reload_with_sections.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_TRUE(config.telemetry.enabled);
    EXPECT_TRUE(config.automation.enabled);
    EXPECT_EQ(config.http.port, 9090);
    EXPECT_EQ(config.runtime.name, "first");

    std::string config_content2 = R"(
providers:
  - id: provider2
    command: /path/to/provider2

logging:
  level: info
)";

    std::string config_path2 = create_config_file("reload_without_sections.yaml", config_content2);
    ASSERT_TRUE(load_config(config_path2, config, error)) << "Error: " << error;
    EXPECT_EQ(config.runtime.name, "");
    EXPECT_EQ(config.http.port, 8080);
    EXPECT_FALSE(config.telemetry.enabled);
    EXPECT_FALSE(config.automation.enabled);
    EXPECT_TRUE(config.automation.behavior_tree.empty());
    ASSERT_EQ(config.providers.size(), 1);
    EXPECT_EQ(config.providers[0].id, "provider2");
}

TEST_F(ConfigTest, MissingProviderCommand) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    # Missing command field

logging:
  level: info
)";

    std::string config_path = create_config_file("missing_command.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("missing 'command' field"), std::string::npos);
}

TEST_F(ConfigTest, PollingIntervalTooSmall) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

polling:
  interval_ms: -100

logging:
  level: info
)";

    std::string config_path = create_config_file("invalid_polling.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("polling interval"), std::string::npos);
}
TEST_F(ConfigTest, RestartPolicyShortTimeout) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider
    restart_policy:
      enabled: true
      max_attempts: 1
      backoff_ms: [100]
      timeout_ms: 500

logging:
  level: info
)";

    std::string config_path = create_config_file("restart_policy_short_timeout.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("timeout_ms must be >= 1000ms"), std::string::npos);
}

TEST_F(ConfigTest, RestartPolicyNegativeSuccessResetWindow) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test_provider
    command: /path/to/provider
    restart_policy:
      enabled: true
      max_attempts: 1
      backoff_ms: [100]
      timeout_ms: 30000
      success_reset_ms: -1

logging:
  level: info
)";

    std::string config_path = create_config_file("restart_policy_negative_success_reset.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("success_reset_ms must be >= 0"), std::string::npos);
}

TEST_F(ConfigTest, StartupTimeoutTooLow) {
    std::string config_content = R"(
runtime:
  startup_timeout_ms: 4000  # Below minimum of 5000ms

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("startup_timeout_low.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("startup_timeout_ms must be between 5000 and 300000"), std::string::npos);
}

TEST_F(ConfigTest, StartupTimeoutTooHigh) {
    std::string config_content = R"(
runtime:
  startup_timeout_ms: 400000  # Above maximum of 300000ms

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("startup_timeout_high.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    EXPECT_FALSE(load_config(config_path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("startup_timeout_ms must be between 5000 and 300000"), std::string::npos);
}

TEST_F(ConfigTest, StartupTimeoutValid) {
    std::string config_content = R"(
runtime:
  startup_timeout_ms: 60000  # Valid value

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info
)";

    std::string config_path = create_config_file("startup_timeout_valid.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_EQ(config.runtime.startup_timeout_ms, 60000);
}

TEST_F(ConfigTest, TelemetryQueueSizeValid) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info

telemetry:
  enabled: true
  influxdb:
    url: http://localhost:8086
    org: testorg
    bucket: testbucket
    token: testtoken
    queue_size: 50000
)";

    std::string config_path = create_config_file("telemetry_queue_size.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_TRUE(config.telemetry.enabled);
    EXPECT_EQ(config.telemetry.queue_size, 50000);
}

TEST_F(ConfigTest, TelemetryQueueSizeDefaultWhenOmitted) {
    std::string config_content = R"(
runtime:

http:
  enabled: false

providers:
  - id: test
    command: /path/to/provider

logging:
  level: info

telemetry:
  enabled: true
  influxdb:
    url: http://localhost:8086
    org: testorg
    bucket: testbucket
    token: testtoken
)";

    std::string config_path = create_config_file("telemetry_no_queue_size.yaml", config_content);
    RuntimeConfig config;
    std::string error;

    ASSERT_TRUE(load_config(config_path, config, error)) << "Error: " << error;
    EXPECT_TRUE(config.telemetry.enabled);
    EXPECT_EQ(config.telemetry.queue_size, 10000);  // Default value
}
