## Client connection logic:
Connections use the technique discussed in the main README:
Each client thread uses a single control block and creates all its QPs
on port index (using base `base_port_index`) `clt_i % num_client_ports`. It
connects all these QPs to QP's on server port indexed
`clt_i % num_server_ports` (using the server's `base_port_index`). When
responding to client `clt_i`, the server sends response using its control block
indexed `clt_i % num_server_ports`.

## 6ede187af0e2e1a72f7d0d34b69232690ce704f6 on E5-2683-v3:
```
Cores	Baseline	+Batching		+3 QP
1		6.80		17.6			27.4
3		13.35		44.7			69.6
5		16.65		69.5			97.5
```

## dc49e27981f7928e5271cd2770d0d0370e59e01a on E5-2683-v3
This is after streamlining with the HERD code. Single-core performance with
3 QPs per port dropped to 24.4 Mops, probably due to less aggressive client-side
pipelining.
