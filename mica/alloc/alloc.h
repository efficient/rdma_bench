// Copyright 2014 Carnegie Mellon University
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
 * Memory allocation using segregated fit (similar to Lea).
 * Taken from MICA (NSDI 14) code.
 */

#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct mica_alloc_item {
  uint64_t item_size; /* XXX: isn't this breaking 8-byte alignment? */
  uint8_t data[0];
};

/*
 * 32 classes for freelists.
 * The 1st 31 classes are used in 8-byte increments; the last freelist is used
 * for everything else.
 *
 * 0. Chunks with >= (32 + 8) bytes.
 * 1. Chunks with >= (32 + 16) bytes.
 * ...
 * 30. Chunks with >= (32 + 248) bytes.
 * 31. Chunks with >= (32 + 256) bytes.
 */
#define MICA_ALLOC_NUM_CLASSES (32)
#define MICA_ALLOC_CLASS_INCREMENT (8) /* 8-byte increment for classes */

#define MICA_ALLOC_INSUFFICIENT_SPACE ((uint64_t)-1)

#define MICA_ALLOC_FREE (0UL)
#define MICA_ALLOC_OCCUPIED (1UL)

#define MICA_ALLOC_TAG_SIZE(vec) ((vec) & ((1UL << 63UL) - 1UL))
#define MICA_ALLOC_TAG_STATUS(vec) ((vec) >> 63UL)
#define MICA_ALLOC_TAG_VEC(size, status) ((size) | (status) << 63UL)

typedef int bool;
#define true(1)
#define false(0)

#define MICA_ROUNDUP8(x) (((x) + 7UL) & (~7UL))
#define MICA_ROUNDUP2M(x) (((x) + 2097151UL) & (~2097151UL))

/*
 * Data structure layout.
 * ======================
 *
 * free_head[class] -> the first free chunk of the class (NULL if none exists)
 *
 * free chunk (of size N) - N is the same or larger than the size of the class
 * ======================
 * 8-byte: status (1 bit), size (63 bit)
 * 8-byte: prev free chunk of the same class (NULL if head)
 * 8-byte: next free chunk of the same class (NULL if tail)
 * (N - 32 bytes)
 * 8-byte: status (1 bit), size (63 bit)
 *
 *
 * occupied chunk (of size N) - overhead of 16 bytes
 * ==========================
 * 8-byte: status (1 bit), size (63 bit)
 * (N - 16 bytes)
 * 8-byte: status (1 bit), size (63 bit)
 *
 *
 * Notes:
 * 1. Only free chunks have prev and next pointer fields. The space overhead
 * per item is 16B, not 32B. XXX: Is this really true?
 * 2. The size of a chunk is it's allocator size, i.e., inclusive of allocator
 * headers such as pointers and status tags.
 */

/* Per-item space overhead for occupied chunks. */
#define MICA_ALLOC_OVERHEAD (16)

/*
 * The minimum chunk size is 32 bytes because free chunks require 4 8-byte
 * metadata fields.
 */
#define MICA_ALLOC_MIN_SIZE (32UL)
#define MICA_ALLOC_MAX_SIZE ((1UL << 40) - 1)

struct mica_alloc {
  uint32_t lock;
  uint64_t size; /* Total memory available to the allocator */
  uint8_t* data; /* Base address of the reserved memory */
  uint8_t* free_head[MICA_ALLOC_NUM_CLASSES]; /* Per-class head free ptr */
};

void mica_alloc_reset(struct mica_alloc* alloc);

void mica_alloc_init(struct mica_alloc* alloc, uint64_t size, size_t numa_node);

struct mica_alloc_item* mica_alloc_get_item(const struct mica_alloc* alloc,
                                            uint64_t alloc_offset);

uint64_t mica_alloc_allocate(struct mica_alloc* alloc, uint32_t item_size);

void mica_alloc_deallocate(struct mica_alloc* alloc, uint64_t alloc_offset);
