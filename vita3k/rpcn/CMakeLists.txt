add_library(
    rpcn
    STATIC
    include/rpcn/client.h
    include/rpcn/stream.h
    include/rpcn/types.h
    src/client.cpp
)

target_include_directories(rpcn PUBLIC include)
target_link_libraries(rpcn PRIVATE util ssl)
if (WIN32)
    target_link_libraries(rpcn PRIVATE winsock)
endif()
