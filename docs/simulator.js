/**
 * Rozhodovač — Interaktivní simulátor Waveshare ESP32-S3 AMOLED 2.16
 * Zjednodušená simulace chování firmwaru: SVG pixel-art Clawd, chat (nejnovější
 * zpráva nahoře jako na displeji), stolní hodiny HH:MM:SS po 20 s nečinnosti na USB
 * a zvukové efekty ES8311 přes Web Audio API.
 */

(function () {
  // ─── Web Audio API Beeper ───────────────────────────────────────────────
  let audioCtx = null;
  let soundEnabled = true;

  function initAudio() {
    if (!audioCtx) {
      const AudioContext = window.AudioContext || window.webkitAudioContext;
      if (AudioContext) audioCtx = new AudioContext();
    }
    if (audioCtx && audioCtx.state === 'suspended') {
      audioCtx.resume();
    }
  }

  function playTone(freq, durationMs) {
    if (!soundEnabled) return;
    try {
      initAudio();
      if (!audioCtx) return;
      const osc = audioCtx.createOscillator();
      const gain = audioCtx.createGain();
      osc.type = 'sine';
      osc.frequency.setValueAtTime(freq, audioCtx.currentTime);

      const now = audioCtx.currentTime;
      const dur = durationMs / 1000;
      gain.gain.setValueAtTime(0.01, now);
      gain.gain.exponentialRampToValueAtTime(0.2, now + 0.01);
      gain.gain.exponentialRampToValueAtTime(0.001, now + dur);

      osc.connect(gain);
      gain.connect(audioCtx.destination);
      osc.start(now);
      osc.stop(now + dur);
    } catch (e) {
      console.warn("Audio play failed:", e);
    }
  }

  // Melodie stejné jako ve firmwaru (src/main.cpp, sekce Zvuky): [Hz, ms], 0 Hz = pauza
  const MELODY = {
    request:   [[1400, 55], [0, 15], [1900, 65]],
    question:  [[1320, 60], [0, 20], [1760, 80]],
    danger:    [[2400, 70], [0, 40], [2400, 70], [0, 40], [2400, 90]],
    approve:   [[1800, 45], [2400, 65]],
    deny:      [[600, 70], [440, 110]],
    answer:    [[1600, 40], [2000, 55]],
    done:      [[1047, 90], [1319, 90], [1568, 140]],
    timeout:   [[1200, 80], [900, 80], [700, 120]],
    connect:   [[1200, 50], [1800, 70]],
    batLow:    [[880, 120], [0, 80], [880, 120]],
    chargeOn:  [[1500, 40], [2200, 60]],
    pet:       [[2300, 30], [2600, 40]],
  };

  function playMelody(notes) {
    let t = 0;
    notes.forEach(([f, ms]) => {
      if (f) setTimeout(() => playTone(f, ms), t);
      t += ms;
    });
  }

  function beepApprove() { playMelody(MELODY.approve); }
  function beepAnswer()  { playMelody(MELODY.answer); }
  function beepDeny()    { playMelody(MELODY.deny); }
  function beepTap()     { playTone(2100, 20); }

  // ─── State Management ───────────────────────────────────────────────────
  const state = {
    view: 'normal',       // 'normal' | 'fullchat' | 'clock' | 'decision' | 'profile' | 'info' | 'menu'
    activeNav: 'action',  // 'action' | 'profile' | 'info' | 'menu'
    promptActive: false,
    promptType: 'question', // 'question' | 'danger'
    promptHeader: 'DOTAZ / ROZHODNUTÍ',
    promptTool: 'Dotaz na postup',
    promptHint: 'Chcete nasadit novou verzi do produkce?',
    optA: '1. Nasadit',
    optB: '2. Počkat',
    promptOptions: ['1. Nasadit', '2. Počkat'],
    promptSel: 0,
    promptSelTapped: false, // u 3+ voleb první klepnutí vybere, druhé odešle
    promptShownMs: 0,       // ochranná doba po objevení dotazu
    promptTtlS: 0,          // doba na odpověď podle mostu AGY (0 = Claude, neznámá)
    demoQuestionIdx: 0,
    menuSel: 0,             // vybraná položka menu (BOOT posouvá, IO18 vybírá)
    demo: false,
    screenOff: false,
    
    tokensToday: 42800,
    tokensTotal: 184500,
    level: 3,
    approvals: 14,          // schválené akce
    denials: 1,             // zamítnuté akce
    answers: 6,             // odpovědi na otázky s volbami (počítají se zvlášť)
    channel: 'BLE',         // Claude Desktop se připojuje jen přes Bluetooth
    client: 'claude',       // 'claude' | 'agy' — maskot Clawd nebo Agy podle připojeného klienta
    chatScroll: 0,          // posun historie (IO18), 0 = nejnovější nahoře
    promptArrivedMs: 0,
    lastInteractMs: Date.now(),
    
    chatMessages: [
      { text: "19:08 AGY: rozhodovac (relace aktivní)", newest: false },
      { text: "19:09 Spustit příkaz: git status", newest: false },
      { text: "19:10 Zápis souboru: docs/index.html", newest: false },
      { text: "19:11 Upravit soubor: docs/style.css", newest: false },
      { text: "19:12 Rozhodovač: připraven k akci", newest: true }
    ],

    // Mascot animation
    clawdEmotion: 'idle', // 'idle' | 'busy' | 'heart' | 'celebrate'
    tickCount: 0
  };

  // ─── DOM References ─────────────────────────────────────────────────────
  const el = {
    screenHeaderTime: document.getElementById('screen-header-time'),
    screenChannel: document.getElementById('screen-channel'),
    clockTime: document.getElementById('clock-time'),
    clockDate: document.getElementById('clock-date'),
    
    views: {
      normal: document.getElementById('sim-view-normal'),
      fullchat: document.getElementById('sim-view-fullchat'),
      clock: document.getElementById('sim-view-clock'),
      decision: document.getElementById('sim-view-decision'),
      profile: document.getElementById('sim-view-profile'),
      info: document.getElementById('sim-view-info'),
      menu: document.getElementById('sim-view-menu')
    },
    
    navBtns: {
      action: document.getElementById('nav-btn-action'),
      profile: document.getElementById('nav-btn-profile'),
      info: document.getElementById('nav-btn-info'),
      menu: document.getElementById('nav-btn-menu')
    },

    compactChat: document.getElementById('compact-chat-box'),
    compactChatLines: document.getElementById('compact-chat-lines'),
    fullChatClose: document.getElementById('full-chat-close'),
    fullChatList: document.getElementById('full-chat-list'),

    decisionHeader: document.getElementById('sim-decision-header'),
    decisionWait: document.getElementById('sim-decision-wait'),
    decisionTool: document.getElementById('sim-decision-tool'),
    decisionHint: document.getElementById('sim-decision-hint'),
    decisionBtnRow: document.getElementById('sim-decision-btn-row'),
    btnDecisionA: document.getElementById('sim-btn-decision-a'),
    btnDecisionB: document.getElementById('sim-btn-decision-b'),
    btnDecisionALbl: document.getElementById('sim-btn-a-label'),
    btnDecisionBLbl: document.getElementById('sim-btn-b-label'),

    clawdSvg: document.getElementById('sim-clawd-svg'),
    agySvg: document.getElementById('sim-agy-svg'),
    profTitle: document.getElementById('prof-title'),
    heartsBubble: document.getElementById('sim-hearts-bubble'),
    
    profLvl: document.getElementById('prof-lvl'),
    profAppr: document.getElementById('prof-appr'),
    profDeny: document.getElementById('prof-deny'),
    profAnsw: document.getElementById('prof-answ'),
    profTokensToday: document.getElementById('prof-tokens-today'),
    profTokensTotal: document.getElementById('prof-tokens-total')
  };

  // ─── Screen View Switching ──────────────────────────────────────────────
  function switchView(viewName) {
    state.view = viewName;
    Object.keys(el.views).forEach(k => {
      if (el.views[k]) {
        el.views[k].classList.toggle('active', k === viewName);
      }
    });

    // Update bottom navigation active pill
    if (viewName === 'normal' || viewName === 'fullchat') {
      state.activeNav = 'action';
    } else if (viewName === 'profile') {
      state.activeNav = 'profile';
    } else if (viewName === 'info') {
      state.activeNav = 'info';
    } else if (viewName === 'menu') {
      state.activeNav = 'menu';
    }

    Object.keys(el.navBtns).forEach(k => {
      if (el.navBtns[k]) {
        el.navBtns[k].classList.toggle('active', k === state.activeNav);
      }
    });
  }

  // ─── Real-time Desk Clock (HH:MM:SS) ────────────────────────────────────
  const DOW_CZ = ["Neděle", "Pondělí", "Úterý", "Středa", "Čtvrtek", "Pátek", "Sobota"];

  function updateClock() {
    const now = new Date();
    const h = String(now.getHours()).padStart(2, '0');
    const m = String(now.getMinutes()).padStart(2, '0');
    const s = String(now.getSeconds()).padStart(2, '0');
    
    // Header mini clock
    if (el.screenHeaderTime) {
      el.screenHeaderTime.textContent = `${h}:${m}`;
    }

    // Big desk clock HH:MM:SS
    if (el.clockTime) {
      el.clockTime.textContent = `${h}:${m}:${s}`;
    }
    if (el.clockDate) {
      const dow = DOW_CZ[now.getDay()];
      const d = now.getDate();
      const mo = now.getMonth() + 1;
      const y = now.getFullYear();
      el.clockDate.textContent = `${dow} ${d}.${mo}.${y}`;
    }
  }

  // ─── Chat Rendering ─────────────────────────────────────────────────────
  // Firmware ukazuje nejnovější zprávu nahoře, IO18 / tažení posouvá ke starším.
  const COMPACT_ROWS = 3;

  function escapeHtml(text) {
    const d = document.createElement('div');
    d.textContent = text;
    return d.innerHTML;
  }

  function chatMaxScroll() {
    return Math.max(0, state.chatMessages.length - COMPACT_ROWS);
  }

  function renderChat() {
    const newestFirst = state.chatMessages.slice().reverse();
    if (el.compactChatLines) {
      const rows = newestFirst.slice(state.chatScroll, state.chatScroll + COMPACT_ROWS);
      el.compactChatLines.innerHTML = rows.map(msg => `
        <div class="${msg.newest ? 'line-newest' : ''}">${escapeHtml(msg.text)}</div>
      `).join('');
    }

    if (el.fullChatList) {
      el.fullChatList.innerHTML = newestFirst.map(msg => `
        <div style="margin-bottom: 6px; ${msg.newest ? 'color: #fff;' : ''}">${escapeHtml(msg.text)}</div>
      `).join('');
    }
  }

  function addChatMessage(text) {
    state.chatMessages.forEach(m => m.newest = false);
    state.chatMessages.push({ text, newest: true });
    if (state.chatMessages.length > 16) state.chatMessages.shift();
    state.chatScroll = 0;
    renderChat();
  }

  function scrollChat() {
    if (state.view === 'fullchat' && el.fullChatList) {
      const list = el.fullChatList;
      const atEnd = list.scrollTop + list.clientHeight >= list.scrollHeight - 2;
      list.scrollTop = atEnd ? 0 : list.scrollTop + 40;
      return;
    }
    state.chatScroll = state.chatScroll >= chatMaxScroll() ? 0 : state.chatScroll + 1;
    renderChat();
  }

  // ─── Idle → desk clock, decision wait timer ─────────────────────────────
  function markInteraction() {
    state.lastInteractMs = Date.now();
  }

  // Zvuk dotazu podle druhu jako ve firmwaru: otázka, riziková akce (AGY i Claude),
  // běžná žádost o schválení
  function playPromptSound() {
    if (state.promptType === 'question') playMelody(MELODY.question);
    else if (state.promptType === 'danger') playMelody(MELODY.danger);
    else playMelody(MELODY.request);
  }

  // Jako firmware: po objevení dotazu se dotyk a tlačítka 700 ms ignorují,
  // kritická akce se schvaluje podržením 1 s
  const PROMPT_GUARD_MS = 700;
  const DANGER_HOLD_MS = 1000;
  function promptGuardActive() { return Date.now() - state.promptShownMs < PROMPT_GUARD_MS; }
  // AGY rizikovou akci označí, u Claude ji firmware pozná podle příkazu (ukázka je git push --force)
  function promptIsDanger() { return state.promptType === 'danger'; }

  // Připomínky: u AGY v polovině a ve 4/5 doby na odpověď, u Claude po 30, 60 a 90 s
  function reminderAtS(n) {
    if (state.promptTtlS > 0) {
      if (n === 1) return state.promptTtlS / 2;
      if (n === 2) return state.promptTtlS * 4 / 5;
      return Infinity;
    }
    return n <= 3 ? 30 * n : Infinity;
  }

  function tickIdleAndPrompt() {
    if (state.promptActive && el.decisionWait) {
      const waited = Math.floor((Date.now() - state.promptArrivedMs) / 1000);
      if (state.promptTtlS > 0) {
        const remaining = Math.max(0, state.promptTtlS - waited);
        el.decisionWait.textContent = `zbývá ${remaining}s`;
        el.decisionWait.style.color = remaining <= 15 ? 'var(--color-red)' : '';
        if (remaining === 0) {
          // Dotaz vypršel: AGY se zeptá v terminálu
          state.promptActive = false;
          playMelody(MELODY.timeout);
          switchView('normal');
          return;
        }
      } else {
        el.decisionWait.textContent = `${waited}s`;
        el.decisionWait.style.color = waited >= 10 ? 'var(--color-red)' : '';
      }
      if (state.promptReminders < 3 && waited >= reminderAtS(state.promptReminders + 1)) {
        state.promptReminders++;
        playPromptSound();
      }
    }
    // Firmware přejde do hodin při napájení z USB, bez dotazu a po 20 s bez dotyku
    if (state.view === 'normal' && !state.promptActive && !state.screenOff &&
        Date.now() - state.lastInteractMs > 20000) {
      switchView('clock');
    }
  }

  // ─── Profile Stats Rendering ────────────────────────────────────────────
  function renderProfile() {
    if (el.profLvl) el.profLvl.textContent = `Lv ${state.level}`;
    if (el.profAppr) el.profAppr.textContent = `OK ${state.approvals}`;
    if (el.profDeny) el.profDeny.textContent = `NE ${state.denials}`;
    if (el.profAnsw) el.profAnsw.textContent = `Odp ${state.answers}`;
    if (el.profTokensToday) el.profTokensToday.textContent = `Dnes: ${(state.tokensToday / 1000).toFixed(1)}k`;
    if (el.profTokensTotal) el.profTokensTotal.textContent = `Celkem: ${(state.tokensTotal / 1000).toFixed(1)}k`;
  }

  // ─── Decision Prompts (Question vs Danger Action) ────────────────────────
  const DEMO_QUESTIONS = [
    {
      hint: 'Kterou architekturu zvolit pro nový modul?',
      options: ['1. Modulární', '2. Monolit', '3. Serverless']
    },
    {
      hint: 'Jak naložit s nalezenými zranitelnostmi v kódu?',
      options: ['1. Opravit ihned', '2. Vytvořit issue', '3. Ignorovat', '4. Zastavit build']
    },
    {
      hint: 'Chcete nasadit novou verzi do produkce?',
      options: ['1. Nasadit', '2. Počkat']
    },
    {
      hint: 'Který modul otestovat jako první?',
      options: ['1. Mzdy', '2. Fakturace', '3. Doprava', '4. Sklad', '5. Personalistika', '6. Reporting']
    }
  ];

  function renderDecisionButtons() {
    if (!el.decisionBtnRow) return;
    const opts = state.promptOptions || [];
    const count = opts.length;
    // Od 5 voleb firmware schová maskota a zhustí řádky, simulátor zhustí karty
    if (el.views.decision) el.views.decision.classList.toggle('dense', count >= 5);

    if (count <= 2) {
      el.decisionBtnRow.className = 'decision-btn-row';
      const optA = opts[0] || 'SCHVÁLIT';
      const optB = opts[1] || 'ZAMÍTNOUT';
      const isQuestion = state.promptType === 'question';
      const danger = promptIsDanger();
      el.decisionBtnRow.innerHTML = `
        <button class="sim-decide-btn sim-btn-yes" id="sim-btn-decision-a">
          <span id="sim-btn-a-label">${escapeHtml(optA)}</span>
          <span class="sim-decide-sub">${danger ? 'podržet 1 s' : '[dotyk / BOOT]'}</span>
        </button>
        <button class="sim-decide-btn sim-btn-no" id="sim-btn-decision-b">
          <span id="sim-btn-b-label">${escapeHtml(optB)}</span>
          <span class="sim-decide-sub">[dotyk / IO18]</span>
        </button>
      `;
      const btnA = document.getElementById('sim-btn-decision-a');
      const btnB = document.getElementById('sim-btn-decision-b');
      if (btnA) {
        if (danger) bindHold(btnA, () => resolveDecision(0));
        else btnA.addEventListener('click', () => { if (!promptGuardActive()) resolveDecision(0); });
      }
      if (btnB) btnB.addEventListener('click', () => { if (!promptGuardActive()) resolveDecision(1); });
    } else {
      el.decisionBtnRow.className = 'decision-btn-row stacked';
      const colors = ['#00d1ff', '#f59e0b', '#a855f7', '#10b981'];
      el.decisionBtnRow.innerHTML = opts.map((opt, i) => {
        const isSel = i === state.promptSel;
        const color = colors[i % colors.length];
        return `
          <button class="sim-decide-btn opt-row ${isSel ? 'selected' : ''}" data-idx="${i}" style="border-left: 3px solid ${color};">
            <span class="opt-label">${escapeHtml(opt)}</span>
            <span class="opt-hw-hint">${isSel && state.promptSelTapped ? 'znovu = odeslat' : (isSel ? 'IO18 potvrdit ↵' : 'dotyk')}</span>
          </button>
        `;
      }).join('');

      el.decisionBtnRow.querySelectorAll('.opt-row').forEach(btn => {
        btn.addEventListener('click', () => {
          if (promptGuardActive()) return;
          const idx = parseInt(btn.getAttribute('data-idx'), 10);
          // První klepnutí vybere, druhé na stejnou volbu odešle
          if (state.promptSel === idx && state.promptSelTapped) {
            resolveDecision(idx);
          } else {
            beepTap();
            state.promptSel = idx;
            state.promptSelTapped = true;
            renderDecisionButtons();
          }
        });
      });
    }
  }

  // ─── Menu a zhasnutý displej ────────────────────────────────────────────
  const MENU_N = 6;

  function renderMenu() {
    document.querySelectorAll('.sim-menu-item').forEach(item => {
      item.classList.toggle('selected', parseInt(item.dataset.idx, 10) === state.menuSel);
    });
    const demo = document.getElementById('sim-menu-demo');
    if (demo) demo.textContent = state.demo ? 'ZAP' : 'VYP';
  }

  function openMenu() {
    state.menuSel = 0;
    renderMenu();
    switchView('menu');
  }

  function setScreenOff(off) {
    state.screenOff = off;
    const screen = document.querySelector('.amoled-screen');
    if (screen) screen.classList.toggle('screen-off', off);
  }

  // Probuzení zhasnutého displeje: stisk ho jen rozsvítí, nic dalšího nespustí
  function wakeIfOff() {
    if (!state.screenOff) return false;
    setScreenOff(false);
    markInteraction();
    if (state.view === 'clock') switchView('normal');
    return true;
  }

  function executeMenuItem(idx) {
    state.menuSel = idx;
    renderMenu();
    switch (idx) {
      case 0: break;   // Nastavení je jen na krabičce
      case 1: switchView('clock'); break;
      case 2: state.demo = !state.demo; renderMenu(); break;
      case 3:
      case 4:
        switchView('normal');
        setScreenOff(true);
        break;
      case 5: switchView('normal'); break;
    }
  }

  function showDecisionPrompt(type) {
    setScreenOff(false);
    state.promptActive = true;
    state.promptType = type;
    state.promptArrivedMs = Date.now();
    // Otázku s volbami posílá jen hook pro AGY, schválení příkazu umí oba klienti
    if (type === 'question') setClient('agy', true);   // bez zvuku připojení, hraje zvuk otázky
    state.promptReminders = 0;
    state.promptSel = 0;
    state.promptSelTapped = false;
    state.promptShownMs = Date.now();
    // Most AGY dává na otázku 90 s a na rizikovou akci 45 s, Claude čas neposílá
    state.promptTtlS = state.client === 'agy' ? (type === 'question' ? 90 : 45) : 0;
    playPromptSound();

    if (type === 'question') {
      const q = DEMO_QUESTIONS[state.demoQuestionIdx % DEMO_QUESTIONS.length];
      state.demoQuestionIdx++;
      state.promptHeader = 'DOTAZ / ROZHODNUTÍ';
      state.promptTool = `Dotaz (${q.options.length} volby)`;
      state.promptHint = q.hint;
      state.promptOptions = q.options.slice();
    } else {
      state.promptHeader = 'ROZHODNOUT AKCI?';
      state.promptTool = state.client === 'agy' ? 'Kritická akce' : 'Spustit příkaz';
      state.promptHint = 'git push --force origin main';
      state.promptOptions = ['SCHVÁLIT', 'ZAMÍTNOUT'];
    }

    if (el.decisionHeader) el.decisionHeader.textContent = state.promptHeader;
    if (el.decisionTool) el.decisionTool.textContent = state.promptTool;
    if (el.decisionHint) el.decisionHint.textContent = state.promptHint;
    
    renderDecisionButtons();
    tickIdleAndPrompt();

    switchView('decision');
  }

  // Podržení tlačítka (myš i dotyk) po dobu DANGER_HOLD_MS, s průběhem ve stylu --hold
  function bindHold(elm, onDone, enabled = () => true) {
    let timer = null;
    let start = 0;
    let raf = null;
    const stop = () => {
      if (timer) clearTimeout(timer);
      if (raf) cancelAnimationFrame(raf);
      timer = null;
      raf = null;
      elm.style.setProperty('--hold', '0%');
    };
    const paint = () => {
      const pct = Math.min(100, (Date.now() - start) * 100 / DANGER_HOLD_MS);
      elm.style.setProperty('--hold', pct + '%');
      raf = requestAnimationFrame(paint);
    };
    elm.addEventListener('pointerdown', (e) => {
      if (!enabled() || promptGuardActive()) return;
      e.preventDefault();
      start = Date.now();
      paint();
      timer = setTimeout(() => { stop(); onDone(); }, DANGER_HOLD_MS);
    });
    ['pointerup', 'pointerleave', 'pointercancel'].forEach(ev => elm.addEventListener(ev, () => {
      if (timer && Date.now() - start < DANGER_HOLD_MS) playTone(900, 20);   // krátký stisk nestačí
      stop();
    }));
  }

  function resolveDecision(choice) {
    if (!state.promptActive) return;
    state.promptActive = false;

    const timeStr = new Date().toTimeString().slice(0, 5);
    const isQuestion = state.promptType === 'question';
    const quick = Date.now() - state.promptArrivedMs < 8000;
    const opts = state.promptOptions || ['SCHVÁLIT', 'ZAMÍTNOUT'];

    let chosenText = '';
    let isApprove = false;
    if (typeof choice === 'boolean') {
      isApprove = choice;
      chosenText = choice ? opts[0] : (opts[1] || opts[0]);
    } else if (typeof choice === 'number') {
      chosenText = opts[choice] || `Volba ${choice + 1}`;
      isApprove = (choice === 0);
    } else {
      chosenText = String(choice);
      isApprove = true;
    }

    if (isQuestion) {
      // Odpověď na otázku: vlastní počítadlo a stejný neutrální tón pro všechny volby
      beepAnswer();
      state.answers++;
      addChatMessage(`${timeStr} [VOLBA] ${chosenText}`);
      if (quick) triggerClawdHearts();
    } else if (isApprove) {
      beepApprove();
      state.approvals++;
      addChatMessage(`${timeStr} [SCHVÁLENO] ${state.promptTool}`);
      if (quick) triggerClawdHearts();
    } else {
      beepDeny();
      state.denials++;
      addChatMessage(`${timeStr} [ZAMÍTNUTO] ${state.promptTool}`);
    }
    markInteraction();

    renderProfile();
    // Návrat na hlavní obrazovku jen když uživatel mezitím nepřepnul jinam
    setTimeout(() => {
      if (state.view === 'decision') switchView('normal');
    }, 450);
  }

  // ─── Clawd Animations & Interactions ────────────────────────────────────
  function triggerClawdHearts() {
    if (el.heartsBubble) {
      el.heartsBubble.style.display = 'block';
      setTimeout(() => {
        el.heartsBubble.style.display = 'none';
      }, 1000);
    }
  }

  // Periodic subtle mascot animation: Clawd se pohupuje, Agy se vznáší výš
  function animateClawd() {
    state.tickCount++;
    const svg = state.client === 'agy' ? el.agySvg : el.clawdSvg;
    if (!svg) return;

    if (state.clawdEmotion === 'idle') {
      const amp = state.client === 'agy' ? 5 : 2;
      const bob = Math.sin(state.tickCount * 0.15) * amp;
      svg.style.transform = `translateY(${bob}px)`;
    }
  }

  // ─── Klient a kanál: štítek "AGY·USB" / "CLAUDE·BLE" a maskot ───────────
  // Claude Desktop se připojuje jen přes Bluetooth, USB používá jen bridge pro AGY.
  function renderClient() {
    const agy = state.client === 'agy';
    // SVG prvky nemají vlastnost .hidden jako HTML prvky, proto atribut
    if (el.agySvg) el.agySvg.toggleAttribute('hidden', !agy);
    if (el.clawdSvg) el.clawdSvg.toggleAttribute('hidden', agy);
    if (el.clawdSvg) el.clawdSvg.style.transform = '';
    if (el.agySvg) el.agySvg.style.transform = '';
    if (el.profTitle) el.profTitle.textContent = `PROFIL: ${agy ? 'Agy' : 'Clawd'}`;
    if (el.screenChannel) {
      const ble = state.channel === 'BLE';
      el.screenChannel.textContent = `${agy ? 'AGY' : 'CLAUDE'}·${state.channel}`;
      el.screenChannel.style.color = ble ? 'var(--color-cyan)' : 'var(--color-green)';
      el.screenChannel.style.borderColor = ble ? 'var(--color-cyan)' : 'var(--color-green)';
      el.screenChannel.style.background = ble ? 'rgba(6,182,212,0.15)' : 'rgba(16,185,129,0.15)';
    }
    const trChannel = document.getElementById('trigger-sim-channel');
    if (trChannel) {
      trChannel.disabled = !agy;
      trChannel.title = agy ? '' : 'Claude Desktop se připojuje jen přes Bluetooth';
    }
    const trClient = document.getElementById('trigger-sim-client');
    if (trClient) {
      const label = trClient.querySelector('span:last-child');
      if (label) label.textContent = agy ? 'Připojit Claude (Clawd)' : 'Připojit AGY (Agy)';
    }
  }

  function setClient(client, silent = false) {
    if (state.client !== client && !silent) playMelody(MELODY.connect);
    state.client = client;
    if (client === 'claude') state.channel = 'BLE';
    renderClient();
  }

  // ─── Event Listeners & Wiring ───────────────────────────────────────────
  function initEvents() {
    // Bottom Nav clicks
    if (el.navBtns.action) {
      el.navBtns.action.addEventListener('click', () => {
        beepTap();
        switchView('normal');
      });
    }
    if (el.navBtns.profile) {
      el.navBtns.profile.addEventListener('click', () => {
        beepTap();
        renderProfile();
        switchView('profile');
      });
    }
    if (el.navBtns.info) {
      el.navBtns.info.addEventListener('click', () => {
        beepTap();
        switchView('info');
      });
    }
    if (el.navBtns.menu) {
      el.navBtns.menu.addEventListener('click', () => {
        beepTap();
        if (state.view === 'menu') switchView('normal');
        else openMenu();
      });
    }

    // Compact chat card click -> expand to fullscreen chat
    if (el.compactChat) {
      el.compactChat.addEventListener('click', () => {
        beepTap();
        renderChat();
        switchView('fullchat');
      });
    }

    // Fullscreen chat close click -> collapse back to normal
    if (el.fullChatClose) {
      el.fullChatClose.addEventListener('click', () => {
        beepTap();
        switchView('normal');
      });
    }

    // Petting Clawd
    if (el.clawdSvg) {
      el.clawdSvg.parentElement.addEventListener('click', () => {
        playMelody(MELODY.pet);
        triggerClawdHearts();
      });
    }

    // Decision touch buttons
    if (el.btnDecisionA) {
      el.btnDecisionA.addEventListener('click', () => resolveDecision(true));
    }
    if (el.btnDecisionB) {
      el.btnDecisionB.addEventListener('click', () => resolveDecision(false));
    }

    // Physical Hardware Buttons on top edge
    const btnBoot = document.getElementById('hw-btn-boot');
    const btnUser = document.getElementById('hw-btn-user');
    if (btnBoot) {
      btnBoot.addEventListener('click', () => {
        if (wakeIfOff()) return;
        if (state.view === 'menu') {
          beepTap();
          state.menuSel = (state.menuSel + 1) % MENU_N;
          renderMenu();
        } else if (state.view === 'decision') {
          if (promptGuardActive()) return;
          if (state.promptOptions && state.promptOptions.length > 2) {
            // Cyklování výběru pro 3 a více voleb (jako na displeji firmwaru)
            beepTap();
            state.promptSel = (state.promptSel + 1) % state.promptOptions.length;
            state.promptSelTapped = false;
            renderDecisionButtons();
          } else if (promptIsDanger()) {
            // BOOT se u kritické akce drží (viz bindHold níže), kliknutí nestačí
          } else {
            resolveDecision(0);
          }
        } else {
          beepTap();
          // BOOT přepíná obrazovky Akce → Profil → Info jako firmware
          const modes = ['normal', 'profile', 'info'];
          const idx = modes.indexOf(state.view);
          switchView(idx < 0 ? 'normal' : modes[(idx + 1) % modes.length]);
        }
      });
    }
    // Podržení BOOT u kritické akce schválí
    if (btnBoot) {
      const dangerNow = () => state.view === 'decision' && state.promptActive && promptIsDanger();
      bindHold(btnBoot, () => { if (dangerNow()) resolveDecision(0); }, dangerNow);
    }
    if (btnUser) {
      btnUser.addEventListener('click', () => {
        if (wakeIfOff()) return;
        if (state.view === 'menu') {
          beepTap();
          executeMenuItem(state.menuSel);
        } else if (state.view === 'decision') {
          if (promptGuardActive()) return;
          if (state.promptOptions && state.promptOptions.length > 2) {
            // Potvrzení vybrané volby pro 3 a více voleb
            resolveDecision(state.promptSel);
          } else {
            resolveDecision(1);
          }
        } else {
          beepTap();
          if (state.view === 'clock') switchView('normal');
          else if (state.view === 'normal' || state.view === 'fullchat') scrollChat();
        }
      });
    }

    // External Control Panel Triggers
    const trQuestion = document.getElementById('trigger-sim-question');
    const trDanger = document.getElementById('trigger-sim-danger');
    const trClock = document.getElementById('trigger-sim-clock');
    const trPet = document.getElementById('trigger-sim-pet');
    const trChannel = document.getElementById('trigger-sim-channel');

    if (trQuestion) {
      trQuestion.addEventListener('click', () => showDecisionPrompt('question'));
    }
    if (trDanger) {
      trDanger.addEventListener('click', () => showDecisionPrompt('danger'));
    }
    // Položky menu dotykem, jako na krabičce
    document.querySelectorAll('.sim-menu-item').forEach(item => {
      item.addEventListener('click', () => {
        beepTap();
        executeMenuItem(parseInt(item.dataset.idx, 10));
      });
    });
    // Dotyk na zhasnutém displeji ho jen rozsvítí
    const screen = document.querySelector('.amoled-screen');
    if (screen) {
      screen.addEventListener('click', (e) => {
        if (wakeIfOff()) {
          e.stopPropagation();
          e.preventDefault();
        }
      }, true);
    }

    if (trClock) {
      trClock.addEventListener('click', () => {
        beepTap();
        switchView(state.view === 'clock' ? 'normal' : 'clock');
      });
    }
    const trDone = document.getElementById('trigger-sim-done');
    if (trDone) {
      trDone.addEventListener('click', () => {
        playMelody(MELODY.done);
        addChatMessage(`${new Date().toTimeString().slice(0, 5)} Hotovo – čeká na zadání`);
        triggerClawdHearts();
        if (state.view !== 'decision') switchView('normal');
      });
    }
    const trBattery = document.getElementById('trigger-sim-battery');
    const batPill = document.getElementById('screen-bat');
    if (trBattery && batPill) {
      trBattery.addEventListener('click', () => {
        const low = batPill.dataset.low !== '1';
        batPill.dataset.low = low ? '1' : '0';
        batPill.textContent = low ? '18%' : '100%';
        batPill.parentElement.style.color = low ? 'var(--color-red)' : '';
        playMelody(low ? MELODY.batLow : MELODY.chargeOn);
      });
    }

    if (trPet) {
      trPet.addEventListener('click', () => {
        playMelody(MELODY.pet);
        triggerClawdHearts();
      });
    }
    if (trChannel) {
      trChannel.addEventListener('click', () => {
        beepTap();
        if (state.client !== 'agy') return;
        state.channel = state.channel === 'USB' ? 'BLE' : 'USB';
        renderClient();
      });
    }
    const trClient = document.getElementById('trigger-sim-client');
    if (trClient) {
      trClient.addEventListener('click', () => {
        beepTap();
        setClient(state.client === 'agy' ? 'claude' : 'agy');
      });
    }

    // Sound toggle
    const soundChk = document.getElementById('sim-sound-toggle');
    if (soundChk) {
      soundChk.addEventListener('change', (e) => {
        soundEnabled = e.target.checked;
        if (soundEnabled) playTone(1800, 30);
      });
    }

    // Guide Tab Switching
    const guideTabs = document.querySelectorAll('.guide-tab-btn');
    const guideContents = document.querySelectorAll('.guide-tab-content');
    guideTabs.forEach(btn => {
      btn.addEventListener('click', () => {
        const target = btn.dataset.tab;
        guideTabs.forEach(b => b.classList.toggle('active', b === btn));
        guideContents.forEach(c => c.classList.toggle('active', c.id === `tab-${target}`));
      });
    });

    // Copy code button helpers (kopíruje jen příkaz, bez komentáře za #)
    document.querySelectorAll('.btn-copy').forEach(btn => {
      const original = btn.textContent;
      btn.addEventListener('click', () => {
        const code = btn.parentElement.querySelector('code');
        const codeText = (code ? code.textContent : '').replace(/\s+#.*$/, '').trim();
        const done = (label, color) => {
          btn.textContent = label;
          btn.style.color = color;
          setTimeout(() => {
            btn.textContent = original;
            btn.style.color = '';
          }, 2000);
        };
        if (!navigator.clipboard) {
          done('Nelze kopírovat', 'var(--color-red)');
          return;
        }
        navigator.clipboard.writeText(codeText)
          .then(() => done('Zkopírováno!', 'var(--color-green)'))
          .catch(() => done('Nelze kopírovat', 'var(--color-red)'));
      });
    });

    // Jakýkoli dotyk krabičky nebo ovládacího panelu odkládá přechod do hodin
    document.querySelectorAll('.device-wrapper, .simulator-panel').forEach(node => {
      node.addEventListener('click', markInteraction, true);
    });
    if (el.views.clock) {
      el.views.clock.addEventListener('click', () => {
        beepTap();
        switchView('normal');
      });
    }
  }

  // ─── Plynulý posun na sekci ─────────────────────────────────────────────
  // CSS scroll-behavior: smooth Chrome na Windows ignoruje, když jsou v systému
  // vypnuté animace, a stránka pak skáče. Posun proto řídí JavaScript.
  function initSmoothScroll() {
    const header = document.querySelector('.site-header');
    let anim = null;

    function stop() {
      if (anim) cancelAnimationFrame(anim);
      anim = null;
    }

    function scrollToY(targetY) {
      stop();
      const startY = window.scrollY;
      const dist = targetY - startY;
      if (Math.abs(dist) < 2) return;
      const duration = Math.min(900, Math.max(350, Math.abs(dist) * 0.35));
      const t0 = performance.now();
      const ease = t => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2);
      const step = now => {
        const t = Math.min(1, (now - t0) / duration);
        window.scrollTo({ top: startY + dist * ease(t), left: 0, behavior: 'instant' });
        anim = t < 1 ? requestAnimationFrame(step) : null;
      };
      anim = requestAnimationFrame(step);
    }

    // Kolečko myši nebo dotyk posun přeruší, ať se s uživatelem nepere
    ['wheel', 'touchstart', 'keydown'].forEach(ev => window.addEventListener(ev, stop, { passive: true }));

    document.querySelectorAll('a[href^="#"]').forEach(link => {
      link.addEventListener('click', (e) => {
        const id = link.getAttribute('href').slice(1);
        const target = id ? document.getElementById(id) : null;
        if (id && !target) return;
        e.preventDefault();
        const offset = (header ? header.offsetHeight : 0) + 8;
        const y = target ? target.getBoundingClientRect().top + window.scrollY - offset : 0;
        scrollToY(Math.max(0, y));
        history.replaceState(null, '', id ? `#${id}` : location.pathname + location.search);
      });
    });
  }

  // ─── Gallery Lightbox ───────────────────────────────────────────────────
  // Galerie ukazuje ostré zmenšeniny, po kliknutí se otevře plné rozlišení.
  // Bez JavaScriptu odkaz otevře obrázek přímo.
  function initLightbox() {
    const box = document.getElementById('lightbox');
    const img = document.getElementById('lightbox-img');
    const caption = document.getElementById('lightbox-caption');
    const links = Array.from(document.querySelectorAll('.gallery-zoom'));
    if (!box || !img || !links.length) return;

    let current = 0;
    let lastFocus = null;

    function show(index) {
      current = (index + links.length) % links.length;
      const link = links[current];
      const thumb = link.querySelector('img');
      const title = link.closest('.gallery-item')?.querySelector('.gallery-caption strong');
      img.src = link.href;
      img.alt = thumb ? thumb.alt : '';
      caption.textContent = title ? title.textContent : '';
    }

    function open(index) {
      lastFocus = document.activeElement;
      show(index);
      box.hidden = false;
      document.body.style.overflow = 'hidden';
      document.getElementById('lightbox-close').focus();
    }

    function close() {
      box.hidden = true;
      img.src = '';
      document.body.style.overflow = '';
      if (lastFocus) lastFocus.focus();
    }

    links.forEach((link, i) => {
      link.addEventListener('click', (e) => {
        e.preventDefault();
        open(i);
      });
    });
    document.getElementById('lightbox-close').addEventListener('click', close);
    document.getElementById('lightbox-prev').addEventListener('click', () => show(current - 1));
    document.getElementById('lightbox-next').addEventListener('click', () => show(current + 1));
    box.addEventListener('click', (e) => {
      if (e.target === box) close();
    });
    document.addEventListener('keydown', (e) => {
      if (box.hidden) return;
      if (e.key === 'Escape') close();
      else if (e.key === 'ArrowLeft') show(current - 1);
      else if (e.key === 'ArrowRight') show(current + 1);
    });
  }

  // ─── Initialization ─────────────────────────────────────────────────────
  document.addEventListener('DOMContentLoaded', () => {
    initEvents();
    initLightbox();
    initSmoothScroll();
    renderClient();
    renderChat();
    renderProfile();
    updateClock();
    setInterval(updateClock, 1000);
    setInterval(tickIdleAndPrompt, 250);
    setInterval(animateClawd, 120);
  });
})();
