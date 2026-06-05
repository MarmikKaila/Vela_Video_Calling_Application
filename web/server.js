// Minimal LAN meeting server: HTTPS static hosting + WebSocket signaling relay.
//
// The browser client (public/) does the actual media with built-in WebRTC in a
// full mesh (each participant connects directly to every other). This server
// only (a) serves the page over HTTPS — required so browsers allow camera/mic
// off localhost — and (b) relays join / signal / leave messages between the
// peers in a room. It never touches media.
//
// Run:  node server.js [port]      (default 8443)
// A self-signed certificate is generated on first run (cert.pem/key.pem).

const fs = require('fs');
const path = require('path');
const https = require('https');
const http = require('http');
const { execSync } = require('child_process');
const os = require('os');

let WebSocketServer;
try {
  ({ WebSocketServer } = require('ws'));
} catch {
  console.error('Missing dependency. Run:  npm install   (in the web/ folder)');
  process.exit(1);
}

// PLAIN_HTTP=1 runs plain HTTP — for hosting behind a platform that terminates
// TLS at its edge (fly.io, Render, a tunnel). Browsers still see HTTPS via the
// edge, so camera/mic work. Locally we serve HTTPS ourselves (self-signed) so
// the camera works on a LAN IP.
const PLAIN = process.env.PLAIN_HTTP === '1';
const PORT = parseInt(process.env.PORT, 10) || parseInt(process.argv[2], 10) || 8443;
const DIR = path.join(__dirname, 'public');
const CERT = path.join(__dirname, 'cert.pem');
const KEY = path.join(__dirname, 'key.pem');

// --- Self-signed certificate (one-time, local HTTPS only) ------------------
if (!PLAIN && (!fs.existsSync(CERT) || !fs.existsSync(KEY))) {
  console.log('Generating a self-signed certificate (one-time)...');
  execSync(
    `openssl req -x509 -newkey rsa:2048 -nodes -keyout "${KEY}" -out "${CERT}" ` +
      `-days 365 -subj "/CN=videocall.local"`,
    { stdio: 'ignore' }
  );
}

// --- ICE servers (STUN + TURN) ----------------------------------------------
// Browsers need STUN to discover their public address and TURN to relay media
// when a direct path is blocked (symmetric/strict NAT) — that is what makes
// calls work across different networks. Defaults are free: Google STUN + the
// Open Relay project's public TURN (no signup). Override with ICE_SERVERS_JSON
// to point at your own coturn / a managed TURN free tier.
const ICE_SERVERS = process.env.ICE_SERVERS_JSON
  ? JSON.parse(process.env.ICE_SERVERS_JSON)
  : [
      { urls: 'stun:stun.l.google.com:19302' },
      { urls: 'turn:openrelay.metered.ca:80', username: 'openrelayproject', credential: 'openrelayproject' },
      { urls: 'turn:openrelay.metered.ca:443', username: 'openrelayproject', credential: 'openrelayproject' },
      // TCP/443 helps when UDP is blocked on restrictive networks.
      { urls: 'turn:openrelay.metered.ca:443?transport=tcp', username: 'openrelayproject', credential: 'openrelayproject' },
    ];

// --- Static file serving ----------------------------------------------------
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' };
function handler(req, res) {
  const urlPath = decodeURIComponent(req.url.split('?')[0]);
  // Runtime config for the client (ICE servers, kept out of the static JS).
  if (urlPath === '/config') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    return res.end(JSON.stringify({ iceServers: ICE_SERVERS }));
  }
  let file = path.join(DIR, urlPath === '/' ? 'index.html' : urlPath);
  if (!file.startsWith(DIR)) return res.writeHead(403).end(); // no traversal
  fs.readFile(file, (err, data) => {
    if (err) return res.writeHead(404).end('Not found');
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
    res.end(data);
  });
}

const server = PLAIN
  ? http.createServer(handler)
  : https.createServer({ cert: fs.readFileSync(CERT), key: fs.readFileSync(KEY) }, handler);

// --- Signaling relay --------------------------------------------------------
// rooms: roomName -> Map(peerId -> ws). Each ws gets an id + room on join.
const rooms = new Map();
let nextId = 1;

function send(ws, obj) {
  if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(obj));
}

const wss = new WebSocketServer({ server });
wss.on('connection', (ws) => {
  ws.id = String(nextId++);
  ws.room = null;

  ws.on('message', (raw) => {
    let msg;
    try { msg = JSON.parse(raw); } catch { return; }

    if (msg.type === 'join') {
      ws.room = String(msg.room || 'demo');
      if (!rooms.has(ws.room)) rooms.set(ws.room, new Map());
      const peers = rooms.get(ws.room);
      // Tell the newcomer who is already here (it will initiate to them).
      send(ws, { type: 'joined', id: ws.id, peers: [...peers.keys()] });
      // Tell everyone else the newcomer arrived.
      for (const [, peer] of peers) send(peer, { type: 'peer-joined', id: ws.id });
      peers.set(ws.id, ws);
      console.log(`[room ${ws.room}] ${ws.id} joined (${peers.size} total)`);
      return;
    }

    if (msg.type === 'signal') {
      const peers = rooms.get(ws.room);
      const target = peers && peers.get(String(msg.to));
      if (target) send(target, { type: 'signal', from: ws.id, data: msg.data });
      return;
    }
  });

  ws.on('close', () => {
    const peers = rooms.get(ws.room);
    if (!peers) return;
    peers.delete(ws.id);
    for (const [, peer] of peers) send(peer, { type: 'peer-left', id: ws.id });
    if (peers.size === 0) rooms.delete(ws.room);
    console.log(`[room ${ws.room}] ${ws.id} left`);
  });
});

// --- Print the invite link --------------------------------------------------
function lanIp() {
  for (const ifaces of Object.values(os.networkInterfaces())) {
    for (const i of ifaces) {
      if (i.family === 'IPv4' && !i.internal) return i.address;
    }
  }
  return 'localhost';
}

server.listen(PORT, () => {
  if (PLAIN) {
    console.log(`Meeting server (HTTP, behind edge TLS) listening on :${PORT}`);
    console.log('Share your public https URL with ?room=<name>.');
    return;
  }
  const ip = lanIp();
  console.log('\nMeeting server running. Share this invite link:\n');
  console.log(`    https://${ip}:${PORT}/?room=demo\n`);
  console.log('Same Wi-Fi: works as-is. Across networks: expose it publicly');
  console.log('(cloudflared tunnel / fly.io) — TURN is already configured.');
  console.log('On each device: open the link, accept the cert warning once,');
  console.log('then click Join and allow Camera + Microphone.\n');
});
