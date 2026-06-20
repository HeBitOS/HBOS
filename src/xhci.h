#ifndef HBOS_XHCI_H
#define HBOS_XHCI_H

#include <stdint.h>
#include <stdbool.h>

#define XHCI_MAX_SLOTS      64
#define XHCI_MAX_PORTS      32
#define XHCI_MAX_EP          32
#define XHCI_TRB_RING_SIZE   256
#define XHCI_EVENT_RING_SIZE 256
#define XHCI_CMD_RING_SIZE   256

#define XHCI_PCI_CLASS  0x0C
#define XHCI_PCI_SUBCLASS 0x03
#define XHCI_PCI_PROGIF   0x30

#define XHCI_MAX_SCRATCHPAD_BUFFERS 32

typedef struct {
    uint64_t base;
    uint64_t *base_ptr;
    uint32_t pci_bar;
    uint64_t mmio_phys;
    volatile uint32_t *mmio;
    volatile uint32_t *doorbell;
    volatile uint32_t *runtime;
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t max_scratchpad;
    uint32_t page_size;
    uint64_t dcbaa_phys;
    uint64_t *dcbaa;
    uint64_t *scratchpad_array;
    uint64_t *scratchpad_buffers[XHCI_MAX_SCRATCHPAD_BUFFERS];
    uint64_t cmd_ring_phys;
    uint64_t *cmd_ring;
    uint32_t cmd_ring_idx;
    uint32_t cmd_ring_cycle;
    uint64_t event_ring_phys;
    uint64_t *event_ring;
    uint64_t *event_ring_seg;
    uint32_t event_ring_idx;
    uint32_t event_ring_cycle;
    uint64_t *device_context_base[XHCI_MAX_SLOTS + 1];
    uint64_t *input_context[XHCI_MAX_SLOTS + 1];
    uint32_t slot_enabled[XHCI_MAX_SLOTS + 1];
    uint64_t *ep_rings[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint64_t ep_rings_phys[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint32_t ep_ring_idx[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint32_t ep_ring_cycle[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint32_t ep_configured[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint8_t *ep_data_buf[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint64_t ep_data_buf_phys[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint32_t ep_data_buf_len[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    volatile uint32_t ep_has_data[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint32_t initialized;
} xhci_t;

typedef struct {
    uint32_t param0;
    uint32_t param1;
    uint32_t param2;
    uint32_t param3;
} xhci_trb_t;

typedef struct {
    uint32_t type;
    uint32_t speed;
    uint32_t slot_id;
    uint32_t address;
    uint32_t max_packet_size;
    uint32_t port;
    uint32_t ep0_state;
    uint64_t ep0_ring_phys;
    xhci_trb_t *ep0_ring;
    uint32_t ep0_ring_idx;
    uint32_t ep0_ring_cycle;
} xhci_device_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed)) usb_interface_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bReportDescriptorType;
    uint16_t wReportDescriptorLength;
} __attribute__((packed)) usb_hid_desc_t;

int xhci_init(void);
void xhci_poll(void);
int xhci_device_count(void);
int xhci_device_slot(int idx);
int xhci_get_device_desc(int idx, usb_device_desc_t *desc);
int xhci_control_transfer(int slot_id, uint8_t bmRequestType,
                          uint8_t bRequest, uint16_t wValue,
                          uint16_t wIndex, void *data, uint16_t wLength);
int xhci_bulk_transfer(int slot_id, int ep_addr, void *data, uint32_t len);
int xhci_interrupt_transfer(int slot_id, int ep_addr, void *data, uint32_t len, uint32_t interval);

#endif
