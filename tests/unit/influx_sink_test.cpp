/**
 * influx_sink_test.cpp - InfluxDB Telemetry Sink unit tests
 *
 *
 * Tests:
 * 1. Tag escaping (comma, equals, space)
 * 2. Field string escaping (quotes, backslash)
 * 3. Line protocol formatting for different value types
 * 4. Special float values (NaN, Inf)
 * 5. Quality field always present
 * 6. Mode change event formatting
 * 7. Parameter change event formatting
 * 8. InfluxConfig validation
 *
 * Note: Batching and HTTP interaction require integration testing with
 * real InfluxDB or HTTP mocking library. These tests focus on unit-testable
 * formatting and escaping logic.
 */

#include "telemetry/influx_sink.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>

#include "events/event_types.hpp"

using namespace anolis::telemetry;
using namespace anolis::events;

// ============================================================================
// Escape Function Tests
// ============================================================================

TEST(InfluxEscapeTest, EscapeTagHandlesComma) {
    std::string input = "value,with,commas";
    std::string expected = "value\\,with\\,commas";
    EXPECT_EQ(escape_tag(input), expected);
}

TEST(InfluxEscapeTest, EscapeTagHandlesEquals) {
    std::string input = "key=value";
    std::string expected = "key\\=value";
    EXPECT_EQ(escape_tag(input), expected);
}

TEST(InfluxEscapeTest, EscapeTagHandlesSpace) {
    std::string input = "value with spaces";
    std::string expected = "value\\ with\\ spaces";
    EXPECT_EQ(escape_tag(input), expected);
}

TEST(InfluxEscapeTest, EscapeTagHandlesAllSpecialChars) {
    std::string input = "tag,with=multiple special";
    std::string expected = "tag\\,with\\=multiple\\ special";
    EXPECT_EQ(escape_tag(input), expected);
}

TEST(InfluxEscapeTest, EscapeTagHandlesEmptyString) { EXPECT_EQ(escape_tag(""), ""); }

TEST(InfluxEscapeTest, EscapeTagHandlesNoSpecialChars) {
    std::string input = "normalvalue123";
    EXPECT_EQ(escape_tag(input), input);
}

TEST(InfluxEscapeTest, EscapeFieldStringHandlesQuotes) {
    std::string input = "value\"with\"quotes";
    std::string expected = "value\\\"with\\\"quotes";
    EXPECT_EQ(escape_field_string(input), expected);
}

TEST(InfluxEscapeTest, EscapeFieldStringHandlesBackslash) {
    std::string input = "path\\to\\file";
    std::string expected = "path\\\\to\\\\file";
    EXPECT_EQ(escape_field_string(input), expected);
}

TEST(InfluxEscapeTest, EscapeFieldStringHandlesBoth) {
    std::string input = "value\\with\"both";
    std::string expected = "value\\\\with\\\"both";
    EXPECT_EQ(escape_field_string(input), expected);
}

TEST(InfluxEscapeTest, EscapeFieldStringHandlesEmptyString) { EXPECT_EQ(escape_field_string(""), ""); }

TEST(InfluxEscapeTest, EscapeFieldStringHandlesNormal) {
    std::string input = "normal_value_123";
    EXPECT_EQ(escape_field_string(input), input);
}

// ============================================================================
// Line Protocol Formatting Tests - Double Values
// ============================================================================

