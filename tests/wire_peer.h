#pragma once
#include "../src/OMTSender.h"
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include "../src/omt_os_win.h"
#else
#include <unistd.h>
#endif

struct WirePeer {
    int fd = -1;
    explicit WirePeer(int port) {
        fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in a{}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
        if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a))) throw std::runtime_error("connect");
#ifdef _WIN32
        DWORD timeout = 2000;
#else
        timeval timeout{2,0};
#endif
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    }
    ~WirePeer() { if (fd >= 0) close(fd); }
    WirePeer(const WirePeer&) = delete;
    void command(const char* xml) {
        const size_t size = std::strlen(xml) + 1;
        omt::FrameHeader h{1, 1, 0, static_cast<uint16_t>(size), static_cast<int32_t>(size)};
        std::vector<char> bytes(sizeof(h) + size);
        std::memcpy(bytes.data(), &h, sizeof(h));
        std::memcpy(bytes.data() + sizeof(h), xml, size);
        size_t offset = 0;
        while (offset < bytes.size()) {
            int n = send(fd, bytes.data() + offset, static_cast<int>(bytes.size() - offset), 0);
            if (n <= 0) throw std::runtime_error("send");
            offset += n;
        }
    }
    void read(void* target, size_t count) {
        char* p = static_cast<char*>(target);
        while (count) {
            int n = recv(fd, p, static_cast<int>(count), 0);
            if (n <= 0) throw std::runtime_error("receive timeout/EOF");
            p += n; count -= n;
        }
    }
    std::vector<uint8_t> frame(int type, omt::FrameHeader& h) {
        for (int i = 0; i < 32; ++i) {
            read(&h, sizeof(h));
            if (h.version != 1 || h.dataLength < 0 || h.dataLength > 10*1024*1024)
                throw std::runtime_error("invalid frame");
            std::vector<uint8_t> body(h.dataLength);
            read(body.data(), body.size());
            if (h.frameType == type) return body;
        }
        throw std::runtime_error("expected media");
    }
};

template<class F> bool waitUntil(F predicate) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < end);
    return false;
}
