#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <queue>

#include <rex/math.h>
#include <rex/system/xobject.h>
#include <rex/types.h>
#include <rex/platform.h>

// bo1-nx: the native socket address passed to the host's bind/connect/sendto/recvfrom must match the host's
// struct sockaddr. BSD-derived stacks (macOS, and Horizon's FreeBSD-based socket service through libnx) start
// with uint8_t sa_len/sin_len followed by uint8_t sa_family; Linux and Windows use a uint16_t family. With the
// Linux layout on the Switch, AF_INET (2) landed in sin_len and the family read as 0, so bind() failed and Call
// of Duty: Black Ops quit at start-up ("WARNING: Couldn't allocate IP port").
#if REX_PLATFORM_MAC || REX_PLATFORM_SWITCH
#define REX_XSOCKADDR_BSD_LAYOUT 1
#else
#define REX_XSOCKADDR_BSD_LAYOUT 0
#endif

namespace rex::system {

struct XSOCKADDR {
  rex::be<uint16_t> address_family;
  char sa_data[14];
};

struct N_XSOCKADDR {
  N_XSOCKADDR() {}
  N_XSOCKADDR(const XSOCKADDR* other) { *this = *other; }
  N_XSOCKADDR& operator=(const XSOCKADDR& other) {
#if REX_XSOCKADDR_BSD_LAYOUT
    sa_len = uint8_t(sizeof(*this));
    address_family = uint8_t(uint16_t(other.address_family));
#else
    address_family = other.address_family;
#endif
    std::memcpy(sa_data, other.sa_data, rex::countof(sa_data));
    return *this;
  }

#if REX_XSOCKADDR_BSD_LAYOUT
  uint8_t sa_len;
  uint8_t address_family;
#else
  uint16_t address_family;
#endif
  char sa_data[14];
};

struct XSOCKADDR_IN {
  rex::be<uint16_t> sin_family;

  // Always big-endian!
  rex::be<uint16_t> sin_port;
  rex::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

// Xenia native sockaddr_in
struct N_XSOCKADDR_IN {
  N_XSOCKADDR_IN() {}
  N_XSOCKADDR_IN(const XSOCKADDR_IN* other) { *this = *other; }
  N_XSOCKADDR_IN& operator=(const XSOCKADDR_IN& other) {
#if REX_XSOCKADDR_BSD_LAYOUT
    sin_len = uint8_t(sizeof(*this));
    sin_family = uint8_t(uint16_t(other.sin_family));
#else
    sin_family = other.sin_family;
#endif
    sin_port = other.sin_port;
    sin_addr = other.sin_addr;
    std::memset(x_sin_zero, 0, sizeof(x_sin_zero));

    return *this;
  }

#if REX_XSOCKADDR_BSD_LAYOUT
  uint8_t sin_len;
  uint8_t sin_family;
#else
  uint16_t sin_family;
#endif
  rex::be<uint16_t> sin_port;
  rex::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

class XSocket : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Socket;

  enum AddressFamily {
    X_AF_INET = 2,
  };

  enum Type {
    X_SOCK_STREAM = 1,
    X_SOCK_DGRAM = 2,
  };

  enum Protocol {
    X_IPPROTO_TCP = 6,
    X_IPPROTO_UDP = 17,

    // LIVE Voice and Data Protocol
    // https://blog.csdn.net/baozi3026/article/details/4277227
    // Format: [cbGameData][GameData(encrypted)][VoiceData(unencrypted)]
    X_IPPROTO_VDP = 254,
  };

  XSocket(KernelState* kernel_state);
  ~XSocket();

  uint64_t native_handle() const { return native_handle_; }
  uint16_t bound_port() const { return bound_port_; }

  X_STATUS Initialize(AddressFamily af, Type type, Protocol proto);
  X_STATUS Close();

  X_STATUS SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen);
  X_STATUS IOControl(uint32_t cmd, uint8_t* arg_ptr);

  X_STATUS Connect(N_XSOCKADDR* name, int name_len);
  X_STATUS Bind(N_XSOCKADDR_IN* name, int name_len);
  X_STATUS Listen(int backlog);
  object_ref<XSocket> Accept(N_XSOCKADDR* name, int* name_len);
  int Shutdown(int how);

  int Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags);
  int Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags);

  int RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
               uint32_t* from_len);
  int SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to, uint32_t to_len);

  struct packet {
    // These values are in network byte order.
    rex::be<uint16_t> src_port;
    rex::be<uint32_t> src_ip;

    uint16_t data_len;
    uint8_t data[1];
  };

  // Queue a packet into our internal buffer.
  bool QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len);

 private:
  XSocket(KernelState* kernel_state, uint64_t native_handle);
  uint64_t native_handle_ = -1;

  AddressFamily af_;    // Address family
  Type type_;           // Type (DGRAM/Stream/etc)
  Protocol proto_;      // Protocol (TCP/UDP/etc)
  bool secure_ = true;  // Secure socket (encryption enabled)

  bool bound_ = false;  // Explicitly bound to an IP address?
  uint16_t bound_port_ = 0;

  bool broadcast_socket_ = false;

  std::unique_ptr<rex::thread::Event> event_;
  std::mutex incoming_packet_mutex_;
  std::queue<uint8_t*> incoming_packets_;
};

}  // namespace rex::system

static_assert(sizeof(rex::system::N_XSOCKADDR_IN) == 16, "native sockaddr_in must be 16 bytes");
static_assert(sizeof(rex::system::N_XSOCKADDR) == 16, "native sockaddr must be 16 bytes");
