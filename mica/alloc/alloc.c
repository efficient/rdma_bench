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

#include "alloc.h"

// TODO: use address order for each freelist to reduce fragmentation and improve
// locality.

// TODO: use the LSB (not MSB) to store status as all sizes are aligned to
// 8-byte boundary.

/* Initialize the allocator. We need an shm key argument here */
void mica_alloc_init(struct mica_alloc* alloc, uint64_t size,
                     size_t numa_node) {
  assert(alloc != NULL);
  assert(size <= MICA_ALLOC_MAX_SIZE);
  assert(numa_node >= 0 && numa_node <= 7);

  alloc->lock = 0;

  size = MICA_ROUNDUP2M(size);
  assert(size <= MICA_ALLOC_MAX_SIZE);
  alloc->size = size;

  // XXX: use hrd_shm_alloc.
  alloc->data = malloc(size);
  assert(alloc->data != NULL);
  memset(alloc->data, 0, size);

  mica_alloc_reset(alloc);
}

/* Helper functions */
size_t mica_alloc_size_to_class_roundup(uint64_t size) {
  assert(size <= MICA_ALLOC_MAX_SIZE);

  if (size <= MICA_ALLOC_MIN_SIZE +
                  (MICA_ALLOC_NUM_CLASSES - 1) * MICA_ALLOC_CLASS_INCREMENT) {
    return (size - MICA_ALLOC_MIN_SIZE + MICA_ALLOC_CLASS_INCREMENT - 1) /
           MICA_ALLOC_CLASS_INCREMENT;
  } else {
    return MICA_ALLOC_NUM_CLASSES - 1;
  }
}

size_t mica_alloc_size_to_class_rounddown(uint64_t size) {
  assert(size <= MICA_ALLOC_MAX_SIZE);
  assert(size >= MICA_ALLOC_MIN_SIZE);

  if (size < MICA_ALLOC_MIN_SIZE +
                 MICA_ALLOC_NUM_CLASSES * MICA_ALLOC_CLASS_INCREMENT) {
    //
    return (size - MICA_ALLOC_MIN_SIZE) / MICA_ALLOC_CLASS_INCREMENT;
  } else {
    return MICA_ALLOC_NUM_CLASSES - 1;
  }
}

/* Add a chunk back to its free list. */
void mica_alloc_insert_free_chunk(struct mica_alloc* alloc,
                                  uint8_t* chunk_start, uint64_t chunk_size) {
#ifdef MICA_VERBOSE
  printf("mica_alloc_insert_free_chunk: start %p, size %lu\n", chunk_start,
         chunk_size);
#endif
  size_t chunk_class = mica_alloc_size_to_class_rounddown(chunk_size);

  *(uint64_t*)chunk_start = *(uint64_t*)(chunk_start + chunk_size - 8) =
      MICA_ALLOC_TAG_VEC(chunk_size, MICA_ALLOC_FREE);

  /* We make this free chunk the head, so there's no previous free chunk */
  *(uint8_t**)(chunk_start + 8) = NULL;

  /* Point to the old head */
  *(uint8_t**)(chunk_start + 16) = alloc->free_head[chunk_class];

  /* If we had a valid head for this class, update its prev pointer */
  if (alloc->free_head[chunk_class] != NULL) {
    /* The prev pointer of the head should be NULL */
    assert(*(uint8_t**)(alloc->free_head[chunk_class] + 8) == NULL);
    *(uint8_t**)(alloc->free_head[chunk_class] + 8) = chunk_start;
  }

  alloc->free_head[chunk_class] = chunk_start; /* Set as a new head */
}

/*
 * Remove a free chunk from its free list. This does not require calculating
 * the free chunk's size class because we have list pointers in the chunk.
 */
void mica_alloc_remove_free_chunk_from_free_list(struct mica_alloc* alloc,
                                                 uint8_t* chunk_start,
                                                 uint64_t chunk_size) {
#ifdef MICA_VERBOSE
  printf("mica_alloc_remove_free_chunk_from_free_list: start %p, size %lu\n",
         chunk_start, chunk_size);
#endif
  uint8_t* prev_chunk_start = *(uint8_t**)(chunk_start + 8);
  uint8_t* next_chunk_start = *(uint8_t**)(chunk_start + 16);

  /*
   * If the previous chunk is valid, chain to next. Otherwise, we were the
   * head, so now set the next chunk as head.
   */
  if (prev_chunk_start != NULL) {
    *(uint8_t**)(prev_chunk_start + 16) = next_chunk_start;
  } else {
    size_t chunk_class = mica_alloc_size_to_class_rounddown(chunk_size);
    assert(alloc->free_head[chunk_class] == chunk_start);

    alloc->free_head[chunk_class] = next_chunk_start;
  }

  if (next_chunk_start != NULL) {
    *(uint8_t**)(next_chunk_start + 8) = prev_chunk_start;
  }
}

