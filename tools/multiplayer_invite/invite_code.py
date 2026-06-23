#!/usr/bin/env python3
"""Create and decode TP multiplayer invite codes."""

from __future__ import annotations

import argparse
import base64
import binascii
import ipaddress
import json
import secrets
import sys
from typing import Any


CODE_PREFIX = "TP1-"
DEFAULT_SECRET = b"tp-multiplayer-dev-invite-secret"
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
COMPACT_VERSION = 1
COMPACT_TRANSPORT_DIRECT = 1


def b64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def b64url_decode(data: str) -> bytes:
    padding = "=" * (-len(data) % 4)
    return base64.urlsafe_b64decode(data + padding)


def signature_int(payload: bytes, secret: bytes) -> int:
    digest = FNV_OFFSET
    for byte in secret + payload:
        digest ^= byte
        digest = (digest * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return digest


def sign(payload: bytes, secret: bytes) -> str:
    digest = signature_int(payload, secret)
    return b64url_encode(digest.to_bytes(8, "big"))


def append_string(buffer: bytearray, value: str) -> None:
    encoded = value.encode("utf-8")
    if len(encoded) > 255:
        raise ValueError(f"value is too long for compact invite format: {value!r}")
    buffer.append(len(encoded))
    buffer.extend(encoded)


def create_compact_payload(payload: dict[str, Any]) -> bytes:
    if payload["version"] != 1 or payload["transport"] != "direct":
        raise ValueError("compact invite format currently supports direct transport only")

    ip = ipaddress.ip_address(payload["host"])
    if ip.version != 4:
        raise ValueError("compact invite format currently supports IPv4 only")

    port = int(payload["port"])
    if not 1 <= port <= 65535:
        raise ValueError("port must be between 1 and 65535")

    buffer = bytearray()
    buffer.append(COMPACT_VERSION)
    buffer.append(COMPACT_TRANSPORT_DIRECT)
    buffer.extend(ip.packed)
    buffer.extend(port.to_bytes(2, "big"))
    append_string(buffer, payload["room"])
    append_string(buffer, payload["session_id"])
    append_string(buffer, payload["session_key"])
    return bytes(buffer)


def create_legacy_code(payload: dict[str, Any], secret: bytes) -> str:
    payload_bytes = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return f"{CODE_PREFIX}{b64url_encode(payload_bytes)}.{sign(payload_bytes, secret)}"


def create_code(args: argparse.Namespace) -> int:
    payload: dict[str, Any] = {
        "version": 1,
        "transport": args.transport,
        "host": args.host,
        "port": args.port,
        "room": args.room,
        "session_id": args.session_id or secrets.token_urlsafe(9),
        "session_key": args.session_key or secrets.token_urlsafe(16),
    }

    secret = args.secret.encode("utf-8")
    if args.legacy:
        token = create_legacy_code(payload, secret)
    else:
        try:
            compact_payload = create_compact_payload(payload)
            signed_payload = compact_payload + signature_int(compact_payload, secret).to_bytes(8, "big")
            token = f"{CODE_PREFIX}{b64url_encode(signed_payload)}"
        except ValueError:
            token = create_legacy_code(payload, secret)

    print(token)
    return 0


def decode_legacy_code(body: str, secret: bytes) -> dict[str, Any]:
    try:
        payload_token, signature = body.split(".", 1)
        payload_bytes = b64url_decode(payload_token)
    except ValueError:
        raise ValueError("Invalid code structure") from None

    expected = sign(payload_bytes, secret)
    if not secrets.compare_digest(signature, expected):
        raise ValueError("Invalid code signature")

    try:
        return json.loads(payload_bytes.decode("utf-8"))
    except json.JSONDecodeError:
        raise ValueError("Invalid code payload") from None


def read_string(payload: bytes, offset: int, payload_size: int) -> tuple[str, int]:
    if offset >= payload_size:
        raise ValueError("Invalid compact payload")

    length = payload[offset]
    offset += 1
    end = offset + length
    if end > payload_size:
        raise ValueError("Invalid compact payload")
    return payload[offset:end].decode("utf-8"), end


def decode_compact_code(body: str, secret: bytes) -> dict[str, Any]:
    try:
        data = b64url_decode(body)
    except (ValueError, binascii.Error):
        raise ValueError("Invalid payload encoding") from None

    if len(data) < 1 + 1 + 4 + 2 + 1 + 1 + 1 + 8:
        raise ValueError("Compact payload too short")

    payload = data[:-8]
    actual = int.from_bytes(data[-8:], "big")
    expected = signature_int(payload, secret)
    if not secrets.compare_digest(actual.to_bytes(8, "big"), expected.to_bytes(8, "big")):
        raise ValueError("Invalid code signature")

    offset = 0
    version = payload[offset]
    offset += 1
    transport = payload[offset]
    offset += 1
    if version != COMPACT_VERSION or transport != COMPACT_TRANSPORT_DIRECT:
        raise ValueError("Unsupported compact payload")

    host = str(ipaddress.ip_address(payload[offset:offset + 4]))
    offset += 4
    port = int.from_bytes(payload[offset:offset + 2], "big")
    offset += 2
    room, offset = read_string(payload, offset, len(payload))
    session_id, offset = read_string(payload, offset, len(payload))
    session_key, offset = read_string(payload, offset, len(payload))
    if offset != len(payload) or port <= 0:
        raise ValueError("Invalid compact payload")

    return {
        "version": version,
        "transport": "direct",
        "host": host,
        "port": port,
        "room": room,
        "session_id": session_id,
        "session_key": session_key,
    }


def decode_code(args: argparse.Namespace) -> int:
    if not args.code.startswith(CODE_PREFIX):
        print("Invalid code prefix", file=sys.stderr)
        return 2

    body = args.code[len(CODE_PREFIX):]
    try:
        payload = (
            decode_legacy_code(body, args.secret.encode("utf-8"))
            if "." in body
            else decode_compact_code(body, args.secret.encode("utf-8"))
        )
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 3 if "signature" in str(exc).lower() else 2

    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="TP multiplayer invite code tool")
    parser.add_argument(
        "--secret",
        default=DEFAULT_SECRET.decode("utf-8"),
        help="development HMAC secret; replace for real deployments",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    create = subparsers.add_parser("create", help="create an invite code")
    create.add_argument("--host", required=True)
    create.add_argument("--port", required=True, type=int)
    create.add_argument("--room", default="dev")
    create.add_argument("--transport", default="direct")
    create.add_argument("--session-id")
    create.add_argument("--session-key")
    create.add_argument("--legacy", action="store_true", help="force old JSON invite format")
    create.set_defaults(func=create_code)

    decode = subparsers.add_parser("decode", help="decode and validate an invite code")
    decode.add_argument("--code", required=True)
    decode.set_defaults(func=decode_code)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
