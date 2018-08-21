#include <gflags/gflags.h>
#include <stdlib.h>
#include <limits>
#include <thread>
#include "libhrd_cpp/hrd.h"

static constexpr size_t kAppNumClients = 1;
static_assert(is_power_of_two(kAppNumClients), "");

static constexpr size_t kAppNumQPs = 1;
static constexpr size_t kAppBufSize = 4096;
static constexpr size_t kAppMaxPostlist = 128;
static constexpr size_t kAppUnsigBatch = 64;

struct thread_params_t {
  size_t id;
  double* tput;
};

DEFINE_uint64(machine_id, std::numeric_limits<size_t>::max(), "Machine ID");
DEFINE_uint64(num_threads, 0, "Number of threads");
DEFINE_uint64(is_client, 0, "Is this process a client?");
DEFINE_uint64(dual_port, 0, "Use two ports?");
DEFINE_uint64(size, 0, "RDMA size");
DEFINE_uint64(postlist, kAppMaxPostlist, "Postlist size");

void run_server(thread_params_t* params) {
  size_t srv_gid = params->id;  // Global ID of this server thread
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : (srv_gid % 2) * 2;

  hrd_dgram_config_t dgram_config;
  dgram_config.num_qps = kAppNumQPs;
  dgram_config.prealloc_buf = nullptr;
  dgram_config.buf_size = kAppBufSize;
  dgram_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(srv_gid, ib_port_index, kHrdInvalidNUMANode,
                               nullptr, &dgram_config);
  memset(const_cast<uint8_t*>(cb->dgram_buf), 0, kAppBufSize);

  // Buffer to send responses from
  auto* resp_buf = new uint8_t[FLAGS_size];
  memset(resp_buf, 1, FLAGS_size);

  struct ibv_ah* ah[kAppNumClients];
  hrd_qp_attr_t* clt_qp[kAppNumClients];

  // Connect this server to all clients
  for (size_t i = 0; i < kAppNumClients; i++) {
    char clt_name[kHrdQPNameSize];
    sprintf(clt_name, "client-%zu", i);

    // Get the UD queue pair for the ith client
    clt_qp[i] = nullptr;
    while (clt_qp[i] == nullptr) {
      clt_qp[i] = hrd_get_published_qp(clt_name);
      if (clt_qp[i] == nullptr) usleep(200000);
    }

    printf("main: Server %zu got client %zu of %zu clients.\n", srv_gid, i,
           kAppNumClients);

    struct ibv_ah_attr ah_attr;
    memset(&ah_attr, 0, sizeof(struct ibv_ah_attr));
    ah_attr.is_global = 0;
    ah_attr.dlid = clt_qp[i]->lid;
    ah_attr.sl = 0;
    ah_attr.src_path_bits = 0;
    ah_attr.port_num = cb->resolve.dev_port_id;

    ah[i] = ibv_create_ah(cb->pd, &ah_attr);
    rt_assert(ah[i] != nullptr, "Failed to create address handle");
  }

  struct ibv_send_wr wr[kAppMaxPostlist], *bad_send_wr;
  struct ibv_wc wc[kAppMaxPostlist];
  struct ibv_sge sgl[kAppMaxPostlist];
  size_t rolling_iter = 0;         // For throughput measurement
  size_t nb_tx[kAppNumQPs] = {0};  // For selective signaling
  size_t qp_i = 0;                 // Round-robin between QPs across postlists

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  while (1) {
    if (rolling_iter >= MB(4)) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (end.tv_nsec - start.tv_nsec) / 1000000000.0;
      double my_tput = rolling_iter / (seconds * 1000000);
      printf("main: Server %zu: %.2f M/s. \n", srv_gid, my_tput);
      params->tput[srv_gid] = my_tput;

      if (srv_gid == 0) {
        double tot = 0;
        for (size_t i = 0; i < FLAGS_num_threads; i++) tot += params->tput[i];
        hrd_red_printf("main: Total tput = %.2f M/s.\n", tot);
      }

      rolling_iter = 0;
      clock_gettime(CLOCK_REALTIME, &start);
    }

    for (size_t w_i = 0; w_i < FLAGS_postlist; w_i++) {
      size_t cn = nb_tx[qp_i] % kAppNumClients;

      wr[w_i].wr.ud.ah = ah[cn];
      wr[w_i].wr.ud.remote_qpn = clt_qp[cn]->qpn;
      wr[w_i].wr.ud.remote_qkey = kHrdDefaultQKey;

      wr[w_i].opcode = IBV_WR_SEND;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == FLAGS_postlist - 1) ? nullptr : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags =
          nb_tx[qp_i] % kAppUnsigBatch == 0 ? IBV_SEND_SIGNALED : 0;
      if (nb_tx[qp_i] % kAppUnsigBatch == 0 && nb_tx[qp_i] > 0) {
        hrd_poll_cq(cb->dgram_send_cq[qp_i], 1, wc);
      }

      wr[w_i].send_flags |= IBV_SEND_INLINE;

      sgl[w_i].addr = reinterpret_cast<uint64_t>(resp_buf);
      sgl[w_i].length = FLAGS_size;

      nb_tx[qp_i]++;
      rolling_iter++;
    }

    int ret = ibv_post_send(cb->dgram_qp[qp_i], &wr[0], &bad_send_wr);
    rt_assert(ret == 0);

    qp_i++;
    if (qp_i == kAppNumQPs) qp_i = 0;
  }
}

