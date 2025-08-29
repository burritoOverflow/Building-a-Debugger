function(add_test_cpp_target name)
    add_executable(${name} ${name}.cpp)
    # we'll compile all as PIE, without any optimizations, and with debug information
    # even in Release builds
    target_compile_options(${name} PRIVATE -g -O0 -pie -gdwarf-4)
    # ensure whenever we build the test target, the test programs are built as well
    add_dependencies(tests ${name})
endfunction()

function(add_asm_target name)
    add_executable(${name} ${name}.s)
    # as above, build as PIE
    # (debug info and optimization flags aren't necessary
    # as we won;t use debug info for assembly and assembler doesn't carry out optimizations)
    target_compile_options(${name} PRIVATE -pie)
    add_dependencies(tests ${name})
endfunction()

add_test_cpp_target(run_endlessly)
add_test_cpp_target(end_immediately)
add_test_cpp_target(hello_sdb)
add_test_cpp_target(memory)
add_test_cpp_target(anti_debugger)

add_executable(multi_cu multi_cu_main.cpp multi_cu_other.cpp)
target_compile_options(multi_cu PRIVATE -g -O0 -pie -gdwarf-4)
add_dependencies(tests multi_cu)

add_asm_target(reg_write)
add_asm_target(reg_read)