TEST(InfluxLineProtocolTest, FormatDoubleValue) {
    StateUpdateEvent event;
    event.provider_id = "test_provider";
    event.device_id = "test_device";
    event.signal_id = "temperature";
    event.value = 23.5;
    event.quality = Quality::OK;
    event.timestamp_ms = 1234567890;

    std::string line = format_line_protocol(event);

    // Expected format: anolis_signal,provider_id=test_provider,device_id=test_device,signal_id=temperature
    // value_double=23.5,quality="OK" 1234567890
    EXPECT_TRUE(line.find("anolis_signal") != std::string::npos);
    EXPECT_TRUE(line.find("provider_id=test_provider") != std::string::npos);
    EXPECT_TRUE(line.find("device_id=test_device") != std::string::npos);
    EXPECT_TRUE(line.find("signal_id=temperature") != std::string::npos);
    EXPECT_TRUE(line.find("value_double=23.5") != std::string::npos);
    EXPECT_TRUE(line.find("quality=\"OK\"") != std::string::npos);
    EXPECT_TRUE(line.find("1234567890") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatIncludesRuntimeNameTag) {
    StateUpdateEvent event;
    event.provider_id = "test_provider";
    event.device_id = "test_device";
    event.signal_id = "temperature";
    event.value = 23.5;
    event.quality = Quality::OK;
    event.timestamp_ms = 1234567890;

    std::string line = format_line_protocol(event, "bioreactor-telemetry");
    EXPECT_TRUE(line.find("runtime_name=bioreactor-telemetry") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatNegativeDouble) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = -15.75;
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("value_double=-15.75") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatZeroDouble) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = 0.0;
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("value_double=0") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatNaNSkipsValueField) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = std::numeric_limits<double>::quiet_NaN();
    event.quality = Quality::FAULT;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);

    // NaN should be skipped, but quality should still be present
    EXPECT_TRUE(line.find("value_double=") == std::string::npos);
    EXPECT_TRUE(line.find("quality=\"FAULT\"") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatInfinitySkipsValueField) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = std::numeric_limits<double>::infinity();
    event.quality = Quality::FAULT;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);

    // Infinity should be skipped, but quality should still be present
    EXPECT_TRUE(line.find("value_double=") == std::string::npos);
    EXPECT_TRUE(line.find("quality=\"FAULT\"") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatNegativeInfinitySkipsValueField) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = -std::numeric_limits<double>::infinity();
    event.quality = Quality::FAULT;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("value_double=") == std::string::npos);
    EXPECT_TRUE(line.find("quality=\"FAULT\"") != std::string::npos);
}

// ============================================================================
// Line Protocol Formatting Tests - Integer Values
// ============================================================================

TEST(InfluxLineProtocolTest, FormatInt64Value) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = static_cast<int64_t>(12345);
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);

    // int64 values should have 'i' suffix
    EXPECT_TRUE(line.find("value_int=12345i") != std::string::npos);
    EXPECT_TRUE(line.find("quality=\"OK\"") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatNegativeInt64) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = static_cast<int64_t>(-9999);
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("value_int=-9999i") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatUint64Value) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = static_cast<uint64_t>(67890);
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);

    // uint64 values should have 'u' suffix
    EXPECT_TRUE(line.find("value_uint=67890u") != std::string::npos);
}

// ============================================================================
// Line Protocol Formatting Tests - Boolean Values
// ============================================================================

TEST(InfluxLineProtocolTest, FormatBoolTrue) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = true;
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("value_bool=true") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatBoolFalse) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = false;
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("value_bool=false") != std::string::npos);
}

// ============================================================================
// Line Protocol Formatting Tests - String Values
// ============================================================================

TEST(InfluxLineProtocolTest, FormatStringValue) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = std::string("hello_world");
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("value_string=\"hello_world\"") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatStringWithQuotes) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = std::string("value\"with\"quotes");
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("value_string=\"value\\\"with\\\"quotes\"") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatStringWithBackslash) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = std::string("path\\to\\file");
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("value_string=\"path\\\\to\\\\file\"") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatEmptyString) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = std::string("");
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("value_string=\"\"") != std::string::npos);
}

// ============================================================================
// Line Protocol Quality Field Tests
// ============================================================================

TEST(InfluxLineProtocolTest, FormatQualityOK) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = 100.0;
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("quality=\"OK\"") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatQualitySTALE) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = 100.0;
    event.quality = Quality::STALE;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("quality=\"STALE\"") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatQualityUNAVAILABLE) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = 100.0;
    event.quality = Quality::UNAVAILABLE;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("quality=\"UNAVAILABLE\"") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatQualityFAULT) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = 100.0;
    event.quality = Quality::FAULT;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);
    EXPECT_TRUE(line.find("quality=\"FAULT\"") != std::string::npos);
}

// ============================================================================
// Line Protocol Tag Escaping Tests
// ============================================================================

