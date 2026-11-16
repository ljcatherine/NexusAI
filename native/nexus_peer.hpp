// Peer filter shared by the control API (:8082) and the media listener (:8083).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace ps5lm {
// Host-order IPv4 peer address. Loopback 127/8 and Tailscale CGNAT 100.64.0.0/10 are always
// admitted; 10/8, 172.16/12 and 192.168/16 only when allow_lan. Everything else is refused.
inline bool peer_allowed_ipv4(uint32_t host_order, bool allow_lan) {
    if ((host_order >> 24) == 127) return true;
    if ((host_order & 0xffc00000u) == 0x64400000u) return true;
    if (!allow_lan) return false;
    return (host_order >> 24) == 10 || (host_order & 0xfff00000u) == 0xac100000u ||
        (host_order & 0xffff0000u) == 0xc0a80000u;
}
} // namespace ps5lm
