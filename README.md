# RDMA-bench
A framework to understand RDMA performance. This is the source code for our
[USENIX ATC paper](http://www.cs.cmu.edu/~akalia/doc/atc16/rdma_bench_atc.pdf).

## Required hardware and software
 * InfiniBand HCAs. Some C++ benchmarks work with RoCE HCAs.
 * Linux-based OS with RDMA drivers (Mellanox OFED or upstream OFED). Ubuntu,
   RHEL, and CentOS have been tested.
 * Required packages: cmake, memcached, gflags, libmemcached-dev, libnuma-dev
 * Root access is required only for hugepages.

## Required settings
All benchmarks require one server machine and multiple client machines. Every
benchmark is contained in one directory.
 * The number of client machines required is described in each benchmark's README
   file. The server will wait for all clients to launch, so the benchmarks won't
   make progress until the correct number of clients are launched.
 * Modify `HRD_REGISTRY_IP` in `run-servers.sh` and `run-machines.sh` to the IP
   address of the server machine. The server runs a memcached instance that is
   used as a queue pair registry.
 * Allocate hugepages on all machines, and set unlimited SHM limits:
```	
  sudo echo 8192 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
  sudo bash -c "echo kernel.shmmax = 9223372036854775807 >> /etc/sysctl.conf"
  sudo bash -c "echo kernel.shmall = 1152921504606846720 >> /etc/sysctl.conf"
  sudo sysctl -p /etc/sysctl.conf
```
   
## Benchmark description
The benchmarks used in the paper are described below. This repository contains
other benchmarks as well.

| Benchmark | Description |
| ------------- | ------------- |
| `herd` | An improved implementation of the [HERD key-value cache](http://www.cs.cmu.edu/~akalia/doc/sigcomm14/herd_readable.pdf). |
| `mica` | A simplified implementation of [MICA](https://github.com/efficient/mica). |
| | |
| `atomics-sequencer` | Sequencer using one-sided fetch-and-add. Also emulates DrTM-KV. |
| `ws-sequencer` | Sequencer using HERD RPCs (UC WRITE requests, UD SEND responses). |
| `ss-sequencer` | Sequencer using header-only datagram RPCs (i.e., UD SENDs only). |
| | |
| `rw-tput-sender` | Microbenchmark to measure throughput of outbound READs and WRITEs. |
| `rw-tput-receiver` | Microbenchmark to measure throughput of inbound READs and WRITEs. |
| `ud-sender` | Microbenchmark to measure throughput of outbound SENDs. |
| `ud-receiver` | Microbenchmark to measure throughput of inbound SENDs. |
| | |
| `rw-allsig` | WQE cache misses for outbound READs and WRITEs. |
| | |
| `write-incomplete` | This PoC shows that a completed WRITE can be invisible to the remote CPU. |
| `write-reordering` | A test for left-to-right ordering of WRITEs. |


## Implementation details

### `libhrd`
The `libhrd` library is used to implement all benchmarks. It consists of
convenience functions for initial RDMA setup, such as creating and connecting
QPs, and allocating hugepage memory.

### Memcached
Distributing QP information (required for connection setup in connected
transports, and routing in datagram transports) requires a temporary out-of-band
communication channel. To simplify this process, we use a `memcached` instance
to publish (e.g., `hrd_publish_conn_qp()`) and pull QP information (e.g.,
`hrd_get_published_qp`) using global QP names.

### Client connection logic
The code was written to work on a cluster that has dual-port NICs, but the switch
connectivity does not allow cross-port communication. Using both ports in this
constrained environment makes the initial QP connection setup slightly
complicated. All benchmarks also work on single-port NICs. Usually, we use the
following logic while setting up connections:

* There are `N` client threads in the system and each client thread uses `Q` QPs.
* The server has `num_server_ports` ports starting from port `base_port_index`.
  Similarly, clients have `num_client_ports`. The `base_port_index` may be
  different for server and clients.
* On the CIB cluster, port `i` on a NIC can only communicate with port `i`
  on other NICs. So `base_port_index` must be same for clients and server, and
  `num_client_ports == num_server_ports`.
* One server thread (the master thread in case there are worker threads) creates
  `N * Q` QPs on each server port. For applications requiring a request region,
  only one memory region is created and registered with all of the
  `num_server_ports` control blocks. Only some of these QPs actually get used by
  clients.
* Client threads have a global index `clt_i`. Each client thread uses a single
  control block and creates all its QPs on port index (using base
  `base_port_index`) `clt_i % num_client_ports`. It connects all these QPs to
  QPs on server port indexed `clt_i % num_server_ports` (using the server's
  `base_port_index`). This works for both CIB, and Apt and Intel clusters that
  support any-to-any communication between ports.

### Selective signaling logic
Most benchmarks post one signaled work request per `UNSIG_BATCH` work requests.
This is done to reduce CQE DMAs. With `UNSIG_BATCH = 4`, a sequence of work
requests looks as follows. Note that a work request is **not** `post()`ed
immediately; it is added to a list and posted when the number of work requests
in the list equals `postlist`.

```
wr 0 -> signaled
wr 1 -> unsignaled
wr 2 -> unsignaled
wr 3 -> unsignaled
	Poll for wr 0's completion. A postlist should have ended.
wr 4 -> signaled
...
wr 5 -> unsignaled
	Poll for wr 4's completion. Another postlist should have ended.
```

This imposes 2 requirements:

 * **Postlist check:** `postlist <= UNSIG_BATCH`. We poll for a completion
**before** queueing work request `UNSIG_BATCH + 1`. If `postlist > UNSIG_BATCH`,
nothing will have been posted at this point, so polling will get stuck.

 * **Queue capacity check:** `HRD_Q_DEPTH >= 2 * UNSIG_BATCH`. With the above
scheme, up to `2 * UNSIG_BATCH - 1` work requests can be un-ACKed by the QP.
With a QP of size `N`, `N - 1` work requests are allowed to be un-ACKed by the
InfiniBand/RoCE specification.

## Work in progress
The benchmarks are being ported to use C++ and CMake. Some benchmarks will
continue to use C (i.e., `libhrd`); others will move to C++ (i.e., `libhrd_cpp`).

## Contact
Anuj Kalia (akalia@cs.cmu.edu)

## License
		Copyright 2016, Carnegie Mellon University

        Licensed under the Apache License, Version 2.0 (the "License");
        you may not use this file except in compliance with the License.
        You may obtain a copy of the License at

            http://www.apache.org/licenses/LICENSE-2.0

        Unless required by applicable law or agreed to in writing, software
        distributed under the License is distributed on an "AS IS" BASIS,
        WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
        See the License for the specific language governing permissions and
        limitations under the License.
