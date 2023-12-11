#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "devices/block.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include <bitmap.h>
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "threads/loader.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

/** Page allocator.  Hands out memory in page-size (or
   page-multiple) chunks.  See malloc.h for an allocator that
   hands out smaller chunks.

   System memory is divided into two "pools" called the kernel
   and user pools.  The user pool is for user (virtual) memory
   pages, the kernel pool for everything else.  The idea here is
   that the kernel needs to have memory for its own operations
   even if user processes are swapping like mad.

   By default, half of system RAM is given to the kernel pool and
   half to the user pool.  That should be huge overkill for the
   kernel pool, but that's just fine for demonstration purposes. */

/** A memory pool. */
struct pool
  {
    struct lock lock;                   /**< Mutual exclusion. */
    struct bitmap *used_map;            /**< Bitmap of free pages. */
    uint8_t *base;                      /**< Base of pool. */
  };
static struct swapTable swap_table;
/** Two pools: one for kernel data, one for user pages. */
struct pool kernel_pool, user_pool;
static struct frameTable* frame_table;
static size_t init_pool (struct pool *, void *base, size_t page_cnt,
                       const char *name);
static bool page_from_pool (const struct pool *, void *page);
static void * get_data_page(struct frame *frame);
size_t get_frame_no(struct frame* f) {
  return ((uint32_t)f - (uint32_t)frame_table->frames) / sizeof(struct frame);
}
size_t get_kpage_no(void* page) {
  return pg_no(page) - pg_no(user_pool.base);
}
// void swap_table_init(struct swapTable *swap_table);
// 将数据写入交换分区，并且返回写入的sector的idx
static size_t
write_to_swap_parition(void * data_page) {
  ASSERT(data_page != NULL);
  lock_acquire(&swap_table.lock);
  // 一个block以sector为单位读取，要寻找连续的sector向其中写入数据页
  int sector_nums = PGSIZE / BLOCK_SECTOR_SIZE;
  size_t swap_slot_idx = bitmap_scan_and_flip(swap_table.used_map, 0, sector_nums, false);
  if (swap_slot_idx == BITMAP_ERROR) PANIC("swap partition OOM\n");
  lock_release(&swap_table.lock);

  struct block * swap_block = block_get_role(BLOCK_SWAP);
  for (int i = 0; i < sector_nums; i++) {
    block_write(swap_block, swap_slot_idx + i, data_page + i * BLOCK_SECTOR_SIZE);
  }
  return swap_slot_idx;
}
static void 
clear_frame(struct frame* frame) {
  ASSERT(frame != NULL);
  ASSERT(lock_held_by_current_thread(&frame_table->lock));
  frame->pagedir = NULL;
  frame->ref = 0;
  frame->vaddr = 0;
  frame->pinned = false;
}
static void 
evict(struct frame *cur_frame) {
  ASSERT(lock_held_by_current_thread(&frame_table->lock));
  void *data_page = get_data_page(cur_frame);
  ASSERT(!cur_frame->pinned);
  uint32_t *pagedir = cur_frame->pagedir;
  void *vaddr = (void*)cur_frame->vaddr;
  // 如果是脏页，淘汰时需要写回交换分区
  if (pagedir_is_dirty(pagedir, vaddr)) {
    // 将数据写入交换分区
    size_t swap_slot_idx = write_to_swap_parition(data_page);
    set_pte_to_swap_slot(pagedir, vaddr, swap_slot_idx);
    // printf("evict dirty page, vaddr %p, frame_no %d to swap slot %d\n", vaddr, get_frame_no(cur_frame), swap_slot_idx);
  } else {
    // 清除PTE中的present位
    pagedir_clear_page(pagedir, vaddr);
    // printf("evict clean page, vaddr %p, frame_no %d\n", vaddr, get_frame_no(cur_frame));
  }
  clear_frame(cur_frame); 
}
size_t
fetch_data_from_swap_parition(uint32_t *pd, const void* vaddr, void * buffer) {
  size_t swap_slot_idx = get_pte_swap_slot(pd, vaddr); 
  int sector_nums = PGSIZE / BLOCK_SECTOR_SIZE;
  struct block * swap_block = block_get_role(BLOCK_SWAP);
  for (int i = 0; i < sector_nums; i++) {
    block_read(swap_block, swap_slot_idx + i, buffer + i * BLOCK_SECTOR_SIZE);
  }
  // 从交换分区读取内容后需要释放slot
  free_swap_slot(swap_slot_idx, sector_nums);
  return swap_slot_idx;
}
static void *
clock_replacement(int page_cnt) {
  // 替换算法只有在userpool满的时候才会被调用，而userpool分配页面只会分配单个的page（数据页不需要连续）
  // 所以这里的page_cnt只能是1
  ASSERT(page_cnt == 1);
  lock_acquire(&frame_table->lock);
  int pos = frame_table->clock_point;
  struct frame* cur_frame = NULL;
  int circle_cnt = -1;
  while (1) {
    pos++;
    if (pos == frame_table->length) pos = 0;
    // 如果扫描完了一圈，那么就使用不同替换策略；起点是clock_point加一
    if (pos == (frame_table->clock_point + 1) % frame_table->length) circle_cnt = (circle_cnt + 1) % 2;
    cur_frame = frame_table->frames + pos;
    if (cur_frame->pinned) {
      continue;
    }
    ASSERT(cur_frame->ref != 0);
    uint32_t *pd = cur_frame->pagedir;
    uint32_t vaddr = cur_frame->vaddr;
    ASSERT(pd != NULL);
    ASSERT(vaddr != 0);

    ASSERT(pg_ofs((void *)vaddr) == 0);
    if (circle_cnt == 0 && !pagedir_is_accessed(pd, (void *)vaddr) && !pagedir_is_dirty(pd, (void*)vaddr)) {
      break;
    }
    if (circle_cnt == 1) {
      ASSERT(pagedir_is_accessed(pd, (void *)vaddr) || pagedir_is_dirty(pd, (void*)vaddr));
      if (!pagedir_is_accessed(pd, (void *)vaddr) && pagedir_is_dirty(pd, (void *)vaddr)) break;
      pagedir_set_accessed(pd, (void *)vaddr, false);
    }
  }
  frame_table->clock_point = pos;
  frame_table->free_num += 1;
  ASSERT(cur_frame != NULL);
  evict(cur_frame);
  lock_release(&frame_table->lock);
  return get_data_page(cur_frame);
}
void 
free_swap_slot(size_t slot_start_idx, size_t sector_nums) {
  lock_acquire(&swap_table.lock);
  if (free_swap_slot == 760) printf("free_swap_slot:%d\n",slot_start_idx);
  ASSERT(bitmap_all(swap_table.used_map, slot_start_idx, sector_nums));
  bitmap_set_multiple(swap_table.used_map, slot_start_idx, sector_nums, false);
  lock_release(&swap_table.lock);
}
static void 
frame_table_init(uint32_t user_pages) {
  // 将user pool中的所有page作为frame，在kernel pool中为frame table分配空间
  frame_table = (struct frameTable*)palloc_get_page(PAL_ASSERT);
  size_t frame_table_size = user_pages * sizeof(struct frame);
  frame_table_size = ROUND_UP(frame_table_size, PGSIZE);
  frame_table->frames = (struct frame*)palloc_get_multiple(PAL_ASSERT, frame_table_size / PGSIZE);
  frame_table->length = user_pages;
  frame_table->free_num = user_pages;
  frame_table->clock_point = -1;
  lock_init(&frame_table->lock);
  for (int i = 0; i < frame_table->length; i++) {
    struct frame *cur = frame_table->frames + i; 
    cur->ref = 0;
    cur->pagedir = NULL;
    cur->vaddr = 0;
    cur->pinned = false;
  }
}
/** Initializes the page allocator.  At most USER_PAGE_LIMIT
   pages are put into the user pool. */
