import threading
from enum import IntEnum
from typing import Dict, Optional


class RouteType(IntEnum):
    DIRECT = 0
    VIA_HOP = 1


class RouteEntry:
    def __init__(self, node_id: int, next_hop_node_id: int, route_type: RouteType, fd: Optional[int] = None):
        self.node_id = node_id
        self.next_hop_node_id = next_hop_node_id
        self.route_type = route_type
        self.fd = fd


class RoutingTable:
    def __init__(self):
        self._entries: Dict[int, RouteEntry] = {}
        self._lock = threading.Lock()

    def add_direct(self, node_id: int, fd: int) -> bool:
        with self._lock:
            entry = RouteEntry(node_id, node_id, RouteType.DIRECT, fd)
            self._entries[node_id] = entry
            return True

    def add_via_hop(self, node_id: int, next_hop_node_id: int) -> bool:
        with self._lock:
            entry = RouteEntry(node_id, next_hop_node_id, RouteType.VIA_HOP)
            self._entries[node_id] = entry
            return True

    def remove(self, node_id: int) -> bool:
        with self._lock:
            if node_id in self._entries:
                del self._entries[node_id]
                return True
            return False

    def remove_via_node(self, via_node_id: int) -> int:
        removed = 0
        with self._lock:
            to_remove = []
            for node_id, entry in self._entries.items():
                if entry.route_type == RouteType.VIA_HOP and entry.next_hop_node_id == via_node_id:
                    to_remove.append(node_id)
            for node_id in to_remove:
                del self._entries[node_id]
                removed += 1
        return removed

    def get_route(self, node_id: int) -> Optional[RouteEntry]:
        with self._lock:
            return self._entries.get(node_id)

    def is_direct(self, node_id: int) -> bool:
        entry = self.get_route(node_id)
        if entry:
            return entry.route_type == RouteType.DIRECT
        return False

    def get_next_hop(self, node_id: int) -> Optional[int]:
        entry = self.get_route(node_id)
        if entry:
            return entry.next_hop_node_id
        return None

    def log_table(self):
        with self._lock:
            print(f"Routing table ({len(self._entries)} entries):")
            for node_id, entry in self._entries.items():
                if entry.route_type == RouteType.DIRECT:
                    print(f"  -> node {node_id}: direct (fd={entry.fd})")
                else:
                    print(f"  -> node {node_id}: via node {entry.next_hop_node_id}")
