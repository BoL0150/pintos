#include "threads/interrupt.h"
#include "filesys/free-map.h"
#include "lib/kernel/bitmap.h"
#include "buf.h"
#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/** Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

static void free_inode_disk_data(struct inode *inode);
static void free_data_block(block_sector_t *addrs, size_t length, size_t depth);

/** Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}
// 分配sector的时候要把磁盘块变成0
static void 
sector_zero(block_sector_t sector) {
  struct buf * buf = bread(fs_device, sector); 
  memset((void*)buf->data, 0, BLOCK_SECTOR_SIZE);
  brelease(buf, true);
}
static void
sector_allocate(block_sector_t *sector) {
  free_map_allocate(1, sector);
  sector_zero(*sector);
}
static
block_sector_t allocata_sector_if_needed(bool alloc, block_sector_t *sector) {
  if (alloc && *sector == 0) {
    sector_allocate(sector);
  }
  return *sector;
}
/** Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos, bool alloc) 
{
  ASSERT (inode != NULL);
  ASSERT (lock_held_by_current_thread(&inode->lock));
  // #ifndef filesys 
  // if (pos < inode->data.length)
  //   return inode->data.start + pos / BLOCK_SECTOR_SIZE;
  // else
  //   return -1;
  // #else

  // 允许文件增长后，写的位置有可能大于length，
  // if (pos >= inode->data.length) return -1;

  // pos是inode的第几块数据块（从1开始）;0~511字节属于第一块，512~1023字节属于第二块
  block_sector_t sector_offset = bytes_to_sectors(pos + 1);  
  ASSERT(sector_offset != 0);

  if (sector_offset <= DIRECT_SECTOR_NUM) {
    return allocata_sector_if_needed(alloc, &inode->data.addrs[sector_offset - 1]);
  } 

  sector_offset -= DIRECT_SECTOR_NUM;

  if (sector_offset <= INDIRECT_SECTOR_NUM) {
    // 目前只有一个间接块，所以读取间接块是写死的
    block_sector_t indirect_block_sector = allocata_sector_if_needed(alloc, &inode->data.addrs[NDIRECT]);
    if (indirect_block_sector == 0) return 0;

    struct buf *indirect_block = bread(fs_device, indirect_block_sector);
    block_sector_t result = allocata_sector_if_needed(alloc, &((block_sector_t*)indirect_block->data)[sector_offset - 1]);
    // 分配了那就是脏
    brelease(indirect_block, alloc);
    return result;
  } 

  sector_offset -= INDIRECT_SECTOR_NUM;
  block_sector_t doubly_indirect_block_sector = allocata_sector_if_needed(alloc, &inode->data.addrs[NDIRECT + 1]);
  if (doubly_indirect_block_sector == 0) return 0;
  
  struct buf *doubly_indirect_block = bread(fs_device, doubly_indirect_block_sector);
  block_sector_t indirect_block_sector = allocata_sector_if_needed(alloc, &((block_sector_t*)doubly_indirect_block->data)[(sector_offset - 1)/ INDIRECT_SECTOR_NUM]);
  brelease(doubly_indirect_block, alloc);

  if (indirect_block_sector == 0) return 0;
  struct buf *indirect_block = bread(fs_device, indirect_block_sector);
  block_sector_t result = allocata_sector_if_needed(alloc, &((block_sector_t*)indirect_block->data)[(sector_offset - 1) % INDIRECT_SECTOR_NUM]);
  brelease(indirect_block, alloc);
  return result;
}

/** List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/** Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}
static bool 
inode_lock(struct inode *inode) {
  if (lock_held_by_current_thread(&inode->lock)) {
    // printf("**************inode already locked by thread:%d!****************\n", inode->lock.holder->tid);
    return false;
  }
  ASSERT(inode != NULL && inode->open_cnt >= 1);
  ASSERT(inode->sector != INVALID_SECTOR);
  lock_acquire(&inode->lock);
  if (inode->valid == 0) {
    struct buf *buf = bread(fs_device, inode->sector);
    memcpy((void*)&inode->data, buf->data, BLOCK_SECTOR_SIZE);
    brelease(buf, false);   
    inode->valid = 1;
    inode->nlink = inode->data.nlink;
  }
  return true;
}
static void 
inode_unlock(struct inode *inode) {
  ASSERT(inode != NULL && lock_held_by_current_thread(&inode->lock) && inode->open_cnt >= 1);
  lock_release(&inode->lock);
}

// 每次更新完inode都要调用一次update，把inode的内容更新到buf中
static void
inode_update(struct inode *inode) {
  ASSERT(inode != NULL && lock_held_by_current_thread(&inode->lock) && inode->valid && inode->open_cnt >= 1);
  inode->data.nlink = inode->nlink;
  buf_write(inode->sector, 0, (void*)(&inode->data), BLOCK_SECTOR_SIZE);
}
/** Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
// 在sector位置处创建一个inode，并且将inode的数据长度初始化为length；
// Inode的位置在调用此函数之前已经被分配好了，此函数只需要为数据块分配空间即可
bool
inode_create (block_sector_t sector, off_t length, bool lazy, bool is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      // sector的个数
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->nlink = 0;
      disk_inode->is_dir = is_dir;
      for (int i = 0; i < NDIRECT + NINDIRECT + NININDIRECT; i++) {
        ASSERT(disk_inode->addrs[i] == 0);
      }
      // 在创建free_map的inode的时候不能使用lazy，因为如果使用lazy，以后向free_map的disk_inode写入会导致调用free_map_allocate分配bit，
      // 而free_map_allocate更新了内存的free_map之后又需要更新disk_inode
      if (!lazy) {
        ASSERT(sectors <= NDIRECT);
        for (int i = 0; i < sectors; i++) {
          sector_allocate(&disk_inode->addrs[i]);
        }
      }
      success = true;
      // #ifndef filesys
      // if (free_map_allocate (sectors, &disk_inode->start)) 
      //   {
      //     block_write (fs_device, sector, disk_inode);
      //     if (sectors > 0) 
      //       {
      //         static char zeros[BLOCK_SECTOR_SIZE];
      //         size_t i;
      //         
      //         for (i = 0; i < sectors; i++) 
      //           block_write (fs_device, disk_inode->start + i, zeros);
      //       }
      //     success = true; 
      //   } 
      // #else
      // 类似懒分配，创建inode的时候不给数据块分配磁盘空间，只记录长度；当读取的时候返回0，写入的时候再分配数据块
      // 将修改保存
      buf_write(sector, 0, (void*)disk_inode, BLOCK_SECTOR_SIZE);
      free (disk_inode);
    }
  return success;
}

/** Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  enum intr_level old_level = intr_disable();
  /* Check whether this inode is already open. */
  // inode的cache，同步不同线程对inode的访问
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          intr_set_level(old_level);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL) {
    intr_set_level(old_level);
    return NULL;
  }

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  inode->valid = false;
  inode->nlink = 0;
  inode_lock_init(inode);
  // inode open时不需要读取数据，只有当真正需要时（加锁时）才读取
  // #ifndef FILESYS 
  // block_read (fs_device, inode->sector, &inode->data);
  // #else
  // struct buf *buf = bread(fs_device, inode->sector);
  // memcpy((void *)&inode->data, buf->data, BLOCK_SECTOR_SIZE);
  // brelease(buf);
  // #endif
  intr_set_level(old_level);
  return inode;
}

/** Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  // ASSERT(intr_get_level() == INTR_OFF);
  enum intr_level old_level = intr_disable();
  if (inode != NULL)
    inode->open_cnt++;
  intr_set_level(old_level);
  return inode;
}

/** Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  enum intr_level old_level = intr_disable();
  block_sector_t inumber = inode->sector;
  intr_set_level(old_level);
  return inumber;
}

/** Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;
  enum intr_level old_level = intr_disable();
  /* Release resources if this was the last opener. */
  if (inode->open_cnt == 1) {
    list_remove(&inode->elem);
    // 只有valid才有可能修改过nlink，才有必要释放磁盘的数据
    if (inode->valid && inode->nlink == 0) {
      bool locked = inode_lock(inode);
      free_inode_disk_data(inode); 
      // 由于inode被从icache中驱逐了，并且free_inode_disk_data修改了inode，所以这里要把更新写回上一层缓存
      inode_update(inode);
      if (locked) inode_unlock(inode);
    }
    free(inode);
  } else {
    inode->open_cnt--;
  }
  intr_set_level(old_level);
}
// 释放sector的时候除了需要把bitmap中置位（同时写回磁盘），不需要把内存中的sector置为INVALID，因为用户非要访问，得到垃圾内容不能怪我们
// 以后分配sector的时候，需要把分配的sector读入内存，将其置为0；否则会读入脏数据
static void
free_sector(block_sector_t sector_idx, size_t cnt) {
  ASSERT(cnt == 1);
  free_map_release(sector_idx, 1);
}
static
void free_inode_disk_data(struct inode *inode) {
  ASSERT(inode->valid);
  ASSERT(lock_held_by_current_thread(&inode->lock));

  free_sector(inode->sector, 1);
  block_sector_t sectors = bytes_to_sectors(inode->data.length);
  block_sector_t* addrs = inode->data.addrs;
  // 直接暴力把所有的间接块遍历一次，判断是否分配
  free_data_block(addrs, NDIRECT, 0);
  if (sectors > DIRECT_SECTOR_NUM) free_data_block(addrs + NDIRECT, NINDIRECT, 1);
  if (sectors > DIRECT_SECTOR_NUM + INDIRECT_SECTOR_NUM) free_data_block(addrs + NDIRECT + NINDIRECT, NININDIRECT, 2);
  // inode_update(inode); 这里没必要更新inode，因为inode还在icache中
  // free_data_block(addrs + NDIRECT, NINDIRECT, 0);
  // free_data_block(addrs + NDIRECT + NINDIRECT, NININDIRECT, 0);
}
static 
void free_data_block(block_sector_t *addrs, size_t length, size_t depth) {
  for (int i = 0; i < length; i++) {
    if (addrs[i] == 0) continue;
    // 如果有间接块，那么需要先释放间接块，再释放当前的块
    if (depth > 0) {
      struct buf *buf = bread(fs_device, addrs[i]);
      free_data_block((block_sector_t*)buf->data, BLOCK_SECTOR_SIZE / sizeof(block_sector_t), depth - 1);
      brelease(buf, true);
    }
    free_sector(addrs[i], 1);
    addrs[i] = 0;
  }
}
/** Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_unlink (struct inode *inode) 
{
  enum intr_level old_level = intr_disable();
  ASSERT (inode != NULL);
  bool locked = inode_lock(inode);
  ASSERT(inode->nlink >= 1);
  inode->nlink--;
  inode->data.nlink = inode->nlink;
  inode_unlock(locked);
  intr_set_level(old_level);
}

/** Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;
  bool locked = inode_lock(inode);
  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      // read时未分配的不需要分配，直接返回0即可
      block_sector_t sector_idx = byte_to_sector (inode, offset, false);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      #ifndef FILESYS
      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      #else
      struct buf* buf;
      if(sector_idx != 0) {
        buf = bread(fs_device, sector_idx);
        memcpy(buffer + bytes_read, buf->data + sector_ofs, chunk_size);
        // pintos与xv6不一样，从buffer cache读取到另一个buffer中，所以memcpy之后就不会直接访问buffer cache了，可以直接释放
        brelease(buf, false);
      } else {
        memset((void*)(buffer + bytes_read), 0, chunk_size);
      }
      #endif 
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  if (bounce != NULL) free (bounce);
  if (locked) inode_unlock(inode);
  return bytes_read;
}

/** Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  bool locked = inode_lock(inode);

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      // write时需要将未分配的block分配
      block_sector_t sector_idx = byte_to_sector (inode, offset, true);
      // 这里没必要更新inode，因为inode还在icache中
      // inode_update(inode); // byte_to_sector中有可能在addr中分配了内容，所以inode要更新
      ASSERT(sector_idx != 0);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      #ifndef FILESYS 
      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else 
        {
          /* We need a bounce buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }
      #else
      buf_write(sector_idx, sector_ofs, (void*)buffer + bytes_written, chunk_size);
      #endif 

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  if (bounce != NULL) free (bounce);
  // 文件增长
  if (offset > inode->data.length) 
    inode->data.length = offset;
  if (locked) inode_unlock(inode);
  return bytes_written;
}

/** Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  enum intr_level old_level = intr_disable();
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  intr_set_level(old_level);
}

/** Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  enum intr_level old_level = intr_disable();
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  intr_set_level(old_level);
}

/** Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  bool locked = inode_lock(inode);
  off_t res = inode->data.length;
  if(locked) inode_unlock(inode);
  return res;
}

bool
is_inode_dir(struct inode *inode) {
  bool locked = inode_lock(inode);
  bool res = inode->data.is_dir;
  if (locked) inode_unlock(inode);
  return res;
}
