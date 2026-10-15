#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace rex::net {

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
  // bo1-nx: the guest passes Winsock request codes and a big-endian u_long argument. Winsock's FIONBIO (0x8004667E)
  // only happens to equal the BSD value; Linux uses another code, and libnx's socket layer may not implement it as an
  // ioctl at all. Non-blocking mode is what games use it for, so set it the portable way with fcntl.
  constexpr uint32_t kWinsockFionbio = 0x8004667E;
  if (cmd == kWinsockFionbio && arg) {
    const uint32_t enable = (uint32_t(arg[0]) << 24) | (uint32_t(arg[1]) << 16) | (uint32_t(arg[2]) << 8) | arg[3];
    const int fd = static_cast<int>(handle);
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
      return -1;
    }
    return fcntl(fd, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
  }
  return ioctl(static_cast<int>(handle), cmd, arg);
}

}  // namespace rex::net
