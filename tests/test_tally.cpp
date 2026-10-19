/**
 * test_tally.cpp - loopback wire test for tally command parsing
 *
 * Connects a raw TCP client to a running omt::Sender and feeds it framed
 * metadata commands, then observes the aggregated tally state through the
 * public Sender API. This drives the full production path (accept ->
 * Channel -> receiverLoop -> processMetadata), not a unit-level re-run of
 * the matcher.
 *
 * The legacy Program== (double equals) tokens are what official receivers
 * actually put on the wire — see the NB in OMTConstants.h and libomtnet
 * commits 139d799 / 12f0b2f. The well-formed single-= forms are what this
 * port historically matched; both must keep working.
 */

#include "../src/OMTSender.h"
#include "../src/OMTProtocol.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
  #include "../src/omt_os_win.h"   // full shim: WSAStartup + send/close mapping
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

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ PASS ]" : "[ FAIL ]", what);
    if (!ok) g_failures++;
}

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

// Frames a metadata command exactly the way libomtnet receivers send it:
// 16-byte header (metadataLength = dataLength = len+1) then XML + trailing NUL.
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
        if (n <= 0) {
            std::fprintf(stderr, "test: socket send failed\n");
            std::exit(2);
        }
        off += static_cast<size_t>(n);
    }
}

bool pollState(const omt::Sender& s, bool wantPreview, bool wantProgram, int timeoutMs = 3000) {
    for (int waited = 0; waited < timeoutMs; waited += 20) {
        if (s.hasTallyPreview() == wantPreview && s.hasTallyProgram() == wantProgram) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return s.hasTallyPreview() == wantPreview && s.hasTallyProgram() == wantProgram;
}

} // namespace

int main() {
    omt::SenderConfig cfg;
    cfg.name = "tally-test";
    cfg.port = 16487;   // outside the 6400..6600 production range
    omt::Sender sender(cfg);
    if (!sender.start()) {
        std::fprintf(stderr, "test: sender failed to start\n");
        return 2;
    }

    int fd = connectClient(cfg.port);
    if (fd < 0) {
        std::fprintf(stderr, "test: loopback connect failed\n");
        return 2;
    }

    // Legacy Program== tokens — what libomtnet, vMix and every official receiver sends.
    sendCommand(fd, TALLY_PROGRAM_LEGACY);
    check(pollState(sender, false, true), "legacy PROGRAM sets program tally");

    sendCommand(fd, TALLY_NONE_LEGACY);
    check(pollState(sender, false, false), "legacy NONE clears tally");

    sendCommand(fd, TALLY_PREVIEW_LEGACY);
    check(pollState(sender, true, false), "legacy PREVIEW sets preview tally");

    sendCommand(fd, TALLY_PREVIEWPROGRAM_LEGACY);
    check(pollState(sender, true, true), "legacy PREVIEWPROGRAM sets both tallies");

    // Well-formed single-= forms keep working (what earlier libomtcpp matched).
    sendCommand(fd, TALLY_PROGRAM);
    check(pollState(sender, false, true), "well-formed PROGRAM sets program tally");

    sendCommand(fd, TALLY_NONE);
    check(pollState(sender, false, false), "well-formed NONE clears tally");

    // Sanity: the two families really differ in exactly the Program== typo.
    check(std::string(TALLY_PROGRAM).find("Program==") == std::string::npos,
          "well-formed PROGRAM has no Program== typo");
    check(std::string(TALLY_PROGRAM_LEGACY).find("Program==") != std::string::npos,
          "legacy PROGRAM carries the Program== typo");

#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
    sender.stop();

    std::printf(g_failures ? "FAILED (%d)\n" : "OK\n", g_failures);
    return g_failures ? 1 : 0;
}
