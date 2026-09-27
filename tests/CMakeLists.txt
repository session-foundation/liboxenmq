
add_subdirectory(Catch2)

add_executable(tests
    main.cpp
    test_address.cpp
    test_batch.cpp
    test_connect.cpp
    test_commands.cpp
    test_failures.cpp
    test_inject.cpp
    test_pubsub.cpp
    test_requests.cpp
    test_routing_id.cpp
    test_socket_limit.cpp
    test_tagged_threads.cpp
    test_timer.cpp
)

find_package(Threads)

target_link_libraries(tests Catch2::Catch2 oxenmq Threads::Threads oxen::logging)

# Several tests drive raw zmq sockets of their own (over TCP, never sharing objects with oxenmq),
# and a shared oxenmq built with a static libzmq keeps that copy to itself.
if(oxenmq_private_libzmq)
    target_link_libraries(tests sessiondep::libzmq)
endif()

set_target_properties(tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

add_custom_target(check COMMAND tests)
