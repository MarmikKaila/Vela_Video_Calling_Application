// Browser WebRTC mesh client.
//
// Each participant connects directly to every other (a full mesh — fine for a
// handful of people on a LAN). The only server interaction is signaling over a
// WebSocket: announce join, then relay SDP offers/answers + ICE candidates.
// Glare is avoided by a simple rule: the newer participant initiates the offer.
// Display names ride along inside the SDP signal messages (no server change).
//
// No STUN/TURN: on a LAN, host ICE candidates connect directly.

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);

const slugify = (s) =>
  (s || '').toLowerCase().trim().replace(/[^a-z0-9-]+/g, '-').replace(/^-+|-+$/g, '');
function genRoom() {
  const c = 'abcdefghjkmnpqrstuvwxyz23456789'; // no ambiguous chars
  const pick = (n) => Array.from({ length: n }, () => c[Math.floor(Math.random() * c.length)]).join('');
  return `${pick(4)}-${pick(4)}`;
}

let ROOM = '';
// Prefill the room: use the one from an invite link, otherwise a fresh code so
// you never silently land in a shared default room.
$('roomInput').value = slugify(params.get('room') || '') || genRoom();
if (params.get('room')) $('roomLine').textContent = "You've been invited to a meeting";
$('newRoomBtn').onclick = () => { $('roomInput').value = genRoom(); };

const RTC_CONFIG = { iceServers: [] };

let localStream = null;
let camOn = true;
let micOn = true;
let myName = '';
let ws = null;
let myId = null;
const peers = new Map(); // id -> { pc, refs, name }

const initialOf = (name) => (name || '?').trim().charAt(0).toUpperCase() || '?';

// ---------------------------------------------------------------- Lobby ----
async function ensureStream() {
  if (localStream) return true;
  try {
    localStream = await navigator.mediaDevices.getUserMedia({ video: true, audio: true });
    return true;
  } catch {
    const h = $('lobbyHint');
    h.textContent = 'Camera/microphone blocked. Allow access in the browser, then reload.';
    h.classList.add('err');
    return false;
  }
}

async function startPreview() {
  if (!(await ensureStream())) return;
  $('previewVideo').srcObject = localStream;
  applyLocalTrackState();
}

function applyLocalTrackState() {
  if (!localStream) return;
  const a = localStream.getAudioTracks()[0];
  const v = localStream.getVideoTracks()[0];
  if (a) a.enabled = micOn;
  if (v) v.enabled = camOn;
  // Lobby buttons + preview
  $('lobbyMic').classList.toggle('off', !micOn);
  $('lobbyMic').querySelector('use').setAttribute('href', micOn ? '#i-mic' : '#i-mic-off');
  $('lobbyCam').classList.toggle('off', !camOn);
  $('lobbyCam').querySelector('use').setAttribute('href', camOn ? '#i-cam' : '#i-cam-off');
  $('previewOff').classList.toggle('hidden', camOn);
  $('previewAvatar').textContent = initialOf($('nameInput').value || 'You');
}

$('lobbyMic').onclick = () => { micOn = !micOn; applyLocalTrackState(); };
$('lobbyCam').onclick = () => { camOn = !camOn; applyLocalTrackState(); };
$('nameInput').oninput = () => { $('previewAvatar').textContent = initialOf($('nameInput').value || 'You'); };

$('joinBtn').onclick = async () => {
  if (!(await ensureStream())) return;
  ROOM = slugify($('roomInput').value) || genRoom();
  myName = ($('nameInput').value || '').trim() || 'Guest';
  // Reflect the room in the URL so "Copy invite link" shares this exact room.
  const url = new URL(location.href);
  url.searchParams.set('room', ROOM);
  history.replaceState(null, '', url);
  $('callRoom').textContent = ROOM;
  $('lobby').classList.add('hidden');
  $('call').classList.remove('hidden');
  buildSelfTile();
  connectSignaling();
};

// ---------------------------------------------------------------- Tiles ----
function makeTile(id, name, isSelf) {
  const frag = $('tileTpl').content.cloneNode(true);
  const tile = frag.querySelector('.tile');
  tile.id = `tile-${id}`;
  if (isSelf) tile.classList.add('self');
  const refs = {
    tile,
    video: tile.querySelector('video'),
    off: tile.querySelector('.tile-off'),
    avatar: tile.querySelector('.avatar'),
    name: tile.querySelector('.name'),
    micBadge: tile.querySelector('.mic-badge'),
  };
  refs.name.textContent = name;
  refs.avatar.textContent = initialOf(name);
  if (isSelf) refs.video.muted = true; // never echo our own audio
  $('grid').appendChild(tile);
  return refs;
}

function setTileCam(refs, on) { refs.off.classList.toggle('hidden', on); }
function setTileMic(refs, on) { refs.micBadge.classList.toggle('hidden', on); }

