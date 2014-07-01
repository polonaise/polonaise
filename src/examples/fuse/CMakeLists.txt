add_definitions(-DFUSE_USE_VERSION=26)
add_definitions(-D_FILE_OFFSET_BITS=64)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

include_directories(${GENERATED_CODE} ${THRIFT_INCLUDE_DIRS})
add_executable(polonaise-fuse-client main.cc)
target_link_libraries(polonaise-fuse-client fuse polonaise ${THRIFT_LIBRARIES})
install(TARGETS polonaise-fuse-client DESTINATION bin)
