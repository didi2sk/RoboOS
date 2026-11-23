#ifndef SD_H
#define SD_H
#include "kernel.h"

#define SD_OK       0
#define SD_TIMEOUT -1
#define SD_ERROR   -2
#define SD_NOCARD  -3

int  sd_init(void);
int  sd_read(u32 lba, void *buf, u32 count);     /* 512-byte sectors */
int  sd_write(u32 lba, const void *buf, u32 count);
int  sd_ready(void);
u64  sd_sectors(void);
int  sd_is_sdhc(void);
int  sd_bus_width(void);
const char *sd_error(int e);
int  sd_init_on(int which);                      /* 0 auto, 1 SDHOST, 2 Arasan */
const char *sd_controller(void);                 /* "SDHOST" / "Arasan (EMMC)" */
int  sd_sdhost_error(void);                      /* why SDHOST was not used (0 = it is) */
/* sdhost.c */
int  sdhost_init(void);
int  sdhost_read(u32 lba, void *buf, u32 count);
int  sdhost_write(u32 lba, const void *buf, u32 count);
u64  sdhost_sectors(void);
int  sdhost_is_sdhc(void);
int  sdhost_bus_width(void);
void sdhost_print_fail(int e);
void sd_print_fail(int e);                       /* error + failed init step + registers */

/* fat.c - FAT16/FAT32 (read with long names, write 8.3) */
#define FAT_MAX_NAME 64

struct fat_file {
    u32 first_cluster;   /* 0 = FAT16 root directory */
    u32 size;
    u8  is_dir;
    /* read position */
    u32 pos, cur_cluster, cur_index;
};

struct fat_dirent {
    char name[FAT_MAX_NAME];
    u32  size;
    u8   is_dir;
};

int  fat_mount(void);
int  fat_mounted(void);
const char *fat_type(void);
u32  fat_cluster_bytes(void);
u64  fat_volume_bytes(void);
int  fat_open(const char *path, struct fat_file *f);              /* 0 = ok */
int  fat_read(struct fat_file *f, void *buf, u32 len);            /* bytes read, <0 error */
/* create or overwrite a file (8.3 name) in an existing directory; 0 = ok */
int  fat_write_file(const char *path, const void *data, u32 len);
extern void (*fat_write_progress)(u32 done, u32 len);    /* set while a long write runs (progress on screen) */
int  fat_mkdir(const char *path);                                /* 8.3 name; 0 = ok */
int  fat_remove(const char *path);                               /* file or empty dir; 0 = ok */
const char *fat_write_error(int e);
int  fat_name_ok(const char *path);                              /* valid 8.3 name for writing? */
/* call cb for every entry of a directory; stops early if cb returns nonzero */
int  fat_list(const char *path, int (*cb)(const struct fat_dirent *e, void *ctx), void *ctx);

#endif
