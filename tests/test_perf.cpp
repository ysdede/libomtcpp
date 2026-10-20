/**
 * test_perf.cpp - Performance benchmark for loopback streaming
 */

#include "../src/OMTSender.h"
#include "../src/OMTProtocol.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#ifdef _WIN32
  #include "../src/omt_os_win.h"
#else
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
#endif

using omt::FrameHeader;
using omt::FrameType;
using namespace omt::MetadataConstants;

namespace {

int connectClient(int port) {
    int fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
        return -1;
    }
    return fd;
}

void sendCommand(int fd, const char* xml) {
    const size_t len = std::strlen(xml);
    std::string wire(FrameHeader::SIZE + len + 1, '\0');
    FrameHeader h;
    std::memset(&h, 0, sizeof(h));
    h.version = 1;
    h.frameType = static_cast<uint8_t>(FrameType::Metadata);
    h.timestamp = 0;
    h.metadataLength = static_cast<uint16_t>(len + 1);
    h.dataLength = static_cast<int32_t>(len + 1);
    std::memcpy(&wire[0], &h, FrameHeader::SIZE);
    std::memcpy(&wire[FrameHeader::SIZE], xml, len);
    size_t off = 0;
    while (off < wire.size()) {
        int n = send(fd, wire.data() + off, static_cast<int>(wire.size() - off), 0);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

} // namespace

int main() {
    omt::SenderConfig cfg;
    cfg.name = "perf-test";
    cfg.port = 16488;
    omt::Sender sender(cfg);
    if (!sender.start()) {
        std::fprintf(stderr, "test_perf: sender failed to start\n");
        return 2;
    }

    int clientFd = connectClient(cfg.port);
    if (clientFd < 0) {
        std::fprintf(stderr, "test_perf: loopback connect failed\n");
        return 2;
    }

    // Subscribe client to video
    sendCommand(clientFd, CHANNEL_SUBSCRIBE_VIDEO);

    // Wait until channel is registered
    for (int i = 0; i < 50; i++) {
        if (sender.videoSubscriberCount() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (sender.videoSubscriberCount() == 0) {
        std::fprintf(stderr, "test_perf: video subscription timeout\n");
        return 2;
    }

    // Start background receiver thread to drain socket buffer
    std::atomic<bool> drainRunning{true};
    std::atomic<uint64_t> totalBytesReceived{0};
    std::thread receiverThread([clientFd, &drainRunning, &totalBytesReceived]() {
        std::vector<char> buf(128 * 1024);
        while (drainRunning.load()) {
            ssize_t n = recv(clientFd, buf.data(), buf.size(), 0);
            if (n > 0) {
                totalBytesReceived += static_cast<uint64_t>(n);
            } else if (n < 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            } else {
                break;
            }
        }
    });

    // Prepare media frame (1080p BGRA frame data)
    int width = 1920;
    int height = 1080;
    int stride = width * 4;
    std::vector<uint8_t> dummyData(height * stride, 0xFF);

    omt::MediaFrame frame{};
    frame.data = dummyData.data();
    frame.dataLength = static_cast<int>(dummyData.size());
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.codec = omt::Codec::BGRA;
    frame.frameRateN = 60;
    frame.frameRateD = 1;
    frame.colorSpace = omt::ColorSpace::BT709;

    // Warmup
    for (int i = 0; i < 50; i++) {
        sender.send(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Benchmark loop: send 2,000 frames
    constexpr int kFrameCount = 2000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < kFrameCount; i++) {
        sender.send(frame);
        // Slight pacing to avoid queue overflow, while testing high throughput
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }

    // Wait until drain completes sending
    for (int i = 0; i < 100; i++) {
        if (sender.getStats().totalFramesSent >= kFrameCount) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsedSec = std::chrono::duration<double>(end - start).count();

    auto stats = sender.getStats();

    drainRunning = false;
#ifdef _WIN32
    closesocket(clientFd);
#else
    ::shutdown(clientFd, SHUT_RDWR);
    ::close(clientFd);
#endif
    if (receiverThread.joinable()) {
        receiverThread.join();
    }

    sender.stop();

    double fps = stats.totalFramesSent / elapsedSec;
    double mbps = (stats.totalBytesSent * 8.0) / (elapsedSec * 1e6);

    std::printf("--- Benchmark Results ---\n");
    std::printf("Frames offered: %lld\n", (long long)stats.totalFramesOffered);
    std::printf("Frames sent:    %lld\n", (long long)stats.totalFramesSent);
    std::printf("Frames dropped: %lld\n", (long long)stats.totalFramesDropped);
    std::printf("Total time:     %.4f s\n", elapsedSec);
    std::printf("Throughput:     %.2f FPS\n", fps);
    std::printf("Data rate:      %.2f Mbps\n", mbps);
    std::printf("-------------------------\n");

    return 0;
}