void
palloc_init (size_t user_page_limit)
{
  /* Free memory starts at 1 MB and runs to the end of RAM. */
  // 从物理内存的1MB开始到物理内存结束的内存是可分配的空闲内存，1MB下面是内核的加载位置
  // 将他们转换成对应的内核虚拟地址（内核虚拟地址从3GB开始，与物理地址从0开始一一对应）
  // 所以物理地址的1MB处，在内核中对应的虚拟地址是3G+1MB
  uint8_t *free_start = ptov (1024 * 1024);
  uint8_t *free_end = ptov (init_ram_pages * PGSIZE);
  size_t free_pages = (free_end - free_start) / PGSIZE;
  size_t user_pages = free_pages / 2;
  size_t kernel_pages;
  if (user_pages > user_page_limit)
    user_pages = user_page_limit;
  kernel_pages = free_pages - user_pages;

  /* Give half of memory to kernel, half to user. */
  init_pool (&kernel_pool, free_start, kernel_pages, "kernel pool");
  size_t user_pool_page_cnt = init_pool (&user_pool, free_start + kernel_pages * PGSIZE,
             user_pages, "user pool");
  frame_table_init(user_pool_page_cnt);
}
// 给定page，找到对应的frame；page一定要是user pool中分配的
static struct frame *
get_frame(void *page) {
  ASSERT(page != NULL);
  ASSERT(pg_ofs(page) == 0);
  ASSERT(page_from_pool(&user_pool, page));
  int frame_no = pg_no(page) - pg_no(user_pool.base);
  return frame_table->frames + frame_no;
}
// 返回给定frame对应的数据page
static void *
get_data_page(struct frame *frame) {
  ASSERT(frame != NULL);
  int page_no = ((uint32_t)frame - (uint32_t)frame_table->frames) / sizeof(struct frame);
  void * page = user_pool.base + page_no * PGSIZE;
  ASSERT(pg_ofs(page) == 0);
  if (get_frame(page) != frame) {
    printf("%p,%p\n",get_frame(page), frame);
  }
  ASSERT(get_frame(page) == frame);
  if (get_frame_no(frame) != get_kpage_no(page)) {
    printf("frame_no:%d;kpage_no:%d\n", get_frame_no(frame), get_kpage_no(page));
  }
  ASSERT(get_frame_no(frame) == get_kpage_no(page));
  return page;
}
void map_frame_to(void* upage, void* kpage) {
  ASSERT(upage != NULL);
  ASSERT(kpage != NULL);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(pg_ofs(kpage) == 0);
  ASSERT(page_from_pool(&user_pool, kpage));

  struct frame *frames = get_frame(kpage);
  lock_acquire(&frame_table->lock);
  // 目前只支持一个frame映射到一个页表
  ASSERT(frames->ref == 0);
  frames->ref++;
  frames->pagedir = thread_current()->pagedir;
  frames->vaddr = (uint32_t)upage;
  // 映射完之后就可以不需要pin住frame了
  frames->pinned = false;
  lock_release(&frame_table->lock);
}
void unmap_frame(void* kpage) {
  ASSERT(kpage != NULL);
  ASSERT(pg_ofs(kpage) == 0);
  ASSERT(page_from_pool(&user_pool, kpage));
  struct frame *frames = get_frame(kpage);
  lock_acquire(&frame_table->lock);
  ASSERT(!frames->pinned);
  // 目前只支持一个frame映射到一个页表
  frames->ref = 0;
  frames->pagedir = NULL;
  frames->vaddr = 0;
  lock_release(&frame_table->lock);
}
/** Obtains and returns a group of PAGE_CNT contiguous free pages.
   If PAL_USER is set, the pages are obtained from the user pool,
   otherwise from the kernel pool.  If PAL_ZERO is set in FLAGS,
   then the pages are filled with zeros.  If too few pages are
   available, returns a null pointer, unless PAL_ASSERT is set in
   FLAGS, in which case the kernel panics. */
