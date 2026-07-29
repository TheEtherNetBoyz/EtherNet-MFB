# Using Dusklight Multiplayer

## Prerequisites

Dusklight Multiplayer can be played through a direct connection or a privately hosted relay. There is currently no public relay.

For a direct connection, the host must either configure TCP and UDP port forwarding on their router, use a VPN that supports port forwarding such as ProtonVPN, or have everyone join the same Radmin VPN/Hamachi network.

For a relay connection, the person running the relay must forward the chosen port for both TCP and UDP.

## Direct lobbies

After starting the game, use either "~" or "Shift + F1" to open the Imgui menu, then go to "Tools", then "Online", and select "Direct."

To host, enter your name, room, public IP or hostname and port, then click "Host Lobby." Send the invite code to whoever is joining.

To join, enter your name and paste in the invite code, then click "Join Lobby." The invite code already contains the IP and port.

## Relay lobbies

The person running the relay opens the relay launcher, enters their current public IP or hostname and forwarded port, then starts the relay and sends the relay code to the players.

In the Online menu, select "Relay." To create a lobby, enter the relay code, nickname, lobby name and a password of at least 6 characters under "Host", then click "Host Relay Lobby."

To join, enter the same relay code, lobby name and password under "Join", enter your own nickname, then click "Join Relay Lobby."

The player who creates the lobby controls its settings, but the relay machine hosts the connection. If that player leaves, another player becomes the lobby owner.

## Design

Most in-game events are synced over a TCP connection, this includes but is not limited to: 

- **Collected Items**
- **Event Flags**
- **Region Flags**
- **Dungeon Switches** (Puzzles, Unlocked doors etc...)
- **PvP Combat**

In the Online tab, we have "sync flags" and "sync + warp" options to allow you to sync your inventory, flags and stage/room with another player. We also have sync prompts that show up after a player completes a major in-game event (such as completing a dungeon) to allow you to catch up to them.

The Link dummies are sent over UDP. Due to the complexity of Daalink, we had to resort to streaming Links already calculated matrices over to the other users, I would imagine most connections are good enough to support this, however it's worth keeping in mind for if you ever notice the remote Links being choppy/unstable.

Midna is currently disabled for the remote Links as she used up too much bandwidth and de-synced too often.

Twilight Princess is designed with single player assumptions, it is likely that there will never be a Twilight Princess multiplayer mod that stands up to something like SM64 Co-op DX with fully synced worlds and a "true" multiplayer experience.

## Warnings

This hasn't been extensively tested outside of multiplayer any% speed-runs and a few randomizer seeds, there is no guarantee that you won't encounter numerous bugs or crashes especially on a casual playthrough. If you do encounter any issues, feel free to send me some info and the log from AppData/Roaming/TwilitRealm/Dusklight/Logs (send to "mdra" on Discord).

A few already known issues:

- **Live Switches** - All dungeon key doors should update live as a peer opens them, however there are other locked doors/gates and events that may not be synced until an area reload, these have to be fixed on a case by case basis.
- **State/Layer Update Crashes** - Some areas, Ordon in particular, will crash a lot depending on where the peers are as you update the area to a new state (e.g. updating Ordon to the next day while a peer is in the area.) Numerous guards have been put in place to try and prevent these crashes (such as forced area reloads) but nothing so far is 100% safe.
- **Save-Warping** - Mostly relevant for speed-runs, if a player is on the title screen while a peer collects a flag, that flag will be lost once you load back in to the game. The workaround for this is to manually sync flags once you have reloaded the save, however this can cause issues if your route (for speed-runs) relies on players being in different states.
