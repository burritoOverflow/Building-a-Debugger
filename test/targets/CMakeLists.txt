function(add_test_cpp_target name)
    add_executable(${name} ${name}.cpp)
    # we'll compile all as PIE, without any optimizations, and with debug information
    # even in Release builds
    target_compile_options(${name} PRIVATE -g -O0 -pie)
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

add_asm_target(reg_write)
add_asm_target(reg_read)
