// LoopNest v2 stage 2: a fake JUCE backend for ui/index.html outside the plugin.
// Injected before the page (Playwright add_init_script). It stands in for WebEditor.cpp:
// the slider / toggle relays with the processor's real ranges and defaults, the ln_*
// native functions, and ln_state (30 Hz) / ln_files / ln_peaks / ln_hatch / ln_hatched / ln_drop.
// The fixture reproduces the locked mock's rest state (DRUMS, 214 loops, rimshot_shuffle
// loaded, SYNC at 90) so screenshots compare against its reference shots. Its waveform
// and levels come from the mock's own drum-pattern generator, which the page no longer has.
//
// URL query: live=1 (start in LIVE), freeze=1 (hold the playhead), hatch=armed|stale,
// empty=1 (no folder, no sample), paused=1.
(() => {
  'use strict';
  const q = new URLSearchParams(location.search);

  // ---- parameters: [start, end, interval, default] (PluginProcessor.cpp createParameterLayout)
  const SLIDERS = {
    pitch: [-12, 12, 0.01, 0], pitchGlide: [0, 100, 1, 0], input: [-60, 6, 0.1, 0], output: [-60, 6, 0.1, 0],
    start: [0, 1, 0, 0], end: [0, 1, 0, 1], mix: [0, 100, 1, 100],
    warp: [0, 100, 0.5, 0], warpRate: [0, 100, 1, 40], drive: [0, 100, 1, 0], driveTone: [-100, 100, 1, 0],
    flutter: [0, 100, 0.5, 0], flutterRate: [0, 100, 1, 50], echo: [0, 100, 1, 0], reverb: [0, 100, 1, 0],
    echoLo: [0, 1, 0, 0], echoHi: [0, 1, 0, 1], reverbLo: [0, 1, 0, 0], reverbHi: [0, 1, 0, 1],
    eqLo: [0, 1, 0, 0], eqHi: [0, 1, 0, 1], width: [0, 100, 1, 0], widthLo: [0, 1, 0, 0], widthHi: [0, 1, 0, 1],
    reverbDecay: [0, 100, 1, 31], echoTime: [0, 100, 1, 44], widthMid: [-100, 100, 1, 0],
  };
  // choice parameters (WebComboBoxRelay): [choices, default index]
  const COMBOS = { reverbType: [['Plate', 'Hall', 'Spring'], 1] };
  const POWER = ['warpOn', 'flutterOn', 'driveOn', 'echoOn', 'reverbOn', 'widthOn'];   // panel power, default ON
  const TOGGLES = { bypass: false, gainMatch: false, warpOn: true, flutterOn: true, driveOn: true, echoOn: true, reverbOn: true, widthOn: true };
  const FUNCS = ['ln_ready', 'ln_spin', 'ln_spinPick', 'ln_back', 'ln_forward', 'ln_play', 'ln_loop', 'ln_live', 'ln_sync', 'ln_printFx', 'ln_preset',
                 'ln_presetStep', 'ln_seek', 'ln_bank', 'ln_extract', 'ln_extractCancel', 'ln_overlay',
                 'ln_pick', 'ln_openRecent', 'ln_resize', 'ln_bpm', 'ln_bpmFit', 'ln_barOrigin', 'ln_bins', 'ln_bin', 'ln_binNew', 'ln_binShow', 'ln_tempo'];
  const sv = {}, tv = {}, cv = {};
  for (const [id, r] of Object.entries(SLIDERS)) sv[id] = r[3];
  for (const [id, c] of Object.entries(COMBOS)) cv[id] = c[1];
  for (const [id, v] of Object.entries(TOGGLES)) tv[id] = v;
  const log = { invoke: [], gestures: [], sets: [], overlay: [], resize: [] };

  // ---- factory presets (Source/FactoryPresets.h), natural units, CharacterParams order
  const PIDS = ['warp', 'warpRate', 'drive', 'driveTone', 'flutter', 'flutterRate', 'echo', 'reverb', 'echoLo', 'echoHi',
    'reverbLo', 'reverbHi', 'eqLo', 'eqHi', 'width', 'widthLo', 'widthHi', 'reverbDecay', 'echoTime', 'widthMid', 'reverbType'];
  const PRESETS = [
    ['Vinyl',    [ 8.5,40,18,-10,17.5,55,  0, 8, 0.00,1.00,0.32,0.85,0.04,0.92, 15,0.10,1.00, 17,44,  0, 1]],
    ['Tape',     [  15,35,35,-20,  15,50,  0,12, 0.00,1.00,0.34,0.80,0.04,0.80, 15,0.10,1.00, 20,44,  0, 1]],
    ['Cassette', [23.5,30,22,-15,  30,60,  0, 8, 0.00,1.00,0.33,0.78,0.06,0.70, 25,0.12,1.00, 17,44,  0, 1]],
    ['Dust',     [  23,55,55,-25,12.5,50, 10,10, 0.30,0.85,0.34,0.72,0.06,0.68, 18,0.10,1.00, 23,50,  0, 1]],
    ['Radio',    [   0,50,40, 10,   5,50,  0, 0, 0.00,1.00,0.00,1.00,0.32,0.62,  0,0.00,1.00, 31,44, 15, 1]],
    ['Crush',    [   5,50,75, 20,   5,50,  0, 0, 0.00,1.00,0.00,1.00,0.06,0.95, 10,0.10,1.00, 31,44,  0, 1]],
    ['Slap',     [   0,50,15,  0, 7.5,50, 55, 0, 0.30,0.85,0.00,1.00,0.04,0.85, 30,0.15,1.00, 31,25,  0, 1]],
    ['Plate',    [   0,50,10,  0,   0,50,  0,40, 0.00,1.00,0.35,0.95,0.03,1.00, 25,0.10,1.00, 20,44,  0, 1]],
    ['Hall',     [   0,50,10,  0,   0,50,  0,65, 0.00,1.00,0.36,0.92,0.03,1.00, 40,0.10,1.00, 69,44,  0, 1]],
    ['Width',    [   0,50, 0,  0,   0,50,  0, 0, 0.00,1.00,0.00,1.00,0.00,1.00, 70,0.15,1.00, 31,44,-20, 1]],
    ['Lo-Fi',    [  16,45,40,-15,  20,55, 15,18, 0.30,0.80,0.34,0.78,0.06,0.72, 20,0.10,1.00, 23,40,  0, 1]],
  ];


  // ---- the mock's loop generator (fixture only)
  function rng(seed) { let s = seed >>> 0 || 1; return () => { s ^= s << 13; s >>>= 0; s ^= s >> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; }; }
  function nameSeed(str) { let h = 2166136261; for (let i = 0; i < str.length; i++) { h ^= str.charCodeAt(i); h = Math.imul(h, 16777619); } return (h >>> 0) % 100000 + 1; }
  const WORDS_A = ['dusty','crate','boom','loose','dirty','warm','basement','smoked','lazy','heavy','cracked','soul'];
  const WORDS_B = ['break','groove','kit','swing','shuffle','knock','bounce','pocket'];
  function pattern(seed, fromRow) {
    const r = rng(seed * 7919 + 13);
    const fromName = fromRow ? true : r() < 0.7;   // the mock's name draws, so the pattern matches its waveform
    r(); r(); r(); if (!fromName) r();
    const kicks = [0, 2]; if (r() < 0.6) kicks.push(2.5); if (r() < 0.4) kicks.push(0.75); if (r() < 0.3) kicks.push(3.5);
    const snares = [1, 3]; const ghosts = []; for (const g of [1.75, 3.25, 3.75, 0.5]) if (r() < 0.35) ghosts.push(g);
    const hatDiv = r() < 0.5 ? 0.5 : 0.25, swing = 0.02 + r() * 0.08, hatAmp = 0.10 + r() * 0.10;
    const fillBar = r() < 0.5 ? 3 : 7;
    return { kicks, snares, ghosts, hatDiv, swing, hatAmp, fillBar, body: 0.025 + r() * 0.03 };
  }
  function hash(n) { const x = Math.sin(n * 127.1 + 311.7) * 43758.5453; return x - Math.floor(x); }
  function ampAt(pat, beat) {
    const bar = Math.floor(beat / 4), b = beat - bar * 4;
    let e = pat.body * (0.5 + 0.5 * hash(beat * 91.3));
    const hit = (pos, amp, dec) => { let d = b - pos; if (d < 0) d += 4; if (d < 1.5) e += amp * Math.exp(-d * dec); };
    for (const k of pat.kicks) hit(k, 0.95, 7);
    for (const s of pat.snares) hit(s, 0.78 * (0.75 + 0.25 * hash(bar * 3 + s)), 9);
    for (const g of pat.ghosts) hit(g, 0.3, 14);
    for (let h = 0; h < 4; h += pat.hatDiv) {
      const sw = (Math.round(h / pat.hatDiv) % 2) ? pat.swing : 0;
      hit(h + sw, pat.hatAmp * (0.7 + 0.3 * hash(bar * 17 + h * 5)), 28);
    }
    if (bar % 8 === pat.fillBar && b > 3) hit(3 + Math.floor((b - 3) * 4) / 4, 0.6, 16);
    const noise = 0.75 + 0.25 * hash(beat * 613.7);
    return Math.min(1, e * noise);
  }
  const ROWS = [
    'basement_swing_89bpm.wav', 'dusty_break_92bpm.wav', 'rimshot_shuffle_88bpm.wav', 'tape_kick_loop_90bpm.wav',
    'lofi_knock_85bpm.wav', 'brushes_slow_76bpm.wav', 'boom_bap_03_94bpm.wav', 'vinyl_hats_90bpm.wav', 'snare_drag_87bpm.wav',
    'cold_room_break_93bpm.wav', 'ghost_notes_86bpm.wav', 'crate_dig_07_91bpm.wav', 'half_time_knock_70bpm.wav',
    'shaker_pocket_95bpm.wav', 'rim_and_clap_88bpm.wav', 'wobble_kit_84bpm.wav', 'late_snare_90bpm.wav',
    'soft_break_02_82bpm.wav', 'tambourine_ride_96bpm.wav', 'sp_chop_kit_89bpm.wav', 'muted_toms_78bpm.wav',
    'cardboard_kick_92bpm.wav', 'lazy_swing_80bpm.wav', 'dusty_break_2_92bpm.wav'];
  { const r = rng(7); while (ROWS.length < 214) ROWS.push(`${WORDS_A[Math.floor(r() * 12)]}_${WORDS_B[Math.floor(r() * 8)]}_${String(ROWS.length).padStart(3, '0')}_${86 + Math.floor(r() * 11)}bpm.wav`); }
  if (q.get('long') === '1') ROWS[2] = 'Kaelin Elliis Bundle Vol. 8 -9.5__KAELIN ELLIS_BREAKS_COMEBACK_88bpm_extended_take.wav';
  const bpmOf = nm => { const m = /(\d+) ?bpm/i.exec(nm); return m ? +m[1] : 0; };
  const FX_PAT = pattern(4242, false);

  const empty = q.get('empty') === '1';
  const S = {
    hasSample: !empty, sample: empty ? '' : ROWS[2], folder: empty ? '' : 'DRUMS', folderMissing: false,
    count: empty ? 0 : ROWS.length, index: empty ? -1 : 2, canSpin: !empty, histBack: !empty, histFwd: false, loading: false,
    playing: !empty && q.get('paused') !== '1', loop: true, live: q.get('live') === '1', sync: true, printFx: false,
    playhead: 0, nativeBpm: empty ? 0 : 88, bpmSource: empty ? 'none' : 'name', tagBpm: empty ? 0 : 88, freeBpm: 0, barOrigin: 0, bin: '', binMissing: false, hostBpm: 90,
    hatch: empty ? 'none' : (q.get('hatch') || 'idle'), printName: '', preset: -1, presetName: 'INIT', presetDirty: false,
    extracting: false, extractProgress: 0, extractKept: 0, extractStatus: '', inL: 0, inR: 0, outL: 0, outR: 0,
  };
  let pat = pattern(nameSeed(ROWS[2]), true), seconds = 64 * 60 / 88, liveBeat = 0;
  if (S.hatch === 'armed' || S.hatch === 'stale') S.printName = 'rimshot_shuffle_88bpm_+0st_2026-09-30_0112.wav';
  let ready = false, timer = 0, lastTick = 0;

  function emit(id, obj) {
    setTimeout(() => window.__JUCE__.backend.emitByBackend(id, JSON.stringify(obj)), 0);
  }
  const RECENTS = empty ? [] : [{ name: 'DRUMS', path: '/Users/fixture/Samples/DRUMS' },
                                 { name: 'BREAKS', path: '/Users/fixture/Samples/Crates/BREAKS' },
                                 { name: 'Vinyl Chops', path: '/Volumes/Archive/Vinyl Chops' }];
  function filesMsg() { return { folder: S.folder, path: empty ? '' : RECENTS[0].path, names: empty ? [] : ROWS.slice(), recents: RECENTS.slice() }; }
  function peaksMsg() {
    if (!S.hasSample) return { path: '', name: '', seconds: 0, peaks: [] };
    const peaks = [];
    for (let i = 0; i < 1600; i++) {
      let m = 0;
      for (let s = 0; s < 5; s++) m = Math.max(m, ampAt(pat, (i + s / 5) / 1600 * 64));
      peaks.push(Math.round(m * 1000) / 1000);
    }
    // hi: 32768 sqrt-companded bytes, base64 (WebEditor kHiColumns)
    const HI = 32768; let bin = '';
    for (let i = 0; i < HI; i++) bin += String.fromCharCode(Math.round(Math.sqrt(ampAt(pat, i / HI * 64)) * 255));
    return { path: '/fixture/DRUMS/' + S.sample, name: S.sample, seconds, peaks, hi: btoa(bin), hits: hitsOf(pat) };
  }
  // The pattern's own onsets over its 16 bars, 0..1 (UiBridge::computeHits finds these in real audio).
  function hitsOf(pat) {
    const set = new Set();
    for (let bar = 0; bar < 16; bar++) {
      for (const b of [...pat.kicks, ...pat.snares, ...pat.ghosts]) set.add(bar * 4 + b);
      for (let h = 0; h < 4; h += pat.hatDiv) set.add(bar * 4 + h + ((Math.round(h / pat.hatDiv) % 2) ? pat.swing : 0));
    }
    return [...set].sort((a, b) => a - b).map(b => b / 64);
  }
  // LoopNestProcessor's inferNativeBpm on the trimmed region: 1/2/4/8 bars, 60..200, nearest the host.
  function fitRegion() {
    const sec = seconds * (sv.end - sv.start); let best = 0;
    for (const n of [1, 2, 4, 8]) { const b = 240 * n / sec; if (b >= 60 && b <= 200 && (!best || Math.abs(Math.log(b / S.hostBpm)) < Math.abs(Math.log(best / S.hostBpm)))) best = b; }
    if (best) { S.nativeBpm = best; S.bpmSource = 'length'; staleIfArmed(); }
    return !!best;
  }
  const follows = () => S.bpmSource === 'none';   // a trim never moves a tempo there already is
  function setSlider(id, v) { sv[id] = v; emit('__juce__slider' + id, { eventType: 'valueChanged', value: v }); }
  // a combo box's relay value is normalised: index / (choices - 1)
  function setCombo(id, i) { cv[id] = i; emit('__juce__comboBox' + id, { eventType: 'valueChanged', value: i / (COMBOS[id][0].length - 1) }); }
  function loadRow(i) {
    S.freeBpm = 0; S.sync = true;   // every new loop follows the host
    S.index = i; S.sample = ROWS[i]; S.barOrigin = 0; S.nativeBpm = S.tagBpm = bpmOf(ROWS[i]); S.bpmSource = S.nativeBpm ? 'name' : 'length';
    pat = pattern(nameSeed(ROWS[i]), i < 24); seconds = 64 * 60 / (S.nativeBpm || 90);
    S.playhead = 0; S.hatch = 'idle'; S.printName = '';
    setSlider('start', 0); setSlider('end', 1);
    emit('ln_peaks', peaksMsg());
  }
  // Spin history (LoopNestProcessor::historyBack): only SPIN records, the fixture one spin in (row 0,
  // then row 2); a spin after stepping back drops the forward ones; 21 entries at most.
  const H = { list: empty ? [] : [0, 2], pos: empty ? -1 : 1 };
  function syncHist() { S.histBack = H.pos > 0; S.histFwd = H.pos >= 0 && H.pos < H.list.length - 1; }
  function noteLeaving(i) { if (H.pos >= 0) H.list.length = H.pos + 1; H.list.push(i); if (H.list.length > 21) H.list.shift(); H.pos = H.list.length - 1; syncHist(); }
  function stepHist(d) { const to = H.pos + d; if (H.pos < 0 || to < 0 || to >= H.list.length) return false; H.pos = to; S.live = false; loadRow(H.list[to]); syncHist(); return true; }
  const pv = id => id in COMBOS ? cv[id] : sv[id];
  function presetMatches(i) { return i >= 0 && PIDS.every((id, k) => Math.abs(pv(id) - PRESETS[i][1][k]) < 1e-3) && POWER.every(id => tv[id]); }
  function selectPreset(i) {
    S.preset = i; S.presetName = i < 0 ? 'INIT' : PRESETS[i][0];
    PIDS.forEach((id, k) => id in COMBOS ? setCombo(id, i < 0 ? COMBOS[id][1] : PRESETS[i][1][k])
                                         : setSlider(id, i < 0 ? SLIDERS[id][3] : PRESETS[i][1][k]));
    for (const id of POWER) { tv[id] = true; emit('__juce__toggle' + id, { eventType: 'valueChanged', value: true }); }   // presets turn every panel on
  }
  const BINS = ['grimey', 'half-time'];   // the Prints root's subfolders
  function staleIfArmed() { if (S.hatch === 'armed') S.hatch = 'stale'; }
  const NATIVE = {
    ln_ready() { ready = true; emit('ln_files', filesMsg()); emit('ln_peaks', peaksMsg()); emit('ln_state', { ...S }); start(); return true; },
    ln_spin() { if (!S.canSpin) return; S.live = false; let i; do { i = Math.floor(Math.random() * ROWS.length); } while (i === S.index); noteLeaving(i); loadRow(i); S.playing = true; },
    ln_spinPick(i) { i = i | 0; if (i < 0 || i >= ROWS.length) return false; S.live = false; noteLeaving(i); setTimeout(() => { loadRow(i); S.playing = true; }, 150); return true; },
    ln_back() { return stepHist(-1); },
    ln_forward() { return stepHist(+1); },
    ln_play() { if (S.live) { S.live = false; S.playing = true; } else S.playing = !S.playing; },
    ln_loop(b) { S.loop = !!b; },
    ln_live(b) { S.live = !!b; },
    ln_sync(b) { S.sync = !!b; S.freeBpm = 0; staleIfArmed(); },
    ln_tempo(b) { b = +b || 0; if (b > 0) { S.freeBpm = Math.max(30, Math.min(300, b)); S.sync = false; } else S.freeBpm = 0; staleIfArmed(); },
    ln_printFx(b) { S.printFx = !!b; staleIfArmed(); },
    ln_preset(i) { selectPreset(Math.max(-1, Math.min(PRESETS.length - 1, i | 0))); staleIfArmed(); },
    ln_presetStep(d) { const n = PRESETS.length + 1; selectPreset(((S.preset + 1 + (d | 0)) % n + n) % n - 1); staleIfArmed(); },
    ln_bpm(b) { b = +b || 0; if (b > 0) { S.nativeBpm = Math.max(30, Math.min(300, b)); S.bpmSource = 'user'; } else if (S.tagBpm) { S.nativeBpm = S.tagBpm; S.bpmSource = 'name'; } else if (!fitRegion()) S.bpmSource = S.nativeBpm ? 'length' : 'none'; staleIfArmed(); },
    ln_bpmFit() { const ok = fitRegion(); if (ok) S.barOrigin = sv.start; return ok; },
    ln_bins() { return BINS.slice().sort(); },
    ln_bin(n) { n = String(n || ''); if (n && !BINS.includes(n)) return false; S.bin = n; S.binMissing = false; staleIfArmed(); return true; },
    ln_binNew(n) { n = String(n || '').trim(); if (!n) return false; if (!BINS.includes(n)) BINS.push(n); S.bin = n; S.binMissing = false; staleIfArmed(); return true; },
    ln_binShow() { return true; },
    ln_barOrigin(n) { S.barOrigin = Math.max(0, Math.min(1, +n || 0)); },
    ln_seek(n) { S.playhead = Math.max(0, Math.min(1, +n || 0)); },
    ln_bank() {},
    ln_extract() {
      if (S.extracting) return;
      S.extracting = true; S.extractProgress = 0; S.extractKept = 0; S.extractStatus = 'scanning';
      const t0 = performance.now();
      const iv = setInterval(() => {
        const u = Math.min(1, (performance.now() - t0) / 4000);
        S.extractProgress = u; S.extractKept = Math.floor(u * 37);
        if (u >= 1) { clearInterval(iv); S.extracting = false; S.extractStatus = 'kept 37 drum loops'; }
      }, 100);
    },
    ln_extractCancel() { S.extracting = false; S.extractStatus = 'cancelled'; },
    ln_overlay(b) { log.overlay.push(!!b); },
    ln_pick(i) { i = i | 0; if (i < 0 || i >= ROWS.length) return; S.live = false; setTimeout(() => { loadRow(i); S.playing = true; }, 150); },   // no history entry: only SPIN records
    ln_openRecent() {},
    ln_resize(x) { log.resize.push(+x); },
  };
  function start() {
    if (timer) return;
    lastTick = performance.now();
    timer = setInterval(tick, 1000 / 30);
  }
  function tick() {
    const now = performance.now(), dt = (now - lastTick) / 1000; lastTick = now;
    const freeze = q.get('freeze') === '1';
    let inL = 0, outL = 0;
    if (!S.live && S.playing && S.hasSample) {
      const a = S.playhead;
      if (!freeze) {
        const st = sv.start, en = Math.max(sv.start + 0.001, sv.end);
        S.playhead += dt / seconds;
        if (S.playhead >= en) { if (S.loop) S.playhead = st + (S.playhead - en); else { S.playhead = en; S.playing = false; } }
      }
      for (let s = 0; s <= 6; s++) outL = Math.max(outL, ampAt(pat, (a + (S.playhead - a) * s / 6) * 64));
      outL *= Math.pow(10, sv.output / 20) * Math.pow(10, sv.input / 20);
    }
    if (S.live) {
      const b0 = liveBeat; liveBeat = (liveBeat + dt * S.hostBpm / 60) % 64;
      for (let s = 0; s <= 6; s++) inL = Math.max(inL, ampAt(FX_PAT, (b0 + dt * S.hostBpm / 60 * s / 6) % 64) * 0.72);
      outL = tv.bypass ? inL : inL * Math.pow(10, sv.output / 20);
    }
    S.inL = S.inR = +inL.toFixed(4); S.outL = S.outR = +outL.toFixed(4);
    if (S.preset >= 0 && !presetMatches(S.preset)) S.presetDirty = true; else S.presetDirty = false;
    emit('ln_state', { ...S });
  }

  function handle(msg) {
    const { eventId, payload } = msg;
    if (eventId === '__juce__invoke') {
      log.invoke.push([payload.name, ...(payload.params || [])]);
      const f = NATIVE[payload.name];
      const r = f ? f(...(payload.params || [])) : undefined;
      emit('__juce__complete', { promiseId: payload.resultId, result: r === undefined ? null : r });
      return;
    }
    if (eventId.startsWith('__juce__slider')) {
      const id = eventId.slice(14), r = SLIDERS[id];
      if (!r) return;
      if (payload.eventType === 'requestInitialUpdate') {
        emit(eventId, { eventType: 'propertiesChanged', start: r[0], end: r[1], skew: 1, name: id, label: '',
                        numSteps: r[2] > 0 ? Math.round((r[1] - r[0]) / r[2]) + 1 : 0x7fffffff, interval: r[2],
                        parameterIndex: Object.keys(SLIDERS).indexOf(id) });
        emit(eventId, { eventType: 'valueChanged', value: sv[id] });
      } else if (payload.eventType === 'valueChanged') {
        sv[id] = payload.value; log.sets.push([id, payload.value]);
        if (PIDS.includes(id) || id === 'pitch' || id === 'start' || id === 'end' || id === 'mix') staleIfArmed();
      } else {
        log.gestures.push([id, payload.eventType]);
        // a trim handle let go: a following tempo re-reads the region (the processor's gesture-end refit)
        if (payload.eventType === 'sliderDragEnded' && (id === 'start' || id === 'end') && follows()) fitRegion();
      }
      return;
    }
    if (eventId.startsWith('__juce__comboBox')) {
      const id = eventId.slice(16);
      if (!(id in COMBOS)) return;
      if (payload.eventType === 'requestInitialUpdate') {
        emit(eventId, { eventType: 'propertiesChanged', name: id, parameterIndex: 200, choices: COMBOS[id][0] });
        setCombo(id, cv[id]);
      } else if (payload.eventType === 'valueChanged') {
        cv[id] = Math.round(payload.value * (COMBOS[id][0].length - 1)); log.sets.push([id, cv[id]]); staleIfArmed();
      }
      return;
    }
    if (eventId.startsWith('__juce__toggle')) {
      const id = eventId.slice(14);
      if (!(id in TOGGLES)) return;
      if (payload.eventType === 'requestInitialUpdate') {
        emit(eventId, { eventType: 'propertiesChanged', name: id, parameterIndex: 100 });
        emit(eventId, { eventType: 'valueChanged', value: tv[id] });
      } else if (payload.eventType === 'valueChanged') { tv[id] = !!payload.value; log.sets.push([id, tv[id]]); if (POWER.includes(id)) staleIfArmed(); }
    }
  }

  window.__JUCE__ = {
    initialisationData: {
      __juce__platform: ['macos'], __juce__functions: FUNCS, __juce__registeredGlobalEventIds: [],
      __juce__sliders: Object.keys(SLIDERS), __juce__toggles: Object.keys(TOGGLES), __juce__comboBoxes: Object.keys(COMBOS),
    },
    postMessage: str => handle(JSON.parse(str)),
  };
  // The native overlays' side, for tests: hover / press / click the HATCH tile, a drag over the rack, host automation.
  window.__shim = {
    S, sv, tv, cv, log, ROWS,
    hatchHover: b => emit('ln_hatch', { hover: !!b }),
    hatchPress: b => emit('ln_hatch', { press: !!b }),
    rackDrop: b => emit('ln_drop', { hover: !!b }),   // the native rack drop target: an accepted drag over the rack
    hatchClick() {
      if (S.hatch !== 'idle' && S.hatch !== 'stale') return false;
      S.playing = false; S.hatch = 'armed';
      S.printName = (S.bin && !S.binMissing ? S.bin + '_' : '') + S.sample.replace(/\.wav$/i, '') + '_+0st_2026-09-30_0112.wav';
      emit('ln_hatched', { ok: true, name: S.printName });
      return true;
    },
    // host automation / undo: no gesture. A trim the host moved settles a moment later (the processor's timer).
    automate: (id, v) => { if (id in COMBOS) { setCombo(id, v); return; }
                           setSlider(id, v); if ((id === 'start' || id === 'end') && follows()) setTimeout(fitRegion, 250); },
  };
})();
