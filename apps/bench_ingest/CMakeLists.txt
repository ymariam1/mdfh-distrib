add_executable(bench_ingest bench_ingest.cpp)
target_link_libraries(bench_ingest PRIVATE 
    mdfh 
    Boost::boost
    CLI11::CLI11
)
target_compile_features(bench_ingest PRIVATE cxx_std_23) 