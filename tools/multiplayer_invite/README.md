# TP Multiplayer Invite Codes

This tool prototypes the user-facing lobby-code flow for direct/P2P sessions.
It creates opaque codes that can be shown in the game UI instead of IP/port
fields.

New codes use the compact signed development format. The tool can still decode
older long JSON-style codes, and `--legacy` can create them for comparison.

Create a code:

```powershell
py -3 .\tp_pc\tools\multiplayer_invite\invite_code.py create --host 127.0.0.1 --port 34197 --room dev
```

Decode a code:

```powershell
py -3 .\tp_pc\tools\multiplayer_invite\invite_code.py decode --code <CODE>
```

Create an old JSON-style code:

```powershell
py -3 .\tp_pc\tools\multiplayer_invite\invite_code.py create --host 127.0.0.1 --port 34197 --room dev --legacy
```

The prototype signs the payload with a small shared development checksum so
tampering can be detected in tests. It does not encrypt the payload yet. That is
fine for development because the goal of this step is the lobby-code UX and
parser shape; release-grade codes should move to authenticated encryption or a
reviewed existing token format.
