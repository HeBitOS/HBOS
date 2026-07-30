#ifndef HBOS_MBEDTLS_SHIM_SYS_SOCKET_H
#define HBOS_MBEDTLS_SHIM_SYS_SOCKET_H
/*
 * Mbed TLS only probes this header to decide whether a platform inet_pton()
 * is available.  HBOS deliberately leaves AF_INET6 undefined here so the
 * library uses its freestanding internal IPv4/IPv6 text parser.
 */
#endif
