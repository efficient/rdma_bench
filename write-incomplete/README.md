This proof-of-concept shows that an RDMA write completion at the client
does not imply visibility to remote CPU. The data may be in the remote
NIC's cache. This is allowed by the InfiniBand spec
(version 1.3, Section 9.7.5.1.6).

This PoC works with two ConnectX-5 RoCE NICs on one machine, with a
client and server thread attached to different NICs
