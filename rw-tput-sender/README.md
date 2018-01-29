# RDMA read/write throughput
A benchmark to measure throughput of outbound RDMA reads or writes.

## Important parameters
These parameters are defined in `main.c`, `run-servers.sh`, and `run-machine.sh`.

1. Client-server configuration
  * `num_server_threads`: Number of worker threads at the server machine
  * `num_threads`: Number of client threads at each client machine
2. RDMA optimizations
  * `postlist`: Number of SENDs issued by a server using one Doorbell
  * `use_uc`: Use UC transport for RDMA writes
  * `NUM_QPS`: Number of QPs used by each server thread
  * `UNSIG_BATCH`: One `post_send()` per `UNSIG_BATCH` reads/writes is signaled

## Running the benchmark
1. Change `HRD_REGISTRY_IP` in both `run-servers.sh` and `run-machine.sh` to
   the server's IP address.
2. At the server machine: `./run-servers.sh`
3. At client machine `i` of `num_server_threads / num_threads` client machines:
   `./run-machine.sh i`

## Connection logic
 * Each client thread establishes a connections with one server thread. This
   is done to keep the communication fan-out at the server small.
