#include <gflags/gflags.h>
#include <limits>
#include <thread>
#include "libhrd_cpp/hrd.h"

static constexpr size_t kAppBufSize = MB(2);
static constexpr int kAppBaseSHMKey = 2;

static constexpr size_t kAppDefaultRunTime = 1000000;
static constexpr size_t kAppRunTimeSlack = 10;

// Number of outstanding requests kept by a server thread across all QPs.
// This is only used for READs where we can detect completion by polling on
// a READ's destination buffer.
//
// For WRITEs, this is hard to do unless we make every send() signaled. So,
// the number of per-thread outstanding operations per thread with WRITEs is
// O(NUM_CLIENTS * kAppUnsigBatch).
static constexpr size_t kAppWindowSize = 32;
static_assert(is_power_of_two(kAppWindowSize), "");

// Sweep paramaters
static constexpr size_t kAppNumServers = 1;
static constexpr size_t kAppNumClients = 16;  // Total client QPs in cluster
static constexpr size_t kAppNumClientMachines = 2;
static constexpr size_t kAppUnsigBatch = 4;

static_assert(kHrdSQDepth == 128, "");  // Small queues => more scalaing
static_assert(kAppNumClients % kAppNumClientMachines == 0, "");

// We don't use postlist, so we don't need a postlist check
static_assert(kHrdSQDepth >= 2 * kAppUnsigBatch, "");  // Queue capacity check

struct thread_params_t {
  size_t id;
  double* tput;
};

DEFINE_uint64(machine_id, std::numeric_limits<size_t>::max(), "Machine ID");
DEFINE_uint64(is_client, 0, "Is this process a client?");
DEFINE_uint64(run_time, 0, "Running time");
DEFINE_uint64(dual_port, 0, "Use two ports?");
DEFINE_uint64(use_uc, 0, "Use unreliable connected transport?");
DEFINE_uint64(do_read, 0, "Do RDMA reads?");
DEFINE_uint64(size, 0, "RDMA size");

