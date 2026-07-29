#!/usr/bin/env python3
"""Black-box, three-client compatibility tests for tp_multiplayer_relay."""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import time
import unittest
from pathlib import Path
from typing import Any


GAMEPLAY_ROUTE_TYPES = (
    "event_bit",
    "tbox_bit",
    "switch_bit",
    "room_switch_bit",
    "item_bit",
    "dungeon_item_bit",
    "save_snapshot",
    "key_num",
    "light_drop_num",
    "light_drop_get_flag",
    "max_life_update",
    "bottle_slots",
    "bomb_bag_slot",
    "rupee_count",
    "item_get",
    "rando_item_get",
    "item_first_bit",
    "collect_crystal",
    "collect_mirror",
    "dark_clear_lv",
    "transform_lv",
    "region_bit",
    "collect",
    "visited_room",
    "letter_get",
    "presence",
    "progression_state",
    "puppet_preference",
    "midna_preference",
    "midna_pose",
    "pvp_hit",
    "ganondorf_owner_claim",
    "ganondorf_owner",
    "ganondorf_hit",
    "ganondorf_reaction",
    "ganondorf_player_damage",
    "ganondorf_state",
    "ooccoo_state",
)

HOST_CONTROL_TYPES = (
    "dummy_model",
    "sync_flags",
    "sync_world",
    "remote_collision",
    "pvp_enabled",
)

UDP_HEADER = struct.Struct("<4sBBHIHHIIH32s")
UDP_ACK = struct.Struct("<IB32sB")
UDP_REGISTER_TYPE = 6


def udp_packet(
    packet_type: int,
    sender_id: str,
    payload: bytes,
    sequence: int = 1,
) -> bytes:
    sender = sender_id.encode("utf-8")[:31].ljust(32, b"\0")
    return UDP_HEADER.pack(
        b"DMPU",
        1,
        packet_type,
        UDP_HEADER.size,
        sequence,
        0,
        1,
        len(payload),
        len(payload),
        len(payload),
        sender,
    ) + payload


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class RelayClient:
    def __init__(self, port: int) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=2.0)
        self.sock.settimeout(2.0)
        self.buffer = bytearray()
        self.port = port
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(("127.0.0.1", 0))
        self.udp.settimeout(0.3)

    def close(self) -> None:
        self.sock.close()
        self.udp.close()

    def send(self, message: dict[str, Any]) -> None:
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n"
        self.sock.sendall(payload)

    def send_bytes(self, payload: bytes) -> None:
        self.sock.sendall(payload)

    def receive(self, timeout: float = 2.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out waiting for relay message")
            self.sock.settimeout(remaining)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("relay closed the connection")
            self.buffer.extend(chunk)
        line, _, rest = self.buffer.partition(b"\n")
        self.buffer = bytearray(rest)
        return json.loads(line)

    def expect_type(self, message_type: str, timeout: float = 2.0) -> dict[str, Any]:
        message = self.receive(timeout)
        if message.get("type") != message_type:
            raise AssertionError(f"expected {message_type!r}, received {message!r}")
        return message

    def expect_error(self, error: str, timeout: float = 2.0) -> None:
        message = self.expect_type("error", timeout)
        if message.get("error") != error:
            raise AssertionError(f"expected error {error!r}, received {message!r}")

    def register_udp(self, welcome: dict[str, Any]) -> None:
        token = welcome["udp_token"].encode("utf-8")
        self.udp.sendto(
            udp_packet(UDP_REGISTER_TYPE, welcome["client_id"], token, 0),
            ("127.0.0.1", self.port),
        )
        self.expect_type("udp_ready")


class RelayProcess:
    def __init__(self, executable: Path) -> None:
        self.port = reserve_port()
        self.process = subprocess.Popen(
            [
                str(executable),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--public-host",
                "127.0.0.1",
                "--public-port",
                str(self.port),
                "--hello-timeout-ms",
                "200",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env={**os.environ, "DUSK_MP_RELAY_PACKET_TRACE": "0"},
        )
        if not self.process.stdout:
            raise RuntimeError("relay stdout was not captured")
        self.relay_code_line = self.process.stdout.readline().strip()
        if not self.relay_code_line.startswith("Relay code: TP1-"):
            raise RuntimeError(
                f"relay did not print a usable endpoint code: {self.relay_code_line!r}"
            )
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                output = self.process.stdout.read() if self.process.stdout else ""
                raise RuntimeError(f"relay exited during startup:\n{output}")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.05):
                    return
            except OSError:
                time.sleep(0.02)
        raise TimeoutError("relay did not start listening")

    def close(self) -> None:
        self.process.terminate()
        try:
            self.process.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3.0)
        if self.process.stdout:
            self.process.stdout.close()


