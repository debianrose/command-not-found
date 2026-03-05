cmake_minimum_required(VERSION 3.25)
project(command-not-found)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(COMMAND_NOT_FOUND_VERSION "3.2")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -flto -fdata-sections -ffunction-sections -fomit-frame-pointer -fno-exceptions -fno-rtti")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,-O3 -Wl,--as-needed -Wl,--gc-sections -flto -s")

add_custom_target(generate-db 
    COMMAND ${CMAKE_COMMAND} -E env 
        "TERMUX_SCRIPTDIR=$ENV{TERMUX_SCRIPTDIR}"
        "TERMUX_PREFIX=$ENV{TERMUX_PREFIX}" 
        "TERMUX_ARCH=$ENV{TERMUX_ARCH}"
        ./generate-db.js
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating command databases..."
    VERBATIM
)

add_executable(command-not-found command-not-found.cpp)
add_dependencies(command-not-found generate-db)

target_compile_options(command-not-found PRIVATE  
    -Wall -Wextra -Werror -pedantic
)

target_compile_definitions(command-not-found PRIVATE 
    COMMAND_NOT_FOUND_VERSION="${COMMAND_NOT_FOUND_VERSION}"
    __TERMUX_PREFIX__="$ENV{TERMUX_PREFIX}"
)

if(CMAKE_COMPILER_IS_GNUCXX)
    target_link_options(command-not-found PRIVATE
        -static-libstdc++
    )
endif()

install(TARGETS command-not-found DESTINATION libexec/termux)