void run_server(thread_params_t* params) {
  size_t srv_gid = params->id;  // Global ID of this server thread
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : srv_gid % 2;
  int shm_key = kAppBaseSHMKey + static_cast<int>(srv_gid);

  hrd_conn_config_t conn_config;
  conn_config.num_qps = kAppNumClients;
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = shm_key;

  auto* cb =
      hrd_ctrl_blk_init(srv_gid, ib_port_index, 0, &conn_config, nullptr);

  // Set the buffer to 0 so that we can detect READ completion by polling.
  memset(const_cast<uint8_t*>(cb->conn_buf), 0, kAppBufSize);

  for (size_t i = 0; i < kAppNumClients; i++) {
    char srv_qp_name[kHrdQPNameSize];
    sprintf(srv_qp_name, "server-%zu-%zu", srv_gid, i);
    hrd_publish_conn_qp(cb, i, srv_qp_name);
  }

  hrd_qp_attr_t* clt_qp[kAppNumClients];
  for (size_t i = 0; i < kAppNumClients; i++) {
    char clt_qp_name[kHrdQPNameSize];
    sprintf(clt_qp_name, "client-%zu-%zu", i, srv_gid);

    clt_qp[i] = nullptr;
    while (clt_qp[i] == nullptr) {
      clt_qp[i] = hrd_get_published_qp(clt_qp_name);
      if (clt_qp[i] == nullptr) usleep(20000);
    }

    printf("main: Server %zu found client %zu! Connecting..\n", srv_gid, i);
    hrd_connect_qp(cb, i, clt_qp[i]);
    hrd_wait_till_ready(clt_qp_name);
  }

  printf("main: Server %zu ready\n", srv_gid);

  struct ibv_send_wr wr, *bad_send_wr;
  struct ibv_sge sgl;
  struct ibv_wc wc;
  size_t rolling_iter = 0;             // For performance measurement
  size_t nb_tx[kAppNumClients] = {0};  // Per-QP signaling
  size_t nb_tx_tot = 0;                // For windowing (for READs only)

  struct timespec run_start, run_end;
  struct timespec msr_start, msr_end;
  clock_gettime(CLOCK_REALTIME, &run_start);
  clock_gettime(CLOCK_REALTIME, &msr_start);

  auto opcode = FLAGS_do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;
  uint64_t seed = 0xdeadbeef;

  while (1) {
    if (rolling_iter >= MB(4)) {
      clock_gettime(CLOCK_REALTIME, &msr_end);
      double msr_seconds = (msr_end.tv_sec - msr_start.tv_sec) +
                           (msr_end.tv_nsec - msr_start.tv_nsec) / 1000000000.0;
      double tput = rolling_iter / msr_seconds;

      clock_gettime(CLOCK_REALTIME, &run_end);
      double run_seconds = (run_end.tv_sec - run_start.tv_sec) +
                           (run_end.tv_nsec - run_start.tv_nsec) / 1000000000.0;
      if (run_seconds >= FLAGS_run_time) {
        printf("main: Server %zu exiting.\n", srv_gid);
        hrd_ctrl_blk_destroy(cb);
        return;
      }

      printf(
          "main: Server %zu: %.2f ops. Total active QPs = %zu. "
          "Outstanding ops per thread (for READs) = %zu. "
          "Seconds = %.1f of %zu.\n",
          srv_gid, tput, kAppNumServers * kAppNumClients, kAppWindowSize,
          run_seconds, FLAGS_run_time);

      params->tput[srv_gid] = tput;
      if (srv_gid == 0) {
        double tot = 0;
        for (size_t i = 0; i < kAppNumServers; i++) tot += params->tput[i];
        hrd_red_printf("Total tput = %.2f ops\n", tot);
      }

      rolling_iter = 0;
      clock_gettime(CLOCK_REALTIME, &msr_start);
    }

    size_t window_i = nb_tx_tot % kAppWindowSize;  // Current window slot to use

    // For READs, restrict outstanding ops per-thread to kAppWindowSize
    if (opcode == IBV_WR_RDMA_READ && nb_tx_tot >= kAppWindowSize) {
      while (cb->conn_buf[window_i * FLAGS_size] == 0) {
        // Wait for a window slow to open up
      }
      cb->conn_buf[window_i * FLAGS_size] = 0;
    }

    // Choose the next client to send a packet to
    size_t cn = hrd_fastrand(&seed) % kAppNumClients;
    wr.opcode = opcode;
    wr.num_sge = 1;
    wr.next = nullptr;
    wr.sg_list = &sgl;

    wr.send_flags = nb_tx[cn] % kAppUnsigBatch == 0 ? IBV_SEND_SIGNALED : 0;
    if (nb_tx[cn] % kAppUnsigBatch == 0 && nb_tx[cn] > 0) {
      // This can happen if a client dies before the server
      int ret = hrd_poll_cq_ret(cb->conn_cq[cn], 1, &wc);
      if (ret == -1) {
        hrd_ctrl_blk_destroy(cb);
        return;
      }
    }

    wr.send_flags |= (FLAGS_do_read == 0) ? IBV_SEND_INLINE : 0;

    sgl.addr = reinterpret_cast<uint64_t>(&cb->conn_buf[window_i * FLAGS_size]);
    sgl.length = FLAGS_size;
    sgl.lkey = cb->conn_buf_mr->lkey;

    size_t remote_offset = hrd_fastrand(&seed) % (kAppBufSize - FLAGS_size);
    wr.wr.rdma.remote_addr = clt_qp[cn]->buf_addr + remote_offset;
    wr.wr.rdma.rkey = clt_qp[cn]->rkey;

    nb_tx[cn]++;

    int ret = ibv_post_send(cb->conn_qp[cn], &wr, &bad_send_wr);
    rt_assert(ret == 0);
    rolling_iter++;
  }
}

