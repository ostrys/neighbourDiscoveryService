
# Neighbor Discovery Service

A background service that discovers neighbors running the same service in connected Ethernet networks.

## Overview

The service broadcasts discovery packets (L2 Ethernet frames) on all active Ethernet interfaces and listens for broadcasts from neighbors. Service maintains an active neighbor list with connection details (MAC, IPv4, IPv6) and provides a CLI tool to query the current state.

Full requirements defined in [requirements.md](./requirements.md).

## Architecture

### Components

1. **Background Service** (`ndisc_svc`)
   - Broadcasts discovery frames every 5 seconds (`SEND_INTERVAL_SEC` defined in `common.hpp`)
   - Receives and processes neighbor broadcasts
   - Maintains active neighbor list (using `std::unordered_map` of structs)
   - Provides IPC server for queries (Unix domain socket)

2. **CLI Tool** (`ndisc_cli`)
   - Queries service via Unix socket
   - Displays all active neighbors and connections
   - Shows interface names, MAC addresses, and IP addresses

### Protocol

**Ethertype**: `0x88B5` [IEEE 802 Local Experimental](https://www.iana.org/assignments/ieee-802-numbers/ieee-802-numbers.xhtml)


**Ethernet frame structure**:
```
+------------------+--------+  Header
| Destination MAC  | 6 B    |
| Source MAC       | 6 B    |
| EtherType        | 2 B    |
+------------------+--------+  Payload
| Machine ID       | 32 B   |
| IPv4 Address     | 4  B   |
| IPv6 Address     | 16 B   |
+------------------+--------+
Total: 66 bytes (14-byte Ethernet header + 52-byte payload).
```

Machine ID is read from `/etc/machine-id` (Ubuntu standard).

### Neighbor Discovery Internals

#### Interface enumeration (kernel view)
The service refreshes its interface list with `getifaddrs()` before each send interval. Interfaces are kept only when:
- Flags include `IFF_UP` **and** `IFF_RUNNING`
- Name is not `lo`
- Link-layer data is present (MAC + `ifindex` from `AF_PACKET`)
- Optional address data is collected from `AF_INET` (IPv4) and `AF_INET6` (IPv6)

| ifindex | name | MAC | IPv4 | IPv6 |
|---------|------|-----|------|------|
| 2 | eth0 | aa:bb:cc:dd:ee:ff | 192.0.2.10/24 | 2001:db8::1234/64 |

Interfaces missing link-layer info are dropped, and disappearing interfaces are closed and removed.

#### Discovery sockets and syscalls
Each monitored interface owns a raw socket pinned to that device and protocol:

| Step | Syscall/API | Key parameters / behavior |
|------|-------------|---------------------------|
| 1 | `socket(AF_PACKET, SOCK_RAW, htons(0x88B5))` | Raw L2 socket for EtherType `0x88B5` (NEIGHBOR_DISC). |
| 2 | `setsockopt(SO_RCVBUF, 8 MB)` | Enlarges per-interface receive buffer to a theoretical ~127,100 frames (8 MiB = 8,388,608 bytes; calculation uses the NeighborPayload-sized frame: 14-byte header + 52-byte payload = 66 bytes, above the 64-byte Ethernet minimum; ignores per-packet kernel overhead such as sk_buff metadata). |
| 3 | `bind(sock, sockaddr_ll{ sll_ifindex, proto=0x88B5 })` | Pins socket to interface index + EtherType. |
| 4 | `select()` main loop | Watches all interface sockets + IPC server with timeout to next send. |
| 5 | `recv(MSG_DONTWAIT)` up to `MAX_PKTS_PER_ITER=100000` | Non-blocking drain per interface; ceiling (`MAX_PKTS_PER_ITER` in `common.hpp`) intentionally below the theoretical ~127,100-frame buffer (≈27,100-frame headroom—~21.3% of capacity or ~27.1% of the 100k drain limit) to cap per-iteration work and avoid per-interface starvation while still draining large bursts. |
| 6 | `sendto()` | Broadcasts frame every `SEND_INTERVAL_SEC=5s` per interface using `sockaddr_ll` with broadcast MAC. |

#### Packet layout and flow
- Destination MAC: ff:ff:ff:ff:ff:ff (broadcast)
- Source MAC: interface MAC
- EtherType: `0x88B5` (NEIGHBOR_DISC)
- Payload (`NeighborPayload`, 52 B):
  - `machineId` (32 B) read from `/etc/machine-id`
  - IPv4 (4 B, network byte order via `htonl`)
  - IPv6 (16 B raw bytes)

Flow example:
1. Every 5 seconds, the service rebuilds the interface list, regenerates frames with the current MAC/IP data, and calls `sendto()` on each bound raw socket.
2. The continuous `select()` loop wakes whenever discovery frames arrive; for each ready interface socket it drains up to `MAX_PKTS_PER_ITER` incoming frames with `recv(MSG_DONTWAIT)` and forwards valid neighbor payloads to storage.
3. After receives are drained or the timeout expires, control returns to the top of the loop; the next wake aligns with either another incoming frame or the next 5-second send deadline while also polling the IPC file descriptor.


## Project Structure

- `src/main.cpp` — service entrypoint; main `select()` loop; send/receive scheduling
- `src/utils/interfaces.*` — interface discovery and raw socket setup per interface
- `src/utils/utils.*` — timestamp/log helpers, printing helpers
- `src/neighbor/neighbors.*` — neighbor/connection storage and timeout cleanup
- `src/ipc/server.*` — Unix domain socket IPC server for CLI queries
- `src/cli.cpp` — CLI client that queries IPC server and prints neighbors

## Building

Requires g++ with C++17 support:

```bash
make
```

This produces 2 executables:
- `ndisc_svc` - background service
- `ndisc_cli` - query tool


## Running
### Start Service

Must run as root for raw socket access:

```bash
sudo ./ndisc_svc
```

Or run in background with logging:

```bash
sudo -b ./ndisc_svc >> service.log 2>&1
```

### Query Neighbors

The CLI connects to a Unix domain socket at `/tmp/neighbor_discovery.sock`. Service creates the socket as root with restrictive permissions, `sudo` is needed:

```bash
sudo ./ndisc_cli
```

### Stop Service

```bash
sudo pkill ndisc_svc
```

## CLI Example Output

CLI program example output of a machine, which has 2 neighbors, each with 2 active connections:

```
=== Neighbor 1 ===
Machine ID: 9dceb1c7a7cd4121a5901618be612dfb

  Connection 1:
    Local interface: enp0s8
    Remote MAC:      08:00:27:df:e7:ba
    Remote IPv4:     none
    Remote IPv6:     fe80:0000:0000:0000:1d77:a60f:d7c2:9922

  Connection 2:
    Local interface: enp0s9
    Remote MAC:      08:00:27:da:43:fb
    Remote IPv4:     none
    Remote IPv6:     none

=== Neighbor 2 ===
Machine ID: 1c13c3d37e874add8394969b2cd7a7d9

  Connection 1:
    Local interface: enp0s8
    Remote MAC:      08:00:27:13:8c:73
    Remote IPv4:     192.168.56.10
    Remote IPv6:     fe80:0000:0000:0000:9b87:2f1f:de64:887b

  Connection 2:
    Local interface: enp0s9
    Remote MAC:      08:00:27:56:bd:e9
    Remote IPv4:     192.200.200.20
    Remote IPv6:     none

Total neighbors: 2
```


## Performance Tuning

The service is optimized for 10,000+ neighbors:

- **Socket buffer**: increased receive buffer size to 8 MB via `setsockopt()` (queues up to ~130,000 packets).
- **Memory**: pre-allocated hash maps to avoid rehashing (for neighbor and connection storage).

## Technical Details

- **No threads**: Uses `select()` for multiplexing
- **No exceptions**: Error handling via return codes
- **C++17**: Standard library only (libstdc++)
- **Platform**:  Ubuntu 24.04.3 LTS (at time of writing latest Ubuntu LTS, tested using VirtualBox)
