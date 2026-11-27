#!/usr/bin/env python3
"""
Shadow Hearts: Nocturne — Zero-Dependency Co-op Signaling & UDP Relay Server

Provides:
  1. 6-character unambiguous Room Code allocation (e.g. K7M4PX)
  2. Public IP:Port reflection (STUN-like) for simultaneous UDP hole-punching
  3. Automatic transparent UDP packet relay fallback for symmetric NATs

Usage:
  python coop_relay.py --host 0.0.0.0 --port 47191
"""

from __future__ import annotations

import argparse
import random
import signal
import socket
import struct
import sys
import time
from dataclasses import dataclass, field
from typing import Dict, Optional, Tuple

MAGIC = b"NOCT"
PROTO_VERSION = 1
HEADER_FMT = "<4sBBBBII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 16 bytes

PKT_RELAY_CREATE_ROOM = 0x80
PKT_RELAY_ROOM_CREATED = 0x81
PKT_RELAY_JOIN_ROOM = 0x82
PKT_RELAY_JOIN_OK = 0x83
PKT_RELAY_PEER_ENDPOINT = 0x84
PKT_RELAY_JOIN_ERR = 0x85
PKT_RELAY_FORWARD = 0x86
PKT_RELAY_KEEPALIVE = 0x87
PKT_RELAY_LEAVE = 0x88

FLAG_RELAYED = 0x01

ERR_NOT_FOUND = 1
ERR_ROOM_FULL = 2

# Unambiguous 32-char alphabet (no 0/O/1/I/L)
CODE_ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ"
ROOM_TTL_SECONDS = 300.0
PEER_TTL_SECONDS = 60.0


