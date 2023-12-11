#ifndef THREADS_PALLOC_H
#define THREADS_PALLOC_H

#include <stddef.h>
#include "threads/synch.h"
#include "threads/vaddr.h"
/** How to allocate pages. */
enum palloc_flags
  {
    PAL_ASSERT = 001,           /**< Panic on failure. */
    PAL_ZERO = 002,             /**< Zero page contents. */
    PAL_USER = 004              /**< User page. */
  };

struct swapTable {
  struct lock lock;
  struct bitmap *used_map;
};
// 遍历frameTable中的frame不需要加锁，但是读写frame和frame_table的字段需要加锁
struct frame {
  // 被pin住时页面置换算法将该frame跳过，不允许驱除出去
  bool pinned;
  int ref;
  // struct lock lock;
  uint32_t *pagedir;
  uint32_t vaddr;
};
// 一个frame对应user pool中的一个page
struct frameTable {
  struct lock lock;
  struct frame * frames;
  int free_num;
  int length;
  int clock_point;
};
size_t get_kpage_no(void * p);
size_t get_frame_no(struct frame *f);
void free_swap_slot(size_t slot_start_idx, size_t size);
void palloc_init (size_t user_page_limit);
void *palloc_get_page (enum palloc_flags);
void *palloc_get_multiple (enum palloc_flags, size_t page_cnt);
void palloc_free_page (void *);
void palloc_free_multiple (void *, size_t page_cnt);
// static struct frame * get_frame(void *page);
void map_frame_to(void* upage, void* kpage);
void unmap_frame(void* kpage);
size_t fetch_data_from_swap_parition(uint32_t *pd, const void* vaddr, void * buffer);
void swap_table_init(void);
#endif /**< threads/palloc.h */
