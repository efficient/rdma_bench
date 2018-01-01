#include "main.h"
#include <getopt.h>
#include <signal.h>
#include <atomic>
#include <thread>

__thread FILE* tl_out_fp = nullptr;  // File to record throughput
__thread hrd_ctrl_blk_t* tl_cb;
__thread thread_params_t tl_params;

__thread size_t polling_region_size = SIZE_MAX;
__thread size_t qps_per_thread = SIZE_MAX;

volatile sig_atomic_t ctrl_c_pressed = 0;
std::atomic<size_t> num_threads_exited;

// Is the remote QP for a local QP on the same process?
inline bool is_remote_qp_on_same_process(size_t qp_i) {
  return (qp_i % FLAGS_num_processes == FLAGS_process_id);
}

// Get the name for the QP index qp_i created by this thread
void get_qp_name_local(char namebuf[kHrdQPNameSize], size_t qp_i) {
  assert(qp_i < qps_per_thread);

  size_t for_process = qp_i % FLAGS_num_processes;
  size_t for_vm = qp_i / FLAGS_num_processes;

  sprintf(namebuf, "on_proc_%zu-at_thr_%zu-for_proc_%zu-for_vm_%zu",
          FLAGS_process_id, tl_params.wrkr_lid, for_process, for_vm);
}

// Get the name of the remote QP that QP index qp_i created by this thread
// should connect to.
void get_qp_name_remote(char namebuf[kHrdQPNameSize], size_t qp_i) {
  assert(qp_i < qps_per_thread);

  size_t for_process = qp_i % FLAGS_num_processes;
  size_t for_vm = qp_i / FLAGS_num_processes;

  sprintf(namebuf, "on_proc_%zu-at_thr_%zu-for_proc_%zu-for_vm_%zu",
          for_process, tl_params.wrkr_lid, FLAGS_process_id, for_vm);
}

// Choose a QP to send an RDMA on
static inline size_t choose_qp(uint64_t* seed) {
  size_t qp_i = hrd_fastrand(seed) % qps_per_thread;
  while (is_remote_qp_on_same_process(qp_i)) {
    qp_i = hrd_fastrand(seed) % qps_per_thread;
  }

  return qp_i;
}

void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

void wait_for_all_threads_and_exit() {
  rt_assert(num_threads_exited < FLAGS_num_threads);
  printf("main: Worker %zu waiting for all thread to exit.\n",
         tl_params.wrkr_gid);
  num_threads_exited++;  // Mark self as exited
  while (num_threads_exited != FLAGS_num_threads) {
    usleep(200000);
  }
  exit(0);
}

inline void check_ctrl_c_pressed() {
  if (unlikely(ctrl_c_pressed == 1)) {
    printf("main: Worker %zu destroying cb.\n", tl_params.wrkr_gid);
    hrd_ctrl_blk_destroy(tl_cb);
    wait_for_all_threads_and_exit();
  }
}

/// Poll one completion from \p qpn. Blocking.
void app_poll_cq(const size_t qpn) {
  struct ibv_wc wc;
  int comps = 0;
  while (true) {
    check_ctrl_c_pressed();
    comps = ibv_poll_cq(tl_cb->conn_cq[qpn], 1, &wc);
    if (unlikely(comps < 0 || (comps == 1 && wc.status != IBV_WC_SUCCESS))) {
      printf("main: Worker %zu poll_cq failed. Exiting.\n", tl_params.wrkr_gid);
      hrd_ctrl_blk_destroy(tl_cb);
      wait_for_all_threads_and_exit();
    }

    if (comps == 1) return;
  }
}