void run_client(thread_params_t* params) {
  constexpr size_t num_threads = kAppNumClients / kAppNumClientMachines;

  size_t clt_gid = params->id;  // Global ID of this server thread
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : clt_gid % 2;
  int shm_key = kAppBaseSHMKey + clt_gid % num_threads;

  hrd_conn_config_t conn_config;
  conn_config.num_qps = kAppNumServers;
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = shm_key;

  auto* cb =
      hrd_ctrl_blk_init(clt_gid, ib_port_index, 0, &conn_config, nullptr);

  // Set to some non-zero value so the server can detect READ completion
  memset(const_cast<uint8_t*>(cb->conn_buf), 1, kAppBufSize);

  for (size_t i = 0; i < kAppNumServers; i++) {
    char clt_qp_name[kHrdQPNameSize];
    sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, i);
    hrd_publish_conn_qp(cb, i, clt_qp_name);
  }

  for (size_t i = 0; i < kAppNumServers; i++) {
    char srv_qp_name[kHrdQPNameSize];
    sprintf(srv_qp_name, "server-%zu-%zu", i, clt_gid);

    hrd_qp_attr_t* srv_qp = nullptr;
    while (srv_qp == nullptr) {
      srv_qp = hrd_get_published_qp(srv_qp_name);
      if (srv_qp == nullptr) usleep(20000);
    }

    printf("main: Client %zu found server %zu! Connecting..\n", clt_gid, i);
    hrd_connect_qp(cb, i, srv_qp);

    char clt_qp_name[kHrdQPNameSize];
    sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, i);
    hrd_publish_ready(clt_qp_name);
  }

  printf("main: Client %zu READY\n", clt_gid);

  struct timespec run_start, run_end;
  clock_gettime(CLOCK_REALTIME, &run_start);

  while (true) {
    printf("main: Client %zu: %d\n", clt_gid, cb->conn_buf[0]);

    clock_gettime(CLOCK_REALTIME, &run_end);
    double run_seconds = (run_end.tv_sec - run_start.tv_sec) +
                         (run_end.tv_nsec - run_start.tv_nsec) / 1000000000.0;

    if (run_seconds >= FLAGS_run_time + kAppRunTimeSlack) {
      printf("main: Client %zu: exiting\n", clt_gid);
      hrd_ctrl_blk_destroy(cb);
      return;
    } else {
      printf("main: Client %zu: active for %.2f seconds (of %zu + %zu)\n",
             clt_gid, run_seconds, FLAGS_run_time, kAppRunTimeSlack);
    }

    sleep(1);
  }
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  rt_assert(FLAGS_dual_port <= 1, "Invalid dual_port");
  rt_assert(FLAGS_use_uc <= 1, "Invalid use_uc");
  rt_assert(FLAGS_is_client <= 1, "Invalid is_client");

  size_t num_threads;
  if (FLAGS_is_client == 1) {
    num_threads = kAppNumClients / kAppNumClientMachines;
  } else {
    // All the buffers sent or received should fit in @conn_buf
    rt_assert(FLAGS_size * kAppWindowSize < kAppBufSize, "");

    num_threads = kAppNumServers;
    rt_assert(FLAGS_machine_id == std::numeric_limits<size_t>::max(), "");
    rt_assert(FLAGS_size > 0, "Invalid size");

    if (FLAGS_do_read == 0) rt_assert(FLAGS_size <= kHrdMaxInline, "Inl error");
    rt_assert(FLAGS_do_read <= 1, "Invalid do_read");
  }

  // Launch the server or client threads
  printf("main: Launching %zu threads\n", num_threads);
  std::vector<thread_params_t> param_arr(num_threads);
  std::vector<std::thread> thread_arr(num_threads);
  auto* tput = new double[num_threads];

  for (size_t i = 0; i < num_threads; i++) {
    if (FLAGS_is_client == 1) {
      param_arr[i].id = (FLAGS_machine_id * num_threads) + i;
      thread_arr[i] = std::thread(run_client, &param_arr[i]);
    } else {
      param_arr[i].id = i;
      param_arr[i].tput = tput;
      thread_arr[i] = std::thread(run_server, &param_arr[i]);
    }
  }

  for (auto& t : thread_arr) t.join();
  return 0;
}
