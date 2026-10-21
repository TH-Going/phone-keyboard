#include "app.h"

#include <iphlpapi.h>

static bool address_is_usable(unsigned char a, unsigned char b)
{
    if (a == 0 || a == 127) {
        return false;
    }
    if (a == 169 && b == 254) {
        return false;
    }
    return true;
}

/* Ask the routing table which local address would be used for an outside
 * destination. Sending a UDP datagram is not required, so no packet leaves
 * the machine here. */
static bool routed_ipv4(char *out, size_t cap)
{
    SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_handle == INVALID_SOCKET) {
        return false;
    }

    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(53);
    if (inet_pton(AF_INET, "8.8.8.8", &destination.sin_addr) != 1) {
        closesocket(socket_handle);
        return false;
    }

    bool found = false;
    if (connect(socket_handle, (const struct sockaddr *)&destination, sizeof(destination)) == 0) {
        struct sockaddr_in local;
        int local_len = (int)sizeof(local);
        memset(&local, 0, sizeof(local));
        if (getsockname(socket_handle, (struct sockaddr *)&local, &local_len) == 0) {
            const unsigned char *octets = (const unsigned char *)&local.sin_addr.s_addr;
            if (address_is_usable(octets[0], octets[1])) {
                _snprintf_s(out, cap, _TRUNCATE, "%u.%u.%u.%u",
                            octets[0], octets[1], octets[2], octets[3]);
                found = true;
            }
        }
    }

    closesocket(socket_handle);
    return found;
}

static bool is_private(unsigned char a, unsigned char b)
{
    if (a == 10) {
        return true;
    }
    if (a == 172 && b >= 16 && b <= 31) {
        return true;
    }
    if (a == 192 && b == 168) {
        return true;
    }
    return false;
}

/* Fallback: enumerate the adapters and prefer a private IPv4 address. */
static bool adapter_ipv4(char *out, size_t cap)
{
    ULONG buffer_size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *addresses = NULL;

    for (int attempt = 0; attempt < 3; attempt++) {
        addresses = (IP_ADAPTER_ADDRESSES *)malloc(buffer_size);
        if (!addresses) {
            return false;
        }
        ULONG status = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            NULL, addresses, &buffer_size);
        if (status == NO_ERROR) {
            break;
        }
        free(addresses);
        addresses = NULL;
        if (status != ERROR_BUFFER_OVERFLOW) {
            return false;
        }
    }

    if (!addresses) {
        return false;
    }

    bool found = false;
    for (int pass = 0; pass < 2 && !found; pass++) {
        for (IP_ADAPTER_ADDRESSES *adapter = addresses; adapter && !found;
             adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) {
                continue;
            }
            for (IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress;
                 unicast; unicast = unicast->Next) {
                if (!unicast->Address.lpSockaddr ||
                    unicast->Address.lpSockaddr->sa_family != AF_INET) {
                    continue;
                }
                const struct sockaddr_in *entry =
                    (const struct sockaddr_in *)unicast->Address.lpSockaddr;
                const unsigned char *octets = (const unsigned char *)&entry->sin_addr.s_addr;
                if (!address_is_usable(octets[0], octets[1])) {
                    continue;
                }
                if (pass == 0 && !is_private(octets[0], octets[1])) {
                    continue;
                }
                _snprintf_s(out, cap, _TRUNCATE, "%u.%u.%u.%u",
                            octets[0], octets[1], octets[2], octets[3]);
                found = true;
                break;
            }
        }
    }

    free(addresses);
    return found;
}

bool net_local_ipv4(char *out, size_t cap)
{
    if (routed_ipv4(out, cap)) {
        return true;
    }
    if (adapter_ipv4(out, cap)) {
        return true;
    }
    _snprintf_s(out, cap, _TRUNCATE, "127.0.0.1");
    return false;
}
