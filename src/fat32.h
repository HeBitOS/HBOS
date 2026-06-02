#ifndef HBOS_FAT32_H
#define HBOS_FAT32_H

#include <stdint.h>

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0F

#define FAT32_EOC 0x0FFFFFF8
#define FAT32_BAD 0x0FFFFFF7

typedef struct {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
    uint8_t  boot_code[420];
    uint16_t signature;
} __attribute__((packed)) fat32_bpb_t;

typedef struct {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  creation_tenth;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t access_date;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed)) fat32_dir_entry_t;

typedef struct {
    fat32_bpb_t bpb;
    uint32_t partition_lba;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t cluster_size;
    uint32_t total_clusters;
    int mounted;
} fat32_fs_t;

int fat32_mount(uint32_t partition_lba, fat32_fs_t *fs);
int fat32_read_cluster(fat32_fs_t *fs, uint32_t cluster, uint8_t *buf);
int fat32_next_cluster(fat32_fs_t *fs, uint32_t cluster);
int fat32_read_file(fat32_fs_t *fs, uint32_t first_cluster, uint32_t file_size,
                    uint32_t offset, uint8_t *buf, uint32_t count);
int fat32_readdir(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t index,
                  char *name, uint32_t *type, uint32_t *size);
int fat32_lookup(fat32_fs_t *fs, uint32_t dir_cluster, const char *name,
                 uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attr);
int fat32_write_file(fat32_fs_t *fs, uint32_t first_cluster, uint32_t *file_size,
                     uint32_t offset, const uint8_t *buf, uint32_t count);
int fat32_create_file(fat32_fs_t *fs, uint32_t dir_cluster, const char *name,
                      uint32_t *out_cluster);
int fat32_delete_file(fat32_fs_t *fs, uint32_t dir_cluster, const char *name);

#endif