def fnv1a_32(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def ipv4_to_u32(ip_str: str) -> int:
    try:
        return struct.unpack("!I", socket.inet_aton(ip_str))[0]
    except OSError:
        return 0


def pack_header(pkt_type: int, sender_id: int, flags: int, room_id: int, seq: int = 0) -> bytes:
    return struct.pack(HEADER_FMT, MAGIC, PROTO_VERSION, pkt_type, sender_id, flags, room_id, seq)


@dataclass
class Peer:
    slot: int
    addr: Tuple[str, int]
    name: str
    last_seen: float = field(default_factory=time.monotonic)


@dataclass
class Room:
    code: str
    room_id: int
    game_id: int
    max_players: int
    peers: Dict[int, Peer] = field(default_factory=dict)
    created_at: float = field(default_factory=time.monotonic)
    last_activity: float = field(default_factory=time.monotonic)


class CoopRelayServer:
    def __init__(self, host: str = "0.0.0.0", port: int = 47191) -> None:
        self.host = host
        self.port = port
        self.rooms_by_code: Dict[str, Room] = {}
        self.rooms_by_id: Dict[int, Room] = {}
        self.addr_to_room: Dict[Tuple[str, int], Tuple[int, int]] = {}  # addr -> (room_id, slot)
        self.sock: Optional[socket.socket] = None
        self.running = False
        self.rng = random.SystemRandom()

    def _gen_code(self) -> str:
        for _ in range(128):
            code = "".join(self.rng.choice(CODE_ALPHABET) for _ in range(6))
            rid = fnv1a_32(code.encode("ascii"))
            if code not in self.rooms_by_code and rid not in self.rooms_by_id and rid != 0:
                return code
        raise RuntimeError("Failed to allocate unique room code")

    def _cleanup(self, now: float) -> None:
        dead_rooms = []
        for rid, room in list(self.rooms_by_id.items()):
            dead_slots = [s for s, p in room.peers.items() if now - p.last_seen > PEER_TTL_SECONDS]
            for s in dead_slots:
                peer = room.peers.pop(s)
                self.addr_to_room.pop(peer.addr, None)
            if not room.peers or (now - room.last_activity > ROOM_TTL_SECONDS):
                dead_rooms.append(rid)
        for rid in dead_rooms:
            room = self.rooms_by_id.pop(rid, None)
            if room:
                self.rooms_by_code.pop(room.code, None)
                for p in room.peers.values():
                    self.addr_to_room.pop(p.addr, None)

    def _send(self, payload: bytes, addr: Tuple[str, int]) -> None:
        if self.sock is not None:
            try:
                self.sock.sendto(payload, addr)
            except OSError:
                pass

    def handle_packet(self, data: bytes, addr: Tuple[str, int]) -> None:
        if len(data) < HEADER_SIZE:
            return
        magic, ver, pkt_type, sender_id, flags, room_id, seq = struct.unpack_from(HEADER_FMT, data, 0)
        if magic != MAGIC or ver != PROTO_VERSION:
            return
        body = data[HEADER_SIZE:]
        now = time.monotonic()

        if pkt_type == PKT_RELAY_CREATE_ROOM:
            game_id = body[0] if len(body) >= 1 else 0
            max_players = max(2, min(4, body[1] if len(body) >= 2 else 4))
            name_len = body[2] if len(body) >= 3 else 0
            name = body[3 : 3 + name_len].decode("utf-8", errors="replace")[:24] or "Host"

            # Remove any previous room registration for this address
            prev = self.addr_to_room.pop(addr, None)
            if prev:
                old_room = self.rooms_by_id.get(prev[0])
                if old_room:
                    old_room.peers.pop(prev[1], None)

            code = self._gen_code()
            rid = fnv1a_32(code.encode("ascii"))
            room = Room(code=code, room_id=rid, game_id=game_id, max_players=max_players)
            room.peers[0] = Peer(slot=0, addr=addr, name=name, last_seen=now)
            self.rooms_by_code[code] = room
            self.rooms_by_id[rid] = room
            self.addr_to_room[addr] = (rid, 0)

            pub_ip = ipv4_to_u32(addr[0])
            pub_port = addr[1]
            resp_body = code.encode("ascii") + struct.pack("<IIH", rid, pub_ip, pub_port)
            self._send(pack_header(PKT_RELAY_ROOM_CREATED, 0xFF, 0, rid, seq) + resp_body, addr)
            return

        if pkt_type == PKT_RELAY_JOIN_ROOM:
            if len(body) < 6:
                return
            code = body[:6].decode("ascii", errors="ignore").upper()
            name_len = body[6] if len(body) >= 7 else 0
            name = body[7 : 7 + name_len].decode("utf-8", errors="replace")[:24] or "Guest"

            room = self.rooms_by_code.get(code)
            if room is None:
                self._send(pack_header(PKT_RELAY_JOIN_ERR, 0xFF, 0, 0, seq) + bytes([ERR_NOT_FOUND]), addr)
                return

            # Reuse slot if same addr reconnects
            assigned_slot = None
            for s, p in room.peers.items():
                if p.addr == addr:
                    assigned_slot = s
                    p.name = name
                    p.last_seen = now
                    break
            if assigned_slot is None:
                for s in range(1, room.max_players):
                    if s not in room.peers:
                        assigned_slot = s
                        break
            if assigned_slot is None:
                self._send(pack_header(PKT_RELAY_JOIN_ERR, 0xFF, 0, room.room_id, seq) + bytes([ERR_ROOM_FULL]), addr)
                return

            room.peers[assigned_slot] = Peer(slot=assigned_slot, addr=addr, name=name, last_seen=now)
            room.last_activity = now
            self.addr_to_room[addr] = (room.room_id, assigned_slot)

            host_peer = room.peers.get(0)
            host_ip = ipv4_to_u32(host_peer.addr[0]) if host_peer else 0
            host_port = host_peer.addr[1] if host_peer else 0

            others = [p for s, p in room.peers.items() if s != assigned_slot]
            resp = bytearray(struct.pack("<IBIHB", room.room_id, assigned_slot, host_ip, host_port, len(others)))
            for p in others:
                resp += struct.pack("<BIH", p.slot, ipv4_to_u32(p.addr[0]), p.addr[1])
            self._send(pack_header(PKT_RELAY_JOIN_OK, 0xFF, 0, room.room_id, seq) + bytes(resp), addr)

            # Notify existing peers of the new joiner's public endpoint for simultaneous UDP hole-punching
            joiner_info = struct.pack("<BIH", assigned_slot, ipv4_to_u32(addr[0]), addr[1])
            notify = pack_header(PKT_RELAY_PEER_ENDPOINT, 0xFF, 0, room.room_id, seq) + joiner_info
            for p in others:
                self._send(notify, p.addr)
            return

        if pkt_type == PKT_RELAY_FORWARD:
            if len(body) < 1 + HEADER_SIZE:
                return
            room = self.rooms_by_id.get(room_id)
            if room is None:
                return
            target_slot = body[0]
            inner = bytearray(body[1:])
            if inner[:4] != MAGIC:
                return
            inner[7] |= FLAG_RELAYED  # mark relayed
            room.last_activity = now
            # Authenticate sender endpoint before forwarding (blocks off-path peer spoofing)
            sender_peer = room.peers.get(sender_id)
            if sender_peer is None or sender_peer.addr != addr:
                return
            sender_peer.last_seen = now
            for s, p in room.peers.items():
                if s == sender_id:
                    continue
                if target_slot == 0xFF or target_slot == s:
                    self._send(bytes(inner), p.addr)
            return

        if pkt_type == PKT_RELAY_KEEPALIVE:
            room = self.rooms_by_id.get(room_id)
            if room and sender_id in room.peers:
                peer = room.peers[sender_id]
                # Allow NAT ephemeral port rebind from the same source IP; reject foreign IPs
                if peer.addr != addr:
                    if peer.addr[0] != addr[0]:
                        return
                    self.addr_to_room.pop(peer.addr, None)
                    peer.addr = addr
                    self.addr_to_room[addr] = (room_id, sender_id)
                peer.last_seen = now
                room.last_activity = now
            return

        if pkt_type == PKT_RELAY_LEAVE:
            room = self.rooms_by_id.get(room_id)
            if room and sender_id in room.peers:
                peer = room.peers.pop(sender_id)
                self.addr_to_room.pop(peer.addr, None)
                if not room.peers:
                    self.rooms_by_id.pop(room_id, None)
                    self.rooms_by_code.pop(room.code, None)

    def serve_forever(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((self.host, self.port))
        self.sock.settimeout(0.5)
        self.running = True
        print(f"[coop_relay] Listening on UDP {self.host}:{self.port}", flush=True)
        last_clean = time.monotonic()
        try:
            while self.running:
                try:
                    data, addr = self.sock.recvfrom(65535)
                    self.handle_packet(data, addr)
                except socket.timeout:
                    pass
                except OSError:
                    if not self.running:
                        break
                now = time.monotonic()
                if now - last_clean >= 10.0:
                    self._cleanup(now)
                    last_clean = now
        finally:
            if self.sock:
                self.sock.close()
                self.sock = None

    def stop(self) -> None:
        self.running = False


def main() -> int:
    ap = argparse.ArgumentParser(description="Shadow Hearts: Nocturne Co-op Signaling & UDP Relay Server")
    ap.add_argument("--host", default="0.0.0.0", help="Bind address (default: 0.0.0.0)")
    ap.add_argument("--port", type=int, default=47191, help="UDP port (default: 47191)")
    args = ap.parse_args()

    srv = CoopRelayServer(args.host, args.port)

    def _sig(_signum, _frame):
        srv.stop()

    signal.signal(signal.SIGINT, _sig)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _sig)

    srv.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
