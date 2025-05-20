cmake_minimum_required(VERSION 3.25)

project("sdb" LANGUAGES CXX ASM)

include(CTest)

add_subdirectory("src")
add_subdirectory("tools")

if (BUILD_TESTING)
    find_package(Catch2 CONFIG REQUIRED)
    add_subdirectory("test")
endif ()

# find libedit
find_package(PkgConfig REQUIRED)
pkg_check_modules(libedit REQUIRED IMPORTED_TARGET libedit)

target_link_libraries(sdb PRIVATE sdb::libsdb PkgConfig::libedit)
