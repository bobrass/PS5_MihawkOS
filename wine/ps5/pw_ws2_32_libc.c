/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The resolver calls Winsock's Unix side (ws2_32.prx) makes that a title
 * cannot reach. The SDK's stubs offer getaddrinfo, freeaddrinfo, getnameinfo
 * and gethostbyname only from libScePosixForWebKit, which only the WebKit
 * process gets: in a title those imports stay NULL, and every lookup faulted
 * (measured: GTA IV, tens of thousands of times a minute). gethostbyaddr and
 * h_errno are in no stub at all.
 *
 * These answer numeric addresses, the wildcard and loopback addresses, and
 * "localhost" and the console's own host name, both loopback, themselves. A
 * dotted name goes to the console's own resolver (libSceNet's, as the
 * ProsperoLight title uses it), through the title (pw_wine_resolve), which asks
 * the DNS server the console's network settings name, for an IPv4 address;
 * WINE_PS5_DNS=0 turns that off. A single
 * label never goes out, and is EAI_NONAME: that includes the computer name a
 * prefix made on a PC carries, and resolving that name made GTA IV believe it
 * was online and wait forever on "Starting a new game" (measured), while the
 * failure Wine reports for it (one "Failed to resolve your host name IP" line
 * per lookup) is what lets the game carry on offline. There is no services database: a service is a port number or
 * one of the well-known names pw_port knows. gethostname, inet_pton and inet_ntop are the title's own.
 *
 * The host test builds this file with these names prefixed (Makefile), so
 * glibc's stay in place (tools/host-tests.sh).
 * Imported from prospero-win d1ca1e597648f28e1f1d9c55f7d7ba9b1f0a0690 (its #332 and #335), LGPL-2.1-or-later. */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

/* getnameinfo's buffer sizes: size_t on FreeBSD, socklen_t in glibc. */
#ifdef __GLIBC__
typedef socklen_t pw_name_size;
#else
typedef size_t pw_name_size;
#endif

static _Thread_local int pw_h_errno;

int *__h_errno(void)
{
    return &pw_h_errno;
}

/* One answer: an address family and the address's bytes. */
struct pw_address {
    int family;
    unsigned char bytes[sizeof(struct in6_addr)];
};

/* An addrinfo and everything it points to, freed in one piece. */
struct pw_addrinfo {
    struct addrinfo info;
    struct sockaddr_storage address;
    char name[];
};

static socklen_t pw_sockaddr(struct sockaddr_storage *storage, const struct pw_address *address,
                             unsigned short port)
{
    memset(storage, 0, sizeof(*storage));
    if (address->family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)storage;
#ifdef SIN6_LEN
        in->sin_len = sizeof(*in);
#endif
        in->sin_family = AF_INET;
        in->sin_port = htons(port);
        memcpy(&in->sin_addr, address->bytes, sizeof(in->sin_addr));
        return sizeof(*in);
    }
    struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)storage;
#ifdef SIN6_LEN
    in6->sin6_len = sizeof(*in6);
#endif
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(port);
    memcpy(&in6->sin6_addr, address->bytes, sizeof(in6->sin6_addr));
    return sizeof(*in6);
}

#ifdef __PROSPERO__
/* NAME's IPv4 address, in network order, from the title, which asks the console's resolver (ntdll's
 * pw_wine_resolve; a helper process asks the title over its control socket). This module imports nothing of
 * libSceNet: a helper that loaded it brought the whole title down (battery-r4-01), so only the title does. */
int pw_wine_resolve(const char *name, unsigned char address[4]);

static int pw_resolve(const char *name, unsigned char address[4])
{
    const char *setting = getenv("WINE_PS5_DNS");

    if (setting && !strcmp(setting, "0"))
        return EAI_NONAME;
    return pw_wine_resolve(name, address);
}
#endif

/* "localhost" or the name gethostname reports. */
static int pw_is_local_name(const char *name)
{
    char own[256];

    if (!strcasecmp(name, "localhost"))
        return 1;
    if (gethostname(own, sizeof(own)))
        return 0;
    own[sizeof(own) - 1] = 0;
    return own[0] && !strcasecmp(name, own);
}

/* The addresses NODE stands for in FAMILY, IPv4 first: that is what a
 * title's sockets use. */
