add_library(libsdb process.cpp pipe.cpp registers.cpp breakpoint_site.cpp disassembler.cpp watchpoint.cpp syscalls.cpp)
add_library(sdb::libsdb ALIAS libsdb)
target_link_libraries(libsdb PRIVATE Zydis::Zydis)

set_target_properties(libsdb PROPERTIES
        libsdb
        PROPERTIES OUTPUT_NAME sdb
)

target_compile_features(libsdb PUBLIC cxx_std_17)

target_include_directories(libsdb
        PUBLIC
        # when using an installed version of the library,
        # public headers are in 'include' relative to installed location
        $<INSTALL_INTERFACE:include>
        # when using a version in the build tree use the 'include' directory in the source tree
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src/include
)

include(GNUInstallDirs)
install(TARGETS libsdb
        EXPORT sdb-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# install the public headers
install(
        DIRECTORY ${PROJECT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# make importable by other projects
install(
        EXPORT sdb-targets
        FILE SdbConfig.cmake
        # using the namespace sdb, so users link to the 'sdb::libsdb' target
        NAMESPACE sdb::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sdb
)
