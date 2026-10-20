// Real listener tests: automatic allocation, exact ports, exhaustion and races.
#include "../src/OMTSender.h"
#include "../include/libomt.h"
#include <cstdio>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#ifdef _WIN32
#include "../src/omt_os_win.h"
#else
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#endif

static int failures = 0;
static void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
static int connectTo(int port) {
    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd); return -1;
    }
    return fd;
}
static bool waitClients(omt::Sender& sender, int count) {
    for (int n = 0; n < 100; ++n) {
        if (sender.connectionCount() == count) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
int main() {
    omt::SenderConfig cfg;
    cfg.bindAddress = "127.0.0.1";
    cfg.portStart = 26450;
    cfg.portEnd = 26470;
    std::atomic<int> disconnected{0};
    omt::Sender first(cfg), second(cfg);
    first.setOnDisconnect([&](omt::Channel*) { ++disconnected; });
    second.setOnDisconnect([&](omt::Channel*) { ++disconnected; });
    check(first.port() == 0, "port is zero before start");
    if (!first.start() || !second.start()) return 2;
    const int firstPort = first.port(), secondPort = second.port();
    check(firstPort >= cfg.portStart && secondPort <= cfg.portEnd &&
          firstPort != secondPort, "automatic senders bind different ports");
    check(first.start() && first.port() == firstPort, "start is idempotent");
    int a = connectTo(firstPort);
    check(a >= 0 && waitClients(first, 1) && second.connectionCount() == 0,
          "first endpoint reaches only its sender");
    int b = connectTo(secondPort);
    check(b >= 0 && waitClients(second, 1) && first.connectionCount() == 1,
          "second endpoint reaches only its sender");
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    for (int n = 0; n < 100 && disconnected != 2; ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(disconnected == 2, "both client disconnects are observed");

    auto exactCfg = cfg; exactCfg.port = firstPort;
    omt::Sender conflict(exactCfg);
    check(!conflict.start() && conflict.port() == 0,
          "explicit busy port fails without moving");
    auto fullCfg = cfg; fullCfg.portStart = fullCfg.portEnd = firstPort;
    omt::Sender exhausted(fullCfg);
    check(!exhausted.start() && exhausted.port() == 0, "exhausted range fails");

    // Reserve/release without accepted clients (no TCP TIME_WAIT dependency).
    auto reuseCfg = cfg; reuseCfg.port = 26475;
    omt::Sender reusable(reuseCfg);
    check(reusable.start(), "explicit unused port starts");
    reusable.stop();
    check(reusable.port() == 0, "stop clears published port");
    omt::Sender replacement(reuseCfg);
    check(replacement.start() && replacement.port() == reuseCfg.port,
          "released listener can be reused");

    std::vector<std::unique_ptr<omt::Sender>> senders;
    std::vector<std::thread> threads;
    std::atomic<bool> go{false};
    std::atomic<int> started{0};
    for (int i = 0; i < 8; ++i) senders.emplace_back(new omt::Sender(cfg));
    for (auto& sender : senders) {
        auto* ptr = sender.get();
        threads.emplace_back([&, ptr] {
            while (!go.load()) std::this_thread::yield();
            if (ptr->start()) ++started;
        });
    }
    go = true;
    for (auto& thread : threads) thread.join();
    bool unique = started == 8;
    for (size_t i = 0; i < senders.size(); ++i) {
        unique = unique && senders[i]->port() != firstPort &&
                 senders[i]->port() != secondPort;
        for (size_t j = 0; j < i; ++j)
            unique = unique && senders[i]->port() != senders[j]->port();
    }
    check(unique, "concurrent starts reserve distinct endpoints");

    for (int badPort : {-1, 65536}) {
        auto bad = cfg; bad.port = badPort;
        omt::Sender invalid(bad);
        check(!invalid.start() && invalid.port() == 0, "invalid explicit port rejected");
    }
    for (auto range : {std::pair<int,int>{0, 6400}, {6600, 6400}, {6400, 65536}}) {
        auto bad = cfg; bad.portStart = range.first; bad.portEnd = range.second;
        omt::Sender invalid(bad);
        check(!invalid.start(), "invalid automatic range rejected");
    }
    auto bad = cfg; bad.bindAddress = "not-an-address";
    omt::Sender invalid(bad);
    check(!invalid.start(), "invalid bind address does not fall back");

    auto* ca = omt_send_create("auto-c-one", OMTQuality_Default);
    auto* cb = omt_send_create("auto-c-two", OMTQuality_Default);
    check(ca && cb && omt_send_getport(ca) >= 6400 &&
          omt_send_getport(ca) <= 6600 && omt_send_getport(cb) >= 6400 &&
          omt_send_getport(cb) <= 6600 && omt_send_getport(ca) != omt_send_getport(cb),
          "C API creates simultaneous senders in the default range");
    check(omt_send_getport(nullptr) == 0, "null C handle reports no port");
    omt_send_destroy(cb); omt_send_destroy(ca);
    return failures ? 1 : 0;
}
