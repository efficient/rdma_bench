#include <gflags/gflags.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <thread>
#include <vector>
#include "libhrd_cpp/hrd.h"

static constexpr size_t kAppBufSize = 8;
std::atomic<size_t> expected;

void run_server() {
  struct hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = false;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = 3185;

  auto* cb = hrd_ctrl_blk_init(0 /* id */, 0 /* port */, 0 /* numa */,
                               &conn_config, nullptr /* dgram config */);
  memset(const_cast<uint8_t*>(cb->conn_buf), 0, kAppBufSize);

  hrd_publish_conn_qp(cb, 0, "server");
  printf("main: Server published. Waiting for client.\n");

  hrd_qp_attr_t* clt_qp = nullptr;
  while (clt_qp == nullptr) {
    clt_qp = hrd_get_published_qp("client");
    if (clt_qp == nullptr) usleep(200000);
  }

  printf("main: Server %s found client! Connecting..\n", "server");
  hrd_connect_qp(cb, 0, clt_qp);
  hrd_publish_ready("server");
  printf("main: Server ready\n");

  uint64_t seed = 0xdeadbeef;
  auto* ptr = reinterpret_cast<volatile size_t*>(cb->conn_buf);

  while (true) {
    size_t loc = hrd_fastrand(&seed) % (kAppBufSize / sizeof(size_t));
    size_t val = ptr[loc];
    size_t _expected = expected;
    if (val < _expected) {
      printf("value = %zu, expected = %zu\n", val, _expected);
      usleep(1);
    }
  }
}

void run_client() {
  hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = false;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = 3186;

  auto* cb = hrd_ctrl_blk_init(1 /* id */, 0 /* port */, 0 /* numa */,
                               &conn_config, nullptr /* dgram config */);

  hrd_publish_conn_qp(cb, 0, "client");
  printf("main: Client published. Waiting for server.\n");

  hrd_qp_attr_t* srv_qp = nullptr;
  while (srv_qp == nullptr) {
    srv_qp = hrd_get_published_qp("server");
    if (srv_qp == nullptr) usleep(2000);
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
    ctr++;

    // RDMA write an array with all 8-byte words = ctr
    for (size_t i = 0; i < kAppBufSize / sizeof(size_t); i++) ptr[i] = ctr;

    sge.addr = reinterpret_cast<uint64_t>(cb->conn_buf);
    sge.length = kAppBufSize;
    sge.lkey = cb->conn_buf_mr->lkey;

    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.num_sge = 1;
    wr.next = nullptr;
    wr.sg_list = &sge;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = srv_qp->buf_addr;
    wr.wr.rdma.rkey = srv_qp->rkey;

    int ret = ibv_post_send(cb->conn_qp[0], &wr, &bad_send_wr);
    rt_assert(ret == 0);
    hrd_poll_cq(cb->conn_cq[0], 1, &wc);  // Block till the RDMA write completes

    expected = ctr;
  }
}

int main(int argc, char* argv[]) {
  expected = 0;

  gflags::ParseCommandLineFlags(&argc, &argv, true);
  auto thread_server = std::thread(run_server);
  auto thread_client = std::thread(run_client);

  thread_server.join();
  thread_client.join();
  return 0;
}
