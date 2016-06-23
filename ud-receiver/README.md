# UD RECV throughput
A benchmark to measure throughput of UD RECVs (or inbound SENDs)

## Important parameters
These parameters are defined in `main.c`, `run-servers.sh`, and `run-machine.sh`.

1. Client-server configuration
  * `NUM_SERVER_THREADS`: Number of workers threads at the server machine
  * `num_threads`: Number of client threads at each client machine
2. RDMA optimizations
  * `postlist`: Maximim number of completions polled by a server thread in one
    `poll_cq()`. Also the number of SENDs issued by the client per Doorbell.
  * `NUM_UD_QPS`: Number of QPs used by each client thread
  * `UNSIG_BATCH`: One `post_send()` per `UNSIG_BATCH` SENDs is signaled

## Running the benchmark
1. At the server machine: `./run-server.sh`
2. At client machine `i` of `NUM_SERVER_THREADS / num_threads` client machines:
   `./run-machine.sh i`

## Connection logic
 * Each client thread establishes a connections with one server thread. This
   is done to keep the communication fan-out at the server small.
