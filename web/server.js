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
const { execSync } = require('child_process');
const os = require('os');

let WebSocketServer;
try {
  ({ WebSocketServer } = require('ws'));
} catch {
  console.error('Missing dependency. Run:  npm install   (in the web/ folder)');
  process.exit(1);
}

const PORT = parseInt(process.argv[2], 10) || 8443;
const DIR = path.join(__dirname, 'public');
const CERT = path.join(__dirname, 'cert.pem');
const KEY = path.join(__dirname, 'key.pem');

// --- Self-signed certificate (one-time) ------------------------------------
if (!fs.existsSync(CERT) || !fs.existsSync(KEY)) {
  console.log('Generating a self-signed certificate (one-time)...');
  execSync(
    `openssl req -x509 -newkey rsa:2048 -nodes -keyout "${KEY}" -out "${CERT}" ` +
      `-days 365 -subj "/CN=videocall.local"`,
    { stdio: 'ignore' }
  );
}

// --- Static file serving ----------------------------------------------------
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' };
const server = https.createServer(
  { cert: fs.readFileSync(CERT), key: fs.readFileSync(KEY) },
  (req, res) => {
    const urlPath = decodeURIComponent(req.url.split('?')[0]);
    let file = path.join(DIR, urlPath === '/' ? 'index.html' : urlPath);
    if (!file.startsWith(DIR)) return res.writeHead(403).end(); // no traversal
    fs.readFile(file, (err, data) => {
      if (err) return res.writeHead(404).end('Not found');
      res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
      res.end(data);
    });
  }
);

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
  const ip = lanIp();
  console.log('\nMeeting server running. Share this invite link (same Wi-Fi):\n');
  console.log(`    https://${ip}:${PORT}/?room=demo\n`);
  console.log('On each device: open the link, accept the certificate warning once,');
  console.log('then click Join and allow Camera + Microphone.\n');
});
