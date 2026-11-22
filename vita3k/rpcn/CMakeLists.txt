add_library(
    rpcn
    STATIC
    include/rpcn/client.h
    include/rpcn/p2p.h
    include/rpcn/vport.h
    include/rpcn/stream.h
    include/rpcn/types.h
    src/client.cpp
    src/p2p.cpp
    src/vport.cpp
)

target_include_directories(rpcn PUBLIC include)
target_link_libraries(rpcn PRIVATE util ssl)
if (WIN32)
    target_link_libraries(rpcn PRIVATE winsock)
endif()
