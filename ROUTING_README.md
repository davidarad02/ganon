# Ganon Routing Protocol (AODV-Based)

In a decentralized mesh network, keeping a fully populated routing table pointing to every single node creates severe scalability issues. Previously, Ganon maintained a proactive routing mechanism using recursive `PEER_INFO` floods which caused telemetry packet storms as the network grew.

To address this, Ganon has been migrated to a modern **Reactive Routing Algorithm**, heavily inspired by **AODV** (Ad-hoc On-Demand Distance Vector).

## Why AODV?

Reactive protocols only attempt to discover and maintain routes to destinations when there is active data to be sent. If a node never communicates with node `X`, it never needs to store the routing path to `X`.
This offers several massive benefits for Ganon:
1. **No Telemetry Overload**: Idle nets require 0 bandwidth.
2. **Scalable Routing Tables**: Only actively used destinations are stored in memory.
3. **Loop-Prevention incorporated**: Broadcast floods are controlled via sequence tracking.

## Core Mechanisms

### 1. Route Discovery (RREQ & RREP)
When a node wants to send a message to node `D` and has no route, it broadcasts a Route Request (`RREQ`):
- The `RREQ` floods the network iteratively.
- To prevent broadcast storms, every node caches `RREQ` IDs. Duplicate `RREQ`s are silently dropped.
- As the `RREQ` moves forward, every intermediate node learns a **Reverse Path** back to the sender.

Once the target node `D` receives the `RREQ`:
- `D` generates a unicast Route Reply (`RREP`) addressed to the original sender.
- The `RREP` travels back exactly along the reverse path previously established.
- As the `RREP` travels back, every intermediate node learns the **Forward Path** to `D`.
- When the `RREP` reaches the source, full bidirectional communication is natively established!

### 2. Mesh Topology & Background Learning
Ganon-AODV is efficient: practically every single valid message in the network inherently teaches intermediate nodes!
Whenever *any* valid message is received on a node, Ganon's `ROUTING__on_message` transparently updates the reverse path to the message's original sender, keeping paths fresh and implicitly updating shorter paths using evaluated hops without relying on periodic polling.

### 3. Edge Cases Handled

* **Handling Dead Routes (Node Disconnections):**
  When a node detects a direct neighbor has disconnected, it immediately cleans up its routing table and broadcasts a `MSG__RERR` (Route Error). This instructs all other nodes to remove paths that relied on the missing neighbor. The next attempt to send data to the dropped node will seamlessly trigger a fresh `RREQ` to discover a new path!

* **Two Networks Combining:**
  When two meshes fuse together, `NODE_INIT` connects two nodes locally. Unlike a proactive protocol that immediately forces a massive data dump between the two networks (causing a traffic spike), AODV waits asynchronously. The two networks only start passing routing data *when* nodes from either side attempt to query each other using `RREQ`s. The networks mesh organically with near-zero initial handshake payload.

* **Broadcast Storm Prevention:**
  If the network has physical loops (A - B - C - A), `RREQ` floods could circle endlessly. Ganon employs a thread-safe LRU structure `seen_msgs_cache` to block redundant broadcast IDs from passing twice, killing circular storms immediately.
