#include "main.h"
#include <getopt.h>
#include <signal.h>
#include <thread>

__thread FILE* tl_out_fp = nullptr;  // File to record throughput
__thread hrd_ctrl_blk_t* tl_cb;
__thread thread_params_t tl_params;

// Is the remote QP for a local QP on the same physical machine?
inline bool is_remote_qp_on_same_physical_mc(size_t qp_i) {
  return (qp_i % kAppNumMachines == FLAGS_machine_id);
}

// Get the name for the QP index qp_i created by this thread
void get_qp_name_local(char* namebuf, size_t qp_i) {
  assert(namebuf != nullptr);
  assert(qp_i < kAppNumQPsPerThread);

  size_t for_phys_mc = qp_i % kAppNumMachines;
  size_t for_vm = qp_i / kAppNumMachines;

  sprintf(namebuf, "on_phys_mc_%zu-at_thr_%zu-for_phys_mc_%zu-for_vm_%zu",
          FLAGS_machine_id, tl_params.wrkr_lid, for_phys_mc, for_vm);
}

// Get the name of the remote QP that QP index qp_i created by this thread
// should connect to.
void get_qp_name_remote(char* namebuf, size_t qp_i) {
  assert(namebuf != nullptr);
  assert(qp_i < kAppNumQPsPerThread);

  size_t for_phys_mc = qp_i % kAppNumMachines;
  size_t for_vm = qp_i / kAppNumMachines;

  sprintf(namebuf, "on_phys_mc_%zu-at_thr_%zu-for_phys_mc_%zu-for_vm_%zu",
          for_phys_mc, tl_params.wrkr_lid, FLAGS_machine_id, for_vm);
}

// Record machine throughput
void record_sweep_params() {
  fprintf(tl_out_fp, "Machine %zu: sweep parameters: ", FLAGS_machine_id);
  fprintf(tl_out_fp, "kAppRDMASize %zu, ", kAppRDMASize);
  fprintf(tl_out_fp, "kAppWindowSize %zu, ", kAppWindowSize);
  fprintf(tl_out_fp, "kAppUnsigBatch %zu, ", kAppUnsigBatch);
  fprintf(tl_out_fp, "kAppAllsig %u, ", kAppAllsig);
  fprintf(tl_out_fp, "kAppNumWorkers %zu, ", kAppNumWorkers);
  fflush(tl_out_fp);
}

// Record machine throughput
void record_machine_tput(double total_tput) {
  assert(tl_out_fp != nullptr);
  char timebuf[50];
  hrd_get_formatted_time(timebuf);

  fprintf(tl_out_fp, "Machine %zu: tput = %.2f reqs/s, time %s\n",
          FLAGS_machine_id, total_tput, timebuf);
  fflush(tl_out_fp);
}

// Choose a QP to send an RDMA on
static inline size_t choose_qp(uint64_t* seed) {
  size_t qp_i = 0;

  while (is_remote_qp_on_same_physical_mc(qp_i)) {
    qp_i = hrd_fastrand(seed) % kAppNumQPsPerThread;
  }

  return qp_i;
}

void kill_handler(int) {
  printf("Destroying control block for worker %zu\n", tl_params.wrkr_gid);
  hrd_ctrl_blk_destroy(tl_cb);
}

