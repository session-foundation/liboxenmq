#include "common.h"
#include <oxenc/hex.h>

using namespace oxenmq;

TEST_CASE("basic requests", "[requests]") {
    std::string listen = random_localhost();
    OxenMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_curve(listen);

    std::atomic<int> hellos{0}, his{0};

    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "hello", [&](Message& m) {
            m.send_reply("123");
    });
    server.start();

    OxenMQ client{};

    client.start();

    std::atomic<bool> connected{false}, failed{false};
    std::string pubkey;

    auto c = client.connect_remote(address{listen, server.get_pubkey()},
            [&](auto conn) { pubkey = conn.pubkey(); connected = true; },
            [&](auto, auto) { failed = true; });

    wait_for([&] { return connected || failed; });
    {
        auto lock = catch_lock();
        REQUIRE( connected );
        REQUIRE_FALSE( failed );
        REQUIRE( oxenc::to_hex(pubkey) == oxenc::to_hex(server.get_pubkey()) );
    }

    std::atomic<bool> got_reply{false};
    bool success;
    std::vector<std::string> data;
    client.request(c, "public.hello", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE( data == std::vector<std::string>{{"123"}} );
    }
}

TEST_CASE("request from server to client", "[requests]") {
    std::string listen = random_localhost();
    OxenMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_curve(listen);

    std::atomic<int> hellos{0}, his{0};

    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "hello", [&](Message& m) {
            m.send_reply("123");
    });
    server.start();

    OxenMQ client{};

    client.start();

    std::atomic<bool> connected{false}, failed{false};
    std::string pubkey;

    auto c = client.connect_remote(address{listen, server.get_pubkey()},
            [&](auto conn) { pubkey = conn.pubkey(); connected = true; },
            [&](auto, auto) { failed = true; });

    int i;
    for (i = 0; i < 5; i++) {
        if (connected.load())
            break;
        std::this_thread::sleep_for(50ms);
    }
    {
        auto lock = catch_lock();
        REQUIRE( connected.load() );
        REQUIRE( !failed.load() );
        REQUIRE( i <= 1 );
        REQUIRE( oxenc::to_hex(pubkey) == oxenc::to_hex(server.get_pubkey()) );
    }

    std::atomic<bool> got_reply{false};
    bool success;
    std::vector<std::string> data;
    client.request(c, "public.hello", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    });

    std::this_thread::sleep_for(50ms);
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE( data == std::vector<std::string>{{"123"}} );
    }
}

TEST_CASE("request timeouts", "[requests][timeout]") {
    std::string listen = random_localhost();
    OxenMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_curve(listen);

    std::atomic<int> hellos{0}, his{0};

    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "blackhole", [&](Message& m) { /* doesn't reply */ });
    server.start();

    OxenMQ client{};

    client.CONN_CHECK_INTERVAL = 10ms; // impatience (don't set this low in production code)
    client.start();

    std::atomic<bool> connected{false}, failed{false};
    std::string pubkey;

    auto c = client.connect_remote(address{listen, server.get_pubkey()},
            [&](auto conn) { pubkey = conn.pubkey(); connected = true; },
            [&](auto, auto) { failed = true; });

    wait_for([&] { return connected || failed; });

    REQUIRE( connected );
    REQUIRE_FALSE( failed );
    REQUIRE( oxenc::to_hex(pubkey) == oxenc::to_hex(server.get_pubkey()) );

    std::atomic<bool> got_triggered{false};
    bool success;
    std::vector<std::string> data;
    client.request(c, "public.blackhole", [&](bool ok, std::vector<std::string> data_) {
            got_triggered = true;
            success = ok;
            data = std::move(data_);
        },
        oxenmq::send_option::request_timeout{10ms}
    );

    std::atomic<bool> got_triggered2{false};
    client.request(c, "public.blackhole", [&](bool ok, std::vector<std::string> data_) {
            got_triggered = true;
            success = ok;
            data = std::move(data_);
        },
        oxenmq::send_option::request_timeout{200ms}
    );

    std::this_thread::sleep_for(100ms);
    REQUIRE( got_triggered );
    REQUIRE_FALSE( got_triggered2 );
    REQUIRE_FALSE( success );
    REQUIRE( data == std::vector<std::string>{{"TIMEOUT"}} );

}

TEST_CASE("mass request timeouts", "[requests][timeout]") {
    std::string listen = random_localhost();
    OxenMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_curve(listen);

    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "blackhole", [&](Message& m) { /* doesn't reply */ });
    server.start();

    OxenMQ client{};

    // Long enough that every request below expires before the same cleanup pass.
    client.CONN_CHECK_INTERVAL = 1s;

    client.start();

    std::atomic<bool> connected{false}, failed{false};

    auto c = client.connect_remote(address{listen, server.get_pubkey()},
            [&](auto) { connected = true; },
            [&](auto, auto) { failed = true; });

    wait_for([&] { return connected || failed; });

    REQUIRE( connected );
    REQUIRE_FALSE( failed );

    // The proxy queues the failure callback for each expired request from its own thread.  More
    // than the control socket's pipe can hold (2000 jobs: the high-water marks of both ends, in
    // whole messages) is what it takes to find out whether that goes through the pipe, where the
    // proxy would be waiting on itself.
    constexpr int N = 2500;
    std::atomic<int> timeouts{0}, other{0};
    for (int i = 0; i < N; i++) {
        client.request(c, "public.blackhole", [&](bool ok, std::vector<std::string> data) {
                if (!ok && data == std::vector<std::string>{{"TIMEOUT"}})
                    timeouts++;
                else
                    other++;
            },
            oxenmq::send_option::request_timeout{20ms}
        );
    }

    wait_for([&] { return timeouts + other >= N; }, 3s);
    {
        auto lock = catch_lock();
        REQUIRE( timeouts == N );
        REQUIRE( other == 0 );
    }
}
