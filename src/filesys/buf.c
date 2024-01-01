#include "lib/string.h"
#include "filesys/filesys.h"
#include "lib/stdio.h"
#include "devices/timer.h"
#include "threads/thread.h"
#include "threads/interrupt.h"
#include "buf.h"
#include "debug.h"
#include "devices/block.h"
#include "threads/malloc.h"
#define BUFCNT 64 
// 使用完的buf放在链表前面，链表后面的buf就是LRU的buf
struct bufCache {
    // 使用关中断（自旋锁）保护bufCache和buf中的字段，使用睡眠锁保护buf中的data
    struct buf bufs[BUFCNT];
    struct list free_list;
};
static struct bufCache *filesysBuf;
static void flush_thread(void *aux UNUSED);
// 会被初始化和销毁文件系统时访问，所以没有加锁
bool continue_flush = false;

void
filesysBufCacheInit(void) {
    filesysBuf = (struct bufCache *)malloc(sizeof(struct bufCache));
    list_init(&filesysBuf->free_list);
    for (int i = 0; i < BUFCNT; i++) {
        list_push_back(&filesysBuf->free_list, &filesysBuf->bufs[i].free_list_elem);
        filesysBuf->bufs[i].sector_idx = INVALID_SECTOR;
        filesysBuf->bufs[i].ref_cnt = 0;
        filesysBuf->bufs[i].valid = false;
        filesysBuf->bufs[i].dirty = false;
        lock_init(&filesysBuf->bufs[i].lock);
    }
    continue_flush = true;
    printf("*******filesysBufCacheInit*********\n");
    // 后台线程负责定时刷盘
    thread_create("flush", PRI_DEFAULT, flush_thread, NULL);
}
struct buf *
find_buf(block_sector_t sector_idx) {
    ASSERT(filesysBuf != NULL);
    ASSERT(intr_get_level() == INTR_OFF);
    struct bufCache *bufCache = filesysBuf;
    for (int i = 0; i < BUFCNT; i++) {
        struct buf *cur_buf = &bufCache->bufs[i];
        if (!cur_buf->valid || cur_buf->sector_idx != sector_idx) continue;
        cur_buf->ref_cnt++;
        lock_acquire(&cur_buf->lock);
        return cur_buf;
    }
    return NULL;
}
// 找给定sector_idx 的buffer,如果没找到就返回最近最少使用的空闲buffer
static struct buf*
bget(struct block* block, block_sector_t sector_idx) {
    ASSERT(block_type(block) == BLOCK_FILESYS);
    ASSERT(sector_idx != 0);
    struct bufCache *bufCache = filesysBuf;
    enum intr_level old_level = intr_disable(); 
    for (int i = 0; i < BUFCNT; i++) {
        struct buf *cur_buf = &bufCache->bufs[i];
        if (!cur_buf->valid || cur_buf->sector_idx != sector_idx) continue;
        cur_buf->ref_cnt++;
        intr_set_level(old_level);
        lock_acquire(&cur_buf->lock);
        return cur_buf;
    }
    for (struct list_elem *e = list_rbegin(&bufCache->free_list); e != list_rend(&bufCache->free_list); e = list_prev(e)) {
        struct buf * cur_buf = list_entry(e, struct buf, free_list_elem);
        // printf("free buf no:%d\n", cur_buf->sector_idx);
        // ASSERT(cur_buf->ref_cnt == 0);
        if (cur_buf->ref_cnt != 0) continue; // 由于被引用的buf还在free_list中，所以free_list中的buf的引用计数有可能不是0
        // pintos文件系统使用的是延时写回，并且没有log，所以对buf使用结束后，buf的内容还有可能没有落盘
        if (cur_buf->dirty) {
            // block_write对外设进行操作，不能关闭中断
            block_write(block, cur_buf->sector_idx, cur_buf->data);
            cur_buf->dirty = false;
        }
        cur_buf->valid = 0;
        cur_buf->ref_cnt = 1;
        cur_buf->sector_idx = sector_idx;
        intr_set_level(old_level);
        lock_acquire(&cur_buf->lock);
        return cur_buf; 
    }
    PANIC("bget:no buffers");
}
struct buf*
bread(struct block* block, block_sector_t sector_idx) {
    ASSERT(sector_idx != 0);
    struct buf* buf = bget(block, sector_idx);
    ASSERT(lock_held_by_current_thread(&buf->lock));
    if (!buf->valid) {
        buf->valid = true;
        block_read(block, sector_idx, buf->data);
        // pre-fetch
        // if (sector_idx + 1 < block_size(block)) {
        //     sector_idx += 1;
        //     struct buf* next_buf = bget(block, sector_idx);
        //     // pre-fetch只有异步进行才有意义
        //     if (!next_buf->valid) {
        //         void *args[3] = {block, (void*)sector_idx, next_buf};
        //         printf("block addrs:%p, sector_idx %d, buf_adds %p\n", block, sector_idx, next_buf);
        //         thread_create("read_ahead", PRI_DEFAULT, read_ahead, args);
        //     } else {
        //         brelease(next_buf);
        //     }
        // }
    }
    return buf;
}
// 将buffer中的chunk_size大小的数据写入sector_idx在内存中的buf中，从sector_ofs开始
// 不刷盘，无日志，没有任何持久性和原子性的保证
void
buf_write(block_sector_t sector_idx, size_t sector_ofs, void *buffer, size_t chunk_size) {
    struct buf *buf = bread(fs_device, sector_idx);
    memcpy((void*)(buf->data + sector_ofs), buffer, chunk_size);
    brelease(buf, true);
}
// buf是不是脏的取决于它和磁盘上的sector的内容是否一样
void 
brelease(struct buf * buf, bool dirty) {
    ASSERT(filesysBuf != NULL);
    ASSERT(lock_held_by_current_thread(&buf->lock));
    lock_release(&buf->lock); // lock用来保护对buf的data修改，既然后面都不会修改data了，那么就可以释放lock了
    enum intr_level old_level = intr_disable();
    ASSERT(buf != NULL && buf->ref_cnt != 0);
    buf->ref_cnt--;
    // 如果是脏的话就设置为脏，否则就保持不变，不能置为clean；要不然会覆盖了之前的脏位
    if (dirty) {
        buf->dirty = true;
    }
    if (buf->ref_cnt == 0) {
        list_remove(&buf->free_list_elem);
        list_push_front(&filesysBuf->free_list, &buf->free_list_elem);
    }
    intr_set_level(old_level);
}
static void 
flush_thread(void *aux UNUSED) {
    while (continue_flush) {
        // static int flush_times = 0;
        // printf("\n flush %d\n", flush_times++);
        timer_sleep(20);
        flush();
    } 
}
void 
flush(void) {
    ASSERT(filesysBuf != NULL);
    struct bufCache *bufCache = filesysBuf;
    struct block *block = block_get_role(BLOCK_FILESYS);
    // static int flush_times = 0;
    // printf("flush %d\n", flush_times++);
    timer_sleep(20);
    enum intr_level old_level = intr_disable();
    for (struct list_elem *e = list_begin(&bufCache->free_list); e != list_end(&bufCache->free_list); e = list_next(e)) {
        struct buf * cur_buf = list_entry(e, struct buf, free_list_elem);
        if (cur_buf->dirty) {
            ASSERT(cur_buf->ref_cnt == 0);
            block_write(block, cur_buf->sector_idx, cur_buf->data);
            cur_buf->dirty = false; 
        }
    }
    intr_set_level(old_level);
}
void 
read_ahead(void *aux) {
    printf("pre-fetc\n");
    void ** args = (void**)aux;
    struct block * block = (struct block*)args[0];
    block_sector_t sector_idx = (block_sector_t)args[1];
    struct buf *buf = (struct buf*)args[2];
    printf("block addrs:%p, sector_idx %d, buf_adds %p\n", block, sector_idx, buf);
    block_read(block, sector_idx, buf->data);
    // buf的内容和磁盘上的sector一致，不是脏的
    brelease(buf, false); 
}