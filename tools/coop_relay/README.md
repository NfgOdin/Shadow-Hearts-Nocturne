# Shadow Hearts: Nocturne — Co-op Signaling & UDP Relay Server

Zero-dependency UDP signaling, STUN reflection, simultaneous hole-punching, and relay-fallback server for **Wave E** 6-character Room Codes.

## Local Run
```bash
python3 tools/coop_relay/coop_relay.py --host 0.0.0.0 --port 47191
```

## 1-Click Cloud Deploy (Fly.io / Docker)
```bash
cd tools/coop_relay
fly launch --copy-config
fly deploy
```

Then enter your server's `hostname:47191` in the Gothic Launcher **Co-op Lobby → Relay Server** field.
For LAN or VPN play (Tailscale / ZeroTier / Radmin), no relay server is needed — use **LAN Auto-Discovery** or **Direct IP**.