TEST(InfluxLineProtocolTest, FormatTagsWithSpecialChars) {
    StateUpdateEvent event;
    event.provider_id = "provider,with,commas";
    event.device_id = "device=with=equals";
    event.signal_id = "signal with spaces";
    event.value = 50.0;
    event.quality = Quality::OK;
    event.timestamp_ms = 1000000;

    std::string line = format_line_protocol(event);

    EXPECT_TRUE(line.find("provider_id=provider\\,with\\,commas") != std::string::npos);
    EXPECT_TRUE(line.find("device_id=device\\=with\\=equals") != std::string::npos);
    EXPECT_TRUE(line.find("signal_id=signal\\ with\\ spaces") != std::string::npos);
}

// ============================================================================
// Mode Change Event Tests
// ============================================================================

TEST(InfluxLineProtocolTest, FormatModeChangeEvent) {
    ModeChangeEvent event;
    event.previous_mode = "MANUAL";
    event.new_mode = "AUTO";
    event.timestamp_ms = 1234567890;

    std::string line = format_mode_change_line_protocol(event);

    EXPECT_TRUE(line.find("mode_change") != std::string::npos);
    EXPECT_TRUE(line.find("previous_mode=\"MANUAL\"") != std::string::npos);
    EXPECT_TRUE(line.find("new_mode=\"AUTO\"") != std::string::npos);
    EXPECT_TRUE(line.find("1234567890") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatModeChangeWithSpecialChars) {
    ModeChangeEvent event;
    event.previous_mode = "mode\"with\"quotes";
    event.new_mode = "mode\\with\\backslash";
    event.timestamp_ms = 1000000;

    std::string line = format_mode_change_line_protocol(event);

    EXPECT_TRUE(line.find("previous_mode=\"mode\\\"with\\\"quotes\"") != std::string::npos);
    EXPECT_TRUE(line.find("new_mode=\"mode\\\\with\\\\backslash\"") != std::string::npos);
}

// ============================================================================
// Parameter Change Event Tests
// ============================================================================

TEST(InfluxLineProtocolTest, FormatParameterChangeEvent) {
    ParameterChangeEvent event;
    event.parameter_name = "test_param";
    event.old_value_str = "100";
    event.new_value_str = "200";
    event.timestamp_ms = 1234567890;

    std::string line = format_parameter_change_line_protocol(event);

    EXPECT_TRUE(line.find("parameter_change") != std::string::npos);
    EXPECT_TRUE(line.find("parameter_name=test_param") != std::string::npos);
    EXPECT_TRUE(line.find("old_value=\"100\"") != std::string::npos);
    EXPECT_TRUE(line.find("new_value=\"200\"") != std::string::npos);
    EXPECT_TRUE(line.find("1234567890") != std::string::npos);
}

TEST(InfluxLineProtocolTest, FormatParameterChangeWithSpecialChars) {
    ParameterChangeEvent event;
    event.parameter_name = "param,with=special chars";
    event.old_value_str = "value\"with\"quotes";
    event.new_value_str = "value\\with\\backslash";
    event.timestamp_ms = 1000000;

    std::string line = format_parameter_change_line_protocol(event);

    // Parameter name is a tag (should use tag escaping)
    EXPECT_TRUE(line.find("parameter_name=param\\,with\\=special\\ chars") != std::string::npos);

    // Values are fields (should use field string escaping)
    EXPECT_TRUE(line.find("old_value=\"value\\\"with\\\"quotes\"") != std::string::npos);
    EXPECT_TRUE(line.find("new_value=\"value\\\\with\\\\backslash\"") != std::string::npos);
}

// ============================================================================
// InfluxConfig Tests
// ============================================================================

TEST(InfluxConfigTest, DefaultValues) {
    InfluxConfig config;

    EXPECT_FALSE(config.enabled);
    EXPECT_EQ(config.url, "http://localhost:8086");
    EXPECT_EQ(config.org, "anolis");
    EXPECT_EQ(config.bucket, "anolis");
    EXPECT_TRUE(config.token.empty());
    EXPECT_EQ(config.runtime_name, "default");
    EXPECT_EQ(config.batch_size, 100);
    EXPECT_EQ(config.flush_interval_ms, 1000);
    EXPECT_EQ(config.timeout_ms, 5000);
    EXPECT_EQ(config.retry_interval_ms, 10000);
    EXPECT_EQ(config.queue_size, 10000);
}

TEST(InfluxConfigTest, CustomValues) {
    InfluxConfig config;
    config.enabled = true;
    config.url = "https://influxdb.example.com:8086";
    config.org = "my_org";
    config.bucket = "my_bucket";
    config.token = "my_secret_token";
    config.batch_size = 500;
    config.flush_interval_ms = 5000;

    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.url, "https://influxdb.example.com:8086");
    EXPECT_EQ(config.org, "my_org");
    EXPECT_EQ(config.bucket, "my_bucket");
    EXPECT_EQ(config.token, "my_secret_token");
    EXPECT_EQ(config.batch_size, 500);
    EXPECT_EQ(config.flush_interval_ms, 5000);
}

