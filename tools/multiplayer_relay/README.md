# TP Multiplayer Relay

This is a standalone C++ relay server for relay-mode multiplayer. It accepts
plain TCP clients, groups them by lobby name, checks the lobby password, assigns
opaque `client_id` values, and broadcasts gameplay messages between lobby
members.

Build:

```powershell
cmake --build .\tp_pc\build\windows-msvc --config RelWithDebInfo --target tp_multiplayer_relay
```

Run:

```powershell
.\tp_pc\build\windows-msvc\RelWithDebInfo\tp_multiplayer_relay.exe --host 127.0.0.1 --port 34197 --verbose
```

Run the game client against it:

```powershell
Open Online -> Relay, then enter:

Relay Host: 127.0.0.1
Relay Port: 34197
Username: Player 1
Lobby: dev
Password: any shared lobby password
```

Protocol:

- UTF-8 JSON messages
- one message per line
- clients send `hello` first
- `hello` contains `name`, `room_id`, and `password`
- relay replies with `welcome`
- relay broadcasts `peer_joined`, `peer_left`, `pose`, `reliable`, and the
  current durable-state messages (`event_bit`, `tbox_bit`, `switch_bit`,
  `item_bit`, `dungeon_item_bit`, `save_snapshot`, key/count/collect updates,
  visited-room updates, and letter flags)

The first client to join a lobby creates it. Later clients must provide the same
password. Usernames only need to be unique within that lobby.

## Future Public Deployment

The relay executable is meant to run on a server/VPS later. Players should only
run `dusklight.exe`; the hosted server runs `tp_multiplayer_relay.exe` in the
background.

Minimum early-alpha server shape:

- 1 vCPU
- 1 GB RAM
- 20 GB disk
- a few TB monthly transfer
- one open TCP port for the relay

Before sharing publicly:

1. Build the relay for the server platform:

   ```powershell
   cmake --build .\build\windows-msvc --config RelWithDebInfo --target tp_multiplayer_relay
   ```

2. Copy only the relay binary needed by the server. Do not copy local build
   logs, local config files, save files, or personal test data.

3. Run the relay bound to the public interface:

   ```powershell
   .\tp_multiplayer_relay.exe --host 0.0.0.0 --port 34197
   ```

4. Open the chosen TCP port in the VPS firewall/security group.

5. Point a DNS name at the VPS, for example:

   ```text
   relay.example.com
   ```

6. In the game, use Online -> Relay:

   ```text
   Relay Host: relay.example.com
   Relay Port: 34197
   Username: any display name
   Lobby: shared lobby name
   Password: shared lobby password
   ```

7. Test with at least three clients from outside the server network before
   announcing it.

Operational checklist:

- Run the relay under a service manager so it restarts after crashes/reboots.
- Keep firewall rules narrow: expose only the relay port and SSH/RDP admin port.
- Watch CPU, memory, bandwidth, and process restarts during playtests.
- Keep the max lobby size conservative. The relay currently caps each lobby at
  8 clients and rejects later joins with `lobby_full`.
- Keep message size limits enabled.
- Use throwaway lobby passwords. The current relay protocol is plain TCP, so
  lobby passwords are not protected from network observers.

Production hardening to consider later:

- TLS or WebSocket-over-TLS.
- Per-IP connection limits.
- Basic abuse logging.
- Relay version checks.
- A configured default relay host in the UI.
- Optional lobby tokens/invite codes instead of typing host/port manually.
