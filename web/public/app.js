// Browser WebRTC mesh client.
//
// Each participant connects directly to every other (a full mesh, fine for a
// handful of people on a LAN). The page's only server interaction is signaling
// over a WebSocket: announce join, then relay SDP offers/answers and ICE
// candidates to specific peers. Glare is avoided with a simple rule: the *newer*
// participant always initiates the offer to those already in the room.
//
// No STUN/TURN is configured: on a LAN, host ICE candidates connect directly.

const params = new URLSearchParams(location.search);
const ROOM = params.get('room') || 'demo';
document.getElementById('roomName').textContent = ROOM;

const grid = document.getElementById('grid');
const statusEl = document.getElementById('status');
const joinBtn = document.getElementById('joinBtn');
const micBtn = document.getElementById('micBtn');
const camBtn = document.getElementById('camBtn');

const RTC_CONFIG = { iceServers: [] }; // LAN: host candidates only

let ws = null;
let localStream = null;
let myId = null;
const peers = new Map(); // peerId -> { pc, videoEl }

function setStatus(t) { statusEl.textContent = t; }

function addTile(id, label) {
  let p = peers.get(id);
  if (p && p.videoEl) return p.videoEl;
  const tile = document.createElement('div');
  tile.className = 'tile';
  tile.id = `tile-${id}`;
  const video = document.createElement('video');
  video.autoplay = true;
  video.playsInline = true;
  if (id === 'self') video.muted = true; // never echo our own audio
  const tag = document.createElement('div');
  tag.className = 'label';
  tag.textContent = label;
  tile.append(video, tag);
  grid.append(tile);
  return video;
}

function removeTile(id) {
  const el = document.getElementById(`tile-${id}`);
  if (el) el.remove();
}

function signal(to, data) {
  ws.send(JSON.stringify({ type: 'signal', to, data }));
}

// Create (or fetch) the RTCPeerConnection for a remote peer.
function getPeer(id) {
  let p = peers.get(id);
  if (p) return p;
  const pc = new RTCPeerConnection(RTC_CONFIG);
  p = { pc, videoEl: null };
  peers.set(id, p);

  // Send our media to this peer.
  for (const track of localStream.getTracks()) pc.addTrack(track, localStream);

  pc.onicecandidate = (e) => {
    if (e.candidate) signal(id, { candidate: e.candidate });
  };
  pc.ontrack = (e) => {
    if (!p.videoEl) p.videoEl = addTile(id, `Peer ${id}`);
    p.videoEl.srcObject = e.streams[0];
  };
  pc.onconnectionstatechange = () => {
    if (['failed', 'closed', 'disconnected'].includes(pc.connectionState)) {
      // Cleaned up on peer-left; nothing required here.
    }
  };
  return p;
}

async function makeOffer(id) {
  const { pc } = getPeer(id);
  const offer = await pc.createOffer();
  await pc.setLocalDescription(offer);
  signal(id, { description: pc.localDescription });
}

async function onSignal(from, data) {
  const { pc } = getPeer(from);
  if (data.description) {
    await pc.setRemoteDescription(data.description);
    if (data.description.type === 'offer') {
      const answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);
      signal(from, { description: pc.localDescription });
    }
  } else if (data.candidate) {
    try { await pc.addIceCandidate(data.candidate); } catch (e) { console.warn(e); }
  }
}

function connectSignaling() {
  ws = new WebSocket(`wss://${location.host}`);
  ws.onopen = () => {
    setStatus('connected — joining room…');
    ws.send(JSON.stringify({ type: 'join', room: ROOM }));
  };
  ws.onclose = () => setStatus('disconnected');
  ws.onerror = () => setStatus('signaling error');
  ws.onmessage = async (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.type === 'joined') {
      myId = msg.id;
      setStatus(`in room "${ROOM}" as ${myId}`);
      // We are the newest: initiate an offer to everyone already here.
      for (const peerId of msg.peers) await makeOffer(peerId);
    } else if (msg.type === 'peer-joined') {
      // A newer peer will offer to us; nothing to do until their offer arrives.
    } else if (msg.type === 'peer-left') {
      const p = peers.get(msg.id);
      if (p) p.pc.close();
      peers.delete(msg.id);
      removeTile(msg.id);
    } else if (msg.type === 'signal') {
      await onSignal(msg.from, msg.data);
    }
  };
}

async function join() {
  joinBtn.disabled = true;
  try {
    localStream = await navigator.mediaDevices.getUserMedia({ video: true, audio: true });
  } catch (e) {
    setStatus('camera/mic blocked — check permissions');
    joinBtn.disabled = false;
    return;
  }
  addTile('self', 'You').srcObject = localStream;

  micBtn.disabled = false;
  camBtn.disabled = false;
  micBtn.classList.remove('off');
  camBtn.classList.remove('off');
  micBtn.textContent = 'Mic on';
  camBtn.textContent = 'Camera on';

  connectSignaling();
}

micBtn.onclick = () => {
  const t = localStream.getAudioTracks()[0];
  t.enabled = !t.enabled;
  micBtn.classList.toggle('off', !t.enabled);
  micBtn.textContent = t.enabled ? 'Mic on' : 'Mic off';
};
camBtn.onclick = () => {
  const t = localStream.getVideoTracks()[0];
  t.enabled = !t.enabled;
  camBtn.classList.toggle('off', !t.enabled);
  camBtn.textContent = t.enabled ? 'Camera on' : 'Camera off';
};
joinBtn.onclick = join;
