cmake_minimum_required(VERSION 3.29)
project(fantasy)

set(CMAKE_CXX_STANDARD 23)


find_package(CURL REQUIRED)

find_package(nlohmann_json CONFIG REQUIRED)

find_package(SQLiteCpp CONFIG REQUIRED)

find_package(LibXml2 REQUIRED)






add_executable(fantasy main.cpp)

target_link_libraries(fantasy PRIVATE -static CURL::libcurl nlohmann_json::nlohmann_json SQLiteCpp LibXml2::LibXml2)
