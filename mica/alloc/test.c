#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "alloc.h"

#define ALLOC_SIZE (1024 * 1024 * 1024)
#define MAX_ITEM_SIZE (300)

int main() {
  struct mica_alloc allocator;
  mica_alloc_init(&allocator, ALLOC_SIZE, 0);

  /* Aim for around 85% memory efficiency */
  int num_allocs = (ALLOC_SIZE / ((MAX_ITEM_SIZE / 2) + 9)) * .86;

  uint64_t* offsets = malloc(num_allocs * sizeof(*offsets));
  assert(offsets != NULL);

  uint64_t* sizes = malloc(num_allocs * sizeof(*sizes));
  assert(sizes != NULL);

  int i, j, iters = 0;
  while (1) {
    uint64_t user_bytes = 0;

    for (i = 0; i < num_allocs; i++) {
      uint32_t item_size = (rand() % 300) + 9;
      sizes[i] = item_size;
      user_bytes += item_size;

      offsets[i] = mica_alloc_allocate(&allocator, item_size);
      assert(offsets[i] != MICA_ALLOC_INSUFFICIENT_SPACE);

      struct mica_alloc_item* item =
          mica_alloc_get_item(&allocator, offsets[i]);
      memset(item->data, i, item_size);
    }

    for (i = 0; i < num_allocs; i++) {
      struct mica_alloc_item* item =
          mica_alloc_get_item(&allocator, offsets[i]);

      /* Check the item's size and data */
      assert(item->item_size == sizes[i]);
      for (j = 0; j < sizes[i]; j++) {
        assert(item->data[j] == (uint8_t)i);
      }

      mica_alloc_deallocate(&allocator, offsets[i]);
    }

    printf("Iteration %d succeeded. Memory efficiency = %.2f\n", iters,
           (double)user_bytes / ALLOC_SIZE);
    fflush(stdout);

    iters++;
  }
}