/*
 * Find a chunk with allocator size = @minimum_chunk_size and remove it from
 * the allocator's free lists.
 */
bool mica_alloc_remove_free_chunk_from_head(struct mica_alloc* alloc,
                                            uint64_t minimum_chunk_size,
                                            uint8_t** out_chunk_start,
                                            uint64_t* out_chunk_size) {
  /* Determine the size class to use (best fit) */
  size_t chunk_class = mica_alloc_size_to_class_roundup(minimum_chunk_size);
  for (; chunk_class < MICA_ALLOC_NUM_CLASSES; chunk_class++) {
    if (alloc->free_head[chunk_class] != NULL) {
      break;
    }
  }

  if (chunk_class == MICA_ALLOC_NUM_CLASSES) {
#ifdef MICA_VERBOSE
    printf("mica_alloc_remove_free_chunk_from_head: minsize %lu, no space\n",
           minimum_chunk_size);
#endif
    return false;
  }

  /*
   * Use the first free chunk in the class; the overall policy is still
   * approximately best fit (which is good) due to segregation.
   */
  uint8_t* chunk_start = alloc->free_head[chunk_class];

  uint64_t chunk_status = MICA_ALLOC_TAG_STATUS(*(uint64_t*)chunk_start);
  assert(chunk_status == MICA_ALLOC_FREE);

  uint64_t chunk_size = MICA_ALLOC_TAG_SIZE(*(uint64_t*)chunk_start);
  assert(chunk_size >= minimum_chunk_size);

  /* We should have the same tag at the beginning and end of a chunk */
  assert(*(uint64_t*)chunk_start == *(uint64_t*)(chunk_start + chunk_size - 8));

  mica_alloc_remove_free_chunk_from_free_list(alloc, chunk_start, chunk_size);
  *out_chunk_start = chunk_start;
  *out_chunk_size = chunk_size;

#ifdef MICA_VERBOSE
  printf(
      "mica_alloc_remove_free_chunk_from_head: "
      "minsize %lu, start %p, size %lu\n",
      minimum_chunk_size, *out_chunk_start, *out_chunk_size);
#endif
  return true;
}

void mica_alloc_reset(struct mica_alloc* alloc) {
  memset(alloc->free_head, 0, sizeof(alloc->free_head));
  /* set the entire free space as a free chunk */
  mica_alloc_insert_free_chunk(alloc, alloc->data, alloc->size);
}

struct mica_alloc_item* mica_alloc_get_item(const struct mica_alloc* alloc,
                                            uint64_t alloc_offset) {
  return (struct mica_alloc_item*)(alloc->data + alloc_offset);
}

void mica_alloc_coalese_free_chunk_left(struct mica_alloc* alloc,
                                        uint8_t** chunk_start,
                                        uint64_t* chunk_size) {
  if (*chunk_start == alloc->data) {
    return;
  }

  assert(*chunk_start > alloc->data);

  /* If the adjacent chunk is occupied, we can't coalesce. */
  if (MICA_ALLOC_TAG_STATUS(*(uint64_t*)(*chunk_start - 8)) ==
      MICA_ALLOC_OCCUPIED) {
    return;
  }

  /* If we're here, the adjacent chunk is free so we coalesce */
  uint64_t adj_chunk_size = MICA_ALLOC_TAG_SIZE(*(uint64_t*)(*chunk_start - 8));
  uint8_t* adj_chunk_start = *chunk_start - adj_chunk_size;

  /* Sanity check: the tags of the adjacent chunk should match up */
  assert(*(uint64_t*)adj_chunk_start ==
         *(uint64_t*)(adj_chunk_start + adj_chunk_size - 8));

#ifdef MICA_VERBOSE
  printf(
      "mica_alloc_coalese_free_chunk_left: "
      "start %p, size %lu, left %lu\n",
      *chunk_start, *chunk_size, adj_chunk_size);
#endif

  /* Remove the coalesced chunk from its free list */
  mica_alloc_remove_free_chunk_from_free_list(alloc, adj_chunk_start,
                                              adj_chunk_size);
  *chunk_start = adj_chunk_start;
  *chunk_size = *chunk_size + adj_chunk_size;
}

