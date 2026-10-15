/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cerrno>
#include <cstring>
#include <chrono>
#include <atomic>
#include <vector>

#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <fcntl.h>
#endif

namespace rex::system {

#if REX_PLATFORM_SWITCH
// bo1-nx: on Horizon socket data travels through IPC to the system's network service, which cannot read or write the
// guest memory window (it is a mapped alias, not plain process heap): recvfrom into it failed with EFAULT. Socket
// calls go through a per-thread heap buffer instead.
static uint8_t* SocketBounceBuffer(size_t size) {
  thread_local std::vector<uint8_t> buffer;
  if (buffer.size() < size) {
    buffer.resize(size);
  }
  return buffer.data();
}
#endif

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  int ret = rex::net::socket_close(native_handle_);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  int ret = setsockopt(native_handle_, level, optname, (char*)optval_ptr, optlen);
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  int ret = rex::net::socket_ioctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  int ret = bind(native_handle_, (sockaddr*)name, name_len);
#if REX_PLATFORM_SWITCH
  // bo1-nx: Xbox 360 titles bind UDP port 1000 for system link (Call of Duty: Black Ops does at start-up and treats a
  // failure as fatal). Horizon refuses ports below 1024 to homebrew with EACCES, so retry on a high port. The port
  // is only used on this console (no system link), and the guest keeps seeing the port it asked for.
  if (ret < 0 && errno == EACCES) {
    const uint16_t port = name->sin_port.get();
    if (port != 0 && port < 1024) {
      N_XSOCKADDR_IN remapped = *name;
      remapped.sin_port.set(uint16_t(port + 50000));
      ret = bind(native_handle_, (sockaddr*)&remapped, name_len);
      REXLOG_INFO("[net] privileged port {} refused by Horizon; bound {} instead ({})", port, port + 50000,
                  ret == 0 ? "ok" : "failed");
    }
  }
#endif
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = name->sin_port;

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == -1) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
#if REX_PLATFORM_SWITCH
  uint8_t* bounce = SocketBounceBuffer(buf_len);
  int ret = recv(native_handle_, reinterpret_cast<char*>(bounce), buf_len, flags | MSG_DONTWAIT);
  if (ret > 0) {
    std::memcpy(buf, bounce, size_t(ret));
  }
  return ret;
#else
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
#endif
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  sockaddr_in nfrom{};
  socklen_t nfromlen = sizeof(sockaddr_in);
#if REX_PLATFORM_SWITCH
  // bo1-nx: always non-blocking. On the console recvfrom took ~40% of the "Server" thread (it also runs the game's
  // jobs) while the game hung before its main menu: the call blocked although FIONBIO had been applied. There is no
  // network play here, so a receive never needs to wait. Slow calls are logged a few times for diagnosis.
  uint8_t* bounce = SocketBounceBuffer(buf_len);
  const auto t0 = std::chrono::steady_clock::now();
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(bounce), buf_len, flags | MSG_DONTWAIT,
                     (sockaddr*)&nfrom, &nfromlen);
  const int saved_errno = errno;
  const auto took_us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  static std::atomic<int> slow_logged{0};
  if (took_us > 5000 && slow_logged.fetch_add(1) < 8) {
    const int fl = fcntl(int(native_handle_), F_GETFL, 0);
    REXLOG_WARN("[net] recvfrom took {} us (ret {}, errno {}, O_NONBLOCK {})", took_us, ret, saved_errno,
                fl >= 0 && (fl & O_NONBLOCK) ? "set" : "not set");
  }
  if (ret > 0) {
    std::memcpy(buf, bounce, size_t(ret));
  }
  errno = saved_errno;
#else
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nfrom, &nfromlen);
#endif
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);  // BE <- BE
    // bo1-nx: sin_port is a big-endian wrapper that takes a host value; nfrom holds network order.
    from->sin_port = ntohs(nfrom.sin_port);
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
#if REX_PLATFORM_SWITCH
  uint8_t* bounce = SocketBounceBuffer(buf_len);
  std::memcpy(bounce, buf, buf_len);
  return send(native_handle_, reinterpret_cast<const char*>(bounce), buf_len, flags);
#else
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
#endif
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  // bo1-nx: the guest address fields are big-endian wrappers returning host values; the native sockaddr wants network
  // order (they were copied without conversion, i.e. byte-swapped), and BSD stacks also want sin_len.
  sockaddr_in nto{};
  if (to) {
#if REX_XSOCKADDR_BSD_LAYOUT
    nto.sin_len = sizeof(nto);
#endif
    nto.sin_addr.s_addr = htonl(uint32_t(to->sin_addr));
    nto.sin_family = AF_INET;
    nto.sin_port = htons(uint16_t(to->sin_port));
  }
  (void)to_len;
#if REX_PLATFORM_SWITCH
  uint8_t* bounce = SocketBounceBuffer(buf_len);
  std::memcpy(bounce, buf, buf_len);
  return sendto(native_handle_, reinterpret_cast<char*>(bounce), buf_len, flags,
                to ? (sockaddr*)&nto : nullptr, to ? socklen_t(sizeof(nto)) : 0);
#else
  return sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? (sockaddr*)&nto : nullptr, to ? socklen_t(sizeof(nto)) : 0);
#endif
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
