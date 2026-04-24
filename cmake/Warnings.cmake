# First-party compiler warning policy for anolis targets.
# Generated protobuf translation units are not first-party sources.

function(anolis_apply_warnings target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "anolis_apply_warnings: unknown target '${target_name}'")
    endif()

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
        if(ANOLIS_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE /WX)
        endif()
        # C4702 (unreachable code) fires from BT.CPP's if-constexpr template
        # instantiations during code generation. It is a known MSVC false positive
        # in third-party template-heavy code; /external suppression does not apply
        # to codegen-phase warnings. Disable globally for all anolis targets.
        target_compile_options(${target_name} PRIVATE /wd4702)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        if(ANOLIS_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()
