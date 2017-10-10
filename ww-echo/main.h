#include <stdint.h>

#define BUF_SIZE 8192
#define CACHELINE_SIZE 64
#define MAX_WINDOW_SIZE 128

struct thread_params {
  int id;
  int dual_port;
  int use_uc;
  int size;
  int window;
};

void* run_server(void* arg);
void* run_client(void* arg);
