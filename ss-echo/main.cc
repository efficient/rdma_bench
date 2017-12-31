#include <gflags/gflags.h>
#include <stdlib.h>
#include <string.h>
#include <limits>
#include <thread>
#include "libhrd_cpp/hrd.h"

static constexpr size_t kAppNumQPs = 1;  // UD QPs used by server for RECVs
static constexpr size_t kAppBufSize = 4096;
static constexpr size_t kAppMaxPostlist = 64;
static constexpr size_t kAppUnsigBatch = 64;
static_assert(is_power_of_two(kAppUnsigBatch), "");

struct thread_params_t {
  size_t id;
  double* tput;
};

DEFINE_uint64(machine_id, std::numeric_limits<size_t>::max(), "Machine ID");
DEFINE_uint64(num_client_threads, 0, "Number of client threads/machine");
DEFINE_uint64(num_server_threads, 0, "Number of server threads");
DEFINE_uint64(is_client, 0, "Is this process a client?");
DEFINE_uint64(dual_port, 0, "Use two ports?");
DEFINE_uint64(size, 0, "RDMA size");
DEFINE_uint64(postlist, std::numeric_limits<size_t>::max(), "Postlist size");

void run_client(thread_params_t* params) {
  // The local HID of a control block should be <= 64 to keep the SHM key low.
  // But the number of clients over all machines can be larger.
  size_t clt_gid = params->id;  // Global ID of this client thread
  size_t clt_local_hid = clt_gid % FLAGS_num_client_threads;
  size_t srv_gid = clt_gid % FLAGS_num_server_threads;
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : srv_gid % 2;

  hrd_dgram_config_t dgram_config;
  dgram_config.num_qps = 1;
  dgram_config.prealloc_buf = nullptr;
  dgram_config.buf_size = kAppBufSize;
  dgram_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(clt_local_hid, ib_port_index,
                               kHrdInvalidNUMANode, nullptr, &dgram_config);

  // Buffer to receive responses into
  memset(const_cast<uint8_t*>(cb->dgram_buf), 0, kAppBufSize);

  // Buffer to send requests from
  auto* req_buf = new uint8_t[FLAGS_size];
  memset(req_buf, clt_gid, FLAGS_size);

  char srv_name[kHrdQPNameSize];
  sprintf(srv_name, "server-%zu", srv_gid);
  char clt_name[kHrdQPNameSize];
  sprintf(clt_name, "client-%zu", clt_gid);

  hrd_publish_dgram_qp(cb, 0, clt_name);
  printf("main: Client %s published. Waiting for server %s\n", clt_name,
         srv_name);

  struct hrd_qp_attr_t* srv_qp = nullptr;
  while (srv_qp == nullptr) {
    srv_qp = hrd_get_published_qp(srv_name);
    if (srv_qp == nullptr) usleep(200000);
  }

  printf("main: Client %s found server! Now posting SENDs.\n", clt_name);

  struct ibv_ah_attr ah_attr;
  memset(&ah_attr, 0, sizeof(ah_attr));
  ah_attr.is_global = 0;
  ah_attr.dlid = srv_qp->lid;
  ah_attr.sl = 0;
  ah_attr.src_path_bits = 0;
  ah_attr.port_num = cb->resolve.dev_port_id;

  struct ibv_ah* ah = ibv_create_ah(cb->pd, &ah_attr);
  assert(ah != nullptr);

  struct ibv_send_wr wr[kAppMaxPostlist], *bad_send_wr;
  struct ibv_wc wc[kAppMaxPostlist];
  struct ibv_sge sgl[kAppMaxPostlist];
  size_t rolling_iter = 0;  // For throughput measurement

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  while (true) {
    if (rolling_iter >= KB(512)) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (end.tv_nsec - start.tv_nsec) / 1000000000.0;
      printf("main: Client %zu: %.2f Mops\n", clt_gid, rolling_iter / seconds);
      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    for (size_t w_i = 0; w_i < FLAGS_postlist; w_i++) {
      hrd_post_dgram_recv(cb->dgram_qp[0], const_cast<uint8_t*>(cb->dgram_buf),
                          kAppBufSize, cb->dgram_buf_mr->lkey);

      wr[w_i].wr.ud.ah = ah;
      wr[w_i].wr.ud.remote_qpn = srv_qp->qpn;
      wr[w_i].wr.ud.remote_qkey = kHrdDefaultQKey;

      wr[w_i].opcode = IBV_WR_SEND;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == FLAGS_postlist - 1) ? nullptr : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags = (w_i == 0) ? IBV_SEND_SIGNALED : 0;
      wr[w_i].send_flags |= IBV_SEND_INLINE;

      sgl[w_i].addr = reinterpret_cast<uint64_t>(req_buf);
      sgl[w_i].length = FLAGS_size;

      rolling_iter++;
    }

    int ret = ibv_post_send(cb->dgram_qp[0], &wr[0], &bad_send_wr);
    rt_assert(ret == 0);

    hrd_poll_cq(cb->dgram_send_cq[0], 1, wc);  // Poll SEND (w_i = 0)

    // Poll for all RECVs
    hrd_poll_cq(cb->dgram_recv_cq[0], FLAGS_postlist, wc);
  }
}