void *
palloc_get_multiple (enum palloc_flags flags, size_t page_cnt)
{
  struct pool *pool = flags & PAL_USER ? &user_pool : &kernel_pool;
  void *pages;
  size_t page_idx;

  if (page_cnt == 0)
    return NULL;
  // ASSERT(page_cnt == 1);

  lock_acquire (&pool->lock);
  page_idx = bitmap_scan_and_flip (pool->used_map, 0, page_cnt, false);

  if (page_idx != BITMAP_ERROR)
    pages = pool->base + PGSIZE * page_idx;
  else {
    pages = (pool == &user_pool) ? clock_replacement(page_cnt) : NULL;
  }

  if (pool == &user_pool && pages != NULL) {
    static int user_pages = 0;
    struct frame *frame = get_frame(pages);
    // page的分配和frame的ref的更新应该是原子性的，如果分配了page但是没有更新ref会导致：
    // 替换算法会将page对应的frame驱除出去
    lock_acquire(&frame_table->lock);
    frame->pinned = true;
    lock_release(&frame_table->lock);
  }

  lock_release (&pool->lock);

  if (pages != NULL) 
    {
      if (flags & PAL_ZERO)
        memset (pages, 0, PGSIZE * page_cnt);
      // 更新frame_table中的free_num
      if (pool == &user_pool) {
        lock_acquire(&frame_table->lock);
        frame_table->free_num -= page_cnt;
        lock_release(&frame_table->lock);
      }
    }
  else 
    {
      if (flags & PAL_ASSERT)
        PANIC ("palloc_get: out of pages");
    }

  return pages;
}

