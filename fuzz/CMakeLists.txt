# Fuzz targets (built only when ENABLE_FUZZING=ON; see cmake/AddFuzzTarget.cmake).

# YAML runtime-config loader (anolis::runtime::load_config).
anolis_add_fuzz_target(
  NAME fuzz_config
  SOURCES fuzz_config.cpp
  LINK anolis_runtime
)

# ADPP framed-stdio decoder (anolis::provider::FramedStdioClient::read_frame).
anolis_add_fuzz_target(
  NAME fuzz_frame
  SOURCES fuzz_frame.cpp
  LINK anolis_provider
)
