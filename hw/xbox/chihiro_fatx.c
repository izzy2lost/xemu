/*
 * Chihiro in-memory FATX filesystem builder
 *
 * Builds a FATX partition image in RAM from a host game directory.
 * The user points dvd_path at the game folder (or XBE file);
 * xemu detects it's a directory, builds FATX, serves via IDE hooks.
 *
 * FATX format (Xbox):
 *   Sector 0-7: Superblock (4096 bytes, magic 'FATX')
 *   After superblock: FAT16 table
 *   After FAT: Data clusters (cluster_size = 32 sectors = 16KB)
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 * LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <dirent.h>
#include <sys/stat.h>

/* FATX constants */
#define FATX_MAGIC          0x58544146  /* 'XTAF' LE */
#define FATX_SECTOR_SIZE    512
#define FATX_CLUSTER_SECTS  32          /* 32 sectors = 16KB per cluster */
#define FATX_CLUSTER_SIZE   (FATX_CLUSTER_SECTS * FATX_SECTOR_SIZE)  /* 16384 */
#define FATX_SUPERBLOCK_SIZE 4096       /* 8 sectors */
#define FATX_DIRENT_SIZE    64
#define FATX_DIRENTS_PER_CLUSTER (FATX_CLUSTER_SIZE / FATX_DIRENT_SIZE)
#define FATX_FAT_END        0xFFFF
#define FATX_FAT_FREE       0x0000
#define FATX_MAX_FILES      512
#define FATX_MAX_NAME       42

/* File entry for building */
typedef struct {
    char name[FATX_MAX_NAME + 1];
    char host_path[1024];
    uint32_t size;
    uint32_t first_cluster;
    int is_dir;
    int parent_idx;  /* -1 = root */
} FATXFileEntry;

/* Builder state */
uint32_t fatx_diag_lba = 0; /* LBA of XBE section 11 critical sector (extern) */
uint32_t fatx_diag_lba_sec0 = 0; /* LBA of XBE section 0 critical sector (VA 0x135000) */
static uint8_t *fatx_image = NULL;
static uint32_t fatx_image_size = 0;
static FATXFileEntry fatx_files[FATX_MAX_FILES];
static int fatx_file_count = 0;
static uint32_t fatx_next_cluster = 1;
static uint16_t *fatx_fat = NULL;
static uint32_t fatx_total_clusters = 0;
static uint32_t fatx_fat_offset = 0;    /* byte offset of FAT in image */
static uint32_t fatx_data_offset = 0;   /* byte offset of cluster 0 in image */

/* Allocate a chain of clusters, return first cluster number */
static uint32_t fatx_alloc_chain(uint32_t size)
{
    uint32_t num = (size + FATX_CLUSTER_SIZE - 1) / FATX_CLUSTER_SIZE;
    if (num == 0) num = 1;

    uint32_t first = fatx_next_cluster;
    for (uint32_t i = 0; i < num; i++) {
        uint32_t c = fatx_next_cluster++;
        if (c < fatx_total_clusters) {
            fatx_fat[c] = (i < num - 1) ? (c + 1) : FATX_FAT_END;
        }
    }
    return first;
}

/* Get byte offset in image for a given cluster (1-indexed: cluster 1 = first data cluster) */
static uint32_t fatx_cluster_offset(uint32_t cluster)
{
    return fatx_data_offset + (cluster - 1) * FATX_CLUSTER_SIZE;
}

