import threading
from enum import IntEnum
from typing import Dict, List, Optional


class RouteType(IntEnum):
    DIRECT = 0
    VIA_HOP = 1


class RouteEntry:
    def __init__(self, node_id: int, next_hop_node_id: int, route_type: RouteType, fd: Optional[int] = None, hop_count: int = 1):
        self.node_id = node_id
        self.next_hop_node_id = next_hop_node_id
        self.route_type = route_type
        self.fd = fd
        self.hop_count = hop_count


class RoutingTable:
    def __init__(self):
        self._entries: Dict[int, RouteEntry] = {}
        self._lock = threading.Lock()

    def add_direct(self, node_id: int, fd: int) -> bool:
        with self._lock:
            # Check for existing direct entry
            if node_id not in self._entries:
                self._entries[node_id] = []
            
            for entry in self._entries[node_id]:
                if entry.route_type == RouteType.DIRECT:
                    entry.fd = fd
                    return True
            
            self._entries[node_id].append(RouteEntry(node_id, node_id, RouteType.DIRECT, fd, 1))
            return True

    def add_via_hop(self, node_id: int, next_hop_node_id: int, hop_count: int) -> bool:
        with self._lock:
            if node_id not in self._entries:
                self._entries[node_id] = []
            
            for entry in self._entries[node_id]:
                if entry.next_hop_node_id == next_hop_node_id:
                    entry.hop_count = hop_count
                    return True
            
            self._entries[node_id].append(RouteEntry(node_id, next_hop_node_id, RouteType.VIA_HOP, None, hop_count))
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
            to_remove_nodes = []
            for node_id in self._entries:
                old_list = self._entries[node_id]
                new_list = [e for e in old_list if e.next_hop_node_id != via_node_id]
                if len(old_list) != len(new_list):
                    removed += (len(old_list) - len(new_list))
                    self._entries[node_id] = new_list
                if not self._entries[node_id]:
                    to_remove_nodes.append(node_id)
            for node_id in to_remove_nodes:
                del self._entries[node_id]
        return removed

    def get_all_routes(self, node_id: int) -> List[RouteEntry]:
        with self._lock:
            return list(self._entries.get(node_id, []))

    def get_route(self, node_id: int) -> Optional[RouteEntry]:
        with self._lock:
            entries = self._entries.get(node_id, [])
            if not entries:
                return None
            # Return any direct route first, otherwise first available
            for e in entries:
                if e.route_type == RouteType.DIRECT:
                    return e
            return entries[0]


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
            print(f"Routing table ({len(self._entries)} unique nodes):")
            for node_id in self._entries:
                for entry in self._entries[node_id]:
                    if entry.route_type == RouteType.DIRECT:
                        print(f"  -> node {node_id}: direct (fd={entry.fd})")
                    else:
                        print(f"  -> node {node_id}: via node {entry.next_hop_node_id} (hops={entry.hop_count})")
