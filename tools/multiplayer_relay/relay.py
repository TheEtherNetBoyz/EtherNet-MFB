#!/usr/bin/env python3
"""Small local relay for TP multiplayer protocol experiments."""

from __future__ import annotations

import argparse
import asyncio
import json
import secrets
import time
from dataclasses import dataclass, field
from typing import Any


PROTOCOL_VERSION = 1
MAX_LINE_BYTES = 64 * 1024
STATE_BROADCAST_TYPES = {
    "event_bit",
    "tbox_bit",
    "switch_bit",
    "item_bit",
    "dungeon_item_bit",
    "save_snapshot",
}


@dataclass
class Client:
    client_id: str
    room_id: str
    writer: asyncio.StreamWriter
    name: str = ""
    last_seen: float = field(default_factory=time.monotonic)
    reliable_seen: set[int] = field(default_factory=set)


class Relay:
    def __init__(self, verbose: bool = False) -> None:
        self._rooms: dict[str, dict[str, Client]] = {}
        self._verbose = verbose
        self._pose_counts: dict[str, int] = {}

    async def handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        client: Client | None = None
        try:
            hello = await self._read_message(reader)
            if hello.get("type") != "hello":
                await self._send(writer, {"type": "error", "error": "expected_hello"})
                return

            if int(hello.get("protocol_version", -1)) != PROTOCOL_VERSION:
                await self._send(writer, {"type": "error", "error": "protocol_version"})
                return

            room_id = str(hello.get("room_id") or self._make_id("room"))
            client = Client(
                client_id=self._make_id("client"),
                room_id=room_id,
                writer=writer,
                name=str(hello.get("name") or "Player")[:32],
            )
            room = self._rooms.setdefault(room_id, {})
            existing = [
                {"client_id": peer.client_id, "name": peer.name}
                for peer in room.values()
            ]
            room[client.client_id] = client
            self._log(f"join room={room_id} client={client.client_id} name={client.name}")

            await self._send(writer, {
                "type": "welcome",
                "protocol_version": PROTOCOL_VERSION,
                "room_id": room_id,
                "client_id": client.client_id,
                "peers": existing,
            })
            await self._broadcast(client, {
                "type": "peer_joined",
                "client_id": client.client_id,
                "name": client.name,
            })

            while True:
                message = await self._read_message(reader)
                client.last_seen = time.monotonic()
                await self._route(client, message)
        except (asyncio.IncompleteReadError, ConnectionResetError):
            pass
        except ValueError as exc:
            if client is None:
                await self._send(writer, {"type": "error", "error": str(exc)})
        finally:
            if client is not None:
                await self._remove_client(client)
            writer.close()
            await writer.wait_closed()

    async def _route(self, client: Client, message: dict[str, Any]) -> None:
        msg_type = message.get("type")
        if msg_type == "ping":
            await self._send(client.writer, {"type": "pong", "time": time.time()})
            return

        if msg_type == "pose":
            await self._broadcast(client, {
                "type": "pose",
                "client_id": client.client_id,
                "sequence": int(message.get("sequence", 0)),
                "state": message.get("state", {}),
            })
            count = self._pose_counts.get(client.client_id, 0) + 1
            self._pose_counts[client.client_id] = count
            if self._verbose and count % 30 == 1:
                state = message.get("state", {})
                print(
                    "pose "
                    f"client={client.client_id} seq={message.get('sequence', 0)} "
                    f"stage={state.get('stage', '')} room={state.get('room', '')} "
                    f"pos=({state.get('x', '')},{state.get('y', '')},{state.get('z', '')})",
                    flush=True,
                )
            return

        if msg_type == "reliable":
            sequence = int(message.get("sequence", 0))
            if sequence in client.reliable_seen:
                return
            client.reliable_seen.add(sequence)
            await self._broadcast(client, {
                "type": "reliable",
                "client_id": client.client_id,
                "sequence": sequence,
                "state": message.get("state", {}),
            })
            await self._send(client.writer, {
                "type": "ack",
                "sequence": sequence,
            })
            return

        if msg_type in STATE_BROADCAST_TYPES:
            routed = dict(message)
            routed["client_id"] = client.client_id
            await self._broadcast(client, routed)
            return

        await self._send(client.writer, {"type": "error", "error": "unknown_message"})

    async def _broadcast(self, sender: Client, message: dict[str, Any]) -> None:
        room = self._rooms.get(sender.room_id, {})
        for peer in list(room.values()):
            if peer.client_id == sender.client_id:
                continue
            await self._send(peer.writer, message)

    async def _remove_client(self, client: Client) -> None:
        room = self._rooms.get(client.room_id)
        if room is None:
            return

        room.pop(client.client_id, None)
        self._pose_counts.pop(client.client_id, None)
        self._log(f"leave room={client.room_id} client={client.client_id}")
        await self._broadcast(client, {
            "type": "peer_left",
            "client_id": client.client_id,
        })
        if not room:
            self._rooms.pop(client.room_id, None)

    async def _read_message(self, reader: asyncio.StreamReader) -> dict[str, Any]:
        line = await reader.readline()
        if not line:
            raise asyncio.IncompleteReadError(line, None)
        if len(line) > MAX_LINE_BYTES:
            raise ValueError("message_too_large")
        try:
            message = json.loads(line.decode("utf-8"))
        except json.JSONDecodeError as exc:
            raise ValueError("invalid_json") from exc
        if not isinstance(message, dict):
            raise ValueError("invalid_message")
        return message

    async def _send(self, writer: asyncio.StreamWriter, message: dict[str, Any]) -> None:
        writer.write(json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n")
        await writer.drain()

    def _make_id(self, prefix: str) -> str:
        return f"{prefix}_{secrets.token_urlsafe(9)}"

    def _log(self, message: str) -> None:
        if self._verbose:
            print(message, flush=True)


async def main() -> None:
    parser = argparse.ArgumentParser(description="TP multiplayer relay prototype")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=34197, type=int)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    relay = Relay(verbose=args.verbose)
    server = await asyncio.start_server(relay.handle_client, args.host, args.port)
    sockets = ", ".join(str(sock.getsockname()) for sock in server.sockets or [])
    print(f"TP relay listening on {sockets}")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
