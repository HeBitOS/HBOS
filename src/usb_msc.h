#ifndef HBOS_USB_MSC_H
#define HBOS_USB_MSC_H

#include <stdint.h>

#define MSC_MAX_DEVICES 4

#define MSC_CBW_SIGNATURE  0x43425355
#define MSC_CSW_SIGNATURE  0x53425355

#define MSC_CBW_LENGTH  31
#define MSC_CSW_LENGTH  13

typedef struct {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];
} __attribute__((packed)) msc_cbw_t;

typedef struct {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;
} __attribute__((packed)) msc_csw_t;

typedef struct {
    int slot_id;
    int ep_in;
    int ep_out;
    uint32_t tag;
    int active;
    uint32_t block_size;
    uint32_t block_count;
    uint8_t max_lun;
} msc_device_t;

typedef struct {
    uint8_t operation_code;
    uint8_t flags;
    uint32_t lba;
    uint8_t group_number;
    uint16_t transfer_length;
    uint8_t control;
} __attribute__((packed)) scsi_cmd10_t;

typedef struct {
    uint8_t operation_code;
    uint8_t flags;
    uint32_t lba;
    uint8_t group_number;
    uint32_t transfer_length;
    uint8_t _reserved[2];
    uint8_t control;
} __attribute__((packed)) scsi_cmd16_t;

typedef struct {
    uint8_t operation_code;
    uint8_t _reserved;
    uint32_t allocation_length;
    uint8_t _reserved2;
    uint8_t control;
} __attribute__((packed)) scsi_cmd_read_capacity_t;

typedef struct {
    uint32_t lba;
    uint32_t block_size;
} __attribute__((packed)) scsi_read_capacity_data_t;

int msc_init(void);
int msc_device_count(void);
int msc_read_sector(int dev_idx, uint32_t lba, uint8_t *buf, uint32_t count);
int msc_write_sector(int dev_idx, uint32_t lba, const uint8_t *buf, uint32_t count);
uint32_t msc_get_block_count(int dev_idx);
uint32_t msc_get_block_size(int dev_idx);

#endif