void mica_alloc_coalese_free_chunk_right(struct mica_alloc* alloc,
                                         uint8_t** chunk_start,
                                         uint64_t* chunk_size) {
  if (*chunk_start + *chunk_size == alloc->data + alloc->size) {
    return;
  }

  assert(*chunk_start + *chunk_size < alloc->data + alloc->size);

  /* If the adjacent chunk is occupied, we can't coalesce. */
  if (MICA_ALLOC_TAG_STATUS(*(uint64_t*)(*chunk_start + *chunk_size)) ==
      MICA_ALLOC_OCCUPIED) {
    return;
  }

  /* If we're here, the adjacent chunk is free so we coalesce */
  uint8_t* adj_chunk_start = *chunk_start + *chunk_size;
  uint64_t adj_chunk_size = MICA_ALLOC_TAG_SIZE(*(uint64_t*)adj_chunk_start);

  /* Sanity check: the tags of the adjacent chunk should match up */
  assert(*(uint64_t*)adj_chunk_start ==
         *(uint64_t*)(adj_chunk_start + adj_chunk_size - 8));

#ifdef MICA_VERBOSE
  printf(
      "mica_alloc_coalese_free_chunk_right: "
      "start %p, size %lu, right %lu\n",
      *chunk_start, *chunk_size, adj_chunk_size);
#endif

  /* Remove the coalesced chunk from its free list */
  mica_alloc_remove_free_chunk_from_free_list(alloc, adj_chunk_start,
                                              adj_chunk_size);

  /* chunk_start is unchanged */
  *chunk_size = *chunk_size + adj_chunk_size;
}

/*
 * Allocate a new item of user size @item_size.
 * Returns the offset of the allocated item if allocation is successful, or
 * MICA_ALLOC_INSUFFICIENT_SPACE if allocation fails.
 */
uint64_t mica_alloc_allocate(struct mica_alloc* alloc, uint32_t item_size) {
  /*
   * We need @minimum_chunk_size >= 32 bytes for correct computation of
   * @alloc_size_to_class_roundup(). We ensure this by forcing item_size to be
   * over 8B. In a real application, we'd expect at least a version number
   * to be associated with a key-value item, so this is fine.
   */
  assert(item_size > 8);

  uint64_t minimum_chunk_size =
      MICA_ROUNDUP8((uint64_t)item_size) + MICA_ALLOC_OVERHEAD;

  uint8_t* chunk_start;
  uint64_t chunk_size;

  if (!mica_alloc_remove_free_chunk_from_head(alloc, minimum_chunk_size,
                                              &chunk_start, &chunk_size)) {
    return MICA_ALLOC_INSUFFICIENT_SPACE;
  }

  /* See if we can make a leftover free chunk */
  uint64_t leftover_chunk_size = chunk_size - minimum_chunk_size;

  if (leftover_chunk_size >= MICA_ALLOC_MIN_SIZE) {
    /* Create a leftover free chunk and insert it to its freelist */
    mica_alloc_insert_free_chunk(alloc, chunk_start + minimum_chunk_size,
                                 leftover_chunk_size);

    /*
     * Coalescing is not required here because the previous chunk already
     * used to be a big coalesced free chunk.
     * Adjust the free chunk to avoid overlapping.
     */
    chunk_size = minimum_chunk_size;
  } else {
    leftover_chunk_size = 0;
  }

#ifdef MICA_VERBOSE
  printf(
      "mica_alloc_allocate: item_size %u, minsize %lu, start %p, "
      "size %lu, leftover %lu\n",
      item_size, minimum_chunk_size, chunk_start, chunk_size,
      leftover_chunk_size);
#endif

  /* Set the <size, status> tag on either end of the chunk */
  *(uint64_t*)chunk_start = *(uint64_t*)(chunk_start + chunk_size - 8) =
      MICA_ALLOC_TAG_VEC(chunk_size, MICA_ALLOC_OCCUPIED);

  /*
   * TODO: We are wasting 4 bytes for struct mica_alloc_item for
   * compatibility.  Need to implement an allocator-specific method to obtain
   * the item size.
   */
  struct mica_alloc_item* alloc_item =
      (struct mica_alloc_item*)(chunk_start + 8);
  alloc_item->item_size = item_size;
  return (uint64_t)((uint8_t*)alloc_item - alloc->data);
}

void mica_alloc_deallocate(struct mica_alloc* alloc, uint64_t alloc_offset) {
  struct mica_alloc_item* alloc_item = mica_alloc_get_item(alloc, alloc_offset);

  /* Allocated items begin at an 8-byte offset from the chunk start */
  uint8_t* chunk_start = (uint8_t*)alloc_item - 8;

  /* We should only deallocate allocated chunks */
  assert(MICA_ALLOC_TAG_STATUS(*(uint64_t*)chunk_start) == MICA_ALLOC_OCCUPIED);

  uint64_t chunk_size = MICA_ALLOC_TAG_SIZE(*(uint64_t*)chunk_start);

#ifdef MICA_VERBOSE
  printf("mica_alloc_deallocate: start %p, size %lu\n", chunk_start,
         chunk_size);
#endif

  /*
   * Try to create a larger chunk by coalescing left and right.
   * These functions modify @chunk_start and @chunk_size, so we end up with
   * chunk_start and chunk_size for a new chunk.
   */
  mica_alloc_coalese_free_chunk_left(alloc, &chunk_start, &chunk_size);
  mica_alloc_coalese_free_chunk_right(alloc, &chunk_start, &chunk_size);

  /* Insert the coalesced chunk into some free list */
  mica_alloc_insert_free_chunk(alloc, chunk_start, chunk_size);
}
