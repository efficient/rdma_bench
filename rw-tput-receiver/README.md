# RDMA read/write throughput
A benchmark to measure throughput of inbound RDMA reads or writes.

## Important parameters
These parameters are defined in `main.c`, `run-servers.sh`, and `run-machine.sh`.

1. Client-server configuration
  * `num_server_threads`: Number of workers threads at the server machine
  * `num_threads`: Number of client threads at each client machine
2. RDMA optimizations
  * `postlist`: Number of SENDs issued by a server using one Doorbell
  * `use_uc`: Use UC transport for RDMA writes
  * `UNSIG_BATCH`: One `post_send()` per `UNSIG_BATCH` reads/writes is unsignaled

## Running the benchmark
1. At the server machine: `./run-servers.sh`
2. At client machine `i` of `num_server_threads / num_threads` client machines:
   `./run-machine.sh i`

## Connection logic
 * Each client thread establishes a connections with one server thread.
