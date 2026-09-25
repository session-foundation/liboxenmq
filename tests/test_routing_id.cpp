#include "common.h"
#include <sodium.h>

using namespace oxenmq;

namespace {

std::pair<std::string, std::string> make_keypair() {
    std::string pubkey, privkey;
    pubkey.resize(crypto_box_PUBLICKEYBYTES);
    privkey.resize(crypto_box_SECRETKEYBYTES);
    REQUIRE(sodium_init() != -1);
    crypto_box_keypair(reinterpret_cast<unsigned char*>(&pubkey[0]), reinterpret_cast<unsigned char*>(&privkey[0]));
    return {std::move(pubkey), std::move(privkey)};
}

// A raw curve DEALER presenting an arbitrary routing id, which sends the initial HI.
zmq::socket_t raw_client(zmq::context_t& ctx, const std::string& addr, const std::string& server_pubkey,
        const std::string& routing_id, const std::pair<std::string, std::string>& keys = make_keypair()) {
    zmq::socket_t sock{ctx, zmq::socket_type::dealer};
    sock.set(zmq::sockopt::curve_serverkey, server_pubkey);
    sock.set(zmq::sockopt::curve_publickey, keys.first);
    sock.set(zmq::sockopt::curve_secretkey, keys.second);
    sock.set(zmq::sockopt::routing_id, routing_id);
    sock.set(zmq::sockopt::linger, 0);
    sock.connect(addr);
    sock.send(zmq::message_t{"HI", 2}, zmq::send_flags::none);
    return sock;
}

std::vector<std::vector<std::string>> recv_all(zmq::socket_t& sock, std::chrono::milliseconds duration) {
    std::vector<std::vector<std::string>> msgs;
    auto end = std::chrono::steady_clock::now() + duration * TIME_DILATION;
    zmq::pollitem_t item{sock.handle(), 0, ZMQ_POLLIN, 0};
    for (auto now = std::chrono::steady_clock::now(); now < end; now = std::chrono::steady_clock::now()) {
        if (zmq::poll(&item, 1, std::chrono::duration_cast<std::chrono::milliseconds>(end - now)) <= 0)
            continue;
        auto& parts = msgs.emplace_back();
        zmq::message_t msg;
        do {
            if (!sock.recv(msg, zmq::recv_flags::dontwait))
                break;
            parts.push_back(msg.to_string());
        } while (msg.more());
    }
    return msgs;
}

// A raw ROUTER listener, so that we can see the routing id a connecting OxenMQ presents.
zmq::socket_t raw_listener(zmq::context_t& ctx, const std::string& addr, const std::string& privkey = "") {
    zmq::socket_t sock{ctx, zmq::socket_type::router};
    if (!privkey.empty()) {
        sock.set(zmq::sockopt::curve_server, true);
        sock.set(zmq::sockopt::curve_secretkey, privkey);
    }
    sock.set(zmq::sockopt::router_handover, true);
    sock.set(zmq::sockopt::linger, 0);
    sock.bind(addr);
    return sock;
}

std::string first_route(zmq::socket_t& listener) {
    auto msgs = recv_all(listener, 200ms);
    return msgs.empty() || msgs.front().empty() ? "" : msgs.front().front();
}

bool contains(const std::vector<std::vector<std::string>>& msgs, std::string_view needle) {
    for (auto& parts : msgs)
        for (auto& p : parts)
            if (p == needle)
                return true;
    return false;
}

}  // namespace

TEST_CASE("routing ids presented by outgoing connections", "[routing_id]") {
    auto [s1_pub, s1_priv] = make_keypair();
    auto [s2_pub, s2_priv] = make_keypair();
    auto [c_pub, c_priv] = make_keypair();
    auto s1_addr = random_localhost(), s2_addr = random_localhost();

    zmq::context_t ctx;
    auto s1 = raw_listener(ctx, s1_addr, s1_priv);
    auto s2 = raw_listener(ctx, s2_addr, s2_priv);

    auto make_client = [&, c_pub = c_pub, c_priv = c_priv] {
        auto omq = std::make_unique<OxenMQ>(c_pub, c_priv, false, [](auto) { return ""; });
        omq->start();
        return omq;
    };
    auto noop_success = [](auto) {};
    auto noop_failure = [](auto, auto) {};

    SECTION("curve connections use an id derived from our key and the remote pubkey") {
        auto client = make_client();
        client->connect_remote(address{s1_addr, s1_pub}, noop_success, noop_failure);
        auto id1 = first_route(s1);
        client->connect_remote(address{s2_addr, s2_pub}, noop_success, noop_failure);
        auto id2 = first_route(s2);

        // A second instance with the same keys (e.g. after a restart) must present the same id so
        // that the listener hands over the old connection's route.
        auto restarted = make_client();
        restarted->connect_remote(address{s1_addr, s1_pub}, noop_success, noop_failure);
        auto id1_again = first_route(s1);

        auto lock = catch_lock();
        REQUIRE(id1.size() == 33);
        CHECK(id1[0] == 'K');
        CHECK(id1 != "L" + c_pub);
        CHECK(id1.find(c_pub) == std::string::npos);
        CHECK(id1_again == id1);
        REQUIRE(id2.size() == 33);
        CHECK(id2[0] == 'K');
        CHECK(id2 != id1);
    }

    SECTION("ephemeral curve connections use a random id") {
        auto client = make_client();
        client->connect_remote(address{s1_addr, s1_pub}, noop_success, noop_failure,
                connect_option::ephemeral_routing_id{});
        auto id1 = first_route(s1);
        client->connect_remote(address{s1_addr, s1_pub}, noop_success, noop_failure,
                connect_option::ephemeral_routing_id{});
        auto id2 = first_route(s1);

        auto lock = catch_lock();
        REQUIRE(id1.size() == 33);
        CHECK(id1[0] == 'R');
        REQUIRE(id2.size() == 33);
        CHECK(id2[0] == 'R');
        CHECK(id1 != id2);
    }

    SECTION("plaintext connections use a random id") {
        auto plain_addr = random_localhost();
        auto plain = raw_listener(ctx, plain_addr);
        auto client = make_client();
        client->connect_remote(address{plain_addr}, noop_success, noop_failure);
        auto id1 = first_route(plain);
        client->connect_remote(address{plain_addr}, noop_success, noop_failure);
        auto id2 = first_route(plain);

        auto lock = catch_lock();
        REQUIRE(id1.size() == 33);
        CHECK(id1[0] == 'R');
        CHECK(id1 != "L" + c_pub);
        CHECK(id1 != id2);
    }
}

TEST_CASE("legacy pubkey routing ids are still accepted", "[routing_id]") {
    std::string listen = random_localhost();
    auto [s_pub, s_priv] = make_keypair();
    OxenMQ server{s_pub, s_priv, false, [](auto) { return ""; }};
    server.listen_curve(listen);
    server.start();

    zmq::context_t ctx;
    auto keys = make_keypair();
    auto client = raw_client(ctx, listen, s_pub, "L" + keys.first, keys);
    auto recvd = recv_all(client, 200ms);
    auto lock = catch_lock();
    CHECK(contains(recvd, "HELLO"));
}
