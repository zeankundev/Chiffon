# P2c core networking

## Placement and interfaces

The kernel owns NIC discovery, descriptor rings, packet buffers, interface selection and the Ethernet, ARP, IPv4, ICMP, UDP and TCP paths. Applications enter through the socket layer or the existing network syscalls. Both use the in-kernel network stack. A device attaches a persistent `struct netdev` and supplies transmit, receive polling and optional link-state callbacks. The stack borrows a received frame only during `net_input`. The transmit callback must finish consuming the frame or copy it before returning. Drivers own all DMA addresses. Maintaining this interface preserves existing syscall numbers and keeps packet handling independent of optional user-space services.

## QEMU verification

Build the Demo images using the P0 instructions. From that export, run:

```sh
for arch in x86_64 aarch64; do
    python3 tools/release/network_smoke.py "$arch" "build/demo/$arch-qemu"
done
```

The runner uses QEMU user networking with the board's virtio NIC and an ephemeral disk overlay. It checks the gateway reported by the guest and receives four ICMP responses from that gateway, exercises framebuffer server failure and restart, then receives four more responses. This scenario exercises the user network syscalls and kernel NIC transport. Physical NICs, external networking, throughput and TCP interoperability require separate evidence.