/* Write a directory entry into the image */
static void fatx_write_dirent(uint32_t offset, const char *name,
                               uint32_t first_cluster, uint32_t file_size,
                               int is_dir)
{
    if (offset + FATX_DIRENT_SIZE > fatx_image_size) return;

    uint8_t *e = fatx_image + offset;
    memset(e, 0xFF, FATX_DIRENT_SIZE);

    int namelen = strlen(name);
    if (namelen > FATX_MAX_NAME) namelen = FATX_MAX_NAME;

    e[0] = (uint8_t)namelen;
    e[1] = is_dir ? 0x10 : 0x20;
    memcpy(e + 2, name, namelen);

    /* first cluster (LE32) */
    e[44] = first_cluster & 0xFF;
    e[45] = (first_cluster >> 8) & 0xFF;
    e[46] = (first_cluster >> 16) & 0xFF;
    e[47] = (first_cluster >> 24) & 0xFF;

    /* file size (LE32) — 0 for directories */
    uint32_t sz = is_dir ? 0 : file_size;
    e[48] = sz & 0xFF;
    e[49] = (sz >> 8) & 0xFF;
    e[50] = (sz >> 16) & 0xFF;
    e[51] = (sz >> 24) & 0xFF;

    /* timestamps: 2002-12-01 12:00 */
    e[52] = 0x00; e[53] = 0x60;  /* time */
    e[54] = 0x81; e[55] = 0x2D;  /* date */
    e[56] = 0x00; e[57] = 0x60;
    e[58] = 0x81; e[59] = 0x2D;
    e[60] = 0x00; e[61] = 0x60;
    e[62] = 0x81; e[63] = 0x2D;
}

/* Scan host directory recursively, add files to fatx_files[] */
static int fatx_scan_dir(const char *host_dir, int parent_idx)
{
    DIR *d = opendir(host_dir);
    if (!d) return -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;  /* skip . .. .hidden */
        if (fatx_file_count >= FATX_MAX_FILES) break;

        FATXFileEntry *fe = &fatx_files[fatx_file_count];
        memset(fe, 0, sizeof(*fe));

        strncpy(fe->name, ent->d_name, FATX_MAX_NAME);
        snprintf(fe->host_path, sizeof(fe->host_path), "%s/%s", host_dir, ent->d_name);
        fe->parent_idx = parent_idx;

        struct stat st;
        if (stat(fe->host_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            fe->is_dir = 1;
            fe->size = 0;
            int my_idx = fatx_file_count++;
            fatx_scan_dir(fe->host_path, my_idx);
        } else if (S_ISREG(st.st_mode)) {
            fe->is_dir = 0;
            fe->size = (uint32_t)st.st_size;
            fatx_file_count++;
        }
    }
    closedir(d);
    return 0;
}

/*
 * Build FATX image in memory from a host directory.
 * Returns pointer to image data and sets *out_size.
 * Caller must g_free() the returned pointer.
 */