TEST(InfluxConfigTest, RetryBufferDefaultValue) {
    InfluxConfig config;
    EXPECT_EQ(config.max_retry_buffer_size, 1000);
}

TEST(InfluxSinkTest, HealthAccessorsBeforeStart) {
    // A constructed-but-not-started sink reports its config and zeroed counters.
    // This exercises the accessors backing GET /v0/telemetry/status without any
    // network or running flush thread.
    InfluxConfig config;
    config.enabled = true;
    config.url = "http://influx.example:8086";
    config.org = "my_org";
    config.bucket = "my_bucket";
    config.token = "secret";  // not exposed by any accessor

    InfluxSink sink(config);

    EXPECT_TRUE(sink.enabled());
    EXPECT_FALSE(sink.is_running());
    EXPECT_FALSE(sink.is_connected());
    EXPECT_EQ(sink.total_written(), 0u);
    EXPECT_EQ(sink.total_failed(), 0u);
    EXPECT_EQ(sink.current_batch_size(), 0u);
    EXPECT_EQ(sink.queue_depth(), 0u);     // no subscription before start()
    EXPECT_EQ(sink.dropped_events(), 0u);  // no subscription before start()

    EXPECT_EQ(sink.url(), "http://influx.example:8086");
    EXPECT_EQ(sink.org(), "my_org");
    EXPECT_EQ(sink.bucket(), "my_bucket");
}

TEST(InfluxConfigTest, RetryBufferCustomValue) {
    InfluxConfig config;
    config.max_retry_buffer_size = 5000;
    EXPECT_EQ(config.max_retry_buffer_size, 5000);
}

// ============================================================================
// Line Protocol Format Validation Tests
// ============================================================================

TEST(InfluxLineProtocolTest, ValidateFormatStructure) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = 42.0;
    event.quality = Quality::OK;
    event.timestamp_ms = 1234567890;

    std::string line = format_line_protocol(event);

    // Format: measurement,tag1=val1,tag2=val2 field1=val1,field2=val2 timestamp
    // Should have: measurement name, at least one comma (tags), one space (before fields), one space (before timestamp)

    size_t first_space = line.find(' ');
    EXPECT_NE(first_space, std::string::npos);

    size_t second_space = line.find(' ', first_space + 1);
    EXPECT_NE(second_space, std::string::npos);

    // No third space (timestamp is last)
    size_t third_space = line.find(' ', second_space + 1);
    EXPECT_EQ(third_space, std::string::npos);
}

TEST(InfluxLineProtocolTest, ValidateMeasurementFirst) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = 42.0;
    event.quality = Quality::OK;
    event.timestamp_ms = 1234567890;

    std::string line = format_line_protocol(event);

    // Line should start with measurement name
    EXPECT_EQ(line.substr(0, 14), "anolis_signal,");
}

TEST(InfluxLineProtocolTest, ValidateTimestampLast) {
    StateUpdateEvent event;
    event.provider_id = "provider";
    event.device_id = "device";
    event.signal_id = "signal";
    event.value = 42.0;
    event.quality = Quality::OK;
    event.timestamp_ms = 9876543210;

    std::string line = format_line_protocol(event);

    // Line should end with timestamp
    EXPECT_TRUE(line.find("9876543210") != std::string::npos);
    EXPECT_TRUE(line.rfind("9876543210") == line.length() - 10);  // Last 10 characters
}