def hello(
    name: str,
    room: str = "phase12",
    password: str = "secret",
    action: str = "join",
    settings: dict[str, bool] | None = None,
) -> dict[str, Any]:
    message: dict[str, Any] = {
        "type": "hello",
        "protocol_version": 2,
        "action": action,
        "room_id": room,
        "password": password,
        "name": name,
        "session_id": f"session-{name}",
        "want_puppet": True,
        "want_midna": False,
    }
    if settings is not None:
        message["settings"] = settings
    return message


class RelayTests(unittest.TestCase):
    executable: Path
    relay: RelayProcess
    clients: list[RelayClient]

    @classmethod
    def setUpClass(cls) -> None:
        cls.relay = RelayProcess(cls.executable)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.relay.close()

    def setUp(self) -> None:
        self.clients = []
        self.created_rooms: set[str] = set()

    def tearDown(self) -> None:
        for client in self.clients:
            client.close()

    def client(self) -> RelayClient:
        client = RelayClient(self.relay.port)
        self.clients.append(client)
        return client

    def test_relay_operator_receives_endpoint_code(self) -> None:
        self.assertTrue(self.relay.relay_code_line.startswith("Relay code: TP1-"))

    def test_authenticated_udp_visual_routing(self) -> None:
        sender, sender_welcome = self.join("UdpSender", "udp-visual")
        receiver, receiver_welcome = self.join("UdpReceiver", "udp-visual")
        sender.expect_type("peer_joined")
        sender.register_udp(sender_welcome)
        receiver.register_udp(receiver_welcome)

        pose = udp_packet(2, sender_welcome["client_id"], b"visual-pose", 17)
        sender.udp.sendto(pose, ("127.0.0.1", self.relay.port))
        routed, _ = receiver.udp.recvfrom(2048)
        self.assertEqual(routed, pose)

        spoof = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            spoof.sendto(pose, ("127.0.0.1", self.relay.port))
            with self.assertRaises(socket.timeout):
                receiver.udp.recvfrom(2048)
        finally:
            spoof.close()

        sender.send({"type": "presence", "stage": "F_SP108"})
        receiver.expect_type("presence")
        receiver.send({"type": "presence", "stage": "D_MN05"})
        sender.expect_type("presence")
        sender.udp.sendto(
            udp_packet(2, sender_welcome["client_id"], b"wrong-stage", 18),
            ("127.0.0.1", self.relay.port),
        )
        with self.assertRaises(socket.timeout):
            receiver.udp.recvfrom(2048)

        receiver.send({"type": "presence", "stage": "F_SP108"})
        sender.expect_type("presence")
        receiver.send(
            {"type": "puppet_preference", "want_puppet": False, "want_midna": False}
        )
        sender.expect_type("puppet_preference")
        sender.udp.sendto(
            udp_packet(2, sender_welcome["client_id"], b"opted-out", 19),
            ("127.0.0.1", self.relay.port),
        )
        with self.assertRaises(socket.timeout):
            receiver.udp.recvfrom(2048)

    def test_udp_ack_routing_and_authenticated_rebind(self) -> None:
        sender, sender_welcome = self.join("UdpAckSender", "udp-ack-rebind")
        receiver, receiver_welcome = self.join("UdpAckReceiver", "udp-ack-rebind")
        sender.expect_type("peer_joined")
        sender.register_udp(sender_welcome)
        receiver.register_udp(receiver_welcome)

        acked_sender = (
            sender_welcome["client_id"].encode("utf-8")[:31].ljust(32, b"\0")
        )
        ack_payload = UDP_ACK.pack(17, 2, acked_sender, 0)
        ack = udp_packet(
            5,
            receiver_welcome["client_id"],
            ack_payload,
            17,
        )
        receiver.udp.sendto(ack, ("127.0.0.1", self.relay.port))
        routed_ack, _ = sender.udp.recvfrom(2048)
        self.assertEqual(routed_ack, ack)

        rebound = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rebound.bind(("127.0.0.1", 0))
        try:
            rebound.sendto(
                udp_packet(
                    UDP_REGISTER_TYPE,
                    sender_welcome["client_id"],
                    sender_welcome["udp_token"].encode("utf-8"),
                    0,
                ),
                ("127.0.0.1", self.relay.port),
            )
            time.sleep(0.15)

            rebound_pose = udp_packet(
                2, sender_welcome["client_id"], b"rebound-pose", 18
            )
            rebound.sendto(rebound_pose, ("127.0.0.1", self.relay.port))
            routed_pose, _ = receiver.udp.recvfrom(2048)
            self.assertEqual(routed_pose, rebound_pose)

            sender.udp.sendto(
                udp_packet(2, sender_welcome["client_id"], b"old-endpoint", 19),
                ("127.0.0.1", self.relay.port),
            )
            with self.assertRaises(socket.timeout):
                receiver.udp.recvfrom(2048)
        finally:
            rebound.close()

    def join(self, name: str, room: str = "phase12") -> tuple[RelayClient, dict[str, Any]]:
        client = self.client()
        action = "join" if room in self.created_rooms else "create"
        client.send(hello(name, room, action=action))
        welcome = client.expect_type("welcome")
        self.created_rooms.add(room)
        return client, welcome

    def test_three_client_room_and_disconnect(self) -> None:
        first, first_welcome = self.join("First", "three-clients")
        self.assertEqual(first_welcome["peers"], [])

        second, second_welcome = self.join("Second", "three-clients")
        first_joined = first.expect_type("peer_joined")
        self.assertEqual(first_joined["client_id"], second_welcome["client_id"])
        self.assertEqual(second_welcome["peers"][0]["client_id"], first_welcome["client_id"])

        third, third_welcome = self.join("Third", "three-clients")
        self.assertEqual(len(third_welcome["peers"]), 2)
        first.expect_type("peer_joined")
        second.expect_type("peer_joined")

        third.close()
        self.clients.remove(third)
        self.assertEqual(first.expect_type("peer_left")["client_id"], third_welcome["client_id"])
        self.assertEqual(second.expect_type("peer_left")["client_id"], third_welcome["client_id"])

    def test_fragmented_and_coalesced_input(self) -> None:
        client = self.client()
        encoded = json.dumps(hello("Fragmented", "framing", action="create")).encode("utf-8") + b"\n"
        for byte in encoded:
            client.send_bytes(bytes([byte]))
        client.expect_type("welcome")

        client.send_bytes(b'{"type":"ping"}\n{"type":"ping"}\n')
        client.expect_type("pong")
        client.expect_type("pong")

    def test_password_duplicate_name_and_repeated_hello(self) -> None:
        first, first_welcome = self.join("SameName", "validation")

        wrong = self.client()
        wrong.send(hello("Other", "validation", "wrong-password"))
        wrong.expect_error("bad_password")

        duplicate = self.client()
        duplicate.send(hello("samename", "validation"))
        duplicate_welcome = duplicate.expect_type("welcome")
        self.assertNotEqual(duplicate_welcome["client_id"], first_welcome["client_id"])
        joined = first.expect_type("peer_joined")
        self.assertEqual(joined["client_id"], duplicate_welcome["client_id"])
        self.assertEqual(joined["name"], "samename")

        first.send(hello("Moved", "other-room"))
        first.expect_error("already_joined")

    def test_routing_and_sender_identity(self) -> None:
        first, first_welcome = self.join("Sender", "routing")
        second, second_welcome = self.join("Receiver", "routing")
        first.expect_type("peer_joined")

        first.send(
            {
                "type": "progression_state",
                "client_id": "spoofed",
                "stage": "F_SP108",
                "room": 1,
            }
        )
        routed = second.expect_type("progression_state")
        self.assertEqual(routed["client_id"], first_welcome["client_id"])

        first.send(
            {
                "type": "save_snapshot",
                "target_client_id": second_welcome["client_id"],
                "full_state": {"marker": 7},
            }
        )
        targeted = second.expect_type("save_snapshot")
        self.assertEqual(targeted["client_id"], first_welcome["client_id"])
        self.assertEqual(targeted["full_state"]["marker"], 7)

        first.send(
            {
                "type": "sync_request",
                "target_client_id": second_welcome["client_id"],
                "request_id": "manual-1",
            }
        )
        sync_request = second.expect_type("sync_request")
        self.assertEqual(sync_request["client_id"], first_welcome["client_id"])
        self.assertEqual(sync_request["request_id"], "manual-1")

        for message_type in HOST_CONTROL_TYPES:
            first.send({"type": message_type, "enabled": True})
            first.expect_error("unknown_message")

    def test_complete_protocol_2_gameplay_inventory_routes(self) -> None:
        sender, sender_welcome = self.join("InventorySender", "inventory")
        receiver, _ = self.join("InventoryReceiver", "inventory")
        sender.expect_type("peer_joined")

        for sequence, message_type in enumerate(GAMEPLAY_ROUTE_TYPES):
            sender.send({"type": message_type, "inventory_sequence": sequence})
            routed = receiver.expect_type(message_type)
            self.assertEqual(routed["client_id"], sender_welcome["client_id"])
            self.assertEqual(routed["inventory_sequence"], sequence)

    def test_target_cannot_cross_room_boundary(self) -> None:
        sender, _ = self.join("RoomOne", "target-room-one")
        outsider, outsider_welcome = self.join("RoomTwo", "target-room-two")
        sender.send(
            {
                "type": "save_snapshot",
                "target_client_id": outsider_welcome["client_id"],
            }
        )
        sender.expect_error("unknown_target")

        outsider.send({"type": "ping"})
        outsider.expect_type("pong")

    def test_reliable_deduplication_and_ack(self) -> None:
        first, _ = self.join("ReliableSender", "reliable")
        second, _ = self.join("ReliableReceiver", "reliable")
        first.expect_type("peer_joined")

        payload = {"type": "reliable", "sequence": 9, "state": {"value": 3}}
        first.send(payload)
        second.expect_type("reliable")
        self.assertEqual(first.expect_type("ack")["sequence"], 9)

        first.send(payload)
        with self.assertRaises(TimeoutError):
            second.receive(0.15)

    def test_slow_reader_receives_complete_queued_lines(self) -> None:
        sender, _ = self.join("BurstSender", "queued-output")
        receiver, _ = self.join("BurstReceiver", "queued-output")
        sender.expect_type("peer_joined")

        body = "x" * 16000
        count = 128
        for sequence in range(count):
            sender.send(
                {
                    "type": "presence",
                    "sequence": sequence,
                    "padding": body,
                }
            )

        received = [receiver.expect_type("presence", 5.0) for _ in range(count)]
        self.assertEqual([message["sequence"] for message in received], list(range(count)))
        self.assertTrue(all(message["padding"] == body for message in received))

    def test_hello_timeout_returns_error_then_closes(self) -> None:
        client = self.client()
        client.expect_error("hello_timeout", 2.0)
        with self.assertRaises(ConnectionError):
            client.receive(1.0)

    def test_input_line_limit_returns_error_then_closes(self) -> None:
        client = self.client()
        client.send_bytes(b"x" * (512 * 1024 + 1) + b"\n")
        client.expect_error("message_too_large", 2.0)
        with self.assertRaises(ConnectionError):
            client.receive(1.0)

    def test_pre_hello_and_malformed_messages_are_rejected(self) -> None:
        client = self.client()
        client.send({"type": "presence"})
        client.expect_error("expected_hello")
        client.send_bytes(b"{broken json}\n")
        client.expect_error("invalid_json")
        client.send(hello("Recovered", "validation-recovery", action="create"))
        client.expect_type("welcome")

    def test_explicit_create_join_and_password_rules(self) -> None:
        missing = self.client()
        missing.send(hello("Missing", "does-not-exist"))
        missing.expect_error("lobby_not_found")

        short = self.client()
        short.send(hello("Short", "short-password", "12345", action="create"))
        short.expect_error("password_too_short")

        owner = self.client()
        owner.send(hello("Owner", "explicit-room", action="create"))
        owner_welcome = owner.expect_type("welcome")
        self.assertEqual(owner_welcome["owner_client_id"], owner_welcome["client_id"])

        duplicate_room = self.client()
        duplicate_room.send(hello("OtherOwner", "explicit-room", action="create"))
        duplicate_room.expect_error("lobby_exists")

    def test_owner_settings_and_owner_only_enforcement(self) -> None:
        owner = self.client()
        owner.send(
            hello(
                "Owner",
                "owner-settings",
                action="create",
                settings={
                    "dummy_model": False,
                    "sync_flags": True,
                    "sync_world": True,
                    "remote_collision": False,
                    "pvp": True,
                },
            )
        )
        owner_welcome = owner.expect_type("welcome")
        self.assertFalse(owner_welcome["settings"]["dummy_model"])
        self.assertTrue(owner_welcome["settings"]["sync_world"])
        self.assertFalse(owner_welcome["settings"]["remote_collision"])
        self.assertFalse(owner_welcome["settings"]["pvp"])

        member = self.client()
        member.send(hello("Member", "owner-settings"))
        member_welcome = member.expect_type("welcome")
        owner.expect_type("peer_joined")
        self.assertEqual(member_welcome["settings"], owner_welcome["settings"])

        member.send(
            {
                "type": "room_settings",
                "settings": {"sync_flags": False},
            }
        )
        member.expect_error("owner_only")

        owner.send(
            {
                "type": "room_settings",
                "settings": {
                    "dummy_model": True,
                    "sync_flags": False,
                    "sync_world": False,
                    "remote_collision": True,
                    "pvp": True,
                },
            }
        )
        owner_update = owner.expect_type("room_settings")
        member_update = member.expect_type("room_settings")
        self.assertEqual(owner_update["settings"], member_update["settings"])
        self.assertTrue(member_update["settings"]["pvp"])
        self.assertFalse(member_update["settings"]["sync_flags"])

    def test_owner_transfers_to_oldest_remaining_member(self) -> None:
        owner, owner_welcome = self.join("Owner", "owner-transfer")
        second, second_welcome = self.join("Second", "owner-transfer")
        owner.expect_type("peer_joined")
        third, _ = self.join("Third", "owner-transfer")
        owner.expect_type("peer_joined")
        second.expect_type("peer_joined")

        owner.close()
        self.clients.remove(owner)
        self.assertEqual(second.expect_type("peer_left")["client_id"], owner_welcome["client_id"])
        owner_changed = second.expect_type("owner_changed")
        self.assertEqual(owner_changed["owner_client_id"], second_welcome["client_id"])
        third.expect_type("peer_left")
        self.assertEqual(
            third.expect_type("owner_changed")["owner_client_id"],
            second_welcome["client_id"],
        )

        second.send(
            {
                "type": "room_settings",
                "settings": {"sync_world": True},
            }
        )
        self.assertTrue(second.expect_type("room_settings")["settings"]["sync_world"])
        self.assertTrue(third.expect_type("room_settings")["settings"]["sync_world"])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--relay", required=True, type=Path)
    args, remaining = parser.parse_known_args()
    if not args.relay.is_file():
        parser.error(f"relay executable does not exist: {args.relay}")

    RelayTests.executable = args.relay.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RelayTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
