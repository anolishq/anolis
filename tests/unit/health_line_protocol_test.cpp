/**
 * health_line_protocol_test.cpp - Health snapshot line-protocol tests (#203)
 *
 * Tests:
 * 1. Provider line formatting (nominal, without reported block)
 * 2. Device line formatting with the typed metrics allowlist
 * 3. Strict parsing: malformed values and unknown keys are skipped
 * 4. staleness_ms omitted for never-polled devices
 * 5. Tag escaping and timestamp placement
 *
 * The snapshot collector itself is exercised end-to-end through the
 * /v0/providers/health contract examples and conformance suite; these tests
 * cover the formatting/parsing layer, which is where ingestion-specific risk
 * lives.
 */

#include "telemetry/health_line_protocol.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace anolis::telemetry;
using anolis::health::DeviceHealthSnapshot;
using anolis::health::ProviderHealthSnapshot;
using anolis::health::ReportedDeviceHealth;
using anolis::health::ReportedProviderHealth;

namespace {

ProviderHealthSnapshot base_provider() {
    ProviderHealthSnapshot ps;
    ps.provider_id = "bread0";
    ps.state = "AVAILABLE";
    ps.lifecycle_state = "RUNNING";
    ps.uptime_seconds = 42;
    ps.device_count = 2;
    return ps;
}

}  // namespace

// ============================================================================
// Strict metric parsing
// ============================================================================

TEST(HealthMetricParseTest, ParsesPlainIntegers) {
    int64_t v = 0;
    EXPECT_TRUE(parse_metric_int("0", v));
    EXPECT_EQ(v, 0);
    EXPECT_TRUE(parse_metric_int("12345", v));
    EXPECT_EQ(v, 12345);
}

TEST(HealthMetricParseTest, RejectsMalformedIntegers) {
    int64_t v = 0;
    EXPECT_FALSE(parse_metric_int("", v));
    EXPECT_FALSE(parse_metric_int("-", v));
    // All allowlisted keys are counters/durations (contract minimum 0): a
    // negative value is skipped, never written as a schema-illegal row.
    EXPECT_FALSE(parse_metric_int("-5", v));
    EXPECT_FALSE(parse_metric_int("abc", v));
    EXPECT_FALSE(parse_metric_int("12x", v));
    EXPECT_FALSE(parse_metric_int(" 12", v));
    EXPECT_FALSE(parse_metric_int("12 ", v));
    EXPECT_FALSE(parse_metric_int("--5", v));
    EXPECT_FALSE(parse_metric_int("1.5", v));
    EXPECT_FALSE(parse_metric_int("0x1F", v));
    // Overflows int64 -> rejected, not wrapped
    EXPECT_FALSE(parse_metric_int("99999999999999999999", v));
}

TEST(HealthMetricParseTest, ParsesOnlyExactBooleans) {
    bool v = false;
    EXPECT_TRUE(parse_metric_bool("true", v));
    EXPECT_TRUE(v);
    EXPECT_TRUE(parse_metric_bool("false", v));
    EXPECT_FALSE(v);
    EXPECT_FALSE(parse_metric_bool("TRUE", v));
    EXPECT_FALSE(parse_metric_bool("1", v));
    EXPECT_FALSE(parse_metric_bool("", v));
    EXPECT_FALSE(parse_metric_bool("yes", v));
}

// ============================================================================
// Provider line
// ============================================================================

TEST(HealthLineProtocolTest, ProviderLineNominal) {
    auto ps = base_provider();
    auto lines = format_health_lines(ps, "bioreactor", 1700000000000);

    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0],
              "anolis_provider_health,runtime_name=bioreactor,provider_id=bread0 "
              "state=\"AVAILABLE\",lifecycle_state=\"RUNNING\",degraded=false,failed_device_count=0i,"
              "device_count=2i,uptime_seconds=42i,supervision_enabled=false,attempt_count=0i,"
              "crash_detected=false,circuit_open=false 1700000000000");
}

TEST(HealthLineProtocolTest, ProviderLineIncludesAllowlistedStartupMetrics) {
    auto ps = base_provider();
    ps.degraded = true;
    ps.failed_device_count = 1;
    ReportedProviderHealth rp;
    rp.state = "STATE_DEGRADED";
    rp.metrics["startup_configured_devices"] = "3";
    rp.metrics["startup_initialized_devices"] = "2";
    rp.metrics["startup_failed_devices"] = "1";
    rp.metrics["impl"] = "bread";  // not allowlisted -> skipped
    ps.reported = rp;

    auto lines = format_health_lines(ps, "bioreactor", 5);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("degraded=true,failed_device_count=1i"), std::string::npos);
    EXPECT_NE(lines[0].find(",startup_configured_devices=3i,startup_initialized_devices=2i,startup_failed_devices=1i "),
              std::string::npos);
    EXPECT_EQ(lines[0].find("impl"), std::string::npos);
}

TEST(HealthLineProtocolTest, ProviderTagsAreEscaped) {
    auto ps = base_provider();
    ps.provider_id = "bread 0";
    auto lines = format_health_lines(ps, "my runtime", 5);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].rfind("anolis_provider_health,runtime_name=my\\ runtime,provider_id=bread\\ 0 ", 0), 0u);
}

// ============================================================================
// Device lines
// ============================================================================