static int pw_lookup(const char *node, int family, int flags, struct pw_address addresses[2], int *count)
{
    int wildcard = !node && (flags & AI_PASSIVE);

    memset(addresses, 0, 2 * sizeof(*addresses));
    *count = 1;
    if (node) {
        if (family != AF_INET6 && inet_pton(AF_INET, node, addresses[0].bytes) == 1) {
            addresses[0].family = AF_INET;
            return 0;
        }
        if (family != AF_INET && inet_pton(AF_INET6, node, addresses[0].bytes) == 1) {
            addresses[0].family = AF_INET6;
            return 0;
        }
        if (flags & AI_NUMERICHOST)
            return EAI_NONAME;
        if (!pw_is_local_name(node)) {
#ifdef __PROSPERO__
            int error;
            if (family == AF_INET6 || !strchr(node, '.'))
                return EAI_NONAME;
            if ((error = pw_resolve(node, addresses[0].bytes)))
                return error;
            addresses[0].family = AF_INET;
            return 0;
#else
            return EAI_NONAME;
#endif
        }
    }
    *count = 0;
    if (family != AF_INET6) {
        addresses[*count].family = AF_INET;
        if (!wildcard) {
            addresses[*count].bytes[0] = 127;
            addresses[*count].bytes[3] = 1;
        }
        ++*count;
    }
    if (family != AF_INET) {
        addresses[*count].family = AF_INET6;
        if (!wildcard)
            addresses[*count].bytes[15] = 1;
        ++*count;
    }
    return 0;
}

/* A port number, or one of the well-known service names (IANA): the console has no services database, and Windows
 * programs connect by name (WSAConnectByNameA(..., "http"): WSATYPE_NOT_FOUND on the console, ws2_32 sock.c:3957,
 * 2026-10-25). The names are the same for TCP and UDP here. */
static const struct pw_service { const char *name; unsigned short port; } pw_services[] = {
    { "ftp-data", 20 }, { "ftp", 21 }, { "ssh", 22 }, { "telnet", 23 }, { "smtp", 25 }, { "domain", 53 },
    { "tftp", 69 }, { "http", 80 }, { "www", 80 }, { "www-http", 80 }, { "pop3", 110 }, { "nntp", 119 },
    { "ntp", 123 }, { "imap", 143 }, { "snmp", 161 }, { "ldap", 389 }, { "https", 443 }, { "submission", 587 },
    { "ldaps", 636 }, { "imaps", 993 }, { "pop3s", 995 }, { "socks", 1080 }, { "http-alt", 8080 },
};

static int pw_port(const char *service, int flags, unsigned short *port)
{
    char *end;
    unsigned long value;
    size_t i;

    *port = 0;
    if (!service)
        return 0;
    if (service[0] >= '0' && service[0] <= '9') {
        value = strtoul(service, &end, 10);
        if (!*end && value <= 65535) {
            *port = (unsigned short)value;
            return 0;
        }
    }
    if (flags & AI_NUMERICSERV)
        return EAI_NONAME;
    for (i = 0; i < sizeof(pw_services) / sizeof(pw_services[0]); i++)
        if (!strcasecmp(service, pw_services[i].name)) {
            *port = pw_services[i].port;
            return 0;
        }
    return EAI_SERVICE;
}

void freeaddrinfo(struct addrinfo *info)
{
    while (info) {
        struct addrinfo *next = info->ai_next;
        free(info);
        info = next;
    }
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result)
{
    static const struct addrinfo no_hints;
    struct pw_kind { int socktype, protocol; } kinds[2];
    struct pw_address addresses[2];
    struct addrinfo *head = NULL, **tail = &head;
    unsigned short port;
    int count, kind_count = 0, error;

    *result = NULL;
    if (!hints)
        hints = &no_hints;
    if (!node && !service)
        return EAI_NONAME;
    if (hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET && hints->ai_family != AF_INET6)
        return EAI_FAMILY;
    if (!node && (hints->ai_flags & AI_CANONNAME))
        return EAI_BADFLAGS;
    /* One answer per address and socket type: TCP and UDP when neither the
     * type nor the protocol is given. */
    if (hints->ai_socktype == SOCK_STREAM || hints->ai_socktype == SOCK_DGRAM ||
        hints->ai_socktype == SOCK_RAW) {
        int protocol = hints->ai_protocol ? hints->ai_protocol :
                       hints->ai_socktype == SOCK_STREAM ? IPPROTO_TCP :
                       hints->ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : 0;
        kinds[kind_count++] = (struct pw_kind){ hints->ai_socktype, protocol };
    } else if (hints->ai_socktype) {
        return EAI_SOCKTYPE;
    } else {
        if (!hints->ai_protocol || hints->ai_protocol == IPPROTO_TCP)
            kinds[kind_count++] = (struct pw_kind){ SOCK_STREAM, IPPROTO_TCP };
        if (!hints->ai_protocol || hints->ai_protocol == IPPROTO_UDP)
            kinds[kind_count++] = (struct pw_kind){ SOCK_DGRAM, IPPROTO_UDP };
        if (!kind_count)
            kinds[kind_count++] = (struct pw_kind){ SOCK_RAW, hints->ai_protocol };
    }
    if ((error = pw_port(service, hints->ai_flags, &port)))
        return error;
    if ((error = pw_lookup(node, hints->ai_family, hints->ai_flags, addresses, &count)))
        return error;

    for (int a = 0; a < count; a++) {
        for (int k = 0; k < kind_count; k++) {
            /* The canonical name, the name as given, goes on the first. */
            size_t name_size = head || !(hints->ai_flags & AI_CANONNAME) ? 0 : strlen(node) + 1;
            struct pw_addrinfo *entry = calloc(1, sizeof(*entry) + name_size);

            if (!entry) {
                freeaddrinfo(head);
                return EAI_MEMORY;
            }
            entry->info.ai_flags = hints->ai_flags;
            entry->info.ai_family = addresses[a].family;
            entry->info.ai_socktype = kinds[k].socktype;
            entry->info.ai_protocol = kinds[k].protocol;
            entry->info.ai_addrlen = pw_sockaddr(&entry->address, &addresses[a], port);
            entry->info.ai_addr = (struct sockaddr *)&entry->address;
            if (name_size) {
                memcpy(entry->name, node, name_size);
                entry->info.ai_canonname = entry->name;
            }
            *tail = &entry->info;
            tail = &entry->info.ai_next;
        }
    }
    *result = head;
    return 0;
}

