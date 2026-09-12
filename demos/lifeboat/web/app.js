// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
(() => {
  'use strict';

  const canvas = document.getElementById('world');
  const viewport = canvas.parentElement;
  const ctx = canvas.getContext('2d');
  const $ = id => document.getElementById(id);
  const colors = ['#89ebcb', '#efbc74', '#85acf5'];
  const cargoNames = ['Food & biosystems', 'Energy cells', 'Fabrication stock'];
  const capacity = 28;
  const nodes = [
    { id: 0, x: -440, y: -158, z: 94, title: 'ARRIVAL CONTROL', subtitle: 'INBOUND GUIDANCE', dx: -135, dy: -58 },
    { id: 1, x: -304, y: -48, z: 165, title: 'ASTER', subtitle: 'DOCK 01', dx: -54, dy: -30 },
    { id: 2, x: -206, y: 190, z: 160, title: 'BOREAL', subtitle: 'DOCK 02', dx: -42, dy: -34 },
    { id: 3, x: -10, y: -98, z: 78, title: 'THE HOLD', subtitle: 'CARGO WAREHOUSE', dx: 12, dy: -33 },
    { id: 4, x: 250, y: -74, z: 118, title: 'FABRICATOR', subtitle: 'ASSEMBLY WORKS', dx: 42, dy: -36 },
    { id: 5, x: 213, y: 240, z: 40, title: 'MERIDIAN LINE', subtitle: 'HABITAT TRANSFER', dx: -36, dy: 68 }
  ];
  const descriptions = [
    'Admits ships into the approach channel. When its eight places fill, arrival control waits for a crane to receive cargo.',
    'Shares the arrival channel with Boreal. Finishes each accepted cargo before closing its bay. Its supervisor preserves work through a controller failure.',
    'Shares the arrival channel with Aster. A ready crane accepts the next cargo; closing Aster leaves Boreal to serve the queue.',
    'Stores accepted cargo in a bounded channel. When the fabricator stops receiving, the hold fills and the cranes must wait.',
    'Receives one cargo at a time from the warehouse. Pausing it lets backpressure spread through the dock.',
    'Receives finished cargo from the departure platform and delivers it to the habitat. During evacuation it completes every accepted delivery.'
  ];
  let state = null;
  let connected = false;
  let pending = false;
  let receivedAt = 0;
  let stationNow = 0;
  let renderedAt = 0;
  let sceneRun = 0;
  let socket = null;
  let lastSequence = 0;
  let retryDelay = 500;
  const renderDelay = 0.08;
  const extrapolationLimit = 0.25;
  const historyLimit = 16;
  const timelines = new Map();
  let poses = new Map();
  let renderedMotions = Object.freeze([]);
  let renderedPoseMap = Object.freeze({});
  let cargoHits = [];
  let selectedCargo = 0;
  let selected = -1;
  let xray = false;
  let width = 0;
  let height = 0;
  let pixelRatio = 1;
  let scale = 1;
  let zoom = 1;
  let pan = { x: 0, y: 0 };
  let drag = null;
  let hitAreas = [];
  let lastEvents = '';
  let lastFrame = 0;
  let hover = -1;

  const clamp = (n, lo, hi) => Math.max(lo, Math.min(hi, n));
  const mix = (a, b, t) => a + (b - a) * t;
  const ease = t => t * t * (3 - 2 * t);
  const point = (x, y, z = 0) => ({ x, y, z });
  const project = (x, y, z = 0) => ({ x: (x - y) * 0.82, y: (x + y) * 0.43 - z });
  const timeText = seconds => {
    const n = Math.max(0, Math.floor(seconds));
    return `${String(Math.floor(n / 60)).padStart(2, '0')}:${String(n % 60).padStart(2, '0')}`;
  };
  const hexAlpha = (color, alpha) => color + Math.round(clamp(alpha, 0, 1) * 255).toString(16).padStart(2, '0');
  const statusColor = actor => {
    if (!actor || actor.status === 'offline') return '#516273';
    if (/paused|closed|pressure|restarting/.test(actor.status)) return colors[1];
    return colors[0];
  };
  const actorAt = id => state?.actors.find(actor => actor.id === id);
  const stageCount = stages => state ? state.cargo.filter(item => stages.includes(item.stage)).length : 0;

  function selectActor(id) {
    if (!Number.isInteger(id) || id < 0 || id >= nodes.length) return;
    selected = id;
    selectedCargo = 0;
    updateInspector();
  }

  function selectCargo(id) {
    if (!Number.isInteger(id) || id < 1) return;
    selectedCargo = id;
    selected = -1;
    updateInspector();
  }

  Object.defineProperty(window, 'lifeboat', {
    value: Object.freeze({
      get state() { return state; },
      get renderedMotions() { return renderedMotions; },
      get poses() { return renderedPoseMap; },
      get renderedAt() { return renderedAt; },
      get run() { return sceneRun; },
      get selectedCargo() { return selectedCargo; },
      selectActor, selectCargo
    }),
    writable: false,
    configurable: false
  });

  function updateInspector() {
    if (selectedCargo) {
      const motion = poses.get(`cargo:${selectedCargo}`);
      const item = state?.cargo.find(item => item.id === selectedCargo);
      $('inspectorTitle').textContent = `Cargo ${String(selectedCargo).padStart(3, '0')}`;
      $('inspectorDetail').textContent = `${cargoNames[selectedCargo % colors.length]}. Follow this same cargo from its ship, through storage and fabrication, to the habitat. Finished goods keep their color and ID and receive gold shipping bands.`;
      $('inspectorStatus').textContent = motion ? `${motion.phase.replaceAll('-', ' ')} · ${motion.kind === 'goods' ? 'finished goods' : motion.kind === 'hidden' ? 'inside the building' : 'raw material'} · motion actor cargo:${selectedCargo}`
        : item ? item.stage : 'Journey completed. Select another cargo to follow it.';
      $('inspectorStatus').style.color = colors[selectedCargo % colors.length];
      return;
    }
    if (selected < 0) {
      $('inspectorStatus').textContent = state
        ? `${state.activeActors} logistics actors · ${state.animationActors ?? '—'} motion actors · typed channels`
        : 'Awaiting station telemetry';
      return;
    }
    const actor = actorAt(selected);
    $('inspectorTitle').textContent = actor?.name || nodes[selected].title;
    $('inspectorDetail').textContent = descriptions[selected];
    let detail = actor?.status || 'awaiting telemetry';
    if (actor?.cargo) detail += ` · cargo ${String(actor.cargo).padStart(3, '0')}`;
    if (selected === 0) detail += ` · ${stageCount(['approach'])} approaching`;
    if (selected === 3) detail += ` · ${stageCount(['warehouse'])} in storage stage`;
    if (selected === 5) detail += ` · ${stageCount(['platform'])} at platform`;
    if (selected === 1 && state?.restarts) detail += ` · ${state.restarts} controller restart${state.restarts === 1 ? '' : 's'}`;
    const channels = ['', 'crane:1', 'crane:2', 'door:hold-in / door:hold', 'door:fab-in / factory:4 / door:fab-out', 'tram:5'];
    if (channels[selected]) detail += ` · motion actor ${channels[selected]}`;
    $('inspectorStatus').textContent = detail;
    $('inspectorStatus').style.color = statusColor(actor);
    canvas.setAttribute('aria-label', `${actor?.name || nodes[selected].title}: ${detail}. Use left and right arrows to select other structures.`);
  }

  function updateUI() {
    $('connection').classList.toggle('offline', !connected);
    $('connectionText').textContent = connected ? 'LIVE CONNECTION' : 'RECONNECTING';
    for (const button of document.querySelectorAll('[data-command]')) {
      const isReset = button.dataset.command === 'reset';
      button.disabled = !connected || pending || !state || (isReset ? state.mode !== 'evacuated' : state.mode !== 'running');
    }
    if (!state) return;
    $('shiftLabel').textContent = `SHIFT ${String(state.run).padStart(2, '0')}`;
    $('clock').textContent = timeText(state.now);
    $('delivered').textContent = state.delivered.toLocaleString();
    $('inFlight').textContent = state.inFlight;
    $('actorCount').textContent = state.activeActors;
    if ($('animationActorCount')) $('animationActorCount').textContent = state.animationActors ?? '—';
    if ($('motionCompleted')) $('motionCompleted').textContent = (state.motionCompleted ?? 0).toLocaleString();
    $('transfers').textContent = state.transfers.toLocaleString();
    $('loadText').textContent = `${state.inFlight} / ${capacity}`;
    $('loadBar').style.width = `${clamp(state.inFlight / capacity, 0, 1) * 100}%`;
    $('loadBar').style.background = state.inFlight >= 20 ? colors[1] : colors[0];
    $('modeBadge').textContent = state.mode === 'running' ? 'LIVE' : state.mode === 'draining' ? 'DRAINING' : state.mode === 'failed' ? 'FAILED' : 'COMPLETE';
    $('modeBadge').style.color = ['draining', 'failed'].includes(state.mode) ? colors[1] : colors[0];
    $('bayText').textContent = state.bayClosed ? 'Reopen Aster bay' : 'Close Aster bay';
    $('factoryText').textContent = state.factoryPaused ? 'Resume the fabricator' : 'Pause the fabricator';
    $('surgeText').textContent = state.surge ? 'Return to normal traffic' : 'Call a traffic surge';
    $('bayButton').classList.toggle('active', state.bayClosed);
    $('factoryButton').classList.toggle('active', state.factoryPaused);
    $('surgeButton').classList.toggle('active', state.surge);
    const conserved = state.violations === 0 && (state.motionViolations ?? 0) === 0 && state.created === state.delivered + state.inFlight;
    $('integrity').textContent = conserved ? '◇  Every cargo and motion accounted for' : '△  Station integrity needs attention';
    $('integrity').style.color = conserved ? colors[0] : '#ff8d83';
    $('sceneStatus').textContent = !connected ? 'Telemetry interrupted. Reconnecting…'
      : state.mode === 'failed' ? `Station failure · ${state.inFlight} cargo remaining · check the station log`
      : state.mode === 'evacuated' ? 'All cargo delivered. All actors stopped.'
      : state.mode === 'draining' ? `Draining the dock · ${state.inFlight} cargo remaining`
      : state.factoryPaused ? 'Fabricator paused. Watch storage fill upstream.'
      : actorAt(1)?.status === 'restarting' ? 'Aster controller recovering. Its cargo is preserved.'
      : state.bayClosed ? 'Aster bay closed. Boreal is receiving arrivals.'
      : state.surge ? 'Traffic surge. The channels keep the port in balance.'
      : 'Every moving part has its own CSP actor. Follow a cargo to see the full journey.';
    $('evacuated').hidden = state.mode !== 'evacuated';
    $('evacSummary').textContent = conserved
      ? `All ${state.created} accepted cargo delivered. ${state.transfers} handoffs completed. Every actor has stopped safely.`
      : `The port has stopped with ${state.inFlight} cargo remaining and ${state.violations + (state.motionViolations ?? 0)} recorded integrity violations.`;
    const eventKey = JSON.stringify(state.events);
    if (lastEvents !== eventKey) {
      lastEvents = eventKey;
      const rows = state.events.map(event => {
        const row = document.createElement('li');
        const time = document.createElement('time');
        time.textContent = timeText(event.at);
        const text = document.createElement('span');
        text.textContent = event.text;
        row.append(time, text);
        return row;
      });
      $('events').replaceChildren(...rows);
    }
    updateInspector();
  }

  function acceptSnapshot(value) {
    if (!value || !['running', 'draining', 'evacuated', 'failed'].includes(value.mode)
      || !['run', 'now', 'created', 'delivered', 'inFlight', 'activeActors', 'transfers', 'restarts', 'violations'].every(key => Number.isFinite(value[key]))
      || !Array.isArray(value.actors) || !Array.isArray(value.cargo) || !Array.isArray(value.events)
      || value.actors.length !== nodes.length || value.cargo.length > 512) throw new Error('Station returned invalid telemetry.');
    if (state && (value.run < state.run || (value.run === state.run && value.now < state.now))) return;
    if (state && value.run !== state.run) lastEvents = '';
    for (const key of ['actors', 'cargo', 'events']) {
      value[key].forEach(Object.freeze);
      Object.freeze(value[key]);
    }
    state = Object.freeze(value);
    updateUI();
  }

  async function request(path, method = 'GET') {
    const abort = new AbortController();
    const timer = setTimeout(() => abort.abort(), 3500);
    try {
      const response = await fetch(path, {
        method, signal: abort.signal, cache: 'no-store',
        ...(method === 'POST' ? { headers: { 'X-Lifeboat-Control': '1' } } : {})
      });
      const value = await response.json();
      if (response.ok || response.status === 409) acceptSnapshot(value);
      if (!response.ok || value.ok === false) {
        throw new Error(response.status === 409 ? 'The controller is recovering or the dock is draining. Try again when it is ready.' : `Station request failed (${response.status}).`);
      }
      return value;
    } finally { clearTimeout(timer); }
  }

  function acceptScene(frame) {
    if (frame?.type !== 'scene' || !Number.isSafeInteger(frame.seq) || frame.seq !== lastSequence + 1
      || !Number.isSafeInteger(frame.run) || !Number.isFinite(frame.now) || typeof frame.reset !== 'boolean'
      || !Array.isArray(frame.motions) || !Array.isArray(frame.removed)
      || frame.motions.length > 256 || frame.removed.length > 256) throw new Error('Invalid scene frame');
    if ((!sceneRun || frame.run !== sceneRun) && !frame.reset) throw new Error('Scene reset missing');
    if (frame.reset && !frame.state) throw new Error('Scene snapshot missing');
    // Validate the complete delta before changing the displayed timeline.
    for (const motion of frame.motions) {
      if (typeof motion.key !== 'string' || motion.key.length > 80 || typeof motion.kind !== 'string'
        || typeof motion.phase !== 'string' || !Number.isSafeInteger(motion.revision)
        || !Number.isFinite(motion.at) || !Number.isFinite(motion.duration) || motion.duration < 0
        || !Number.isInteger(motion.cargo) || !Number.isInteger(motion.actor)
        || !Array.isArray(motion.frames) || !motion.frames.length || motion.frames.length > 128
        || motion.frames.some((f, i) => !Array.isArray(f) || f.length !== 4 || !f.every(Number.isFinite)
          || f[0] < 0 || f[0] > 1 || (i && f[0] <= motion.frames[i - 1][0]))) throw new Error('Invalid motion track');
    }
    for (const removal of frame.removed) {
      if (typeof removal.key !== 'string' || !Number.isFinite(removal.at)) throw new Error('Invalid scene removal');
    }
    if (frame.reset) {
      timelines.clear();
      poses.clear();
      sceneRun = frame.run;
      renderedAt = Math.max(0, frame.now - renderDelay);
      selectedCargo = 0;
    }
    for (const motion of frame.motions) {
      let timeline = timelines.get(motion.key);
      if (!timeline) { timeline = { tracks: [], removedAt: null }; timelines.set(motion.key, timeline); }
      if (timeline.tracks.some(track => track.revision >= motion.revision)) continue;
      motion.frames.forEach(Object.freeze);
      Object.freeze(motion.frames);
      timeline.tracks.push(Object.freeze(motion));
      timeline.tracks.sort((a, b) => a.at - b.at || a.revision - b.revision);
      timeline.removedAt = null;
      // Keep the revision containing the delayed render time and its future
      // successors. A new track never blindly replaces the preceding motion.
      while (timeline.tracks.length > 1 && timeline.tracks[1].at <= renderedAt - 2) timeline.tracks.shift();
      while (timeline.tracks.length > historyLimit && timeline.tracks[1].at <= renderedAt) timeline.tracks.shift();
      if (timeline.tracks.length > historyLimit) throw new Error('Motion history exceeded its bound');
    }
    for (const removal of frame.removed) {
      const timeline = timelines.get(removal.key);
      if (timeline) timeline.removedAt = removal.at;
    }
    if (timelines.size > 512) throw new Error('Scene exceeded its bound');
    stationNow = frame.now;
    receivedAt = performance.now();
    lastSequence = frame.seq;
    connected = true;
    retryDelay = 500;
    if (frame.state) acceptSnapshot(frame.state);
    else updateUI();
  }

  function connectScene() {
    const transport = new WebSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/api/stream`);
    socket = transport;
    lastSequence = 0;
    transport.addEventListener('message', event => {
      if (socket !== transport) return;
      try {
        const frame = JSON.parse(event.data);
        acceptScene(frame);
        // Backpressure belongs to this connection. Acknowledge only a fully
        // accepted frame; the server permits just one outstanding frame.
        transport.send(String(frame.seq));
      } catch (error) {
        $('controlMessage').textContent = 'The scene stream needs a fresh snapshot. Reconnecting…';
        transport.close(1002, 'Invalid scene frame');
      }
    });
    transport.addEventListener('error', () => transport.close());
    transport.addEventListener('close', () => {
      if (socket !== transport) return;
      connected = false;
      updateUI();
      setTimeout(connectScene, retryDelay);
      retryDelay = Math.min(retryDelay * 2, 5000);
    });
  }

  function interpolate(motion, now) {
    const t = motion.duration > 0 ? clamp((now - motion.at) / motion.duration, 0, 1) : 1;
    for (let i = 1; i < motion.frames.length; ++i) {
      const b = motion.frames[i];
      if (t <= b[0]) {
        const a = motion.frames[i - 1];
        const u = ease((t - a[0]) / (b[0] - a[0]));
        return point(mix(a[1], b[1], u), mix(a[2], b[2], u), mix(a[3], b[3], u));
      }
    }
    const end = motion.frames.at(-1);
    return point(end[1], end[2], end[3]);
  }

  function sampleScene(now) {
    if (connected) renderedAt = Math.max(renderedAt, stationNow + Math.min(Math.max(0, (now - receivedAt) / 1000), extrapolationLimit) - renderDelay);
    const sampled = new Map();
    for (const [key, timeline] of timelines) {
      if (timeline.removedAt !== null && renderedAt >= timeline.removedAt) {
        if (renderedAt > timeline.removedAt + 2) timelines.delete(key);
        continue;
      }
      let motion = null;
      for (const candidate of timeline.tracks) {
        if (candidate.at > renderedAt) break;
        motion = candidate;
      }
      if (!motion) continue;
      const position = Object.freeze(interpolate(motion, renderedAt));
      sampled.set(key, { ...motion, position });
      while (timeline.tracks.length > 1 && timeline.tracks[1].at <= renderedAt - 2) timeline.tracks.shift();
    }
    poses = sampled;
  }

  for (const button of document.querySelectorAll('[data-command]')) {
    button.addEventListener('click', async () => {
      if (button.disabled || pending) return;
      pending = true;
      $('controlMessage').textContent = '';
      updateUI();
      try { await request(`/api/${button.dataset.command}`, 'POST'); }
      catch (error) {
        $('controlMessage').textContent = error.name === 'AbortError'
          ? 'The station did not reply. Reconnecting to check its state.' : error.message;
      } finally { pending = false; updateUI(); }
    });
  }

  function poly(points, fill, stroke, lineWidth = 1) {
    ctx.beginPath();
    points.forEach((p, i) => { if (i === 0) ctx.moveTo(p.x, p.y); else ctx.lineTo(p.x, p.y); });
    ctx.closePath();
    if (fill) { ctx.fillStyle = fill; ctx.fill(); }
    if (stroke) { ctx.strokeStyle = stroke; ctx.lineWidth = lineWidth; ctx.stroke(); }
  }
  function poly3(points, fill, stroke, lineWidth = 1) {
    poly(points.map(p => project(p.x, p.y, p.z)), fill, stroke, lineWidth);
  }
  function line3(points, color, lineWidth = 1, dash = []) {
    ctx.beginPath();
    points.forEach((p, i) => { const q = project(p.x, p.y, p.z); if (i === 0) ctx.moveTo(q.x, q.y); else ctx.lineTo(q.x, q.y); });
    ctx.strokeStyle = color; ctx.lineWidth = lineWidth; ctx.setLineDash(dash); ctx.stroke(); ctx.setLineDash([]);
  }
  function box(x, y, w, d, h, top = '#334b5b', front = '#172837', side = '#203747', z = 0) {
    const a = point(x, y, z), b = point(x + w, y, z), c = point(x + w, y + d, z), e = point(x, y + d, z);
    const A = point(x, y, z + h), B = point(x + w, y, z + h), C = point(x + w, y + d, z + h), E = point(x, y + d, z + h);
    poly3([b, c, C, B], side, '#09162255', 0.75);
    poly3([e, c, C, E], front, '#09162255', 0.75);
    poly3([A, B, C, E], top, '#7397a02a', 0.8);
    return { a, b, c, e, A, B, C, E };
  }
  function glow(x, y, z, radius, color, strength = 0.4) {
    const p = project(x, y, z);
    const light = ctx.createRadialGradient(p.x, p.y, 0, p.x, p.y, radius);
    light.addColorStop(0, hexAlpha(color, strength)); light.addColorStop(1, hexAlpha(color, 0));
    ctx.fillStyle = light; ctx.fillRect(p.x - radius, p.y - radius, radius * 2, radius * 2);
  }
  function lamp(x, y, z, color = colors[0], radius = 2) {
    const p = project(x, y, z);
    ctx.shadowColor = color; ctx.shadowBlur = 10;
    ctx.fillStyle = color; ctx.beginPath(); ctx.arc(p.x, p.y, radius, 0, Math.PI * 2); ctx.fill();
    ctx.shadowBlur = 0;
  }
  function textAt(text, x, y, z, size = 10, color = '#829cae', align = 'left') {
    const p = project(x, y, z);
    ctx.fillStyle = color; ctx.font = `500 ${size}px ui-monospace, SFMono-Regular, monospace`; ctx.textAlign = align;
    ctx.fillText(text, p.x, p.y);
  }
  function shadow(x, y, w, d, opacity = 0.24) {
    poly3([point(x, y), point(x + w, y), point(x + w + 32, y + d + 23), point(x + 32, y + d + 23)], `rgba(0,4,12,${opacity})`);
  }

  function drawBackdrop(now) {
    const gradient = ctx.createLinearGradient(0, 0, width, height);
    gradient.addColorStop(0, '#091422'); gradient.addColorStop(0.55, '#102133'); gradient.addColorStop(1, '#101e2c');
    ctx.fillStyle = gradient; ctx.fillRect(0, 0, width, height);
    const fog = ctx.createRadialGradient(width * 0.68, height * 0.3, 0, width * 0.68, height * 0.3, width * 0.63);
    fog.addColorStop(0, '#26445a33'); fog.addColorStop(1, '#10213300');
    ctx.fillStyle = fog; ctx.fillRect(0, 0, width, height);
    // These fixed stars are scenery, independent of simulation activity.
    for (let i = 0; i < 170; ++i) {
      const x = ((Math.sin(i * 127.1 + 2) * 43758.5453) % 1 + 1) % 1 * width;
      const y = ((Math.sin(i * 311.7 + 9) * 43758.5453) % 1 + 1) % 1 * (height - 45);
      const alpha = 0.15 + (i % 5) * 0.045 + Math.sin(now * 0.0004 + i) * 0.035;
      ctx.fillStyle = `rgba(170,208,228,${alpha})`; ctx.fillRect(x, y, i % 17 === 0 ? 2 : 1, i % 17 === 0 ? 2 : 1);
    }
    const px = width * 0.89, py = height * 0.17, radius = Math.min(width * 0.155, 150);
    const halo = ctx.createRadialGradient(px, py, radius * 0.85, px, py, radius * 1.25);
    halo.addColorStop(0, '#638ea128'); halo.addColorStop(1, '#638ea100');
    ctx.fillStyle = halo; ctx.beginPath(); ctx.arc(px, py, radius * 1.25, 0, Math.PI * 2); ctx.fill();
    ctx.save(); ctx.beginPath(); ctx.arc(px, py, radius, 0, Math.PI * 2); ctx.clip();
    const planet = ctx.createRadialGradient(px - radius * 0.55, py - radius * 0.3, 0, px, py, radius * 1.65);
    planet.addColorStop(0, '#36505e'); planet.addColorStop(0.43, '#203848'); planet.addColorStop(0.78, '#102131'); planet.addColorStop(1, '#0b1723');
    ctx.fillStyle = planet; ctx.fillRect(px - radius, py - radius, radius * 2, radius * 2);
    ctx.translate(px, py); ctx.rotate(-0.38);
    for (let i = 0; i < 13; ++i) {
      ctx.beginPath(); ctx.ellipse(-radius * 0.2, -radius + i * radius * 0.17, radius * 1.22, radius * (0.12 + i % 3 * 0.04), 0, 0, Math.PI);
      ctx.strokeStyle = i % 3 ? '#60838b17' : '#7896991f'; ctx.lineWidth = 3 + i % 4; ctx.stroke();
    }
    ctx.restore();
    ctx.strokeStyle = '#83b7cb30'; ctx.lineWidth = 1; ctx.beginPath(); ctx.arc(px, py, radius, Math.PI * 0.9, Math.PI * 1.7); ctx.stroke();
    ctx.save(); ctx.translate(width * 0.52, height * 0.47); ctx.rotate(-0.22);
    ctx.strokeStyle = '#4c718316'; ctx.setLineDash([3, 8]); ctx.beginPath(); ctx.ellipse(0, 0, width * 0.57, height * 0.25, 0, 0, Math.PI * 2); ctx.stroke(); ctx.restore();
  }

  function drawDeck() {
    const outline = [point(-450, -210), point(430, -210), point(500, -140), point(500, 295), point(445, 350), point(-400, 350), point(-465, 285), point(-465, -170)];
    const bottom = outline.map(p => point(p.x, p.y, -42));
    for (let i = 0; i < outline.length; ++i) {
      const j = (i + 1) % outline.length;
      poly3([outline[i], outline[j], bottom[j], bottom[i]], i < 3 ? '#183141' : '#0c1a27', '#2f4d5b', 1);
    }
    poly3(outline, '#203444', '#5c7e8b', 1.4);
    // Modular deck plates remain legible beneath the traffic.
    ctx.save();
    const projected = outline.map(p => project(p.x, p.y, 0));
    ctx.beginPath(); projected.forEach((p, i) => i ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y)); ctx.closePath(); ctx.clip();
    for (let x = -480; x <= 500; x += 70) line3([point(x, -225, 0.3), point(x, 360, 0.3)], '#7695a314', 0.7);
    for (let y = -220; y <= 350; y += 70) line3([point(-480, y, 0.3), point(510, y, 0.3)], '#7695a314', 0.7);
    poly3([point(-460, 282), point(500, 282), point(500, 338), point(-460, 338)], '#142836');
    poly3([point(-188, -210), point(-152, -210), point(-152, 280), point(-188, 280)], '#152a38');
    for (let y = -190; y < 235; y += 24) line3([point(-176, y, 0.5), point(-176, y + 12, 0.5)], '#839c9b45', 1.5);
    for (let x = -415; x < 470; x += 28) line3([point(x, 316, 0.5), point(x + 12, 316, 0.5)], '#758f9e40', 1.3);
    ctx.restore();
    const rails = [[-437, -185, 401, -185], [477, -120, 477, 271], [-386, 327, 427, 327]];
    for (const [x1, y1, x2, y2] of rails) {
      line3([point(x1, y1, 11), point(x2, y2, 11)], '#5b7a8866', 1.1);
      const n = Math.ceil(Math.hypot(x2 - x1, y2 - y1) / 48);
      for (let i = 0; i <= n; ++i) { const x = mix(x1, x2, i / n), y = mix(y1, y2, i / n); line3([point(x, y, 1), point(x, y, 11)], '#65829188', 1); }
    }
    for (let x = -370; x < 435; x += 75) {
      box(x, 349, 31, 2, 4, '#325263', '#325263', '#325263', -25);
      lamp(x + 10, 350, -11, '#78bcca', 1.6);
    }
    for (let y = -70; y < 240; y += 75) lamp(501, y, -12, '#84c7d1', 1.5);
    // Deck edge approach beacons and physical mooring fingers.
    for (const y of [-100, 120]) {
      box(-520, y - 27, 71, 42, 15, '#324b5c', '#122633', '#243c4d', -8);
      box(-531, y - 32, 13, 51, 8, '#4d6976', '#294453', '#34505d', -2);
      lamp(-523, y - 22, 8, colors[1]); lamp(-523, y + 11, 8, colors[1]);
      line3([point(-499, y - 16, 8), point(-462, y - 16, 8)], '#dfbb7277', 2);
    }
    textAt('PORT  /  MERIDIAN', -280, 327, 2, 15, '#7f9dad88');
    textAt('KEPLER · 07', 395, 290, 1, 9, '#879daa88');
  }

  function drawTower() {
    shadow(-451, -169, 53, 48);
    box(-450, -170, 48, 45, 7, '#425a65', '#223846', '#2b4552');
    box(-438, -158, 27, 28, 54, '#627b83', '#2a4554', '#385564', 7);
    box(-451, -168, 51, 47, 27, '#4d6874', '#294652', '#365768', 61);
    box(-447, -122, 41, 1, 10, '#7cb2ba', '#52808f', '#43697a', 70);
    line3([point(-424, -145, 88), point(-424, -145, 123)], '#8ca7b0', 1.5);
    const a = project(-424, -145, 110);
    ctx.strokeStyle = '#afc9cf'; ctx.lineWidth = 1.4; ctx.beginPath(); ctx.ellipse(a.x, a.y, 14, 5, -0.3, 0, Math.PI * 2); ctx.stroke();
    lamp(-424, -145, 124, statusColor(actorAt(0)), 2);
    box(-397, -165, 26, 36, 10, '#344c5c', '#1b3240', '#263f4f');
    for (let i = 0; i < 3; ++i) box(-394 + i * 7, -160, 3, 25, 2, '#527b8b', '#233c49', '#233c49', 10);
  }

  function drawRoutes() {
    // Physical guideways describe the station's architecture. Only server
    // Motion tracks move the loads that travel over them.
    const routes = [
      { points: [[-225, -2], [-148, 52], [-148, -196], [-50, -180]], color: '#85c9bb' },
      { points: [[-185, 120], [-148, 52]], color: '#85c9bb' },
      { points: [[-49, -60], [-49, -12], [-9, 12], [120, 12], [175, -23]], color: '#8fb9d7' },
      { points: [[285, -26], [284, -10], [330, 30], [330, 165], [260, 205], [260, 228], [111, 228]], color: '#dfc188' }
    ];
    for (const route of routes) {
      for (let i = 1; i < route.points.length; ++i) {
        const [ax, ay] = route.points[i - 1], [bx, by] = route.points[i];
        const length = Math.hypot(bx - ax, by - ay);
        const nx = -(by - ay) / length, ny = (bx - ax) / length;
        const half = 14;
        poly3([point(ax - nx * half, ay - ny * half, 3), point(bx - nx * half, by - ny * half, 3), point(bx + nx * half, by + ny * half, 3), point(ax + nx * half, ay + ny * half, 3)], '#102a38', '#4a6572', 0.8);
        line3([point(ax - nx * half, ay - ny * half, 6), point(bx - nx * half, by - ny * half, 6)], hexAlpha(route.color, 0.8), 1.8);
        line3([point(ax + nx * half, ay + ny * half, 6), point(bx + nx * half, by + ny * half, 6)], '#557d89', 1.8);
        for (let d = 6; d < length; d += 13) {
          const x = mix(ax, bx, d / length), y = mix(ay, by, d / length);
          line3([point(x - nx * 10, y - ny * 10, 4), point(x + nx * 10, y + ny * 10, 4)], '#6b899875', 1.3);
          if (Math.floor(d / 13) % 4 === 0) lamp(x - nx * half, y - ny * half, 6, route.color, 1.2);
        }
      }
    }
    for (const [x, y, number] of [[-225, -2, '01'], [-185, 120, '02']]) {
      box(x - 10, y - 9, 43, 36, 5, '#36576a', '#213b4b', '#2c4759', 2);
      line3([point(x - 6, y - 6, 8), point(x + 28, y - 6, 8), point(x + 28, y + 23, 8)], '#e5c98c', 1.7);
      textAt(number, x - 10, y + 38, 1, 8, '#d6c79e');
    }
    textAt('RAW / RECEIVE', -156, -167, 9, 8, '#9cbeb8');
    textAt('DISPATCH', 40, 29, 7, 8, '#a2c5d3');
    textAt('FINISHED GOODS', 348, 121, 9, 8, '#dfcba2');
  }

  function drawCrane(id) {
    const node = nodes[id];
    const motion = poses.get(`crane:${id}`);
    const hook = motion?.position || point(node.x - 90, node.y, 110);
    const end = point(hook.x, hook.y, 155);
    const angle = Math.atan2(hook.y - node.y, hook.x - node.x);
    const x = node.x, y = node.y;
    const actor = actorAt(id);
    const warning = /restarting|bay closed/.test(actor?.status || '');
    shadow(x - 34, y - 26, 86, 70, 0.32);
    poly3([point(x - 47, y - 35, 0.5), point(x + 55, y - 35, 0.5), point(x + 55, y + 45, 0.5), point(x - 47, y + 45, 0.5)], '#112938', '#60777466', 1);
    for (let i = 0; i < 5; ++i) line3([point(x - 40 + i * 17, y + 37, 1), point(x - 32 + i * 17, y + 30, 1)], '#cfad6570', 3);
    box(x - 20, y - 20, 42, 42, 13, '#718785', '#3a5057', '#526568');
    box(x - 11, y - 9, 23, 19, 122, '#acbaa8', '#52696a', '#84978c', 13);
    for (let z = 25; z < 120; z += 23) {
      line3([point(x - 12, y + 11, z), point(x + 12, y + 11, z + 20)], '#182f3999', 3);
      line3([point(x - 12, y + 11, z + 20), point(x + 12, y + 11, z)], '#182f3999', 3);
    }
    box(x - 19, y - 18, 38, 38, 15, '#bcc5ac', '#657875', '#8c9d8d', 129);
    const counter = point(x - Math.cos(angle) * 49, y - Math.sin(angle) * 49, 146);
    line3([counter, point(x, y, 146), point(end.x, end.y, 146)], warning ? '#bfa77b' : '#a5b8a4', 10);
    line3([point(counter.x, counter.y, 160), point(x, y, 160), point(end.x, end.y, 160)], '#d2d5b7', 2.4);
    for (let i = 0; i < 7; ++i) {
      const a = i / 7, b = (i + 1) / 7;
      line3([point(mix(x, end.x, a), mix(y, end.y, a), 147), point(mix(x, end.x, b), mix(y, end.y, b), 159)], '#5c7777', 1.5);
    }
    box(counter.x - 15, counter.y - 14, 28, 28, 20, '#697c75', '#3b5156', '#51686a', 131);
    box(x - 31, y - 18, 22, 26, 23, '#537887', '#243f51', '#375a69', 116);
    box(x - 29, y + 9, 18, 1, 8, '#99d1d3', '#81b7bc', '#6d9ca7', 125);
    line3([end, hook], '#bdcbd1a6', 1.1);
    line3([point(hook.x - 12, hook.y, hook.z), point(hook.x + 12, hook.y, hook.z)], '#e2c382', 3);
    lamp(x, y, 165, statusColor(actor), 2.8);
    textAt(id === 1 ? '01' : '02', x + 16, y + 32, 12, 13, '#cbd4bf');
    if (warning) glow(x, y, 95, 60, colors[1], 0.09);
  }

  function drawDoor(key, x, y, base, w = 36, h = 35) {
    const openness = clamp(poses.get(key)?.position.x || 0, 0, 1);
    box(x - w / 2 - 3, y - 1, w + 6, 4, h + 4, '#6b9299', '#375763', '#547c87', base);
    poly3([point(x - w / 2, y + 4, base), point(x + w / 2, y + 4, base), point(x + w / 2, y + 4, base + h), point(x - w / 2, y + 4, base + h)], '#0a1e2d');
    if (openness < 1) {
      const z = base + h * openness;
      poly3([point(x - w / 2, y + 4.2, z), point(x + w / 2, y + 4.2, z), point(x + w / 2, y + 4.2, base + h), point(x - w / 2, y + 4.2, base + h)], '#567785', '#8ca6ae66', 0.7);
      for (let i = 1; i < 6; ++i) {
        const rib = base + h * (openness + (1 - openness) * i / 6);
        line3([point(x - w / 2 + 1, y + 4.5, rib), point(x + w / 2 - 1, y + 4.5, rib)], '#243f4f', 1);
      }
      line3([point(x - w / 2, y + 4.5, z), point(x + w / 2, y + 4.5, z)], '#edc77c', 2);
    }
    lamp(x - w / 2 - 4, y + 4, base + h + 5, openness > 0 ? colors[0] : colors[1], 1.5);
  }

  function drawWarehouse() {
    shadow(-113, -169, 181, 126, 0.34);
    // The rear receiving gate is behind the hold. Its gantry and signal
    // remain visible above the roof while cargo disappears into the intake.
    box(-80, -188, 62, 25, 7, '#395661', '#213b49', '#2e4a58');
    drawDoor('door:hold-in', -50, -184, 8, 42, 36);
    for (const x of [-78, -22]) box(x, -182, 5, 7, 88, '#6d8990', '#375969', '#507380', 8);
    line3([point(-76, -178, 96), point(-20, -178, 96)], '#8ea7a7', 4);
    lamp(-48, -178, 99, (poses.get('door:hold-in')?.position.x || 0) > 0 ? colors[0] : colors[1], 2);
    box(-121, -169, 193, 127, 7, '#47616b', '#233b49', '#2e4955');
    box(-111, -162, 174, 99, 63, '#466370', '#263f51', '#315166', 7);
    // Ribbed sawtooth roof and illuminated receiving doors.
    for (let x = -104; x < 65; x += 21) {
      box(x, -158, 11, 91, 5, '#5b7782', '#405d69', '#466672', 70);
      line3([point(x + 3, -148, 76), point(x + 3, -77, 76)], '#9cb2b54a', 0.8);
    }
    for (const x of [-94, 13]) {
      box(x, -62, 28, 2, 34, '#294957', '#152b3a', '#203e4c', 10);
      for (let z = 17; z < 39; z += 6) line3([point(x + 3, -59, z), point(x + 25, -59, z)], '#56778466', 1);
      lamp(x + 14, -59, 48, statusColor(actorAt(3)), 1.4);
    }
    box(-112, -54, 177, 55, 6, '#34515e', '#203c4a', '#2b4855');
    drawDoor('door:hold', -49, -64, 7, 37, 36);
    for (let x = -105; x < 62; x += 21) line3([point(x, -4, 6.5), point(x + 10, -14, 6.5)], '#c3b58b66', 2);
    textAt('HOLD / 10', -94, -56, 52, 10, '#adc6c9');
  }

  function drawFactory() {
    shadow(149, -147, 181, 136, 0.31);
    box(144, -145, 187, 127, 8, '#42616d', '#233e4d', '#2a4859');
    box(157, -133, 159, 104, 52, '#53717c', '#2b495c', '#365c6e', 8);
    box(189, -127, 106, 88, 28, '#719098', '#365666', '#547683', 60);
    box(203, -116, 73, 65, 10, '#3f6977', '#325661', '#355663', 88);
    for (let i = 0; i < 4; ++i) {
      box(211 + i * 15, -112, 8, 49, 2, '#92c9ca', '#5b8c99', '#5b8c99', 98);
      line3([point(213 + i * 15, -107, 101), point(213 + i * 15, -68, 101)], '#b8e7dc77', 1);
    }
    drawDoor('door:fab-in', 175, -30, 8, 36, 35);
    drawDoor('door:fab-out', 285, -30, 8, 36, 35);
    box(214, -26, 28, 3, 18, '#547884', '#416574', '#426c78', 29);
    for (let i = 0; i < 4; ++i) lamp(218 + i * 6, -21, 38, state?.factoryPaused ? colors[1] : '#8be6d5', 1.3);
    const effect = poses.get('factory:4');
    const pulse = clamp(effect?.position.x || 0, 0, 1);
    if (pulse > 0) {
      glow(235, -56, 83, 66, '#9deeff', pulse * 0.32);
      for (let i = 0; i < 5; ++i) {
        line3([point(217 + i * 9, -83, 101), point(217 + i * 9, -64, 101)], hexAlpha('#d0fbff', pulse), 2);
      }
    }
    for (const x of [167, 306]) {
      box(x, -141, 13, 15, 94, '#8da4a7', '#385767', '#5d7b88');
      box(x - 4, -145, 21, 23, 8, '#738e96', '#4a6873', '#5a7883', 94);
      lamp(x + 7, -133, 105, statusColor(actorAt(4)), 1.7);
    }
    textAt('FAB / 04', 237, -25, 52, 10, '#bed4d7');
  }

  function drawHabitat() {
    shadow(374, 155, 112, 146, 0.3);
    box(361, 160, 126, 144, 9, '#3d5c67', '#294250', '#304c59');
    box(373, 166, 102, 127, 36, '#5d7e85', '#315464', '#466a77', 9);
    for (let row = 0; row < 3; ++row) for (let col = 0; col < 5; ++col) {
      box(378 + col * 18, 294, 9, 1, 5, '#9be6ce', '#93d9c3', '#93d9c3', 16 + row * 8);
    }
    // A curved glazed greenhouse, drawn as overlapping structural ribs.
    for (let i = 0; i < 7; ++i) {
      const y = 171 + i * 17;
      const roof = [];
      for (let s = 0; s <= 20; ++s) {
        const angle = s / 20 * Math.PI;
        roof.push(point(424 - Math.cos(angle) * 49, y, 45 + Math.sin(angle) * 44));
      }
      if (i < 6) {
        const next = roof.slice().reverse().map(p => point(p.x, p.y + 17, p.z));
        poly3([...roof, ...next], i % 2 ? '#386a705f' : '#447f815f', '#8bbdbd33', 1);
      }
      line3(roof, '#a4c8c980', 1.2);
    }
    for (let i = 0; i < 5; ++i) {
      const z = 45 + Math.sin((i + 1) / 6 * Math.PI) * 44;
      const x = 424 - Math.cos((i + 1) / 6 * Math.PI) * 49;
      line3([point(x, 171, z), point(x, 273, z)], '#9bc7c747', 0.7);
    }
    glow(424, 237, 57, 58, '#86ebc0', 0.07);
    textAt('HABITAT 07', 424, 310, 54, 11, '#bed7cf', 'center');
    lamp(481, 290, 17, state?.mode === 'evacuated' ? '#527268' : colors[0], 2);
  }

  function drawPlatform() {
    box(90, 224, 263, 29, 11, '#54717c', '#274351', '#365565');
    box(90, 220, 263, 4, 2, '#d1c189', '#8c865f', '#8c865f', 11);
    for (const y of [272, 293]) {
      line3([point(-89, y, 3), point(462, y, 3)], '#8aadb6', 2.2);
      line3([point(-89, y + 4, 1), point(462, y + 4, 1)], '#0c202e', 3.2);
    }
    for (let x = -83; x < 459; x += 19) line3([point(x, 268, 1), point(x, 299, 1)], '#52768569', 2);
    for (const x of [110, 321]) {
      box(x, 238, 5, 5, 43, '#91b5b8', '#526e7e', '#657f8a');
      line3([point(x, 238, 45), point(x + 16, 238, 45)], '#8dabb4', 2);
      lamp(x + 15, 239, 43, '#adf3df', 1.7);
    }
    textAt('M  /  07', 304, 227, 13, 10, '#d6dec5');
  }

  function drawTram(motion) {
    const { x, y, z } = motion.position;
    shadow(x - 40, y - 2, 97, 20, 0.3);
    box(x - 46, y - 1, 102, 26, 5, '#344c58', '#102939', '#1b3444', z + 4);
    box(x - 43, y, 91, 24, 20, '#b7c9c5', '#527483', '#829ea5', z + 9);
    box(x - 35, y + 2, 74, 20, 4, '#d1d9c9', '#8ca49f', '#b2c3b9', z + 29);
    for (let i = 0; i < 5; ++i) box(x - 35 + i * 14, y + 24, 10, 1, 9, '#85bdcb', '#558694', '#6897a3', z + 17);
    line3([point(x - 41, y + 26, z + 13), point(x + 46, y + 26, z + 13)], '#92ecd5', 2);
    lamp(x + 50, y + 4, z + 15, '#d4f7e1', 1.8); lamp(x + 50, y + 19, z + 15, '#d4f7e1', 1.8);
  }

  function cargoBox(p, type, size = 19, finished = false) {
    const color = colors[type] || colors[0];
    shadow(p.x, p.y, size, size * 0.78, 0.15);
    box(p.x, p.y, size, size * 0.78, size * 0.72, color, hexAlpha(color, 0.65), hexAlpha(color, 0.84), p.z);
    line3([point(p.x + size * 0.22, p.y + size * 0.79, p.z + 2), point(p.x + size * 0.22, p.y + size * 0.79, p.z + size * 0.68)], '#102a3a80', 1.6);
    line3([point(p.x + size * 0.7, p.y + size * 0.79, p.z + 2), point(p.x + size * 0.7, p.y + size * 0.79, p.z + size * 0.68)], '#102a3a80', 1.6);
    line3([point(p.x + 2, p.y + size * 0.35, p.z + size * 0.73), point(p.x + size - 2, p.y + size * 0.35, p.z + size * 0.73)], '#e5fff366', 1);
    if (finished) {
      box(p.x - 1, p.y - 1, size + 2, size * 0.78 + 2, 2, '#e5d29a', '#9f8f63', '#c5b37f', p.z);
      for (const at of [0.16, 0.76]) {
        line3([point(p.x + size * at, p.y, p.z + size * 0.74), point(p.x + size * at, p.y + size * 0.79, p.z + size * 0.74), point(p.x + size * at, p.y + size * 0.79, p.z + 1)], '#ffdf91', 2.4);
      }
      box(p.x + size * 0.35, p.y + size * 0.79, size * 0.25, 0.3, 4, '#fcf1d1', '#fcf1d1', '#fcf1d1', p.z + size * 0.35);
    }
  }

  function cargoVisibility(motion) {
    if (!['raw', 'goods'].includes(motion.kind)) return { visible: false };
    const p = motion.position;
    const buildings = [
      { x1: -121, x2: 72, back: -169, front: -56, roof: 75, doors: [{ key: 'door:hold', x: -49, y: -60, base: 7, w: 37, h: 36 }], depth: -60 },
      { x1: 151, x2: 319, back: -141, front: -22, roof: 65, doors: [{ key: 'door:fab-in', x: 175, y: -26, base: 8, w: 36, h: 35 }, { key: 'door:fab-out', x: 285, y: -26, base: 8, w: 36, h: 35 }], depth: 220 }
    ];
    for (const building of buildings) {
      if (p.z >= building.roof || p.x < building.x1 || p.x > building.x2) continue;
      if (p.y >= building.front && p.y < building.front + 26) return { visible: true, depth: building.depth };
      if (p.y < building.back || p.y >= building.front) continue;
      const door = building.doors.find(door => Math.abs(p.x + 9 - door.x) < door.w / 2 + 8 && p.y > building.front - 26);
      if (!door) return { visible: false };
      const openness = clamp(poses.get(door.key)?.position.x || 0, 0, 1);
      if (openness <= 0.01) return { visible: false };
      return { visible: true, depth: building.depth, clip: [point(door.x - door.w / 2, door.y + 0.1, door.base), point(door.x + door.w / 2, door.y + 0.1, door.base), point(door.x + door.w / 2, door.y + 0.1, door.base + door.h * openness), point(door.x - door.w / 2, door.y + 0.1, door.base + door.h * openness)] };
    }
    const inHabitat = p.x > 371 && p.x < 480 && p.y > 164 && p.y < 297 && p.z < 90;
    return { visible: true, depth: inHabitat ? 684 : undefined };
  }

  function drawCargo(motion, visibility) {
    ctx.save();
    if (visibility.clip) {
      const shape = visibility.clip.map(p => project(p.x, p.y, p.z));
      ctx.beginPath(); shape.forEach((p, i) => i ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y)); ctx.closePath(); ctx.clip();
    }
    cargoBox(motion.position, motion.cargo % colors.length, 19, motion.kind === 'goods');
    ctx.restore();
  }
  function drawShip(motion) {
    const p = motion.position;
    // The server anchor is the cargo's deck origin, shared by ship and box.
    // Shape the empty hull around that origin; never offset the cargo track.
    const x = p.x + 35, y = p.y - 7, z = p.z - 9;
    const color = colors[motion.cargo % colors.length];
    const moving = motion.duration > 0 && renderedAt < motion.at + motion.duration;
    const engine = project(x - 41, y + 15, z + 8);
    if (moving) {
      const exhaust = ctx.createLinearGradient(engine.x, engine.y, engine.x - 52, engine.y - 22);
      exhaust.addColorStop(0, '#a0e7efaa'); exhaust.addColorStop(1, '#6bbef200');
      poly([{ x: engine.x - 4, y: engine.y - 6 }, { x: engine.x - 66, y: engine.y - 27 }, { x: engine.x, y: engine.y + 6 }], exhaust);
    }
    box(x - 40, y - 4, 63, 34, 9, '#7696a0', '#304b60', '#4b6c80', z);
    poly3([point(x + 23, y - 4, z + 9), point(x + 48, y + 13, z + 7), point(x + 23, y + 30, z + 9)], '#aec0be', '#98b4b3', 0.6);
    poly3([point(x + 23, y + 30, z + 9), point(x + 48, y + 13, z + 7), point(x + 44, y + 13, z), point(x + 23, y + 30, z)], '#526e7f');
    box(x + 4, y + 5, 17, 18, 7, '#80bbcb', '#3f697e', '#6a97a6', z + 9);
    box(x - 35, y - 10, 28, 9, 9, '#587986', '#203c50', '#3e6071', z - 1);
    box(x - 35, y + 30, 28, 9, 9, '#587986', '#203c50', '#3e6071', z - 1);
    lamp(x - 37, y - 5, z + 5, moving ? '#a0e6fa' : '#507c94', 2);
    lamp(x - 37, y + 35, z + 5, moving ? '#a0e6fa' : '#507c94', 2);
    lamp(x + 38, y + 13, z + 8, color, 1.5);
  }

  function drawChannelView() {
    if (!xray) return;
    const links = [
      { from: 0, to: 1, points: [[-480, -100], [-330, -100], [-304, -48]], name: 'ARRIVALS / 8', x: -527, y: -19 },
      { from: 0, to: 2, points: [[-480, -100], [-435, 176], [-206, 190]] },
      { from: 1, to: 3, points: [[-304, -48], [-220, -15], [-20, -15]] },
      { from: 2, to: 3, points: [[-206, 190], [-156, 100], [-20, -15]], name: 'STORAGE / 10', x: -160, y: 83 },
      { from: 3, to: 4, points: [[-20, -15], [100, -15], [180, -15]], name: 'RENDEZVOUS', x: 105, y: -15 },
      { from: 4, to: 5, points: [[180, -15], [215, 113], [215, 238]], name: 'DEPARTURES / 4', x: 260, y: 161 }
    ];
    for (const link of links) {
      const blocked = actorAt(link.from)?.status === 'backpressure';
      const color = blocked ? colors[1] : '#8fe6cf';
      line3(link.points.map(p => point(p[0], p[1], 5)), hexAlpha(color, 0.16), 9);
      line3(link.points.map(p => point(p[0], p[1], 6)), hexAlpha(color, 0.84), 1.6, [5, 5]);
      if (link.name) {
        const p = project(link.x, link.y, 12);
        ctx.font = '9px ui-monospace, monospace'; const w = ctx.measureText(link.name).width + 16;
        ctx.fillStyle = '#071824eb'; ctx.fillRect(p.x - w / 2, p.y - 11, w, 18);
        ctx.strokeStyle = hexAlpha(color, 0.4); ctx.lineWidth = 0.7; ctx.strokeRect(p.x - w / 2, p.y - 11, w, 18);
        ctx.textAlign = 'center'; ctx.fillStyle = color; ctx.fillText(link.name, p.x, p.y + 1);
      }
    }
    for (const motion of poses.values()) {
      if (!['raw', 'goods'].includes(motion.kind) || !cargoVisibility(motion).visible) continue;
      const p = motion.position;
      glow(p.x, p.y, p.z + 15, 23, colors[motion.cargo % colors.length], 0.22);
    }
  }

  function screenPoint(p) {
    return { x: width * 0.52 + pan.x + p.x * scale * zoom, y: height * 0.52 + pan.y + p.y * scale * zoom };
  }
  function drawLabels() {
    hitAreas = [];
    const placed = [];
    for (const node of nodes) {
      const anchor = screenPoint(project(node.x, node.y, node.z));
      const p = { x: anchor.x + node.dx * scale * zoom, y: anchor.y + node.dy * scale * zoom };
      const font = width < 500 ? 8 : 9;
      ctx.font = `600 ${font}px ui-sans-serif, system-ui, sans-serif`;
      const labelWidth = ctx.measureText(node.title).width + 27;
      p.x = clamp(p.x, labelWidth / 2 + 10, width - labelWidth / 2 - 10);
      p.y = clamp(p.y, 76, height - 103);
      // Labels have a readable screen-space size even when the port is small.
      // Resolve collisions without changing the actual structure positions.
      for (let attempt = 0; attempt < 6; ++attempt) {
        const collision = placed.find(other => Math.abs(other.x - p.x) < (other.w + labelWidth) / 2 + 5 && Math.abs(other.y - p.y) < 35);
        if (!collision) break;
        p.y = collision.y + 36 < height - 103 ? collision.y + 36 : collision.y - 36;
      }
      placed.push({ x: p.x, y: p.y, w: labelWidth });
      const color = statusColor(actorAt(node.id));
      const active = node.id === selected || node.id === hover;
      ctx.strokeStyle = active ? hexAlpha(color, 0.9) : '#60859688'; ctx.lineWidth = 0.8;
      ctx.beginPath(); ctx.moveTo(anchor.x, anchor.y); ctx.lineTo(p.x, p.y + 13); ctx.stroke();
      ctx.fillStyle = active ? '#18333eee' : '#0b1a28ed';
      ctx.beginPath(); ctx.roundRect(p.x - labelWidth / 2, p.y - 11, labelWidth, 31, 4); ctx.fill();
      ctx.strokeStyle = active ? hexAlpha(color, 0.8) : '#4664737a'; ctx.stroke();
      ctx.fillStyle = color; ctx.beginPath(); ctx.arc(p.x - labelWidth / 2 + 9, p.y - 1, 2.2, 0, Math.PI * 2); ctx.fill();
      ctx.textAlign = 'left'; ctx.fillStyle = active ? '#f0faf6' : '#d1e0e5'; ctx.fillText(node.title, p.x - labelWidth / 2 + 17, p.y + 2);
      ctx.fillStyle = '#7e9cac'; ctx.font = `500 ${font - 2}px ui-monospace, monospace`; ctx.fillText(node.subtitle, p.x - labelWidth / 2 + 17, p.y + 13);
      hitAreas.push({ id: node.id, x: p.x - labelWidth / 2 - 8, y: p.y - 17, w: labelWidth + 16, h: 42 });
      const building = screenPoint(project(node.x, node.y, node.z * 0.5));
      hitAreas.push({ id: node.id, x: building.x - 40 * scale * zoom, y: building.y - 45 * scale * zoom, w: 80 * scale * zoom, h: 90 * scale * zoom });
    }
  }

  function drawCargoLabels() {
    cargoHits = [];
    for (const motion of poses.values()) {
      if (!['raw', 'goods'].includes(motion.kind) || !cargoVisibility(motion).visible) continue;
      const { x, y, z } = motion.position;
      const anchor = screenPoint(project(x + 9, y + 7, z + 18));
      if (anchor.x < 10 || anchor.x > width - 10 || anchor.y < 65 || anchor.y > height - 85) continue;
      const active = motion.cargo === selectedCargo;
      const label = `${String(motion.cargo).padStart(3, '0')}${active ? motion.kind === 'goods' ? ' / FINISHED' : ' / RAW' : ''}`;
      ctx.font = `${active ? 600 : 500} ${active ? 9 : 7}px ui-monospace, monospace`;
      const w = ctx.measureText(label).width + 9;
      ctx.fillStyle = active ? '#0b242eed' : '#0a1c27d9';
      ctx.fillRect(anchor.x - w / 2, anchor.y - 14, w, active ? 17 : 13);
      ctx.fillStyle = colors[motion.cargo % colors.length]; ctx.textAlign = 'center'; ctx.fillText(label, anchor.x, anchor.y - 4);
      if (active) {
        ctx.strokeStyle = colors[motion.cargo % colors.length]; ctx.lineWidth = 1;
        ctx.strokeRect(anchor.x - w / 2 - 1, anchor.y - 15, w + 2, 19);
      }
      cargoHits.push({ id: motion.cargo, x: anchor.x - Math.max(w / 2, 12), y: anchor.y - 18, w: Math.max(w, 24), h: 34 });
    }
    if (selectedCargo) {
      const motion = poses.get(`cargo:${selectedCargo}`);
      const label = `FOLLOWING ${String(selectedCargo).padStart(3, '0')}  ·  ${motion ? motion.phase.replaceAll('-', ' ').toUpperCase() : 'DELIVERED'}`;
      ctx.font = '600 9px ui-monospace, monospace';
      const w = ctx.measureText(label).width + 20;
      ctx.fillStyle = '#0c2330ee'; ctx.fillRect(16, height - 121, Math.min(w, width - 32), 23);
      ctx.fillStyle = colors[selectedCargo % colors.length]; ctx.textAlign = 'left'; ctx.fillText(label, 26, height - 106, width - 50);
    }
  }

  function resizeCanvas() {
    // The viewport owns layout; the absolutely positioned canvas only owns
    // its backing pixels. Measuring intrinsic canvas dimensions here would
    // feed the last backing size into CSS grid's next sizing pass.
    width = viewport.clientWidth;
    height = viewport.clientHeight;
    pixelRatio = Math.min(window.devicePixelRatio || 1, 2);
    const pixelsWide = Math.max(1, Math.round(width * pixelRatio));
    const pixelsHigh = Math.max(1, Math.round(height * pixelRatio));
    if (canvas.width !== pixelsWide) canvas.width = pixelsWide;
    if (canvas.height !== pixelsHigh) canvas.height = pixelsHigh;
    scale = Math.min(width / 1380, Math.max(1, height - 125) / 805);
  }

  function draw(now) {
    if (pixelRatio !== Math.min(window.devicePixelRatio || 1, 2)) resizeCanvas();
    if (width <= 0 || height <= 0) { requestAnimationFrame(frame); return; }
    ctx.setTransform(pixelRatio, 0, 0, pixelRatio, 0, 0);
    drawBackdrop(now);
    sampleScene(now);
    ctx.save(); ctx.translate(width * 0.52 + pan.x, height * 0.52 + pan.y); ctx.scale(scale * zoom, scale * zoom);
    // A soft underside light separates the station from the orbital night.
    glow(35, 145, -50, 480, '#305b72', 0.14);
    drawDeck();
    drawRoutes();
    drawPlatform();
    if (selected >= 0) {
      const n = nodes[selected];
      const diamond = [point(n.x - 60, n.y - 48, 2), point(n.x + 67, n.y - 48, 2), point(n.x + 67, n.y + 52, 2), point(n.x - 60, n.y + 52, 2)];
      poly3(diamond, '#89ebcb0d', '#8aebcc88', 1.5);
    }
    const entities = [
      { depth: -605, draw: drawTower },
      { depth: -330, draw: () => drawCrane(1) },
      { depth: -70, draw: drawWarehouse },
      { depth: -12, draw: () => drawCrane(2) },
      { depth: 200, draw: drawFactory },
      { depth: 685, draw: drawHabitat }
    ];
    const observed = [];
    const observedMap = Object.create(null);
    for (const motion of poses.values()) {
      const p = motion.position;
      let visible = true;
      if (motion.kind === 'ship') entities.push({ depth: p.x + p.y, draw: () => drawShip(motion) });
      else if (motion.kind === 'tram') entities.push({ depth: p.x + p.y + 12, draw: () => drawTram(motion) });
      else if (['raw', 'goods', 'hidden'].includes(motion.kind)) {
        const visibility = cargoVisibility(motion);
        visible = visibility.visible;
        if (visible) entities.push({ depth: visibility.depth ?? p.x + p.y + 30, draw: () => drawCargo(motion, visibility) });
      }
      const value = Object.freeze({ ...p, key: motion.key, kind: motion.kind, phase: motion.phase, cargo: motion.cargo, actor: motion.actor, revision: motion.revision, at: motion.at, duration: motion.duration, visible });
      observedMap[motion.key] = value;
      observed.push(Object.freeze({ ...value, position: p }));
    }
    entities.sort((a, b) => a.depth - b.depth).forEach(entity => entity.draw());
    drawChannelView();
    ctx.restore();
    drawLabels();
    drawCargoLabels();
    // These are the exact sampled poses consumed above by this completed
    // frame, including hidden cargo; no second clock read or resampling.
    renderedMotions = Object.freeze(observed);
    renderedPoseMap = Object.freeze(observedMap);
    requestAnimationFrame(frame);
  }
  function frame(now) {
    // A thirty-frame cadence keeps the observatory inexpensive on laptops.
    if (now - lastFrame < 1000 / 30) { requestAnimationFrame(frame); return; }
    lastFrame = now;
    draw(now);
  }
  function changeZoom(factor, origin = { x: width / 2, y: height / 2 }) {
    const previous = zoom;
    zoom = clamp(zoom * factor, 0.7, 2.2);
    pan.x = origin.x - width * 0.52 - (origin.x - width * 0.52 - pan.x) * zoom / previous;
    pan.y = origin.y - height * 0.52 - (origin.y - height * 0.52 - pan.y) * zoom / previous;
  }
  function resetView() { zoom = 1; pan = { x: 0, y: 0 }; }
  function localPointer(event) { const rect = canvas.getBoundingClientRect(); return { x: event.clientX - rect.left, y: event.clientY - rect.top }; }
  function hit(point) { return hitAreas.find(area => point.x >= area.x && point.x <= area.x + area.w && point.y >= area.y && point.y <= area.y + area.h)?.id ?? -1; }
  function hitCargo(point) { return cargoHits.find(area => point.x >= area.x && point.x <= area.x + area.w && point.y >= area.y && point.y <= area.y + area.h)?.id ?? 0; }
  $('zoomIn').addEventListener('click', () => changeZoom(1.16));
  $('zoomOut').addEventListener('click', () => changeZoom(1 / 1.16));
  $('resetView').addEventListener('click', resetView);
  $('xray').addEventListener('click', () => { xray = !xray; $('xray').setAttribute('aria-pressed', String(xray)); });
  canvas.addEventListener('wheel', event => { event.preventDefault(); changeZoom(Math.exp(-event.deltaY * 0.001), localPointer(event)); }, { passive: false });
  canvas.addEventListener('pointerdown', event => {
    if (event.button !== 0) return;
    const p = localPointer(event);
    drag = { id: event.pointerId, start: p, pan: { ...pan }, moved: false };
    canvas.setPointerCapture(event.pointerId);
  });
  canvas.addEventListener('pointermove', event => {
    const p = localPointer(event);
    if (drag && drag.id === event.pointerId) {
      const dx = p.x - drag.start.x, dy = p.y - drag.start.y;
      if (Math.hypot(dx, dy) > 4) drag.moved = true;
      if (drag.moved) pan = { x: drag.pan.x + dx, y: drag.pan.y + dy };
    } else {
      hover = hit(p);
      canvas.style.cursor = hover < 0 && !hitCargo(p) ? 'grab' : 'pointer';
    }
  });
  canvas.addEventListener('pointerup', event => {
    if (!drag || drag.id !== event.pointerId) return;
    if (!drag.moved) {
      const p = localPointer(event);
      const cargo = hitCargo(p);
      if (cargo) selectCargo(cargo);
      else { const id = hit(p); if (id >= 0) selectActor(id); }
    }
    drag = null;
    if (canvas.hasPointerCapture(event.pointerId)) canvas.releasePointerCapture(event.pointerId);
  });
  canvas.addEventListener('pointercancel', () => { drag = null; });
  canvas.addEventListener('pointerleave', () => { hover = -1; });
  canvas.addEventListener('keydown', event => {
    if (event.key === 'ArrowRight' || event.key === 'ArrowDown') selectActor((selected + 1) % nodes.length);
    else if (event.key === 'ArrowLeft' || event.key === 'ArrowUp') selectActor((selected + nodes.length - 1) % nodes.length);
    else if (event.key === '+' || event.key === '=') changeZoom(1.16);
    else if (event.key === '-') changeZoom(1 / 1.16);
    else if (event.key === '0' || event.key === 'Escape') resetView();
    else return;
    event.preventDefault();
  });
  const resizeObserver = new ResizeObserver(resizeCanvas);
  resizeObserver.observe(viewport);
  window.addEventListener('resize', resizeCanvas);
  resizeCanvas();
  updateUI();
  connectScene();
  requestAnimationFrame(frame);
})();