/** Obtains a single free page and returns its kernel virtual
   address.
   If PAL_USER is set, the page is obtained from the user pool,
   otherwise from the kernel pool.  If PAL_ZERO is set in FLAGS,
   then the page is filled with zeros.  If no pages are
   available, returns a null pointer, unless PAL_ASSERT is set in
   FLAGS, in which case the kernel panics. */
void *
palloc_get_page (enum palloc_flags flags) 
{
  return palloc_get_multiple (flags, 1);
}

/** Frees the PAGE_CNT pages starting at PAGES. */
void
palloc_free_multiple (void *pages, size_t page_cnt) 
{
  struct pool *pool;
  size_t page_idx;

  ASSERT (pg_ofs (pages) == 0);
  if (pages == NULL || page_cnt == 0)
    return;

  if (page_from_pool (&kernel_pool, pages))
    pool = &kernel_pool;
  else if (page_from_pool (&user_pool, pages))
    pool = &user_pool;
  else
    NOT_REACHED ();

  page_idx = pg_no (pages) - pg_no (pool->base);

#ifndef NDEBUG
  memset (pages, 0xcc, PGSIZE * page_cnt);
#endif

  ASSERT (bitmap_all (pool->used_map, page_idx, page_cnt));
  bitmap_set_multiple (pool->used_map, page_idx, page_cnt, false);

  if (pool == &user_pool) {
    // 更新frame_table中的free_num
    lock_acquire(&frame_table->lock);
    frame_table->free_num += page_cnt;
    lock_release(&frame_table->lock);
    // 将frames释放
    unmap_frame(pages);
  }
}

/** Frees the page at PAGE. */
void
palloc_free_page (void *page) 
{
  palloc_free_multiple (page, 1);
}
void
swap_table_init(void) {
  struct block *swap_block = block_get_role(BLOCK_SWAP);
  ASSERT(swap_block != NULL);
  // bitmap所需要的空间，包括bitmap中的所有bit和bitmap结构体
  size_t bm_pages = DIV_ROUND_UP(bitmap_buf_size(block_size(swap_block)), PGSIZE);
  lock_init(&swap_table.lock);
  // 给bitmap分配空间
  void *bitmap_base = malloc(bm_pages * PGSIZE);
  // 在提前分配好的空间中初始化bitmap结构体和bitmap中的所有bit
  swap_table.used_map = bitmap_create_in_buf(block_size(swap_block), bitmap_base, bm_pages * PGSIZE);
}
/** Initializes pool P as starting at START and ending at END,
   naming it NAME for debugging purposes. */
static size_t 
init_pool (struct pool *p, void *base, size_t page_cnt, const char *name) 
{
  /* We'll put the pool's used_map at its base.
     Calculate the space needed for the bitmap
     and subtract it from the pool's size. */
  size_t bm_pages = DIV_ROUND_UP (bitmap_buf_size (page_cnt), PGSIZE);
  if (bm_pages > page_cnt)
    PANIC ("Not enough memory in %s for bitmap.", name);
  page_cnt -= bm_pages;

  printf ("%zu pages available in %s.\n", page_cnt, name);

  /* Initialize the pool. */
  lock_init (&p->lock);
  p->used_map = bitmap_create_in_buf (page_cnt, base, bm_pages * PGSIZE);
  p->base = base + bm_pages * PGSIZE;
  return page_cnt;
}

/** Returns true if PAGE was allocated from POOL,
   false otherwise. */
static bool
page_from_pool (const struct pool *pool, void *page) 
{
  size_t page_no = pg_no (page);
  size_t start_page = pg_no (pool->base);
  size_t end_page = start_page + bitmap_size (pool->used_map);

  return page_no >= start_page && page_no < end_page;
}