uint8_t *chihiro_fatx_build(const char *game_dir, uint32_t *out_size,
                            uint32_t partition_sectors)
{
    fatx_file_count = 0;
    fatx_next_cluster = 1;

    /* Phase 1: Scan directory */
    printf("[FATX] Scanning: %s\n", game_dir);
    if (fatx_scan_dir(game_dir, -1) < 0) {
        printf("[FATX] ERROR: cannot open directory '%s'\n", game_dir);
        return NULL;
    }
    printf("[FATX] Found %d files/dirs\n", fatx_file_count);

    /* Phase 2: Calculate layout to match kernel expectations.
     * The kernel calculates FAT size from the FULL partition, not file data.
     * We must match its layout exactly or the root directory won't be found. */
    uint64_t partition_bytes = (uint64_t)partition_sectors * FATX_SECTOR_SIZE;
    uint32_t kernel_total_clusters =
        (uint32_t)(partition_bytes / FATX_CLUSTER_SIZE) + 1;

    fatx_total_clusters = kernel_total_clusters;
    uint32_t fat_bytes = fatx_total_clusters * 2;  /* FAT16 */
    uint32_t fat_aligned = (fat_bytes + FATX_SUPERBLOCK_SIZE - 1)
                           & ~(FATX_SUPERBLOCK_SIZE - 1);

    fatx_fat_offset = FATX_SUPERBLOCK_SIZE;
    fatx_data_offset = FATX_SUPERBLOCK_SIZE + fat_aligned;

    /* Image only needs space for actual file data, not the whole partition */
    uint64_t file_data = 0;
    for (int i = 0; i < fatx_file_count; i++) {
        if (!fatx_files[i].is_dir) {
            file_data += fatx_files[i].size;
        }
        file_data += FATX_CLUSTER_SIZE;
    }
    file_data += FATX_CLUSTER_SIZE * 16;
    uint32_t needed_clusters =
        (uint32_t)(file_data / FATX_CLUSTER_SIZE) + 256;
    fatx_image_size = fatx_data_offset + needed_clusters * FATX_CLUSTER_SIZE;

    printf("[FATX] Clusters: %u, FAT: %u bytes, Image: %u bytes (%.1f MB)\n",
           fatx_total_clusters, fat_bytes, fatx_image_size,
           fatx_image_size / (1024.0 * 1024.0));

    /* Allocate image */
    fatx_image = (uint8_t *)g_malloc0(fatx_image_size);
    fatx_fat = (uint16_t *)(fatx_image + fatx_fat_offset);

    /* Phase 3: Write superblock */
    uint8_t *sb = fatx_image;
    sb[0] = 0x46; sb[1] = 0x41; sb[2] = 0x54; sb[3] = 0x58; /* FATX */
    sb[4] = 0x78; sb[5] = 0x56; sb[6] = 0x34; sb[7] = 0x12; /* volume ID */
    sb[8] = FATX_CLUSTER_SECTS; sb[9] = 0; sb[10] = 0; sb[11] = 0;
    sb[12] = 1; sb[13] = 0; sb[14] = 0; sb[15] = 0; /* 1 FAT copy */
    memset(sb + 0x30, 0xFF, FATX_SUPERBLOCK_SIZE - 0x30);

    /* Reserve cluster 0 */
    fatx_fat[0] = 0xFFF8;

    /* Phase 4: Allocate clusters for all files */
    for (int i = 0; i < fatx_file_count; i++) {
        FATXFileEntry *fe = &fatx_files[i];
        if (fe->is_dir) {
            /* Directory: allocate 1 cluster for entries (will fill later) */
            fe->first_cluster = fatx_alloc_chain(FATX_CLUSTER_SIZE);
        } else {
            /* File: allocate chain, read data */
            fe->first_cluster = fatx_alloc_chain(fe->size > 0 ? fe->size : 1);

            /* Read file data into cluster chain */
            FILE *f = fopen(fe->host_path, "rb");
            if (f) {
                uint32_t remaining = fe->size;
                uint32_t cluster = fe->first_cluster;
                while (remaining > 0 && cluster < fatx_total_clusters &&
                       fatx_fat[cluster] != FATX_FAT_FREE) {
                    uint32_t off = fatx_cluster_offset(cluster);
                    uint32_t chunk = remaining > FATX_CLUSTER_SIZE ?
                                     FATX_CLUSTER_SIZE : remaining;
                    if (off + chunk <= fatx_image_size) {
                        fread(fatx_image + off, 1, chunk, f);
                    }
                    remaining -= chunk;
                    if (fatx_fat[cluster] == FATX_FAT_END) break;
                    cluster = fatx_fat[cluster];
                }
                fclose(f);
            } else {
                printf("[FATX] WARNING: cannot read '%s'\n", fe->host_path);
            }
        }
    }

    /* DIAG: verify XBE critical bytes in FATX image after fread */
    for (int i = 0; i < fatx_file_count; i++) {
        FATXFileEntry *fe = &fatx_files[i];
        if (fe->is_dir || fe->size < 0x1BE080) continue;
        const char *dot = strrchr(fe->name, '.');
        if (!dot || strcasecmp(dot, ".xbe") != 0) continue;
        /* Found XBE — check file offsets 0x1BE029 and 0x1BE07E */
        uint32_t cluster = fe->first_cluster + 111; /* sequential alloc */
        uint32_t img_off = fatx_cluster_offset(cluster) + 0x2000;
        if (img_off + 0x80 <= fatx_image_size) {
            uint8_t b29 = fatx_image[img_off + 0x29];
            uint8_t b7e = fatx_image[img_off + 0x7E];
            /* Read original file for comparison */
            FILE *vf = fopen(fe->host_path, "rb");
            uint8_t orig29 = 0, orig7e = 0;
            if (vf) {
                fseek(vf, 0x1BE029, SEEK_SET); fread(&orig29, 1, 1, vf);
                fseek(vf, 0x1BE07E, SEEK_SET); fread(&orig7e, 1, 1, vf);
                fclose(vf);
            }
            fatx_diag_lba = (img_off) / FATX_SECTOR_SIZE;
            printf("[FATX] VERIFY '%s': img[0x1BE029]=%02X(file=%02X) "
                   "img[0x1BE07E]=%02X(file=%02X) diag_lba=%u %s\n",
                   fe->name, b29, orig29, b7e, orig7e, fatx_diag_lba,
                   (b29 == orig29 && b7e == orig7e) ? "OK" : "MISMATCH!");
        }
        /* Also verify section 0 data for VA 0x135000 (file offset 0x125000).
         * 0x125000 / 0x4000 = cluster 73, remainder 0x1000 */
        {
            uint32_t cl0 = fe->first_cluster + (0x125000 / FATX_CLUSTER_SIZE);
            uint32_t cl0_off = fatx_cluster_offset(cl0)
                             + (0x125000 % FATX_CLUSTER_SIZE);
            if (cl0_off + 32 <= fatx_image_size) {
                uint8_t s0[4], f0[4] = {0};
                memcpy(s0, fatx_image + cl0_off, 4);
                FILE *vf2 = fopen(fe->host_path, "rb");
                if (vf2) {
                    fseek(vf2, 0x125000, SEEK_SET);
                    fread(f0, 1, 4, vf2);
                    fclose(vf2);
                }
                fatx_diag_lba_sec0 = cl0_off / FATX_SECTOR_SIZE;
                printf("[FATX] VERIFY-SEC0: img[0x125000]="
                       "%02X%02X%02X%02X(file=%02X%02X%02X%02X) "
                       "diag_lba_sec0=%u %s\n",
                       s0[0],s0[1],s0[2],s0[3],
                       f0[0],f0[1],f0[2],f0[3],
                       fatx_diag_lba_sec0,
                       (memcmp(s0, f0, 4) == 0) ? "OK" : "MISMATCH!");
            }
        }
        break;
    }

    /* Phase 5: Build directory entries */
    /* Root directory: all files with parent_idx == -1 */
    uint32_t root_cluster = fatx_alloc_chain(FATX_CLUSTER_SIZE);
    uint32_t root_off = fatx_cluster_offset(root_cluster);
    memset(fatx_image + root_off, 0xFF, FATX_CLUSTER_SIZE);
    int root_entry = 0;

    for (int i = 0; i < fatx_file_count; i++) {
        if (fatx_files[i].parent_idx != -1) continue;
        fatx_write_dirent(root_off + root_entry * FATX_DIRENT_SIZE,
                          fatx_files[i].name, fatx_files[i].first_cluster,
                          fatx_files[i].size, fatx_files[i].is_dir);
        root_entry++;
    }

    /* Subdirectory entries */
    for (int i = 0; i < fatx_file_count; i++) {
        if (!fatx_files[i].is_dir) continue;
        uint32_t dir_off = fatx_cluster_offset(fatx_files[i].first_cluster);
        memset(fatx_image + dir_off, 0xFF, FATX_CLUSTER_SIZE);
        int entry = 0;

        for (int j = 0; j < fatx_file_count; j++) {
            if (fatx_files[j].parent_idx != i) continue;
            fatx_write_dirent(dir_off + entry * FATX_DIRENT_SIZE,
                              fatx_files[j].name, fatx_files[j].first_cluster,
                              fatx_files[j].size, fatx_files[j].is_dir);
            entry++;
        }
    }

    /* Update superblock: root cluster pointer is cluster 1 in FATX
     * Actually, Xbox FATX root directory starts at cluster 1 (first data cluster).
     * We allocated root at root_cluster. Need to make it cluster 1.
     * Swap root cluster data to cluster 1 position. */
    if (root_cluster != 1) {
        /* Swap cluster 1 and root_cluster data */
        uint32_t off1 = fatx_cluster_offset(1);
        uint32_t offR = fatx_cluster_offset(root_cluster);
        uint8_t tmp[FATX_CLUSTER_SIZE];
        memcpy(tmp, fatx_image + off1, FATX_CLUSTER_SIZE);
        memcpy(fatx_image + off1, fatx_image + offR, FATX_CLUSTER_SIZE);
        memcpy(fatx_image + offR, tmp, FATX_CLUSTER_SIZE);

        /* Update FAT: swap entries */
        uint16_t fat1 = fatx_fat[1];
        fatx_fat[1] = fatx_fat[root_cluster];
        fatx_fat[root_cluster] = fat1;

        /* Update file entries that pointed to cluster 1 */
        for (int i = 0; i < fatx_file_count; i++) {
            if (fatx_files[i].first_cluster == 1)
                fatx_files[i].first_cluster = root_cluster;
            else if (fatx_files[i].first_cluster == root_cluster)
                fatx_files[i].first_cluster = 1;
        }

        /* Update directory entries that reference swapped clusters */
        /* Re-write root directory at cluster 1 */
        uint32_t new_root_off = fatx_cluster_offset(1);
        memset(fatx_image + new_root_off, 0xFF, FATX_CLUSTER_SIZE);
        root_entry = 0;
        for (int i = 0; i < fatx_file_count; i++) {
            if (fatx_files[i].parent_idx != -1) continue;
            fatx_write_dirent(new_root_off + root_entry * FATX_DIRENT_SIZE,
                              fatx_files[i].name, fatx_files[i].first_cluster,
                              fatx_files[i].size, fatx_files[i].is_dir);
            root_entry++;
        }

        /* Re-write subdirectory entries */
        for (int i = 0; i < fatx_file_count; i++) {
            if (!fatx_files[i].is_dir) continue;
            uint32_t dir_off = fatx_cluster_offset(fatx_files[i].first_cluster);
            memset(fatx_image + dir_off, 0xFF, FATX_CLUSTER_SIZE);
            int entry = 0;
            for (int j = 0; j < fatx_file_count; j++) {
                if (fatx_files[j].parent_idx != i) continue;
                fatx_write_dirent(dir_off + entry * FATX_DIRENT_SIZE,
                                  fatx_files[j].name, fatx_files[j].first_cluster,
                                  fatx_files[j].size, fatx_files[j].is_dir);
                entry++;
            }
        }
    }

    printf("[FATX] Built: %d files, root at cluster 1, image %u bytes\n",
           fatx_file_count, fatx_image_size);
    printf("[FATX] Layout: superblock=0x0, FAT=0x%X (%u bytes), data=0x%X\n",
           fatx_fat_offset, fat_aligned, fatx_data_offset);

    /* Dump root directory entries for debugging */
    {
        uint32_t root_off = fatx_cluster_offset(1);
        printf("[FATX] Root dir at offset 0x%X (sector %u):\n",
               root_off, root_off / FATX_SECTOR_SIZE);
        for (int i = 0; i < 8; i++) {
            uint8_t *e = fatx_image + root_off + i * FATX_DIRENT_SIZE;
            if (e[0] == 0xFF || e[0] == 0x00) break;
            uint8_t namelen = e[0];
            uint8_t attr = e[1];
            uint32_t fc = e[44] | (e[45]<<8) | (e[46]<<16) | (e[47]<<24);
            uint32_t sz = e[48] | (e[49]<<8) | (e[50]<<16) | (e[51]<<24);
            printf("[FATX]   [%d] namelen=%u attr=0x%02X cluster=%u size=%u name='%.42s'\n",
                   i, namelen, attr, fc, sz, (char*)(e+2));
        }
    }

    *out_size = fatx_image_size;
    return fatx_image;
}

/*
 * Read a sector from the in-memory FATX image.
 * Returns true if sector was served.
 */
bool chihiro_fatx_read_sector(uint32_t lba, void *buffer)
{
    if (!fatx_image) return false;

    uint32_t offset = lba * FATX_SECTOR_SIZE;
    if (offset + FATX_SECTOR_SIZE <= fatx_image_size) {
        memcpy(buffer, fatx_image + offset, FATX_SECTOR_SIZE);
        if (fatx_diag_lba && lba == fatx_diag_lba) {
            uint8_t *b = (uint8_t *)buffer;
            printf("[FATX] READ-DIAG lba=%u: [0x29]=%02X [0x7E]=%02X\n",
                   lba, b[0x29], b[0x7E]);
        }
        if (fatx_diag_lba_sec0 && lba == fatx_diag_lba_sec0) {
            uint8_t *b = (uint8_t *)buffer;
            printf("[FATX] READ-SEC0 lba=%u: %02X %02X %02X %02X %02X\n",
                   lba, b[0], b[1], b[2], b[3], b[4]);
        }
        return true;
    }

    /* Beyond image: return zeros */
    memset(buffer, 0, FATX_SECTOR_SIZE);
    return true;
}
