# TP Multiplayer Relay

This is a standalone C++ relay server for relay-mode multiplayer. It accepts
TCP and UDP clients, groups them by lobby name, checks the lobby password,
assigns opaque `client_id` values, and routes gameplay and visual messages
between lobby members.

Build:

```powershell
cmake --build .\build\windows-msvc-relwithdebinfo --config RelWithDebInfo `
    --target tp_multiplayer_relay_package
```

The completed friend-machine package is written to `relay-package` inside the
build directory and to `dusklight-relay-windows.zip` beside it. Send the ZIP
to the relay operator and have them extract it on their Windows machine. No
source tree, development tools, or separate Visual C++ runtime installation is
required there.

On Windows, open `tp_multiplayer_relay_launcher.exe`. The relay operator enters
the current public IP address or hostname and forwarded port, selects `Start
Relay`, then copies the displayed relay code. The launcher remembers those
values for the next run, runs `tp_multiplayer_relay.exe` from the same
directory, and stops it when the launcher closes.

The launcher records verbose server output in
`%APPDATA%\TwilitRealm\Dusklight\relay\relay.log`. Its `Open Log Folder` button
opens that location. If the server exits unexpectedly, the launcher displays
its exit code so the operator can include it with the log when reporting the
problem.

The command-line equivalent is:

```powershell
.\build\windows-msvc-relwithdebinfo\tp_multiplayer_relay.exe `
    --host 0.0.0.0 `
    --port 34197 `
    --public-host 203.0.113.10 `
    --public-port 34197 `
    --verbose
```

The relay prints a `Relay code` containing only its advertised public endpoint.
The relay operator gives that same code to everyone who will create or join
lobbies on this relay.

Create a room in the game:

```powershell
Open Online -> Relay -> Host, then enter:

Relay code: the code supplied by the relay operator
Nickname: Player 1
Lobby name: dev
Password: at least 6 characters
```

Other players use Online -> Relay -> Join and enter the same relay code, lobby
name, password, and their nickname. The relay code is the encoded IP address
and port, replacing separate endpoint fields in the game UI.

Protocol:

- UTF-8 JSON messages
- one message per line
- clients send `hello` first
- protocol version 2 uses explicit `create` and `join` actions
- `hello` contains `name`, `room_id`, `password`, and the action
- relay replies with `welcome`
- `welcome` includes the logical room owner and host-controlled settings
- relay broadcasts `peer_joined`, `peer_left`, owner changes, settings,
  `pose`, `reliable`, and the gameplay messages listed in
  [PROTOCOL.md](PROTOCOL.md)
- reliable gameplay uses TCP; streamed player visuals use authenticated UDP on
  the same numbered port

The creator owns the lobby settings. If they leave, the oldest remaining client
becomes owner. Nicknames are display labels and do not need to be unique; relay
routing uses the opaque `client_id`.

The relay does not persist gameplay state. Late join state is transferred
client-to-client.

## Automated integration test

The standard-library Python harness starts a real relay process and exercises
three-client joins, framing, validation, sender authentication, targeted
routing, reliable deduplication, queued slow-reader output, and hello timeout:

```powershell
python .\tools\multiplayer_relay\test_relay.py `
    --relay .\build\windows-msvc-relwithdebinfo\tp_multiplayer_relay.exe
```

It can also be registered with CTest by configuring with
`-DDUSK_BUILD_MULTIPLAYER_RELAY_TESTS=ON`.

Per-packet tracing is disabled by default because pose traffic is extremely
verbose. Set `DUSK_MP_RELAY_PACKET_TRACE=1` when a packet-size trace is needed.

## Future Public Deployment

The relay executable is meant to run on a server/VPS later. Players should only
run `dusklight.exe`; the hosted server runs `tp_multiplayer_relay.exe` in the
background.

Minimum early-alpha server shape:

- 1 vCPU
- 1 GB RAM
- 20 GB disk
- a few TB monthly transfer
- one TCP and one UDP firewall rule using the same relay port

Before sharing publicly:

1. Build the relay for the server platform:

   ```powershell
   cmake --build .\build\windows-msvc --config RelWithDebInfo --target tp_multiplayer_relay
   ```

2. Copy only the relay binary needed by the server. Do not copy local build
   logs, local config files, save files, or personal test data.

3. Run the relay bound to the public interface:

   ```powershell
   .\tp_multiplayer_relay.exe `
       --host 0.0.0.0 `
       --port 34197 `
       --public-host relay.example.com `
       --public-port 34197
   ```

4. Open the chosen port for both TCP and UDP in the VPS firewall/security
   group.

5. Point a DNS name at the VPS, for example:

   ```text
   relay.example.com
   ```

6. Copy the relay code printed by the server and give it to the players.

7. In the game, use Online -> Relay:

   ```text
    Relay code: code supplied by the relay operator
    Nickname: any display name
    Lobby name: shared lobby name
    Password: at least 6 characters
   ```

8. Test with at least three clients from outside the server network before
   announcing it.

Operational checklist:

- Run the relay under a service manager so it restarts after crashes/reboots.
- Keep firewall rules narrow: expose only the relay TCP/UDP port and SSH/RDP
  admin port.
- Watch CPU, memory, bandwidth, and process restarts during playtests.
- Keep the max lobby size conservative. The relay currently caps each lobby at
  8 clients and rejects later joins with `lobby_full`.
- Keep the 512 KiB message limit and 8 MiB per-client output queue enabled.
- Use throwaway lobby passwords. The current relay protocol is plain TCP, so
  lobby passwords are not protected from network observers.

Production hardening to consider later:

- TLS or WebSocket-over-TLS.
- Per-IP connection limits.
- Basic abuse logging.
- Relay version checks.
- TLS-protected relay-code and authentication flow.

## VPN-forwarded deployment note

The server keeps its local listen address separate from the public endpoint
encoded in the relay code:

- Listen address: normally `0.0.0.0`, or an advanced local/VPN-interface choice.
- Listen port: the active port allocated by the VPN provider.
- `--public-host`: the VPN server's current public IP or hostname.
- `--public-port`: normally the same active forwarded port.

The advertised public IP generally is not assigned to a local network
interface, so the relay must not try to bind to it. If Proton VPN reconnects,
its public IP and forwarded port may change. The relay operator restarts the
server with the new public values and distributes the newly printed relay code.
That code contains the replacement IP and port. Active TCP and UDP sessions
cannot survive the endpoint change.

Proton documents the current forwarded port in its app. On Linux it is also
available in `/run/user/$UID/Proton/VPN/forwarded_port`; no equivalent
machine-readable Windows interface is assumed here. See Proton's
[port-forwarding documentation](https://protonvpn.com/support/port-forwarding).
