# Anolis Core - Runtime Executable

# Generate protobuf from anolis-protocol (same as provider-sim)
if(NOT DEFINED ANOLIS_PROTOCOL_DIR)
    set(ANOLIS_PROTOCOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/anolis-protocol" CACHE PATH "Path to anolis-protocol repository")
endif()
get_filename_component(ANOLIS_PROTOCOL_DIR_ABS "${ANOLIS_PROTOCOL_DIR}" REALPATH)
set(ADPP_PROTO_DIR "${ANOLIS_PROTOCOL_DIR_ABS}/spec/device-provider")

if(NOT EXISTS "${ADPP_PROTO_DIR}/protocol.proto")
    message(FATAL_ERROR
        "ADPP proto not found at: ${ADPP_PROTO_DIR}/protocol.proto\n"
        "Expected anolis-protocol submodule at: ${ANOLIS_PROTOCOL_DIR_ABS}\n"
        "Run: git submodule update --init --recursive\n"
        "Or set custom path: cmake -DANOLIS_PROTOCOL_DIR=/path/to/anolis-protocol ..."
    )
endif()

# Generate build config header used by shared headers for feature-flag consistency
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/anolis_build_config.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/anolis_build_config.hpp
    @ONLY
)

# Use modern protobuf_generate instead of deprecated protobuf_generate_cpp
add_library(anolis_proto OBJECT ${ADPP_PROTO_DIR}/protocol.proto)
target_link_libraries(anolis_proto PUBLIC protobuf::libprotobuf)
target_include_directories(anolis_proto SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

protobuf_generate(
    TARGET anolis_proto
    LANGUAGE cpp
    IMPORT_DIRS ${ADPP_PROTO_DIR}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)

# Skip clang-tidy on generated protobuf sources/headers to avoid noisy diagnostics
set(PROTO_GEN_CC ${CMAKE_CURRENT_BINARY_DIR}/protocol.pb.cc)
set(PROTO_GEN_H  ${CMAKE_CURRENT_BINARY_DIR}/protocol.pb.h)
set_source_files_properties(${PROTO_GEN_CC} ${PROTO_GEN_H} PROPERTIES SKIP_CLANG_TIDY ON)
# Also disable clang-tidy on the generated protobuf target itself to prevent reruns via compile_commands
set_property(TARGET anolis_proto PROPERTY CXX_CLANG_TIDY "")

# Logging library
add_library(anolis_logging STATIC
    logging/logger.cpp
)
target_include_directories(anolis_logging PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
anolis_apply_warnings(anolis_logging)

# Automation support library (mode/parameter facilities without BehaviorTree dependency)
add_library(anolis_automation_support STATIC
    automation/mode_manager.cpp
    automation/parameter_manager.cpp
)
target_include_directories(anolis_automation_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(anolis_automation_support PUBLIC
    anolis_logging
)
anolis_apply_warnings(anolis_automation_support)

# Provider host library
add_library(anolis_provider STATIC
    provider/framed_stdio_client.cpp
    provider/provider_process.cpp
    provider/provider_handle.cpp
    provider/provider_supervisor.cpp
    provider/provider_registry.cpp
)

target_include_directories(anolis_provider PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(anolis_provider PUBLIC
    anolis_proto
    anolis_logging
    protobuf::libprotobuf
)
anolis_apply_warnings(anolis_provider)

# Registry library
add_library(anolis_registry STATIC
    registry/device_registry.cpp
)

target_include_directories(anolis_registry PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(anolis_registry PUBLIC
    anolis_proto
    anolis_provider
    anolis_logging
    protobuf::libprotobuf
)
anolis_apply_warnings(anolis_registry)

# Events library
add_library(anolis_events STATIC
    events/event_emitter.cpp
)

target_include_directories(anolis_events PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(anolis_events SYSTEM PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(anolis_events PUBLIC
    anolis_logging
)
anolis_apply_warnings(anolis_events)

# Telemetry library (InfluxDB sink)
add_library(anolis_telemetry STATIC
    telemetry/influx_sink.cpp
)

target_include_directories(anolis_telemetry PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(anolis_telemetry PUBLIC
    anolis_events
    httplib::httplib
)
anolis_apply_warnings(anolis_telemetry)

# State cache library
add_library(anolis_state STATIC
    state/state_cache.cpp
)

target_include_directories(anolis_state PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(anolis_state PUBLIC
    anolis_proto
    anolis_provider
    anolis_registry
    anolis_events
    protobuf::libprotobuf
)
anolis_apply_warnings(anolis_state)

# Call router library
add_library(anolis_control STATIC
    control/call_router.cpp
)

target_include_directories(anolis_control PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(anolis_control PUBLIC
    anolis_proto
    anolis_provider
    anolis_registry
    anolis_state
    anolis_automation_support
    protobuf::libprotobuf
)
anolis_apply_warnings(anolis_control)

# Automation library (Behavior Trees)
if(ANOLIS_ENABLE_AUTOMATION)
    find_package(behaviortree_cpp CONFIG REQUIRED)

    add_library(anolis_automation STATIC
        automation/bt_runtime.cpp
        automation/bt_nodes.cpp
    )

    target_include_directories(anolis_automation PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(anolis_automation PUBLIC
        anolis_state
        anolis_control
        anolis_automation_support
        BT::behaviortree_cpp
    )
    anolis_apply_warnings(anolis_automation)

    # Automation sanity test (lightweight, no gtest)
    add_executable(bt_nodes_sanity
        automation/bt_nodes_sanity.cpp
    )

    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_libraries(bt_nodes_sanity PRIVATE
            $<LINK_GROUP:RESCAN,anolis_automation,anolis_control>
            BT::behaviortree_cpp
        )
    else()
        target_link_libraries(bt_nodes_sanity PRIVATE
            anolis_automation
            BT::behaviortree_cpp
        )
    endif()
    anolis_apply_warnings(bt_nodes_sanity)
endif()

# HTTP server library
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(anolis_http STATIC
    http/json.cpp
    http/server.cpp
    http/handlers/device_handlers.cpp
    http/handlers/state_handlers.cpp
    http/handlers/control_handlers.cpp
    http/handlers/system_handlers.cpp
)

target_include_directories(anolis_http PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(anolis_http PUBLIC
    anolis_proto
    anolis_provider
    anolis_registry
    anolis_state
    anolis_control
    anolis_automation_support
    httplib::httplib
    nlohmann_json::nlohmann_json
    protobuf::libprotobuf
)
if(ANOLIS_ENABLE_AUTOMATION)
    target_link_libraries(anolis_http PUBLIC anolis_automation)
endif()
anolis_apply_warnings(anolis_http)

# Runtime library (config + bootstrap)
find_package(yaml-cpp CONFIG REQUIRED)

add_library(anolis_runtime STATIC
    runtime/config.cpp
    runtime/runtime.cpp
    runtime/ownership_validation.cpp
    runtime/signal_handler.cpp
)

target_include_directories(anolis_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(anolis_runtime PUBLIC
    anolis_proto
    anolis_provider
    anolis_registry
    anolis_state
    anolis_control
    anolis_http
    anolis_telemetry
    anolis_automation_support
    yaml-cpp::yaml-cpp
    protobuf::libprotobuf
)
if(ANOLIS_ENABLE_AUTOMATION)
    target_link_libraries(anolis_runtime PUBLIC anolis_automation)
endif()
anolis_apply_warnings(anolis_runtime)

# Minimal runtime executable
add_executable(anolis-runtime
    src/main.cpp
)

target_include_directories(anolis-runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(anolis-runtime SYSTEM PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}  # Generated headers
)

target_link_libraries(anolis-runtime PRIVATE
    anolis_runtime
)
anolis_apply_warnings(anolis-runtime)

if(WIN32)
    target_compile_definitions(anolis-runtime PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
