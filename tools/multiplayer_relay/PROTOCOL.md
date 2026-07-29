# Multiplayer Relay Protocol

This document is the compatibility contract between `multiplayer.cpp` and the
standalone relay.

## Framing and limits

- Reliable transport: plain TCP.
- Encoding: one UTF-8 JSON object per line.
- Current wire version: `2`.
- Maximum encoded input line: 512 KiB, excluding the newline.
- Maximum queued output per client: 8 MiB.
- Maximum clients in one room: 8.
- Maximum room name: 64 bytes.
- Maximum nickname: 32 bytes.
- Minimum password: 6 bytes.
- Maximum password: 128 bytes.
- A client must complete `hello` within 10 seconds.

Plain TCP does not protect the room password. Internet deployment requires a
trusted private network or a later encrypted transport phase.

## Relay code

The standalone relay prints a relay code at startup. This is a client-side
bootstrap code containing the operator's advertised public host and port; it is
not a room identifier and contains no lobby password. The operator gives the
same code to lobby creators and joiners. Room name, password, nickname, and
host/join intent are supplied separately in `hello`.

## UDP visual channel

The relay listens for UDP on the same numbered port as TCP. `welcome` includes
a connection-specific `udp_token`. The client presents that token in a UDP
registration packet; the relay then binds the authenticated `client_id` to the
observed source IP and port. Visual, remote-object, and acknowledgement
datagrams are accepted only from that registered endpoint.

The relay routes pose and Midna chunks only to room members who requested those
visuals and skips known cross-stage recipients. Pose acknowledgements are
targeted back to the original sender. Unregistered or sender-spoofed datagrams
are discarded, and authenticated UDP input is rate limited.

## Connection sequence

Clients explicitly create or join rooms. A successful connection is:

1. Client sends `hello` with `action` set to `create` or `join`.
2. Relay sends `welcome` containing the assigned `client_id`, room owner,
   current room settings, and existing peers.
3. Relay sends `peer_joined` to the existing peers.
4. Gameplay messages can be routed.
5. Relay sends `peer_left` when a joined client disconnects.

`hello` fields:

| Field | Type | Required |
| --- | --- | --- |
| `type` | string (`hello`) | yes |
| `protocol_version` | integer (`2`) | yes |
| `action` | string (`create` or `join`) | yes |
| `room_id` | string | yes |
| `password` | string (at least 6 bytes) | yes |
| `name` | string | yes |
| `settings` | object | required for `create` |
| `session_id` | string | no; currently client metadata |
| `want_puppet` | boolean | no; reserved for preference routing |
| `want_midna` | boolean | no; reserved for preference routing |

Calling `hello` again after joining returns `already_joined`; it cannot move a
socket between rooms.

Nicknames are not connection identities and may be duplicated. Every joined
socket receives a unique opaque `client_id`, which is used for routing.

Creating an existing room returns `lobby_exists`. Joining a missing room
returns `lobby_not_found`; joining with a different password returns
`bad_password`.

The creator is the logical room owner. If the owner disconnects, ownership
passes to the oldest remaining connection and the relay broadcasts
`owner_changed`.

## Relay-handled messages

| Message | Behavior |
| --- | --- |
| `hello` | Validate and join/create a room. |
| `ping` | Reply to the sender with `pong`. |
| `pose` | Broadcast a sanitized pose envelope to other room members. |
| `reliable` | Deduplicate a bounded sequence window, broadcast, then `ack`. |
| `sync_request` | Route only to `target_client_id`. |
| `room_settings` | Owner-only update; validate and broadcast normalized settings. |

For every routed client message, the relay overwrites `client_id` with the
authenticated connection ID. Clients cannot impersonate another room member.
Targets are only valid when they belong to the sender's room.

## Protocol 2 gameplay routing

The following messages may be broadcast or may include `target_client_id`:

| Area | Message types |
| --- | --- |
| World flags | `event_bit`, `tbox_bit`, `switch_bit`, `room_switch_bit`, `item_bit`, `dungeon_item_bit` |
| Inventory/progression | `save_snapshot`, `item_get`, `rando_item_get`, `item_first_bit`, `collect_crystal`, `collect_mirror`, `dark_clear_lv`, `transform_lv`, `region_bit`, `collect`, `visited_room`, `letter_get`, `ooccoo_state` |
| Counts and slots | `key_num`, `light_drop_num`, `light_drop_get_flag`, `max_life_update`, `bottle_slots`, `bomb_bag_slot`, `rupee_count` |
| Peer status/preferences | `presence`, `progression_state`, `puppet_preference`, `midna_preference` |
| Visual/PvP | `midna_pose`, `pvp_hit` |
| Ganondorf encounter | `ganondorf_owner_claim`, `ganondorf_owner`, `ganondorf_hit`, `ganondorf_reaction`, `ganondorf_player_damage`, `ganondorf_state` |

The relay transports these payloads but does not interpret or persist their
gameplay state. Late join synchronization remains client-to-client.

## Room settings

The owner sends all current values in one `room_settings` message:

- `dummy_model`
- `sync_flags`
- `sync_world`
- `remote_collision`
- `pvp`

Every value must be boolean. The relay forces `pvp` off when
`remote_collision` is off. Non-owner updates return `owner_only`. Current
settings are included in every `welcome`, so late joiners use the room's
settings rather than their local defaults.
