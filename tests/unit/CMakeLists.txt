# Anolis Unit Tests

find_package(GTest CONFIG REQUIRED)

set(ANOLIS_UNIT_TEST_SOURCES
    main.cpp
    sanity_test.cpp
    state_cache_test.cpp
    call_router_test.cpp
    device_loss_latch_test.cpp
    safe_state_test.cpp
    reissue_worker_test.cpp
    reissue_off_provider_lock_test.cpp
    actuation_gate_test.cpp
    config_test.cpp
    runtime_ownership_validation_test.cpp
    poll_hint_check_test.cpp
    http_handlers_test.cpp
    http_auth_test.cpp
    parameter_types_test.cpp
    provider_process_test.cpp
    provider_handle_test.cpp
    provider_registry_test.cpp
    provider_supervisor_test.cpp
    device_registry_concurrency_test.cpp
    device_registry_test.cpp
    runtime_preflight_test.cpp
    latch_store_test.cpp
    runtime_latch_persistence_test.cpp
    runtime_stop_chain_test.cpp
    runtime_zero_devices_test.cpp
    runtime_broken_pipe_test.cpp
    event_emitter_test.cpp
    influx_sink_test.cpp
    health_line_protocol_test.cpp
    health_snapshot_test.cpp
    run_journal_test.cpp
    signal_handler_test.cpp
)

if(ANOLIS_ENABLE_AUTOMATION)
    list(APPEND ANOLIS_UNIT_TEST_SOURCES
        bt_runtime_test.cpp
        bt_nodes_test.cpp
        mode_manager_test.cpp
        runtime_mode_hooks_test.cpp
    )
endif()

add_executable(provider_hello_mismatch_helper
    helpers/hello_protocol_mismatch_provider.cpp
)
target_include_directories(provider_hello_mismatch_helper SYSTEM PRIVATE
    ${CMAKE_BINARY_DIR}/core
)
target_link_libraries(provider_hello_mismatch_helper PRIVATE
    anolis_proto
    protobuf::libprotobuf
)
anolis_apply_warnings(provider_hello_mismatch_helper)

add_executable(provider_stalled_read_helper
    helpers/stalled_read_provider.cpp
)
target_include_directories(provider_stalled_read_helper SYSTEM PRIVATE
    ${CMAKE_BINARY_DIR}/core
)
target_link_libraries(provider_stalled_read_helper PRIVATE
    anolis_proto
    protobuf::libprotobuf
)
anolis_apply_warnings(provider_stalled_read_helper)

add_executable(provider_empty_inventory_helper
    helpers/empty_inventory_provider.cpp
)
target_include_directories(provider_empty_inventory_helper SYSTEM PRIVATE
    ${CMAKE_BINARY_DIR}/core
)
target_link_libraries(provider_empty_inventory_helper PRIVATE
    anolis_proto
    protobuf::libprotobuf
)
anolis_apply_warnings(provider_empty_inventory_helper)

# Test executable
add_executable(anolis_unit_tests
    ${ANOLIS_UNIT_TEST_SOURCES}
)

target_include_directories(anolis_unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/core  # Access to internal headers
)
target_include_directories(anolis_unit_tests SYSTEM PRIVATE
    ${CMAKE_BINARY_DIR}/core  # Access to generated proto headers
)

target_compile_definitions(anolis_unit_tests PRIVATE
    PROVIDER_HELLO_MISMATCH_HELPER_PATH="$<TARGET_FILE:provider_hello_mismatch_helper>"
    PROVIDER_STALLED_READ_HELPER_PATH="$<TARGET_FILE:provider_stalled_read_helper>"
    PROVIDER_EMPTY_INVENTORY_HELPER_PATH="$<TARGET_FILE:provider_empty_inventory_helper>"
)
add_dependencies(anolis_unit_tests provider_hello_mismatch_helper provider_stalled_read_helper
                 provider_empty_inventory_helper)

target_link_libraries(anolis_unit_tests PRIVATE
    GTest::gtest
    GTest::gmock
    anolis_runtime
    anolis_state
    anolis_control
    anolis_provider
    anolis_registry
    anolis_http
    anolis_events
    anolis_telemetry
    anolis_runs
    anolis_proto
    protobuf::libprotobuf
)
if(ANOLIS_ENABLE_AUTOMATION)
    target_link_libraries(anolis_unit_tests PRIVATE anolis_automation)
endif()
anolis_apply_warnings(anolis_unit_tests)

# Register tests. Use PRE_TEST so discovery does not execute during build.
include(GoogleTest)
gtest_discover_tests(anolis_unit_tests
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 60
    PROPERTIES LABELS "unit"
)
