// yd.net.ompout local port extension: OS socket shim so the vendored libomtcpp and the
// Receive/ TCP transport (both POSIX-only ports) also compile under MSVC/Windows.
// Adapted from the verified omt-testgen shim (same vendor socket surface, MSVC+mingw
// proven 2026-10-16). The vendor tree keeps its call sites; this header is the single
// place that maps the surface. Winsock needs WSAStartup — done by the static init below.
//
// TWO LEVELS on purpose:
//   OMT_OS_WIN_TYPES  — include from HEADERS: winsock types + ssize_t only, no macros.
//   full shim         — include from .cpp files: the errno/close/send/poll mapping.
// Deliberately NOT #pragma once: the same TU includes this header twice on purpose —
// once at TYPES level (from our headers) and once at FULL level (from the .cpp). The
// full section is additionally gated per-TU by OMT_OS_WIN_FULL_DONE so the inline
// overloads cannot be defined twice in one TU.
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #ifdef _MSC_VER
  #include <BaseTsd.h>
  typedef SSIZE_T ssize_t;
  #else
  #include <sys/types.h>   // mingw ships ssize_t
  #endif
  // Pin the CRT errno.h parse HERE (first, before we redefine errno): UCRT's errno.h
  // unconditionally does `#define errno (*_errno())`, so if any later <cerrno> were the
  // TU's first parse it would win over our macro below (C4005 + wrong body). With the
  // guard already set, every later <cerrno> include is a no-op and our mapping sticks.
  #include <cerrno>

  #if !defined(OMT_OS_WIN_TYPES) && !defined(OMT_OS_WIN_FULL_DONE)
  // ---- macro surface: ONLY from .cpp translation units, never from headers ----
  // Winsock takes char* buffers and reports socket failures through WSA, not errno;
  // the vendor only ever reads errno AFTER a socket call, so errno IS the WSA code here.
  #undef errno
  #define errno (WSAGetLastError())
  // Every name below already exists as a UCRT errno.h macro: #undef first (C4005),
  // then map to the WSA code the vendor's post-call errno reads actually see.
  #undef EINTR
  #define EINTR WSAEINTR
  #undef EAGAIN
  #define EAGAIN WSAEWOULDBLOCK
  #undef EWOULDBLOCK
  #define EWOULDBLOCK WSAEWOULDBLOCK
  // A non-blocking connect on Windows reports WSAEWOULDBLOCK, never WSAEINPROGRESS —
  // this is the standard mapping (the transport's `errno == EINPROGRESS` after
  // connect() must match it, or every Windows Connect() falls to CONNECT_FAILED).
  #undef EINPROGRESS
  #define EINPROGRESS WSAEWOULDBLOCK
  #undef ECONNREFUSED
  #define ECONNREFUSED WSAECONNREFUSED
  #undef EBADF
  #define EBADF WSAEBADF
  #undef ECONNRESET
  #define ECONNRESET WSAECONNRESET
  #undef ECONNABORTED
  #define ECONNABORTED WSAECONNABORTED
  #undef EPIPE
  #define EPIPE 9991   // no WSA_EPIPE; unique so vendor's error switch keeps distinct cases
  #undef ETIMEDOUT
  #define ETIMEDOUT WSAETIMEDOUT
  #undef ENOTCONN
  #define ENOTCONN WSAENOTCONN
  #undef ESHUTDOWN
  #define ESHUTDOWN WSAESHUTDOWN
  #undef EINVAL
  #define EINVAL WSAEINVAL
  #undef EMFILE
  #define EMFILE WSAEMFILE
  #undef ENOBUFS
  #define ENOBUFS WSAENOBUFS
  // ENOMEM must stay DISTINCT from ENOBUFS (a shared value would collapse switch cases).
  #undef ENOMEM
  #define ENOMEM 9992   // no WSA_ENOMEM; unique sentinel, same trick as EPIPE
  #undef EHOSTUNREACH
  #define EHOSTUNREACH WSAEHOSTUNREACH
  #undef ENETUNREACH
  #define ENETUNREACH WSAENETUNREACH
  #undef ENETDOWN
  #define ENETDOWN WSAENETDOWN
  #define SHUT_RD SD_RECEIVE
  #define SHUT_WR SD_SEND
  #define SHUT_RDWR SD_BOTH
  // overloads, NOT macros: a `send` macro would also hit Sender::send(const MediaFrame&)
  inline int setsockopt(SOCKET s, int l, int o, const void* v, int n) { return ::setsockopt(s, l, o, (const char*)v, n); }
  inline int getsockopt(SOCKET s, int l, int o, void* v, int* n) { return ::getsockopt(s, l, o, (char*)v, n); }
  inline int send(SOCKET s, const void* b, int n, int f) { return ::send(s, (const char*)b, n, f); }
  inline int recv(SOCKET s, void* b, int n, int f) { return ::recv(s, (char*)b, n, f); }
  inline int omt_closesocket_fd(SOCKET fd) { return ::closesocket((SOCKET)fd); }
  #define close omt_closesocket_fd
  #define MSG_NOSIGNAL 0
  #define MSG_DONTWAIT 0
  #define O_NONBLOCK 1
  #define F_GETFL 1
  #define F_SETFL 2
  inline int fcntl(SOCKET fd, int cmd, int val = 0) {
      if (cmd == F_SETFL) { u_long nb = (val & O_NONBLOCK) ? 1u : 0u;
                            return ioctlsocket(fd, FIONBIO, &nb) == 0 ? 0 : -1; }
      return 0;   // F_GETFL: the vendor never branches on the returned flags
  }
  // the Windows SDK already ships pollfd/POLLIN/POLLOUT (winsock2.h); only poll()
  // itself is missing (winsock calls it WSAPoll), so we provide just the function.
  // select()-based: FD_SETSIZE is 64 on Windows and the except set is folded into
  // POLLERR|POLLHUP (a failed non-blocking connect signals via except only). Every
  // vendor call site polls exactly ONE fd — keep it that way if this is ever reused.
  inline int poll(struct pollfd* fds, unsigned n, int ms) {
      fd_set r, w, e; FD_ZERO(&r); FD_ZERO(&w); FD_ZERO(&e);
      for (unsigned i = 0; i < n; ++i) {
          SOCKET s = fds[i].fd;
          FD_SET(s, &e);
          if (fds[i].events & POLLIN)  FD_SET(s, &r);
          if (fds[i].events & POLLOUT) FD_SET(s, &w);
      }
      timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
      int rc = ::select(0, &r, &w, &e, ms < 0 ? nullptr : &tv);
      for (unsigned i = 0; i < n; ++i) {
          SOCKET s = fds[i].fd;
          short rv = 0;
          if (FD_ISSET(s, &r)) rv |= POLLIN;
          if (FD_ISSET(s, &w)) rv |= POLLOUT;
          if (FD_ISSET(s, &e)) rv |= POLLERR | POLLHUP;
          fds[i].revents = rv;
      }
      return rc;
  }
  namespace {
    struct OmtWsaInit { OmtWsaInit() { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); } };
    const OmtWsaInit g_omt_wsa_init;   // idempotent; refcounted by winsock itself
  }
  #define OMT_OS_WIN_FULL_DONE 1
  #endif
#else
  #include <unistd.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <sys/types.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
#endif
