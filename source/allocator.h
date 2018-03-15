#pragma once

#ifndef _VGL_ALLOCATOR_H
#define _VGL_ALLOCATOR_H

#include <vitasdk.h>
#include <stdint.h>

uint32_t texmem_init(SceKernelMemBlockType maintype, uint32_t mainsize);
void texmem_destroy(void);
void *texmem_alloc(uint32_t size);
void texmem_free(void *ptr);
uint32_t texmem_memused(void);

#endif