void run_worker(thread_params_t* params) {
  signal(SIGINT, kill_handler);
  signal(SIGKILL, kill_handler);
  signal(SIGTERM, kill_handler);

  tl_params = *params;
  size_t first_in_machine = (tl_params.wrkr_gid % kAppNumThreads == 0);

  printf("Worker %zu: use_uc = %zu\n", tl_params.wrkr_gid, FLAGS_use_uc);

  size_t vport_index = tl_params.wrkr_lid % FLAGS_num_ports;
  size_t ib_port_index = FLAGS_base_port_index + vport_index;

  // Create the output file for this machine
  if (first_in_machine == 1) {
    char filename[100];
    sprintf(filename, "tput-out/machine-%zu", FLAGS_machine_id);
    tl_out_fp = fopen(filename, "w");
    assert(tl_out_fp != nullptr);
    record_sweep_params();
  }

  // Create the control block
  int wrkr_shm_key =
      kAppWorkerBaseSHMKey + (tl_params.wrkr_gid % kAppNumThreads);
  hrd_conn_config_t conn_config;
  conn_config.num_qps = kAppNumQPsPerThread;
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = wrkr_shm_key;
  tl_cb = hrd_ctrl_blk_init(tl_params.wrkr_gid, ib_port_index, FLAGS_numa_node,
                            &conn_config, nullptr);

  if (FLAGS_do_read == 1) {
    // Set to 0 so that we can detect READ completion by polling.
    memset(const_cast<uint8_t*>(tl_cb->conn_buf), 0, kAppBufSize);
  } else {
    // Set to 5 so that WRITEs show up as a weird value
    memset(const_cast<uint8_t*>(tl_cb->conn_buf), 5, kAppBufSize);
  }

  // Publish worker QPs
  for (size_t i = 0; i < kAppNumQPsPerThread; i++) {
    char local_qp_name[kHrdQPNameSize];
    get_qp_name_local(local_qp_name, i);

    hrd_publish_conn_qp(tl_cb, i, local_qp_name);
  }
  printf("main: Worker %zu published local QPs\n", tl_params.wrkr_gid);

  // Find QPs to connect to
  hrd_qp_attr_t* remote_qp_arr[kAppNumQPsPerThread] = {nullptr};
  for (size_t i = 0; i < kAppNumQPsPerThread; i++) {
    // Do not connect if remote QP is on this machine
    if (is_remote_qp_on_same_physical_mc(i)) continue;

    char remote_qp_name[kHrdQPNameSize];
    get_qp_name_remote(remote_qp_name, i);

    printf("main: Worker %zu looking for %s.\n", tl_params.wrkr_gid,
           remote_qp_name);
    while (remote_qp_arr[i] == nullptr) {
      remote_qp_arr[i] = hrd_get_published_qp(remote_qp_name);
      if (remote_qp_arr[i] == nullptr) usleep(20000);
    }

    printf("main: Worker %zu found %s! Connecting..\n", tl_params.wrkr_gid,
           remote_qp_name);
    hrd_connect_qp(tl_cb, i, remote_qp_arr[i]);

    char local_qp_name[kHrdQPNameSize];
    get_qp_name_local(local_qp_name, i);
    hrd_publish_ready(local_qp_name);
  }

  for (size_t i = 0; i < kAppNumQPsPerThread; i++) {
    // Do not connect if remote QP is on this machine
    if (is_remote_qp_on_same_physical_mc(i)) continue;

    char remote_qp_name[kHrdQPNameSize];
    get_qp_name_remote(remote_qp_name, i);

    printf("main: Worker %zu waiting for %s to get ready\n", tl_params.wrkr_gid,
           remote_qp_name);
    hrd_wait_till_ready(remote_qp_name);
  }

  printf("main: Worker %zu ready\n", tl_params.wrkr_gid);

  struct ibv_send_wr wr, *bad_send_wr;
  struct ibv_sge sgl;
  struct ibv_wc wc;
  size_t rolling_iter = 0;                  // For performance measurement
  size_t nb_tx[kAppNumQPsPerThread] = {0};  // Per-QP tracking for signaling
  size_t nb_tx_tot = 0;                     // For windowing (for READs only)
  size_t window_i = 0;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  auto opcode = FLAGS_do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;

  size_t qpn = 0;  // Queue pair number to read or write from
  size_t rec_qpn_arr[kAppWindowSize] = {0};  // Record which QP we used

  // Move fastrand for this worker
  uint64_t seed __attribute__((unused)) = 0xdeadbeef;
  for (size_t i = 0; i < tl_params.wrkr_gid * 10000000; i++) {
    hrd_fastrand(&seed);
  }

  while (1) {
    if (rolling_iter >= MB(2)) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (end.tv_nsec - start.tv_nsec) / 1000000000.0;
      double tput = rolling_iter / seconds;

      printf(
          "main: Worker %zu: %.2f Mops. Total active QPs = %zu. "
          "Outstanding ops per thread (for READs) = %zu.\n",
          tl_params.wrkr_gid, tput, kAppNumThreads * kAppNumQPsPerThread,
          kAppWindowSize);

      // Per-machine throughput computation
      params->tput_arr[tl_params.wrkr_gid % kAppNumThreads] = tput;
      if (first_in_machine == 1) {
        double machine_tput = 0;
        for (size_t i = 0; i < kAppNumThreads; i++) {
          machine_tput += params->tput_arr[i];
        }
        record_machine_tput(machine_tput);
        hrd_red_printf("main: Total tput %.2f Mops\n", machine_tput);
      }
      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    // For READs and kAppAllsig WRITEs, we can restrict outstanding ops per
    // thread to kAppWindowSize.
    if (nb_tx_tot >= kAppWindowSize) {
      if (kAppAllsig == 0 && opcode == IBV_WR_RDMA_READ) {
        while (tl_cb->conn_buf[window_i * kAppRDMASize] == 0) {
        }  // Wait

        // Zero-out the slot for this round
        tl_cb->conn_buf[window_i * kAppRDMASize] = 0;
      } else if (kAppAllsig == 1) {
        size_t qpn_to_poll = rec_qpn_arr[window_i];
        int ret = hrd_poll_cq_ret(tl_cb->conn_cq[qpn_to_poll], 1, &wc);
        // printf("Polled window index = %d\n", window_i);
        if (ret != 0) {
          printf("Worker %zu destroying cb and exiting\n", tl_params.wrkr_gid);
          hrd_ctrl_blk_destroy(tl_cb);
          exit(-1);
        }
      }
    }

    // Choose the next machine to send RDMA to and record it
    qpn = choose_qp(&seed);
    rec_qpn_arr[window_i] = qpn;

    wr.opcode = opcode;
    wr.num_sge = 1;
    wr.next = nullptr;
    wr.sg_list = &sgl;

#if kAppAllsig == 0
    wr.send_flags = (nb_tx[qpn] & kAppUnsigBatch_) == 0 ? IBV_SEND_SIGNALED : 0;
    if ((nb_tx[qpn] & kAppUnsigBatch_) == kAppUnsigBatch_) {
      int ret = hrd_poll_cq_ret(tl_cb->conn_cq[qpn], 1, &wc);
      if (ret != 0) {
        printf("Worker %zu destroying cb and exiting\n", tl_params.wrkr_gid);
        hrd_ctrl_blk_destroy(tl_cb);
        exit(-1);
      }
    }
#else
    wr.send_flags = IBV_SEND_SIGNALED;
#endif
    nb_tx[qpn]++;

    wr.send_flags |= (FLAGS_do_read == 0) ? IBV_SEND_INLINE : 0;

    // Aligning local/remote offset to 64-byte boundary REDUCES performance
    // significantly (similar to atomics).
    size_t _offset = (hrd_fastrand(&seed) & kAppBufSize_);
    while (_offset >= kAppBufSize - kAppRDMASize) {
      _offset = (hrd_fastrand(&seed) & kAppBufSize_);
    }

#if kAppAllsig == 0
    // Use a predictable address to make polling easy
    sgl.addr =
        reinterpret_cast<uint64_t>(&tl_cb->conn_buf[window_i * kAppRDMASize]);
#else
    // We'll use CQE to detect comp; using random address improves perf
    sgl.addr = reinterpret_cast<uint64_t>(&tl_cb->conn_buf[_offset]);
#endif

    sgl.length = kAppRDMASize;
    sgl.lkey = tl_cb->conn_buf_mr->lkey;

    wr.wr.rdma.remote_addr = remote_qp_arr[qpn]->buf_addr + _offset;
    wr.wr.rdma.rkey = remote_qp_arr[qpn]->rkey;

    // printf("Worker %d: Sending request %lld to over QP %d.\n",
    //	tl_params.wrkr_gid, nb_tx_tot, qpn);

    int ret = ibv_post_send(tl_cb->conn_qp[qpn], &wr, &bad_send_wr);
    rt_assert(ret == 0, "ibv_post_send error");

    rolling_iter++;
    nb_tx_tot++;
    mod_add_one<kAppWindowSize>(window_i);
  }
}

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
