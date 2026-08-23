/**
 * @file net_virtio.h
 * @brief VirtIO network driver interface (modern virtio 1.0 PCI transport)
 *
 * Polling-only driver: net.c calls virtio_net_send()/virtio_net_poll()
 * exactly like the E1000/RTL8139/PCnet backends, so no interrupt service
 * path or MSI-X setup is required.
 */

#ifndef HBOS_NET_VIRTIO_H
#define HBOS_NET_VIRTIO_H

#include "pci.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Detect and initialize a virtio-net device.
 *
 * Negotiates VIRTIO_F_VERSION_1 + VIRTIO_NET_F_MAC + VIRTIO_NET_F_STATUS,
 * maps the four modern-transport PCI capabilities, configures the two
 * default virtqueues (rx = 0, tx = 1) and pre-fills the receive ring.
 *
 * @param pdev       PCI device found by net.c
 * @param mac        out: 6-byte MAC address (valid when *mac_valid)
 * @param mac_valid  out: true when the device provided a MAC
 * @param link       out: link state (polled drivers force true)
 * @return 0 on success, -1 on any failure (device left reset)
 */
int virtio_net_init_hw(const pci_device_t *pdev, uint8_t mac[6],
                       bool *mac_valid, bool *link);

/** @brief Transmit one Ethernet frame (copied into a driver-owned buffer). */
int virtio_net_send(const void *frame, uint16_t len);

/**
 * @brief Drain the receive used ring, calling cb for each packet.
 * @param cb        packet callback (same signature as net.c packet_cb_t)
 * @param arg       callback context
 * @param spins     maximum poll iterations
 * @param rx_packets/rx_bytes/rx_dropped  out stats updated by this driver
 * @return 0 normally; non-zero when a callback asks to stop; -1 if not ready
 */
int virtio_net_poll(int (*cb)(const uint8_t *pkt, uint16_t len, void *arg),
                    void *arg, uint32_t spins,
                    uint64_t *rx_packets, uint64_t *rx_bytes,
                    uint64_t *rx_dropped);

#endif /* HBOS_NET_VIRTIO_H */