void run_server(thread_params_t* params) {
  size_t srv_gid = params->id;  // Global ID of this server thread
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : srv_gid % 2;

  hrd_dgram_config_t dgram_config;
  dgram_config.num_qps = kAppNumQPs;
  dgram_config.prealloc_buf = nullptr;
  dgram_config.buf_size = kAppBufSize;
  dgram_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(srv_gid, ib_port_index, kHrdInvalidNUMANode,
                               nullptr, &dgram_config);

  // Buffer to receive requests into
  memset(const_cast<uint8_t*>(cb->dgram_buf), 0, kAppBufSize);

  // Buffer to send responses from
  auto* resp_buf = new uint8_t[FLAGS_size];
  memset(resp_buf, 1, FLAGS_size);

  char srv_name[kHrdQPNameSize];
  sprintf(srv_name, "server-%zu", srv_gid);

  // We post RECVs only on the 1st QP.
  for (size_t i = 0; i < kHrdRQDepth; i++) {
    hrd_post_dgram_recv(cb->dgram_qp[0], const_cast<uint8_t*>(cb->dgram_buf),
                        FLAGS_size + sizeof(struct ibv_grh),
                        cb->dgram_buf_mr->lkey);
  }

  hrd_publish_dgram_qp(cb, 0, srv_name);
  printf("server: Server %s published. Now polling..\n", srv_name);

  // We'll initialize address handles on demand
  struct ibv_ah* ah[kHrdMaxLID];
  memset(ah, 0, kHrdMaxLID * sizeof(uintptr_t));

  struct ibv_send_wr wr[kAppMaxPostlist], *bad_send_wr;
  struct ibv_recv_wr recv_wr[kAppMaxPostlist], *bad_recv_wr;
  struct ibv_wc wc[kAppMaxPostlist];
  struct ibv_sge sgl[kAppMaxPostlist];
  size_t rolling_iter = 0;  // For throughput measurement
  size_t nb_tx[kAppNumQPs] = {0}, nb_tx_tot = 0, nb_post_send = 0;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  size_t recv_offset = 0;
  while (recv_offset < FLAGS_size + 40) recv_offset += 64;
  assert(recv_offset * FLAGS_postlist <= kAppBufSize);

  size_t qp_i = 0;
  while (1) {
    if (rolling_iter >= MB(4)) {
      clock_gettime(CLOCK_REALTIME, &end);

      double seconds = (end.tv_sec - start.tv_sec) +
                       (end.tv_nsec - start.tv_nsec) / 1000000000.0;
      params->tput[srv_gid] = rolling_iter / seconds;
      printf("main: Server %zu: %.2f Mops. Average postlist = %.2f\n", srv_gid,
             params->tput[srv_gid], nb_tx_tot * 1.0 / nb_post_send);

      if (srv_gid == 0) {
        double tot = 0;
        for (size_t i = 0; i < FLAGS_num_server_threads; i++) {
          tot += params->tput[i];
        }
        hrd_red_printf("main: Total = %.2f\n", tot);
      }

      rolling_iter = 0;
      clock_gettime(CLOCK_REALTIME, &start);
    }

    size_t num_comps = 0;
    int ret = ibv_poll_cq(cb->dgram_recv_cq[0], FLAGS_postlist, wc);
    assert(ret >= 0);
    num_comps = static_cast<size_t>(ret);
    if (num_comps == 0) continue;

    // Post a batch of RECVs
    for (size_t w_i = 0; w_i < num_comps; w_i++) {
      sgl[w_i].length = FLAGS_size + sizeof(struct ibv_grh);
      sgl[w_i].lkey = cb->dgram_buf_mr->lkey;
      sgl[w_i].addr =
          reinterpret_cast<uint64_t>(&cb->dgram_buf[recv_offset * w_i]);

      recv_wr[w_i].sg_list = &sgl[w_i];
      recv_wr[w_i].num_sge = 1;
      recv_wr[w_i].next = (w_i == num_comps - 1) ? nullptr : &recv_wr[w_i + 1];
    }

    ret = ibv_post_recv(cb->dgram_qp[0], &recv_wr[0], &bad_recv_wr);
    rt_assert(ret == 0);

    for (size_t w_i = 0; w_i < num_comps; w_i++) {
      int s_lid = wc[w_i].slid;  // Src LID for this request
      if (ah[s_lid] == nullptr) {
        struct ibv_ah_attr ah_attr;
        memset(&ah_attr, 0, sizeof(ah_attr));
        ah_attr.is_global = 0;
        ah_attr.dlid = s_lid;
        ah_attr.sl = 0;
        ah_attr.src_path_bits = 0;
        ah_attr.port_num = cb->resolve.dev_port_id;

        ah[s_lid] = ibv_create_ah(cb->pd, &ah_attr);
        rt_assert(ah[s_lid] != nullptr, "Failed to resovle ah");
      }

      wr[w_i].wr.ud.ah = ah[wc[w_i].slid];
      wr[w_i].wr.ud.remote_qpn = wc[w_i].src_qp;
      wr[w_i].wr.ud.remote_qkey = kHrdDefaultQKey;

      wr[w_i].opcode = IBV_WR_SEND;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == num_comps - 1) ? nullptr : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags =
          (nb_tx[qp_i] % kAppUnsigBatch == 0) ? IBV_SEND_SIGNALED : 0;
      if (nb_tx[qp_i] % kAppUnsigBatch == kAppUnsigBatch - 1) {
        struct ibv_wc signal_wc;
        hrd_poll_cq(cb->dgram_send_cq[qp_i], 1, &signal_wc);
      }

      wr[w_i].send_flags |= IBV_SEND_INLINE;

      sgl[w_i].addr = reinterpret_cast<uint64_t>(resp_buf);
      sgl[w_i].length = FLAGS_size;

      nb_tx[qp_i]++;
      nb_tx_tot++;
      rolling_iter++;
    }

    nb_post_send++;
    ret = ibv_post_send(cb->dgram_qp[qp_i], &wr[0], &bad_send_wr);
    rt_assert(ret == 0);

    qp_i++;
    if (qp_i == kAppNumQPs) qp_i = 0;
  }
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  double* tput = nullptr;  // Leaked

  // Basic flag checks
  rt_assert(FLAGS_dual_port <= 1, "Invalid dual_port");
  rt_assert(FLAGS_is_client <= 1, "Invalid is_client");
  rt_assert(FLAGS_postlist >= 1 && FLAGS_postlist <= kAppMaxPostlist,
            "Invalid postlist");
  rt_assert(
      FLAGS_size > 0 && FLAGS_size + sizeof(struct ibv_grh) <= kAppBufSize,
      "Invalid transfer size");

  // More checks
  rt_assert(FLAGS_postlist <= kAppUnsigBatch, "Postlist check failed");
  static_assert(kHrdSQDepth >= 2 * kAppUnsigBatch, "Queue capacity check");
  rt_assert(FLAGS_postlist <= kHrdRQDepth / 2, "RECV pollbatch too large");

  if (FLAGS_is_client == 1) {
    rt_assert(FLAGS_num_client_threads >= 1, "Invalid num_client_threads");
    // Clients need to know about number of server threads
    rt_assert(FLAGS_num_server_threads >= 1, "Invalid num_server_threads");
    rt_assert(FLAGS_machine_id != std::numeric_limits<size_t>::max(),
              "Invalid machine_id");
  } else {
    // Server does not need to know about number of client threads
    rt_assert(FLAGS_num_client_threads == 0, "Invalid num_client_threads");
    rt_assert(FLAGS_num_server_threads >= 1, "Invalid num_server_threads");
    rt_assert(FLAGS_machine_id == std::numeric_limits<size_t>::max(), "");

    tput = new double[FLAGS_num_server_threads];
    for (size_t i = 0; i < FLAGS_num_server_threads; i++) tput[i] = 0;
  }

  size_t _num_threads = (FLAGS_is_client == 1) ? FLAGS_num_client_threads
                                               : FLAGS_num_server_threads;

  // Launch a single server thread or multiple client threads
  printf("main: Using %zu threads\n", _num_threads);
  std::vector<thread_params_t> param_arr(_num_threads);
  std::vector<std::thread> thread_arr(_num_threads);

  for (size_t i = 0; i < _num_threads; i++) {
    if (FLAGS_is_client == 1) {
      param_arr[i].id = (FLAGS_machine_id * _num_threads) + i;
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
