#include <gflags/gflags.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include "libhrd_cpp/hrd.h"

DEFINE_uint64(is_client, 0, "Is this process a client?");

static constexpr size_t kAppBufSize = 64;

void run_server() {
  struct hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = 0;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = 3185;

  auto* cb = hrd_ctrl_blk_init(0 /* id */, 0 /* port */, 0 /* numa */,
                               &conn_config, nullptr /* dgram config */);

  hrd_publish_conn_qp(cb, 0, "server");
  printf("main: Server published. Waiting for client.\n");

  hrd_qp_attr_t* clt_qp = nullptr;
  while (clt_qp == nullptr) {
    clt_qp = hrd_get_published_qp("client");
    if (clt_qp == nullptr) usleep(200000);
  }

  printf("main: Server %s found client! Connecting..\n", "server");
  hrd_connect_qp(cb, 0, clt_qp);

  // This garbles the server's qp_attr - which is safe
  hrd_publish_ready("server");
  printf("main: Server ready\n");

  uint64_t seed = 0xdeadbeef;
  auto* ptr = reinterpret_cast<volatile size_t*>(cb->conn_buf);

  while (true) {
    size_t loc_1 = hrd_fastrand(&seed) % (kAppBufSize / sizeof(size_t));
    size_t loc_2 = hrd_fastrand(&seed) % (kAppBufSize / sizeof(size_t));

    if (loc_1 >= loc_2) continue;
    // Here, loc_1 < loc 2

    size_t val_1 = ptr[loc_1];

    asm volatile("" ::: "memory");  // Compiler barrier
    asm volatile("lfence" ::: "memory");
    asm volatile("sfence" ::: "memory");
    asm volatile("mfence" ::: "memory");  // Hardware barrier

    size_t val_2 = ptr[loc_2];

    if (val_2 > val_1) {
      printf("violation %zu %zu %zu %zu\n", loc_1, loc_2, val_1, val_2);
      rt_assert(val_2 == val_1 + 1);  // This is expected to fail eventually
    } else if (val_2 < val_1) {
      // printf("ok\n");
    }
  }
}

void run_client() {
  hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = 0;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = 3185;

  auto* cb = hrd_ctrl_blk_init(0 /* id */, 0 /* port */, 0 /* numa */,
                               &conn_config, nullptr /* dgram config */);

  hrd_publish_conn_qp(cb, 0, "client");
  printf("main: Client published. Waiting for server.\n");

  hrd_qp_attr_t* srv_qp = nullptr;
  while (srv_qp == nullptr) {
    srv_qp = hrd_get_published_qp("server");
    if (srv_qp == nullptr) usleep(200000);
  }

  printf("main: Client found server. Connecting..\n");
  hrd_connect_qp(cb, 0, srv_qp);
  printf("main: Client connected!\n");

  hrd_wait_till_ready("server");

  struct ibv_send_wr wr, *bad_send_wr;
  struct ibv_sge sge;
  struct ibv_wc wc;
  size_t ctr = 0;

  auto* ptr = reinterpret_cast<volatile size_t*>(&cb->conn_buf[0]);
  while (true) {
    // RDMA write an array with all 8-byte words = ctr
    for (size_t i = 0; i < kAppBufSize / sizeof(size_t); i++) {
      ptr[i] = ctr;
    }

    ctr++;

    // Post a batch
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.num_sge = 1;
    wr.next = nullptr;
    wr.sg_list = &sge;

    wr.send_flags = IBV_SEND_SIGNALED;

    sge.addr = reinterpret_cast<uint64_t>(cb->conn_buf);
    sge.length = kAppBufSize;
    sge.lkey = cb->conn_buf_mr->lkey;

    wr.wr.rdma.remote_addr = srv_qp->buf_addr;
    wr.wr.rdma.rkey = srv_qp->rkey;

    int ret = ibv_post_send(cb->conn_qp[0], &wr, &bad_send_wr);
    rt_assert(ret == 0);
    hrd_poll_cq(cb->conn_cq[0], 1, &wc);  // Block till the RDMA write completes
  }
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_is_client == 1) {
    run_client();
  } else {
    run_server();
  }

  return 0;
}
