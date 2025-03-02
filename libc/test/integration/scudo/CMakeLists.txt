if(NOT LLVM_LIBC_INCLUDE_SCUDO)
  return()
endif()

# We use a special library consisting of only the SCUDO allocator
# functions to link to the integration tests below. We could instead
# link to libllvmlibc.a directly, but since libllvmlibc.a contains
# functions which depend on the LLVM libc startup system, the integration
# test will have to link to the LLVM libc startup system. LLVM libc's startup
# system is not complete enough to allow this. It is also desireable to
# keep the dependencies as minimal as possible.

add_entrypoint_library(
  libc_for_scudo_integration_test
  DEPENDS
    libc.src.errno.errno
    libc.src.fcntl.open
    libc.src.sched.__sched_getcpucount
    libc.src.stdlib.aligned_alloc
    libc.src.stdlib.calloc
    libc.src.stdlib.free
    libc.src.stdlib.malloc
    libc.src.stdlib.realloc
    libc.src.sys.auxv.getauxval
    libc.src.sys.mman.mmap
    libc.src.sys.mman.munmap
    libc.src.sys.prctl.prctl
    libc.src.unistd.__llvm_libc_syscall
    libc.src.unistd.close
    libc.src.unistd.read
    libc.src.unistd.sysconf
    libc.src.unistd.write
)

add_executable(
  libc-scudo-integration-test
  integration_test.cpp
)

target_link_options(
  libc-scudo-integration-test
  PRIVATE
  -pthread
)

target_link_libraries(libc-scudo-integration-test
  PRIVATE
  libc_for_scudo_integration_test
)

add_executable(
  libc-gwp-asan-uaf-should-crash
  gwp_asan_should_crash.cpp
)

target_link_options(
  libc-gwp-asan-uaf-should-crash
  PRIVATE
  -pthread
)

target_link_libraries(libc-gwp-asan-uaf-should-crash
  PRIVATE
  libc_for_scudo_integration_test
)

add_custom_command(TARGET libc-scudo-integration-test
                   POST_BUILD
                   COMMAND $<TARGET_FILE:libc-scudo-integration-test>
                   COMMENT "Run the test after it is built."
                   VERBATIM)
