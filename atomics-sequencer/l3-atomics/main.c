#include <stdio.h>
#include "hrd.h"

#define SHM_KEY 24
#define NUM_COUNTERS M_2 /* 16 MB ~ L3 cache */
#define NUM_COUNTERS_ (NUM_COUNTERS - 1)

inline uint32_t fastrand(uint64_t* seed) {
  *seed = *seed * 1103515245 + 12345;
  return (uint32_t)(*seed >> 32);
}

int main() {
  int addr = 0;
  long long iters = 0;
  long long* A =
      hrd_malloc_socket(SHM_KEY, NUM_COUNTERS * sizeof(long long), 0);
  assert(A != NULL);
  memset((char*)A, 23, NUM_COUNTERS * sizeof(long long));

  uint64_t seed = 0xdeadbeef;
  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  while (1) {
    if (iters == M_8) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;

      printf("Rate =  %.2f M /s.\n", M_8 / (seconds * 1000000));

      iters = 0;
      clock_gettime(CLOCK_REALTIME, &start);
    }

    addr = (addr + fastrand(&seed)) & NUM_COUNTERS_;
    addr = __sync_fetch_and_add(&A[addr], 1);

    iters++;
  }

  return 0;
}
