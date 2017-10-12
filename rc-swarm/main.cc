#include "main.h"
#include <getopt.h>
#include <thread>

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  double tput_arr[kAppNumThreads];
  for (size_t i = 0; i < kAppNumThreads; i++) tput_arr[i] = 0.0;

  std::array<thread_params_t, kAppNumThreads> param_arr;
  std::array<std::thread, kAppNumThreads> thread_arr;

  printf("main: Launching %zu swarm workers\n", kAppNumThreads);
  for (size_t i = 0; i < kAppNumThreads; i++) {
    param_arr[i].wrkr_gid = (FLAGS_machine_id * kAppNumThreads) + i;
    param_arr[i].wrkr_lid = i;
    param_arr[i].tput_arr = tput_arr;

    thread_arr[i] = std::thread(run_worker, &param_arr[i]);
  }

  for (auto& t : thread_arr) t.join();
  return 0;
}
