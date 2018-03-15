#include <vitasdk.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include "gpu_utils.h"
#include "allocator.h"

// should be at least TEXTURES_NUM
#define TM_MAX_BLOCKS 2048
#define TM_ALIGNMENT 8

#define TM_TYPE_VRAM 1
#define TM_TYPE_RAM 2

typedef struct tm_block_s {
	struct tm_block_s *next;
	int32_t type;
	uintptr_t base;
	uint32_t offset;
	uint32_t size;
} tm_block_t;

static void *tm_main;
static SceUID tm_main_uid;
static uint32_t tm_main_size;
static int32_t tm_main_type;
static int32_t tm_sub_type;

static uint32_t tm_used;

static tm_block_t tm_blockpool[TM_MAX_BLOCKS];
static int tm_blocknum;

static tm_block_t *tm_alloclist;
static tm_block_t *tm_freelist;

// heap funcs //

static tm_block_t *heap_blk_new(void) {
	for (int i = 0; i < TM_MAX_BLOCKS; ++i) {
		if (tm_blockpool[i].type == -1) {
			tm_blockpool[i].type = 0;
			tm_blocknum++;
			return tm_blockpool + i;
		}
	}
	return NULL;
}

static inline void heap_blk_release(tm_block_t *block) {
	block->type = -1;
	tm_blocknum--;
}

static inline int heap_blk_mergeable(tm_block_t *a, tm_block_t *b) {
	return a->type == b->type
		&& a->base + a->size == b->base
		&& a->offset + a->size == b->offset;
}

static void heap_blk_insert_free(tm_block_t *block) {
	// advance through free list until prev < block < curr
	tm_block_t *curr = tm_freelist;
	tm_block_t *prev = NULL;
	while (curr && curr->base < block->base) {
		prev = curr;
		curr = curr->next;
	}

	// insert into list
	if (prev) {
		prev->next = block;
	} else {
		tm_freelist = block;
	}
	block->next = curr;

	// check merge with current
	if (curr && heap_blk_mergeable(block, curr)) {
		block->size += curr->size;
		block->next = curr->next;
		heap_blk_release(curr);
	}

	// check merge with prev
	if (prev	&& heap_blk_mergeable(prev, block)) {
		prev->size += block->size;
		prev->next = block->next;
		heap_blk_release(block);
	}
}

static tm_block_t *heap_blk_alloc(int32_t type, uint32_t size, uint32_t alignment) {
	// find a suitable block in the free list
	tm_block_t *curr = tm_freelist;
	tm_block_t *prev = NULL;
	while (curr) {
		// check this block can handle alignment and size
		uint32_t const skip = ALIGN(curr->base, alignment) - curr->base;
		if (curr->type == type && skip + size <= curr->size) {
			// allocate any blocks we need now to avoid complicated rollback
			tm_block_t *skipBlock = NULL;
			tm_block_t *unusedBlock = NULL;
			if (skip != 0) {
				skipBlock = heap_blk_new();
				if (!skipBlock) {
					return NULL;
				}
			}
			if (skip + size != curr->size) {
				unusedBlock = heap_blk_new();
				if (!unusedBlock) {
					if (skipBlock) {
						heap_blk_release(skipBlock);
					}
					return NULL;
				}
			}

			// add block for skipped memory
			if (skip != 0) {
				// link in
				if (prev) {
					prev->next = skipBlock;
				} else {
					tm_freelist = skipBlock;
				}
				skipBlock->next = curr;

				// set sizes
				skipBlock->type = curr->type;
				skipBlock->base = curr->base;
				skipBlock->offset = curr->offset;
				skipBlock->size = skip;
				curr->base += skip;
				curr->offset += skip;
				curr->size -= skip;

				// update prev
				prev = skipBlock;
			}

			// add block for unused memory
			if (size != curr->size) {
				// link in
				unusedBlock->next = curr->next;
				curr->next = unusedBlock;

				// set sizes
				unusedBlock->type = curr->type;
				unusedBlock->base = curr->base + size;
				unusedBlock->offset = curr->offset + size;
				unusedBlock->size = curr->size - size;
				curr->size = size;
			}

			// unlink from free list
			if (prev) {
				prev->next = curr->next;
			} else {
				tm_freelist = curr->next;
			}

			// push onto alloc list
			curr->next = tm_alloclist;
			tm_alloclist = curr;
			tm_used += curr->size;
			return curr;
		}

		// advance
		prev = curr;
		curr = curr->next;
	}

	// no block found
	return NULL;
}

static void heap_blk_free(uintptr_t base) {
	// find in the allocate block list
	tm_block_t *curr = tm_alloclist;
	tm_block_t *prev = NULL;
	while (curr && curr->base != base) {
		prev = curr;
		curr = curr->next;
	}

	// early out if not found
	if (!curr) return;

	// unlink from allocated list
	if (prev) {
		prev->next = curr->next;
	} else {
		tm_alloclist = curr->next;
	}
	curr->next = NULL;
	tm_used -= curr->size;

	// add as free block
	heap_blk_insert_free(curr);
}

static void heap_init(void) {
	tm_alloclist = NULL;
	tm_freelist = NULL;
	tm_used = 0;
	for (int i = 0; i < TM_MAX_BLOCKS; ++i)
		tm_blockpool[i].type = -1;
}

static void heap_destroy(void) {
	tm_used = 0;
}

static void heap_extend(int32_t type, void *base, uint32_t size) {
	tm_block_t *block = heap_blk_new();
	block->next = NULL;
	block->type = type;
	block->base = (uintptr_t)base;
	block->offset = 0;
	block->size = size;
	heap_blk_insert_free(block);
}

static void *heap_alloc(int32_t type, uint32_t size, uint32_t alignment) {
	// try to allocate
	tm_block_t *block = heap_blk_alloc(type, size, alignment);

	// early out if failed
	if (!block)
		return NULL;

	return (void *)block->base;
}

static void heap_free(void *addr) {
	heap_blk_free((uintptr_t)addr);
}

// high-level texmem funcs //

uint32_t texmem_init(SceKernelMemBlockType maintype, uint32_t mainsize) {
	tm_main = gpu_alloc_map(maintype, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE, mainsize, &tm_main_uid);
	assert(tm_main);

	tm_main_size = mainsize;
	tm_main_type = (maintype == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) ? TM_TYPE_VRAM : TM_TYPE_RAM;
	tm_sub_type = (maintype == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) ? TM_TYPE_RAM : TM_TYPE_VRAM;

	heap_init();
	heap_extend(tm_main_type, tm_main, tm_main_size);
}

void texmem_destroy(void) {
	heap_destroy();
	if (tm_main) gpu_unmap_free(tm_main_uid);
	tm_main = NULL;
	tm_main_uid = 0;
	tm_main_size = 0;
	tm_main_type = 0;
}

void *texmem_alloc(uint32_t size) {
	if (!size) return NULL;
	if (size < TM_ALIGNMENT) size = TM_ALIGNMENT;

	return heap_alloc(tm_main_type, size, TM_ALIGNMENT);
}

void texmem_free(void *ptr) {
	if (!ptr) return;
	heap_free(ptr);
}

uint32_t texmem_memused(void) {
	return tm_used;
}
