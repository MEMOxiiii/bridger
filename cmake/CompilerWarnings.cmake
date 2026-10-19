# bridger_set_warnings(<target>)
#
# Strict warnings, treated as errors, for Bridger's own code only.
# Never call this on third-party targets.
function(bridger_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /WX
            /permissive-
            /utf-8
            /Zc:__cplusplus
            /Zc:inline
            /w14242  # conversion, possible loss of data
            /w14254  # larger bit field to smaller
            /w14263  # member function does not override base virtual
            /w14265  # class has virtual functions but non-virtual destructor
            /w14287  # unsigned/negative constant mismatch
            /w14296  # expression is always true/false
            /w14311  # pointer truncation
            /w14545 /w14546 /w14547 /w14549 /w14555  # suspicious comma / no-effect expressions
            /w14619  # unknown warning number in pragma
            /w14640  # thread-unsafe static member initialization
            /w14826  # sign-extending conversion
            /w14905 /w14906  # string literal casts
            /w14928  # illegal copy-initialization
        )
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # MSVC-only: conforming preprocessor; no warnings from <angle-bracket> (third-party) headers.
            # clang-cl rejects these as unused arguments (fatal under /WX); it already treats
            # -imsvc (vcpkg) include paths as system headers.
            target_compile_options(${target} PRIVATE /Zc:preprocessor /external:anglebrackets /external:W0)
        else()
            # clang-cl understands most MSVC flags; add the clang-specific ones it lacks.
            target_compile_options(${target} PRIVATE
                -Wno-c++98-compat -Wno-c++98-compat-pedantic
                -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
                -Wunused -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
                -Wimplicit-fallthrough)
        endif()
        target_compile_definitions(${target} PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
            -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
            -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # GCC derives these from optimizer data flow, so -O2 and sanitizer builds report false positives, all
            # inside libstdc++ and nlohmann/json inlined into our code (GCC reports them even from system headers),
            # e.g. vector::insert from an initializer list and the memcmp in operator<=>. Clang keeps
            # -Wnull-dereference (a frontend check), MSVC keeps C4700/C4701, and ASan/UBSan catch these at run time.
            target_compile_options(${target} PRIVATE -Wno-null-dereference -Wno-maybe-uninitialized
                                                     -Wno-stringop-overflow -Wno-stringop-overread)
        endif()
    endif()
endfunction()