void worker_main_loop(const hrd_qp_attr_t** remote_qp_arr) {
  struct ibv_send_wr wr, *bad_send_wr;
  struct ibv_sge sgl;
  size_t rolling_iter = 0;  // For performance measurement
  size_t nb_tx_tot = 0;     // For windowing (for READs only)
  size_t window_i = 0;

  std::vector<size_t> nb_tx(qps_per_thread);
  for (auto& s : nb_tx) s = 0;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  // Move fastrand for this worker
  uint64_t seed = 0xdeadbeef;
  for (size_t i = 0; i < tl_params.wrkr_gid * MB(10); i++) hrd_fastrand(&seed);

  size_t qpn = 0;  // Queue pair number to read or write from
  size_t rec_qpn_arr[kAppMaxWindow] = {0};  // Record which QP we used
  while (true) {
    check_ctrl_c_pressed();

    if (rolling_iter >= MB(2)) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (end.tv_nsec - start.tv_nsec) / 1000000000.0;
      double tput = rolling_iter / (seconds * 1000000);

      printf(
          "main: Worker %zu: %.2f M/s. Total active QPs = %zu. "
          "Outstanding ops per thread (for READs) = %zu.\n",
          tl_params.wrkr_gid, tput, FLAGS_num_threads * qps_per_thread,
          FLAGS_window_size);

      tl_params.tput_arr[tl_params.wrkr_lid] = tput;
      if (tl_params.wrkr_lid == 0) {
        double tot = 0;
        for (size_t i = 0; i < FLAGS_num_threads; i++) {
          tot += tl_params.tput_arr[i];
        }
        record_process_tput(tl_out_fp, tot);
        hrd_red_printf("main: Total tput %.2f M/s\n", tot);
      }

      rolling_iter = 0;
      clock_gettime(CLOCK_REALTIME, &start);
    }

    if (nb_tx_tot >= FLAGS_window_size) {
      // Poll for both READs and WRITEs if allsig is enabled
      if (FLAGS_allsig == 1) app_poll_cq(rec_qpn_arr[window_i]);

      // For READs, always poll to ensure <= kAppWindowSize outstanding READs
      if (FLAGS_do_read == 1) {
        volatile uint8_t* poll_buf = &tl_cb->conn_buf[window_i * FLAGS_size];
        // Sanity check: If allsig is set, we polled for READ completion above
        if (FLAGS_allsig == 1) rt_assert(poll_buf[0] != 0);

        while (poll_buf[0] == 0) check_ctrl_c_pressed();
        poll_buf[0] = 0;
      }
    }

    // Choose a QP to send an RDMA on, and record it
    qpn = choose_qp(&seed);
    rec_qpn_arr[window_i] = qpn;

    wr.opcode = FLAGS_do_read == 1 ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE;
    wr.num_sge = 1;
    wr.next = nullptr;
    wr.sg_list = &sgl;
    wr.send_flags = (FLAGS_do_read == 0) ? IBV_SEND_INLINE : 0;

    if (FLAGS_allsig == 0) {
      // Use selective signaling if allsig is disabled
      size_t mod = nb_tx[qpn] % FLAGS_unsig_batch;
      wr.send_flags |= (mod == 0) ? IBV_SEND_SIGNALED : 0;
      if (mod == FLAGS_unsig_batch - 1) app_poll_cq(qpn);
    } else {
      wr.send_flags |= IBV_SEND_SIGNALED;
    }
    nb_tx[qpn]++;

    sgl.addr =
        reinterpret_cast<uint64_t>(&tl_cb->conn_buf[window_i * FLAGS_size]);
    sgl.length = FLAGS_size;
    sgl.lkey = tl_cb->conn_buf_mr->lkey;

    size_t rem_offset = hrd_fastrand(&seed) % kAppBufSize;
    if (kAppRoundOffset) rem_offset = round_up<64, size_t>(rem_offset);
    while (rem_offset <= polling_region_size ||
           rem_offset >= kAppBufSize - FLAGS_size) {
      rem_offset = hrd_fastrand(&seed) % kAppBufSize;
      if (kAppRoundOffset) rem_offset = round_up<64, size_t>(rem_offset);
    }

    wr.wr.rdma.remote_addr = remote_qp_arr[qpn]->buf_addr + rem_offset;
    wr.wr.rdma.rkey = remote_qp_arr[qpn]->rkey;

    // printf("Worker %d: Sending request %lld to over QP %d.\n",
    //	tl_params.wrkr_gid, nb_tx_tot, qpn);

    int ret = ibv_post_send(tl_cb->conn_qp[qpn], &wr, &bad_send_wr);
    rt_assert(ret == 0);

    rolling_iter++;
    nb_tx_tot++;

    window_i++;
    if (window_i == FLAGS_window_size) window_i = 0;
  }
}

