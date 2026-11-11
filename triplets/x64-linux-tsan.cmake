# Custom vcpkg triplet for ThreadSanitizer (TSAN) builds
# 
# This ensures ALL dependencies (protobuf, abseil, yaml-cpp, etc.) are built
# with -fsanitize=thread to maintain ABI compatibility.
#
# Usage:
#   cmake -DVCPKG_TARGET_TRIPLET=x64-linux-tsan ...
#
# CRITICAL:
# - Libraries link statically. vcpkg builds abseil static on Linux whatever
#   the triplet says, so a shared protobuf carries its own abseil copy, with
#   its own hash seed, local to libprotobuf.so so the dynamic linker cannot
#   merge it. A protobuf map filled inside libprotobuf can then miss lookups
#   made from the executable, so a call argument that was sent reads as
#   missing. TSan's documented limit is "Libc/libstdc++ static linking is not
#   supported"; VCPKG_CRT_LINKAGE stays dynamic, so libc, libstdc++ and
#   libtsan stay shared.
# - DO NOT force VCPKG_BUILD_TYPE (prevents Debug/Release ABI mismatches)

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# TSAN flags applied to all dependencies
set(VCPKG_C_FLAGS "-fsanitize=thread -fno-omit-frame-pointer -g")
set(VCPKG_CXX_FLAGS "-fsanitize=thread -fno-omit-frame-pointer -g")
set(VCPKG_LINKER_FLAGS "-fsanitize=thread")
