# TP Multiplayer Relay Prototype

This is a dependency-free local harness for packet-flow experiments. It is not
the primary user-facing direction now. The normal product-shaped path is
invite-code direct/P2P hosting; this relay remains useful as a fallback
prototype and for testing pose/reliable message formats.

Run:

```powershell
python .\tp_pc\tools\multiplayer_relay\relay.py --host 127.0.0.1 --port 34197 --verbose
```

Run the game client against it:

```powershell
$env:DUSK_MP = "1"
$env:DUSK_MP_HOST = "127.0.0.1"
$env:DUSK_MP_PORT = "34197"
$env:DUSK_MP_ROOM = "dev"
$env:DUSK_MP_NAME = "Player 1"
.\tp_pc\build\windows-msvc\RelWithDebInfo\dusklight.exe
```

Protocol:

- UTF-8 JSON messages
- one message per line
- clients send `hello` first
- relay replies with `welcome`
- relay broadcasts `peer_joined`, `peer_left`, `pose`, `reliable`, and the
  current durable-state messages (`event_bit`, `tbox_bit`, `switch_bit`,
  `item_bit`, `dungeon_item_bit`, `save_snapshot`)

Relay clients only receive opaque `client_id` values. Direct invite-code mode is
different: the code hides connection details from the game UI, while the joiner
still connects to the host under the hood.
