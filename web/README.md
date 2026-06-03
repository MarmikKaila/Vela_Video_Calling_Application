# Browser meeting (LAN) — the "send a link, click to join" client

A zero-install way to call across laptops on the **same Wi-Fi**: open a link in a
browser, click **Join**, allow camera/mic, and you're in. The other person needs
**nothing installed** — just a browser. Works across macOS/Windows/Linux and
Intel/Apple Silicon, because the browser's built-in WebRTC does the media.

This is separate from the native C++ app (`group_call`/`sfu_server`), which
remains the custom raw-RTP/SFU implementation. Here the browser handles capture,
codecs, encryption, and rendering; `server.js` only serves the page over HTTPS
and relays signaling. Media flows peer-to-peer in a **mesh** (best for ~2–4
people; an SFU is the scale-up beyond that).

## Run it (host laptop, once)

```sh
brew install node          # if you don't have Node
cd web
npm install                # installs the one dependency (ws)
node server.js             # prints the invite link
```

It prints something like:

```
    https://192.168.1.42:8443/?room=demo
```

## Join

1. Open that link in a browser on **each** laptop (same Wi-Fi).
2. First time per device: the browser warns about the self-signed certificate —
   click **Advanced → proceed** (Chrome: you can type `thisisunsafe` on the
   warning page). This is required so the browser allows camera/mic off
   `localhost`.
3. Click **Join**, allow **Camera + Microphone**.

Everyone who opens the same link (same `?room=`) lands in the same meeting. Use a
different `?room=` value for separate meetings. **Use headphones** — there's no
echo cancellation.

## Notes / limits
- **LAN only.** No STUN/TURN configured; host ICE candidates connect directly on
  one network. Calling across the internet would need a public host + TURN.
- **Mesh topology.** Each pair connects directly, so ~2–4 participants is the
  comfortable range. Past that, route through an SFU (mediasoup/LiveKit, or the
  project's own `sfu_server`).
- The self-signed cert (`cert.pem`/`key.pem`) is generated on first run and
  git-ignored. To trust it permanently, add it to Keychain (optional).