int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, pw_name_size host_size,
                char *service, pw_name_size service_size, int flags)
{
    const void *bytes;
    unsigned short port;
    int loopback;

    if (!address)
        return EAI_FAMILY;
    if (address->sa_family == AF_INET && length >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)address;
        bytes = &in->sin_addr;
        port = ntohs(in->sin_port);
        loopback = ((const unsigned char *)bytes)[0] == 127;
    } else if (address->sa_family == AF_INET6 && length >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)address;
        static const unsigned char one[sizeof(struct in6_addr)] = {[15] = 1};
        bytes = &in6->sin6_addr;
        port = ntohs(in6->sin6_port);
        loopback = !memcmp(bytes, one, sizeof(one));
    } else {
        return EAI_FAMILY;
    }
    if ((!host || !host_size) && (!service || !service_size))
        return EAI_NONAME;
    if (host && host_size) {
        /* The only name an address has here is loopback's. */
        if (!(flags & NI_NUMERICHOST) && loopback) {
            if (strlen("localhost") >= host_size)
                return EAI_OVERFLOW;
            strcpy(host, "localhost");
        } else if (flags & NI_NAMEREQD) {
            return EAI_NONAME;
        } else if (!inet_ntop(address->sa_family, bytes, host, host_size)) {
            return EAI_OVERFLOW;
        }
    }
    if (service && service_size && (size_t)snprintf(service, service_size, "%u", port) >= service_size)
        return EAI_OVERFLOW;
    return 0;
}

/* The old resolver's single answer, in per-thread storage as FreeBSD's is. */
static struct hostent *pw_hostent(const char *name, int family, const void *address, int length)
{
    static _Thread_local struct hostent host;
    static _Thread_local char host_name[NI_MAXHOST];
    static _Thread_local char addresses[1][sizeof(struct in6_addr)];
    static _Thread_local char *address_list[2];
    static _Thread_local char *aliases[1];

    snprintf(host_name, sizeof(host_name), "%s", name);
    memcpy(addresses[0], address, length);
    address_list[0] = addresses[0];
    address_list[1] = NULL;
    aliases[0] = NULL;
    host.h_name = host_name;
    host.h_aliases = aliases;
    host.h_addrtype = family;
    host.h_length = length;
    host.h_addr_list = address_list;
    return &host;
}

struct hostent *gethostbyname(const char *name)
{
    struct addrinfo hints = {0}, *info;
    struct hostent *host;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    if (!name || getaddrinfo(name, NULL, &hints, &info)) {
        pw_h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    host = pw_hostent(info->ai_canonname, AF_INET, &((struct sockaddr_in *)info->ai_addr)->sin_addr,
                      sizeof(struct in_addr));
    freeaddrinfo(info);
    return host;
}

/* A reverse lookup through getnameinfo. */
struct hostent *gethostbyaddr(const void *address, socklen_t length, int family)
{
    struct sockaddr_storage storage;
    struct pw_address answer = { family, {0} };
    char name[NI_MAXHOST];
    socklen_t size;

    if ((family != AF_INET || length != sizeof(struct in_addr)) &&
        (family != AF_INET6 || length != sizeof(struct in6_addr))) {
        pw_h_errno = NO_RECOVERY;
        return NULL;
    }
    memcpy(answer.bytes, address, length);
    /* The address is filled first: an argument list's order is not defined (clang 23 warns). */
    size = pw_sockaddr(&storage, &answer, 0);
    if (getnameinfo((struct sockaddr *)&storage, size, name, sizeof(name), NULL, 0, NI_NAMEREQD)) {
        pw_h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    return pw_hostent(name, family, address, (int)length);
}
