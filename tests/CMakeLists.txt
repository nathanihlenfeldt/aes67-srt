# Unit tests.  They run on any platform: nothing here touches ALSA, libsrt or a
# real daemon, and the modules that will are compiled in behind AUTO options.
add_executable(aes67-srt-tests
  test_app.cpp
  test_audio.cpp
  test_clock_drift.cpp
  test_clock_playout.cpp
  test_clock_ratio.cpp
  test_clock_receive_path.cpp
  test_clock_resampler.cpp
  test_clock_resampler_quality.cpp
  test_commissioning.cpp
  test_config.cpp
  test_daemon.cpp
  test_delay_line.cpp
  test_engine.cpp
  test_float_ring.cpp
  test_http.cpp
  test_main.cpp
  test_test_signal.cpp
  test_transport.cpp
  test_wire.cpp)

target_link_libraries(aes67-srt-tests PRIVATE aes67-srt-core)
target_compile_options(aes67-srt-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_test(NAME aes67-srt-tests COMMAND aes67-srt-tests)

# The binary can also be smoke tested from the command line, which is how CI
# checks that a refusal exits non-zero and says why.
add_test(NAME config-validate
  COMMAND aes67-srt -c ${CMAKE_SOURCE_DIR}/config/aes67-srt.dev.conf --validate)
set_tests_properties(config-validate PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
