#ifndef USERPROG_PAGEDIR_H
#define USERPROG_PAGEDIR_H

#include <stdbool.h>
#include <stdint.h>
#include <packed.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

uint32_t *pagedir_create (void);
void pagedir_destroy (uint32_t *pd);
// uint32_t *lookup_page (uint32_t *pd, const void *vaddr, bool create);
bool pagedir_set_page (uint32_t *pd, void *upage, void *kpage, bool rw);
void *pagedir_get_page (uint32_t *pd, const void *upage);
void pagedir_clear_page (uint32_t *pd, void *upage);
bool pagedir_is_dirty (uint32_t *pd, const void *upage);
void pagedir_set_dirty (uint32_t *pd, const void *upage, bool dirty);
bool pagedir_is_accessed (uint32_t *pd, const void *upage);
void pagedir_set_accessed (uint32_t *pd, const void *upage, bool accessed);
void pagedir_activate (uint32_t *pd);
void set_pte_to_swap_slot(uint32_t *pd, const void *vaddr, size_t swap_slot);
bool is_data_on_swap_partition(uint32_t *pd, const void *vaddr);
size_t get_pte_swap_slot(uint32_t *pd, const void *vaddr);
bool pagedir_is_writable(uint32_t *pd, const void *vpage);
bool pagedir_is_present (uint32_t *pd, const void *vpage);
void clear_pte_swap_flag(uint32_t *pd, const void *vaddr);

#endif /**< userprog/pagedir.h */
