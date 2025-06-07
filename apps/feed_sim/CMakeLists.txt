add_executable(feed_sim sim.cpp)
target_link_libraries(feed_sim PRIVATE 
    mdfh 
    Boost::boost
    CLI11::CLI11
)
target_compile_features(feed_sim PRIVATE cxx_std_23)