void run_worker(thread_params_t* params) {
  signal(SIGINT, ctrl_c_handler);
  signal(SIGKILL, ctrl_c_handler);
  signal(SIGTERM, ctrl_c_handler);

  // Initialize thread-local variables
  tl_params = *params;
  qps_per_thread = FLAGS_num_processes * FLAGS_vms_per_process;

  // The first window_size slots are zeroed out and used for READ completion
  // detection. The remaining slots are non-zero and are fetched via READs.
  polling_region_size = FLAGS_window_size * FLAGS_size;
  rt_assert(polling_region_size < kAppBufSize / 10, "Polling region too large");

  size_t ib_port_index = (FLAGS_numa_node == 0)
                             ? numa_0_ports[tl_params.wrkr_lid % 2]
                             : numa_1_ports[tl_params.wrkr_lid % 2];

  // Create the output file for this process
  if (tl_params.wrkr_lid == 0) {
    char filename[100];
    sprintf(filename, "tput-out/process-%zu", FLAGS_process_id);
    tl_out_fp = fopen(filename, "w");
    assert(tl_out_fp != nullptr);
    record_sweep_params(tl_out_fp);
  }

  // Create the control block
  hrd_conn_config_t conn_config;
  conn_config.num_qps = qps_per_thread;
  conn_config.use_uc = false;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = kAppWorkerBaseSHMKey + tl_params.wrkr_gid;
  conn_config.sq_depth = FLAGS_sq_depth;
  conn_config.max_rd_atomic = FLAGS_max_rd_atomic;
  tl_cb = hrd_ctrl_blk_init(tl_params.wrkr_gid, ib_port_index, FLAGS_numa_node,
                            &conn_config, nullptr);

  // Zero-out the READ polling region; non-zero the rest.
  memset(const_cast<uint8_t*>(tl_cb->conn_buf), 0, polling_region_size);
  memset(const_cast<uint8_t*>(tl_cb->conn_buf + polling_region_size), 1,
         kAppBufSize - polling_region_size);

  // Publish worker QPs
  for (size_t i = 0; i < qps_per_thread; i++) {
    char local_qp_name[kHrdQPNameSize];
    get_qp_name_local(local_qp_name, i);

    hrd_publish_conn_qp(tl_cb, i, local_qp_name);
  }
  printf("main: Worker %zu published local QPs\n", tl_params.wrkr_gid);

  // Find QPs to connect to
  auto** remote_qp_arr = new hrd_qp_attr_t*[qps_per_thread];
  for (size_t i = 0; i < qps_per_thread; i++) {
    // Do not connect if remote QP is on this process
    if (is_remote_qp_on_same_process(i)) continue;

    char remote_qp_name[kHrdQPNameSize];
    get_qp_name_remote(remote_qp_name, i);

    printf("main: Worker %zu looking for %s.\n", tl_params.wrkr_gid,
           remote_qp_name);

    remote_qp_arr[i] = nullptr;
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

  for (size_t i = 0; i < qps_per_thread; i++) {
    // Do not connect if remote QP is on this process
    if (is_remote_qp_on_same_process(i)) continue;

    char remote_qp_name[kHrdQPNameSize];
    get_qp_name_remote(remote_qp_name, i);

    printf("main: Worker %zu waiting for %s to get ready\n", tl_params.wrkr_gid,
           remote_qp_name);
    hrd_wait_till_ready(remote_qp_name);
  }

  printf("main: Worker %zu ready\n", tl_params.wrkr_gid);
  worker_main_loop(const_cast<const hrd_qp_attr_t**>(remote_qp_arr));
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  rt_assert(FLAGS_size <= kHrdMaxInline, "RDMA size too large");
  rt_assert(FLAGS_window_size <= kAppMaxWindow, "Window size too large");
  rt_assert(FLAGS_num_threads > 0 && FLAGS_num_threads <= kAppMaxThreads);
  rt_assert(FLAGS_sq_depth >= 2 * FLAGS_unsig_batch, "");  // Queue cap check

  double tput_arr[kAppMaxThreads];
  for (size_t i = 0; i < kAppMaxThreads; i++) tput_arr[i] = 0.0;

  std::array<thread_params_t, kAppMaxThreads> param_arr;
  std::array<std::thread, kAppMaxThreads> threads;

  printf("main: Launching %zu swarm workers\n", FLAGS_num_threads);
  num_threads_exited = 0;
  for (size_t i = 0; i < FLAGS_num_threads; i++) {
    param_arr[i].wrkr_gid = (FLAGS_process_id * FLAGS_num_threads) + i;
    param_arr[i].wrkr_lid = i;
    param_arr[i].tput_arr = tput_arr;

    threads[i] = std::thread(run_worker, &param_arr[i]);

    // Assume even threads are on NUMA 0, odd are on NUMA 1
    if (FLAGS_numa_node == 0) hrd_bind_to_core(threads[i], 2 * i);
    if (FLAGS_numa_node == 1) hrd_bind_to_core(threads[i], 2 * i + 1);
  }

  for (size_t i = 0; i < FLAGS_num_threads; i++) threads[i].join();
  return 0;
}
