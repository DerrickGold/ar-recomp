cmake_minimum_required(VERSION 3.16)
project(snesrecomp_runtime_consumer LANGUAGES C CXX)

find_package(snesrecomp-runtime CONFIG REQUIRED)

add_executable(snesrecomp_runtime_consumer main.cpp)
target_compile_features(snesrecomp_runtime_consumer PRIVATE cxx_std_17)
target_link_libraries(snesrecomp_runtime_consumer PRIVATE snesrecomp::runtime)
