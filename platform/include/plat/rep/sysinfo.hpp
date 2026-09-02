#pragma once
// plat/rep/sysinfo.hpp — local network identity for the access report's
// "测试终端本机IP地址" / "测试终端本机MAC地址" fields: every up, non-loopback
// adapter with an IPv4 address, most-likely-real one first (Windows already
// orders GetAdaptersAddresses by route metric).

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32")
#pragma comment(lib, "iphlpapi")
#endif

#include <cstdio>
#include <string>
#include <vector>

namespace plat::rep {

struct NetInfo {
  std::string name;  // adapter friendly name, for the log line
  std::string ip;
  std::string mac;
};

#ifdef _WIN32

inline std::vector<NetInfo> local_net_info() {
  std::vector<NetInfo> out;
  ULONG buf_len = 15000;
  std::vector<unsigned char> buf(buf_len);
  auto* addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
  const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &buf_len);
  if (ret == ERROR_BUFFER_OVERFLOW) {
    buf.resize(buf_len);
    addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &buf_len);
  }
  if (ret != NO_ERROR) return out;

  for (auto* a = addrs; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
      const auto* sa = ua->Address.lpSockaddr;
      if (!sa || sa->sa_family != AF_INET) continue;
      char ip[46] = {};
      const auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
      if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;

      NetInfo n;
      n.ip = ip;
      if (a->FriendlyName) {
        int wlen = 0;
        while (a->FriendlyName[wlen]) ++wlen;
        const int nb = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, wlen, nullptr, 0,
                                           nullptr, nullptr);
        n.name.resize(static_cast<std::size_t>(nb));
        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, wlen, n.name.data(), nb, nullptr,
                             nullptr);
      }
      if (a->PhysicalAddressLength == 6) {
        char mac[18];
        std::snprintf(mac, sizeof(mac), "%02X-%02X-%02X-%02X-%02X-%02X", a->PhysicalAddress[0],
                      a->PhysicalAddress[1], a->PhysicalAddress[2], a->PhysicalAddress[3],
                      a->PhysicalAddress[4], a->PhysicalAddress[5]);
        n.mac = mac;
      }
      out.push_back(std::move(n));
    }
  }
  return out;
}

#else

inline std::vector<NetInfo> local_net_info() { return {}; }

#endif

}  // namespace plat::rep
