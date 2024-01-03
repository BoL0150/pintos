#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include "threads/synch.h"
#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "lib/kernel/list.h"
#define NDIRECT 10
#define NINDIRECT 1
#define NININDIRECT 1
#define NADDRS (NDIRECT + NINDIRECT + NININDIRECT)
#define DIRECT_SECTOR_NUM (NDIRECT)
#define INDIRECT_SECTOR_NUM ((NINDIRECT * BLOCK_SECTOR_SIZE) / sizeof(uint32_t))
#define ININDIRECT_SECTOR_NUM ((NININDIRECT * INDIRECT_SECTOR_NUM * BLOCK_SECTOR_SIZE) / sizeof(uint32_t))
struct bitmap;

/** On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    bool is_dir;                        /**是否是目录 */
    uint32_t nlink;                     /**< inode有几个硬链接. */
    off_t length;                       /**< File size in bytes. */
    unsigned magic;                     /**< Magic number. */
    block_sector_t addrs[NADDRS];             /** addrs*/
    uint32_t unused[124 - NADDRS];      /**< Not used. */
  };
/** In-memory inode. */
struct inode 
  {
    uint32_t nlink;                     /** the nlink in disk inode*/
    bool valid;                         /**Inode中是否有数据*/
    struct list_elem elem;              /**< Element in inode list. */
    block_sector_t sector;              /**< Sector number of disk location. */
    int open_cnt;                       /**< Number of openers. */
    bool removed;                       /**< True if deleted, false otherwise. */
    int deny_write_cnt;                 /**< 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /**< Inode content. */
    struct lock lock;
  };
void inode_init (void);
bool inode_create (block_sector_t, off_t, bool lazy, bool is_dir);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_unlink (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);
bool is_inode_dir(struct inode *inode);
bool is_inode_removed(struct inode *inode);

#endif /**< filesys/inode.h */
