function(target_enable_warnings tgt)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${tgt} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wconversion -Wsign-conversion
            -Wmissing-prototypes -Wstrict-prototypes
            -Wundef -Wpointer-arith
            -Wcast-align -Wwrite-strings
            -Wformat=2
            $<$<CONFIG:Debug>:-fno-omit-frame-pointer>)

        # -Wlogical-op is GNU-only; clang rejects it under -Werror.
        if(CMAKE_C_COMPILER_ID MATCHES "GNU")
            target_compile_options(${tgt} PRIVATE -Wlogical-op)
        endif()

        if(COG_ENABLE_WERROR)
            target_compile_options(${tgt} PRIVATE -Werror)
        endif()

        if(COG_ENABLE_ASAN)
            target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()