let selfRefs = null;
function buildSelfTile() {
  selfRefs = makeTile('self', `${myName} (You)`, true);
  selfRefs.video.srcObject = localStream;
  setTileCam(selfRefs, camOn);
  setTileMic(selfRefs, micOn);
  updateCount();
}

function updateCount() { $('count').textContent = String(peers.size + 1); }

// ---------------------------------------------------------- Peer plumbing ----
function signal(to, data) { ws.send(JSON.stringify({ type: 'signal', to, data })); }

function getPeer(id) {
  let p = peers.get(id);
  if (p) return p;
  const pc = new RTCPeerConnection(RTC_CONFIG);
  p = { pc, refs: null, name: `Peer ${id}` };
  peers.set(id, p);

  for (const track of localStream.getTracks()) pc.addTrack(track, localStream);

  pc.onicecandidate = (e) => { if (e.candidate) signal(id, { candidate: e.candidate }); };

  pc.ontrack = (e) => {
    if (!p.refs) { p.refs = makeTile(id, p.name, false); updateCount(); }
    p.refs.video.srcObject = e.streams[0];
    // Reflect the remote's mute/camera state from the track's mute events.
    const t = e.track;
    const sync = () => {
      if (t.kind === 'video') setTileCam(p.refs, !t.muted);
      else setTileMic(p.refs, !t.muted);
    };
    t.onmute = sync; t.onunmute = sync; sync();
  };

  pc.onconnectionstatechange = () => {
    if (['failed', 'disconnected', 'closed'].includes(pc.connectionState)) {
      // peer-left handles teardown; nothing required here.
    }
  };
  return p;
}

function setPeerName(id, name) {
  const p = peers.get(id);
  if (!p || !name) return;
  p.name = name;
  if (p.refs) { p.refs.name.textContent = name; p.refs.avatar.textContent = initialOf(name); }
}

async function makeOffer(id) {
  const { pc } = getPeer(id);
  await pc.setLocalDescription(await pc.createOffer());
  signal(id, { description: pc.localDescription, name: myName });
}

async function onSignal(from, data) {
  if (data.name) setPeerName(from, data.name);
  const { pc } = getPeer(from);
  if (data.description) {
    await pc.setRemoteDescription(data.description);
    if (data.description.type === 'offer') {
      await pc.setLocalDescription(await pc.createAnswer());
      signal(from, { description: pc.localDescription, name: myName });
    }
  } else if (data.candidate) {
    try { await pc.addIceCandidate(data.candidate); } catch (e) { console.warn(e); }
  }
}

// ------------------------------------------------------------- Signaling ----
function connectSignaling() {
  ws = new WebSocket(`wss://${location.host}`);
  ws.onopen = () => ws.send(JSON.stringify({ type: 'join', room: ROOM }));
  ws.onmessage = async (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.type === 'joined') {
      myId = msg.id;
      for (const peerId of msg.peers) await makeOffer(peerId); // newest offers to all
    } else if (msg.type === 'peer-left') {
      const p = peers.get(msg.id);
      if (p) { p.pc.close(); if (p.refs) p.refs.tile.remove(); }
      peers.delete(msg.id);
      updateCount();
    } else if (msg.type === 'signal') {
      await onSignal(msg.from, msg.data);
    }
  };
}

// --------------------------------------------------------------- Controls ----
$('micBtn').onclick = () => {
  micOn = !micOn;
  localStream.getAudioTracks().forEach((t) => (t.enabled = micOn));
  $('micBtn').classList.toggle('off', !micOn);
  $('micBtn').querySelector('use').setAttribute('href', micOn ? '#i-mic' : '#i-mic-off');
  setTileMic(selfRefs, micOn);
};

$('camBtn').onclick = () => {
  camOn = !camOn;
  localStream.getVideoTracks().forEach((t) => (t.enabled = camOn));
  $('camBtn').classList.toggle('off', !camOn);
  $('camBtn').querySelector('use').setAttribute('href', camOn ? '#i-cam' : '#i-cam-off');
  setTileCam(selfRefs, camOn);
};

$('leaveBtn').onclick = () => leave();

$('copyBtn').onclick = async () => {
  try {
    await navigator.clipboard.writeText(location.href);
    const t = $('toast');
    t.classList.remove('hidden');
    setTimeout(() => t.classList.add('hidden'), 1600);
  } catch { /* clipboard may be blocked on insecure origins */ }
};

function leave() {
  for (const [, p] of peers) p.pc.close();
  peers.clear();
  if (ws) { ws.close(); ws = null; }
  $('grid').innerHTML = '';
  $('call').classList.add('hidden');
  $('lobby').classList.remove('hidden');
  startPreview(); // back to lobby with the camera preview live
}

// Start the lobby preview immediately.
startPreview();
