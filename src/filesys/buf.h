#include "debug.h"
#include "devices/block.h"
#include "threads/synch.h"

struct buf {
    struct lock lock;
    block_sector_t sector_idx;
    // 有几个事务正在使用，正在被使用的buf是不能被替换出去的，因为上层的事务并不知情
    int ref_cnt;
    bool valid;
    bool dirty;
    unsigned char data[BLOCK_SECTOR_SIZE];
    struct list_elem free_list_elem;
};
void filesysBufCacheInit(void);
void brelease(struct buf * buf, bool dirty);
struct buf* bread(struct block* block, block_sector_t sector_idx);
void flush(void);
void read_ahead(void *aux);
struct buf * find_buf(block_sector_t sector_idx);
void buf_write(block_sector_t sector_idx, size_t sector_ofs, void *buffer, size_t chunk_size);