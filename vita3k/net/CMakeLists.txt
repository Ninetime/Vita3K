add_library(
    net
    STATIC
    include/net/epoll.h
    include/net/functions.h
    include/net/state.h
    include/net/types.h
    include/net/socket.h
    src/net.cpp
    src/epoll.cpp
    src/posixsocket.cpp
    src/p2psocket.cpp
    src/rpcnp2psocket.cpp
    src/icmpsocket.cpp
)

target_include_directories(net PUBLIC include)
target_link_libraries(net PUBLIC emuenv mem np)
target_link_libraries(net PRIVATE rpcn)
if (WIN32)
    target_link_libraries(net PRIVATE winsock iphlpapi)
endif()
