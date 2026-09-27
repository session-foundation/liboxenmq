#pragma once
#include "oxenmq/oxenmq.h"
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <chrono>
#include <random>
#include <oxen/log.hpp>
#include "oxenmq/fmt.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace oxenmq;

// Apple's mutexes, thread scheduling, and IO handling are garbage and it shows up with lots of
// spurious failures in this test suite (because it expects a system to not suck that badly), so we
// multiply the time-sensitive bits by this factor as a hack to make the test suite work.
constexpr int TIME_DILATION =
#ifdef __APPLE__
    5;
#elif defined(__x86_64__)
    1;
#else
    2;
#endif

static auto startup = std::chrono::steady_clock::now();

inline std::atomic<uint16_t> last_port = 20000 + std::random_device{}() % 20000;

/// Returns a localhost connection string to listen on, on a port nothing is currently bound to.
/// Ports are handed out sequentially from a start chosen at random per process, and each one is
/// checked with a throwaway bind() first: several copies of this suite can run on one machine at
/// the same time (the macOS CI builders do), and a fixed sequence had them collide.
inline std::string random_localhost() {
    while (true) {
        uint16_t port = ++last_port;
        assert(port); // We should never call this enough to overflow
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool free = bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
        close(fd);
        if (free)
            return "tcp://127.0.0.1:" + std::to_string(port);
    }
}


// Catch2 macros aren't thread safe, so guard with a mutex
inline std::unique_lock<std::mutex> catch_lock() {
    static std::mutex mutex;
    return std::unique_lock<std::mutex>{mutex};
}

/// Waits up to 200ms for something to happen.
template <typename Func>
inline void wait_for(Func f, std::chrono::milliseconds wait_time = 200ms) {
    auto start = std::chrono::steady_clock::now();
    auto end = start + wait_time * TIME_DILATION;
    while (std::chrono::steady_clock::now() < end) {
        if (f())
            break;
        std::this_thread::sleep_for(10ms * TIME_DILATION);
    }
    auto lock = catch_lock();
    UNSCOPED_INFO(
            "done waiting after " << (std::chrono::steady_clock::now() - start).count() << "ns");
}

/// Waits on an atomic bool for up to 100ms for an initial connection, which is more than enough
/// time for an initial connection + request.
inline void wait_for_conn(std::atomic<bool>& c) {
    wait_for([&c] { return c.load(); });
}

/// Waits enough time for us to receive a reply from a localhost remote.
inline void reply_sleep() {
    std::this_thread::sleep_for(10ms * TIME_DILATION);
}

namespace oxenmq {
class TestSuiteHelper {
  public:
    static size_t num_peers(const OxenMQ& omq) { return omq.peers.size(); }
};
}  // namespace oxenmq