void run_client(thread_params_t* params) {
  // The local HID of a control block should be <= 64 to keep the SHM key low.
  // But the number of clients over all machines can be larger.
  size_t clt_gid = params->id;  // Global ID of this client thread
  size_t clt_local_hid = clt_gid % FLAGS_num_threads;

  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : (clt_gid % 2) * 2;

  hrd_dgram_config_t dgram_config;
  dgram_config.num_qps = 1;
  dgram_config.prealloc_buf = nullptr;
  dgram_config.buf_size = kAppBufSize;
  dgram_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(clt_local_hid, ib_port_index,
                               kHrdInvalidNUMANode, nullptr, &dgram_config);

  // Fill the RECV queue
  for (size_t i = 0; i < kHrdRQDepth; i++) {
    hrd_post_dgram_recv(cb->dgram_qp[0], const_cast<uint8_t*>(cb->dgram_buf),
                        kAppBufSize, cb->dgram_buf_mr->lkey);
  }

  char clt_name[kHrdQPNameSize];
  sprintf(clt_name, "client-%zu", clt_gid);

  hrd_publish_dgram_qp(cb, 0, clt_name);
  printf("main: Client %s published.\n", clt_name);

  struct ibv_wc wc;
  size_t rolling_iter = 0;  // For throughput measurement
  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  while (true) {
    if (rolling_iter >= KB(512)) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (end.tv_nsec - start.tv_nsec) / 1000000000.0;
      printf("main: Client %zu: %.2f IOPS\n", clt_gid, rolling_iter / seconds);
      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    hrd_poll_cq(cb->dgram_recv_cq[0], 1, &wc);
    hrd_post_dgram_recv(cb->dgram_qp[0], const_cast<uint8_t*>(cb->dgram_buf),
                        kAppBufSize, cb->dgram_buf_mr->lkey);

    rolling_iter++;
  }
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  double* tput = nullptr;  // Leaked

  rt_assert(FLAGS_num_threads >= 1, "Invalid num_threads");
  rt_assert(FLAGS_dual_port <= 1, "Invalid dual_port");
  rt_assert(FLAGS_is_client <= 1, "Invalid is_client");
  rt_assert(FLAGS_size <= kHrdMaxInline, "Invalid SEND size");

  if (FLAGS_is_client == 1) {
    rt_assert(FLAGS_machine_id != std::numeric_limits<size_t>::max(),
              "Invalid machine_id");
    rt_assert(FLAGS_postlist == kAppMaxPostlist, "Invalid postlist");
  } else {
    rt_assert(FLAGS_machine_id == std::numeric_limits<size_t>::max(), "");
    rt_assert(FLAGS_postlist >= 1 && FLAGS_postlist <= kAppMaxPostlist,
              "Invalid postlist");
    rt_assert(FLAGS_postlist <= kAppUnsigBatch, "Postlist check failed");
    static_assert(kHrdSQDepth >= 2 * kAppUnsigBatch, "Queue capacity check");

    tput = new double[FLAGS_num_threads];
    for (size_t i = 0; i < FLAGS_num_threads; i++) tput[i] = 0;
  }

  // Launch a single server thread or multiple client threads
  printf("main: Using %zu threads\n", FLAGS_num_threads);
  std::vector<thread_params_t> param_arr(FLAGS_num_threads);
  std::vector<std::thread> thread_arr(FLAGS_num_threads);

  for (size_t i = 0; i < FLAGS_num_threads; i++) {
    if (FLAGS_is_client == 1) {
      param_arr[i].id = (FLAGS_machine_id * FLAGS_num_threads) + i;
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
