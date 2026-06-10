// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <common/assert.h>
#include "core/libraries/kernel/kernel.h"
#include "net.h"
#include "net_error.h"
#include "sockets.h"

namespace Libraries::Net {

// Folds the P2P virtual port into the real port when sin_port is left empty,
// so peers stay addressable through a plain UDP/TCP host socket.
static OrbisNetSockaddrIn FoldVport(const OrbisNetSockaddr* addr) {
    OrbisNetSockaddrIn real = *reinterpret_cast<const OrbisNetSockaddrIn*>(addr);
    if (real.sin_port == 0) {
        real.sin_port = real.sin_vport;
    }
    return real;
}

int P2PSocket::Bind(const OrbisNetSockaddr* addr, u32 addrlen) {
    if (addr == nullptr) {
        *Libraries::Kernel::__Error() = ORBIS_NET_EINVAL;
        return -1;
    }
    const OrbisNetSockaddrIn real = FoldVport(addr);
    vport = reinterpret_cast<const OrbisNetSockaddrIn*>(addr)->sin_vport;
    LOG_INFO(Lib_Net, "binding P2P socket, port = {}, vport = {}", ntohs(real.sin_port),
             ntohs(vport));
    return PosixSocket::Bind(reinterpret_cast<const OrbisNetSockaddr*>(&real), addrlen);
}

int P2PSocket::Connect(const OrbisNetSockaddr* addr, u32 namelen) {
    if (addr == nullptr) {
        *Libraries::Kernel::__Error() = ORBIS_NET_EINVAL;
        return -1;
    }
    const OrbisNetSockaddrIn real = FoldVport(addr);
    return PosixSocket::Connect(reinterpret_cast<const OrbisNetSockaddr*>(&real), namelen);
}

int P2PSocket::SendPacket(const void* msg, u32 len, int flags, const OrbisNetSockaddr* to,
                          u32 tolen) {
    if (to == nullptr) {
        return PosixSocket::SendPacket(msg, len, flags, to, tolen);
    }
    const OrbisNetSockaddrIn real = FoldVport(to);
    return PosixSocket::SendPacket(msg, len, flags,
                                   reinterpret_cast<const OrbisNetSockaddr*>(&real), tolen);
}

int P2PSocket::GetSocketAddress(OrbisNetSockaddr* name, u32* namelen) {
    const int ret = PosixSocket::GetSocketAddress(name, namelen);
    if (ret >= 0 && name != nullptr) {
        reinterpret_cast<OrbisNetSockaddrIn*>(name)->sin_vport = vport;
    }
    return ret;
}

int P2PSocket::SetSocketOptions(int level, int optname, const void* optval, u32 optlen) {
    const int ret = PosixSocket::SetSocketOptions(level, optname, optval, optlen);
    if (ret < 0) {
        // P2P-specific options (crypto, signatures, ...) have no host equivalent.
        LOG_WARNING(Lib_Net, "ignoring unsupported option, level = {:#x}, optname = {:#x}", level,
                    optname);
        return 0;
    }
    return ret;
}

int P2PSocket::GetSocketOptions(int level, int optname, void* optval, u32* optlen) {
    const int ret = PosixSocket::GetSocketOptions(level, optname, optval, optlen);
    if (ret < 0) {
        LOG_WARNING(Lib_Net, "ignoring unsupported option, level = {:#x}, optname = {:#x}", level,
                    optname);
        if (optval != nullptr && optlen != nullptr) {
            std::memset(optval, 0, *optlen);
        }
        return 0;
    }
    return ret;
}

} // namespace Libraries::Net