TEST(HealthLineProtocolTest, DeviceLineWithTypedAllowlist) {
    auto ps = base_provider();
    DeviceHealthSnapshot ds;
    ds.device_id = "dcmt0";
    ds.health = "OK";
    ds.last_poll_ms = 1699999999000;
    ds.staleness_ms = 120;
    ReportedDeviceHealth rd;
    rd.metrics["io_ok"] = "142";
    rd.metrics["io_failed"] = "3";
    rd.metrics["io_retried_attempts"] = "7";
    rd.metrics["watchdog_armed"] = "true";
    rd.metrics["watchdog_timeout_ms"] = "5000";
    rd.metrics["watchdog_tripped"] = "false";
    rd.metrics["watchdog_trip_count"] = "2";
    rd.metrics["address"] = "0x10";  // not allowlisted -> skipped
    ds.reported = rd;
    ps.devices.push_back(ds);

    auto lines = format_health_lines(ps, "bioreactor", 1700000000000);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[1],
              "anolis_device_health,runtime_name=bioreactor,provider_id=bread0,device_id=dcmt0 "
              "health=\"OK\",registered=true,staleness_ms=120i,stale_signal_count=0i,fault_signal_count=0i,"
              "io_ok=142i,io_failed=3i,io_retried_attempts=7i,"
              "watchdog_timeout_ms=5000i,watchdog_trip_count=2i,watchdog_armed=true,watchdog_tripped=false "
              "1700000000000");
}

TEST(HealthLineProtocolTest, DeviceLineSkipsMalformedValues) {
    auto ps = base_provider();
    DeviceHealthSnapshot ds;
    ds.device_id = "ph0";
    ds.health = "OK";
    ds.last_poll_ms = 1;
    ds.staleness_ms = 50;
    ReportedDeviceHealth rd;
    rd.metrics["io_ok"] = "abc";
    rd.metrics["io_failed"] = "12x";
    rd.metrics["io_retried_attempts"] = "-2";
    rd.metrics["sample_success_count"] = "9";
    rd.metrics["watchdog_armed"] = "TRUE";
    ds.reported = rd;
    ps.devices.push_back(ds);

    auto lines = format_health_lines(ps, "r", 5);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[1],
              "anolis_device_health,runtime_name=r,provider_id=bread0,device_id=ph0 "
              "health=\"OK\",registered=true,staleness_ms=50i,stale_signal_count=0i,fault_signal_count=0i,"
              "sample_success_count=9i 5");
}

TEST(HealthLineProtocolTest, NeverPolledDeviceOmitsStaleness) {
    // A never-polled device (last_poll_ms == 0) omits staleness_ms AND the two
    // per-signal counts: they must not read as 0-fresh / zero-degraded (#220).
    auto ps = base_provider();
    DeviceHealthSnapshot ds;
    ds.device_id = "dcmt9";
    ds.health = "UNKNOWN";
    ds.registered = false;
    ReportedDeviceHealth rd;
    rd.state = "STATE_UNREACHABLE";
    rd.metrics["missing"] = "true";
    ds.reported = rd;
    ps.devices.push_back(ds);

    auto lines = format_health_lines(ps, "r", 5);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[1],
              "anolis_device_health,runtime_name=r,provider_id=bread0,device_id=dcmt9 "
              "health=\"UNKNOWN\",registered=false,missing=true 5");
}

TEST(HealthLineProtocolTest, DeviceLineEmitsFaultHealthAndCounts) {
    auto ps = base_provider();
    ps.provider_id = "ezo0";
    DeviceHealthSnapshot ds;
    ds.device_id = "ph0";
    ds.health = "FAULT";
    ds.last_poll_ms = 1;
    ds.staleness_ms = 160;
    ds.stale_signal_count = 2;
    ds.fault_signal_count = 1;
    ps.devices.push_back(ds);

    auto lines = format_health_lines(ps, "r", 7);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[1],
              "anolis_device_health,runtime_name=r,provider_id=ezo0,device_id=ph0 "
              "health=\"FAULT\",registered=true,staleness_ms=160i,stale_signal_count=2i,fault_signal_count=1i 7");
}

TEST(HealthLineProtocolTest, ExcludedDeviceCarriesIoCountersAndFlag) {
    auto ps = base_provider();
    ps.provider_id = "ezo0";
    DeviceHealthSnapshot ds;
    ds.device_id = "orp0";
    ds.health = "UNKNOWN";
    ds.last_poll_ms = 0;
    ReportedDeviceHealth rd;
    rd.metrics["excluded"] = "true";
    rd.metrics["io_ok"] = "0";
    rd.metrics["io_failed"] = "6";
    rd.metrics["io_retried_attempts"] = "0";
    ds.reported = rd;
    ps.devices.push_back(ds);

    auto lines = format_health_lines(ps, "r", 7);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[1],
              "anolis_device_health,runtime_name=r,provider_id=ezo0,device_id=orp0 "
              "health=\"UNKNOWN\",registered=true,io_ok=0i,io_failed=6i,io_retried_attempts=0i,excluded=true 7");
}

TEST(HealthLineProtocolTest, OneLinePerDevicePlusProvider) {
    auto ps = base_provider();
    for (int i = 0; i < 3; ++i) {
        DeviceHealthSnapshot ds;
        ds.device_id = "d" + std::to_string(i);
        ps.devices.push_back(ds);
    }
    auto lines = format_health_lines(ps, "r", 1);
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0].rfind("anolis_provider_health,", 0), 0u);
    for (size_t i = 1; i < lines.size(); ++i) {
        EXPECT_EQ(lines[i].rfind("anolis_device_health,", 0), 0u);
    }
}
