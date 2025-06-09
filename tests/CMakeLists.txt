# Find required test frameworks
find_package(GTest REQUIRED)
find_package(Catch2 REQUIRED)

# Ring buffer tests
add_executable(test_ring_buffer test_ring_buffer.cpp)
target_link_libraries(test_ring_buffer PRIVATE mdfh GTest::gtest GTest::gtest_main)
target_compile_features(test_ring_buffer PRIVATE cxx_std_23)

# Advanced ring buffer tests
add_executable(test_ring_buffer_advanced test_ring_buffer_advanced.cpp)
target_link_libraries(test_ring_buffer_advanced PRIVATE mdfh GTest::gtest GTest::gtest_main)
target_compile_features(test_ring_buffer_advanced PRIVATE cxx_std_23)

# Enable advanced performance tests if option is set
if(ENABLE_ADVANCED_PERF)
    target_compile_definitions(test_ring_buffer_advanced PRIVATE ENABLE_ADVANCED_PERF=1)
endif()

# Encoding tests
add_executable(test_encoding test_encoding.cpp)
target_link_libraries(test_encoding PRIVATE mdfh GTest::gtest GTest::gtest_main Catch2::Catch2WithMain)
target_compile_features(test_encoding PRIVATE cxx_std_23)

# Multi-feed ingestion tests
add_executable(test_multi_feed_ingestion test_multi_feed_ingestion.cpp)
target_link_libraries(test_multi_feed_ingestion PRIVATE mdfh GTest::gtest GTest::gtest_main)
target_compile_features(test_multi_feed_ingestion PRIVATE cxx_std_23)

# Register tests with CTest
add_test(NAME RingBufferTests COMMAND test_ring_buffer)
add_test(NAME RingBufferAdvancedTests COMMAND test_ring_buffer_advanced)
add_test(NAME EncodingTests COMMAND test_encoding)
add_test(NAME MultiFeedIngestionTests COMMAND test_multi_feed_ingestion)

# Performance tests (separate target for optional execution)
add_executable(perf_tests 
    test_ring_buffer.cpp 
    test_ring_buffer_advanced.cpp 
    test_encoding.cpp
)
target_link_libraries(perf_tests PRIVATE mdfh GTest::gtest GTest::gtest_main Catch2::Catch2WithMain)
target_compile_features(perf_tests PRIVATE cxx_std_23)
target_compile_definitions(perf_tests PRIVATE ENABLE_PERFORMANCE_TESTS=1)

# Enable advanced performance tests for perf_tests target
if(ENABLE_ADVANCED_PERF)
    target_compile_definitions(perf_tests PRIVATE ENABLE_ADVANCED_PERF=1)
endif() 