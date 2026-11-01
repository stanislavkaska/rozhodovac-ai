// Části vycházejí z anthropics/claude-desktop-buddy (Copyright 2026 Anthropic, PBC),
// licence MIT, viz LICENSE a LICENSES/MIT-claude-desktop-buddy.txt.
#include <Arduino.h>
#include <esp_mac.h>
#include <stdarg.h>
#include <math.h>
#include "hw_waveshare.h"
#include "ble_bridge.h"
#include "character.h"
#include "buddy.h"
#include "data.h"

// Advertise as "Claude-XXXX" so both Claude Desktop Hardware Buddy picker and AGY Bridge discover us
static char btName[20] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

// ─── Colors (RGB565) ───────────────────────────────────────────────────────
static const uint16_t COL_BG        = 0x0000;
static const uint16_t COL_CARD      = 0x1082; // subtle dark slate on AMOLED
static const uint16_t COL_CARD_HI   = 0x2124;
static const uint16_t COL_BORDER    = 0x39E7;
static const uint16_t COL_GREEN     = 0x07E0;
static const uint16_t COL_GREEN_BG  = 0x0320;
static const uint16_t COL_GREEN_HI  = 0x0540;
static const uint16_t COL_HOT       = 0xFA20; // orange-red
static const uint16_t COL_RED_BG    = 0x5000;
static const uint16_t COL_RED_HI    = 0x8800;
static const uint16_t COL_CYAN      = 0x07FF;
static const uint16_t COL_YELLOW    = 0xFFE0;
static const uint16_t COL_AMBER_BG  = 0x2080;

// ─── State Machine ─────────────────────────────────────────────────────────
enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
static const char* const stateNamesCz[] = {
  "spánek", "klid", "pracuje", "čeká na rozhodnutí", "oslava", "motání hlavy", "radost"
};

static TamaState    tama = {};
static PersonaState baseState   = P_SLEEP;
static PersonaState activeState = P_SLEEP;
static uint32_t     oneShotUntil = 0;
static uint32_t     lastShakeCheck = 0;
static float        accelBaseline = 1.0f;

static bool    menuOpen     = false;
static uint8_t menuSel      = 0;
static bool    settingsOpen = false;
static uint8_t settingsSel  = 0;
static bool    resetOpen    = false;
static uint8_t resetSel     = 0;
static uint8_t brightLevel  = 3;   // 0..4 -> AMOLED brightness 35..255 (načte se z nastavení, výchozí 3/4)
static bool    btnALong     = false;

enum DisplayMode { DISP_NORMAL = 0, DISP_PET = 1, DISP_INFO = 2, DISP_COUNT = 3 };
static uint8_t  displayMode    = DISP_NORMAL;
static uint8_t  infoPage       = 0;
static const uint8_t INFO_PAGES = 5;
static uint8_t  petPage        = 0;
static const uint8_t PET_PAGES  = 2;
static uint8_t  msgScroll      = 0;
static uint8_t  msgMaxScroll   = 0;   // nastavuje drawHUD podle počtu zalomených řádků
static bool     chatExpanded   = false;
static uint16_t lastLineGen    = 0;
static char     lastPromptId[48] = "";
static uint8_t  lastPromptCh = CH_NONE;   // kanál dotazu lastPromptId, potvrzení se bere jen z něj
static uint32_t lastInteractMs = 0;
static bool     dimmed         = false;
static bool     screenOff      = false;
static bool     swallowBtnA    = false;
static bool     swallowBtnB    = false;

static uint32_t wakeTransitionUntil = 0;
static const uint32_t SCREEN_OFF_MS = 45000;
static bool     napping        = false;
static uint32_t napStartMs     = 0;
static uint32_t promptArrivedMs = 0;   // kdy dotaz vznikl (u AGY podle "age" z mostu)
static uint32_t promptShownMs   = 0;   // kdy se dotaz na krabičce objevil (ochranná doba)
static uint32_t promptTotalS    = 0;   // celková doba na odpověď podle mostu, 0 = neznámá
static uint8_t  promptReminders = 0;   // kolikrát už zazněla připomínka dotazu
static uint8_t  promptSel       = 0;   // vybraná volba u otázek se 3+ možnostmi
static bool     promptSelTapped = false; // u 3+ voleb první klepnutí jen vybere, druhé odešle
static uint32_t responseSentMs  = 0;   // kdy odešla odpověď (čeká se na potvrzení mostu)
static bool     responseAcked   = false;
static uint32_t holdStartMs     = 0;   // podržení SCHVÁLIT u kritické akce (dotyk)
static uint32_t touchDownMs     = 0;   // začátek posledního dotyku

// Po objevení dotazu se dotyk a tlačítka chvíli ignorují: klepnutí nebo stisk
// mířený na předchozí obrazovku by jinak dopadl na SCHVÁLIT nebo ZAMÍTNOUT
static const uint32_t PROMPT_GUARD_MS = 700;
// Kritickou akci schvaluje podržení, ne klepnutí
static const uint32_t DANGER_HOLD_MS  = 1000;
// Most (AGY i Claude Code v terminálu) potvrdí přijetí odpovědi; bez potvrzení
// se tlačítka vrátí (5 s, po BLE může potvrzení čekat za velkým snímkem).
// Claude Desktop nepotvrzuje: když dotaz po odpovědi dlouho nezmizí (zápis
// přes BLE se ztratil), tlačítka se vrátí po delší době.
static const uint32_t ACK_TIMEOUT_MS        = 5000;
static const uint32_t CLAUDE_RESEND_AFTER_MS = 20000;
static uint32_t btnADownMs = 0;   // kdy začal poslední stisk BOOT (puštění před dotazem se ignoruje)

// Statistika odpovědi se u AGY započítá až po potvrzení mostu, aby se
// opakovaný pokus po ztrátě spojení nezapočítal dvakrát
enum PendingStat : uint8_t { PS_NONE, PS_APPROVE, PS_DENY, PS_ANSWER };
static PendingStat pendingStat = PS_NONE;
static uint32_t    pendingStatTook = 0;
static bool     clockForced = false;   // hodiny zapnuté z menu (do dotyku nebo práce agenta)
static bool     clockShown  = false;   // hodiny jsou právě na displeji
static bool     responseSent   = false;

static HwDateTime _clkDt;
uint32_t _clkLastRead = 0;
static bool _onUsb = true;

// ─── Helpers ───────────────────────────────────────────────────────────────
static bool hitRect(int tx, int ty, int rx, int ry, int rw, int rh) {
  return tx >= rx && tx < (rx + rw) && ty >= ry && ty < (ry + rh);
}

static bool isQuestionTool(const char* rawTool) {
  if (!rawTool) return false;
  return strcmp(rawTool, "ask_question") == 0 || strcmp(rawTool, "question") == 0 ||
         strcmp(rawTool, "Otázka") == 0 || strcmp(rawTool, "AskUserQuestion") == 0;
}

static const char* friendlyToolName(const char* rawTool) {
  if (!rawTool || !rawTool[0]) return "Rozhodnutí";
  if (isQuestionTool(rawTool)) return "Dotaz na postup";
  if (strcmp(rawTool, "run_command") == 0 || strcmp(rawTool, "Bash") == 0 ||
      strcmp(rawTool, "bash") == 0 || strcmp(rawTool, "execute_command") == 0) {
    return "Spustit příkaz";
  }
  if (strcmp(rawTool, "write_file") == 0 || strcmp(rawTool, "Write") == 0 ||
      strcmp(rawTool, "create_file") == 0) {
    return "Zapsat soubor";
  }
  if (strcmp(rawTool, "edit_file") == 0 || strcmp(rawTool, "Edit") == 0 ||
      strcmp(rawTool, "MultiEdit") == 0 || strcmp(rawTool, "replace_file_content") == 0 ||
      strcmp(rawTool, "multi_replace_file_content") == 0) {
    return "Upravit soubor";
  }
  if (strcmp(rawTool, "delete_file") == 0 || strcmp(rawTool, "Delete") == 0) {
    return "Smazat soubor";
  }
  if (strcmp(rawTool, "WebFetch") == 0 || strcmp(rawTool, "search_web") == 0) {
    return "Přístup na web";
  }
  return rawTool;
}

static void applyBrightness() {
  static const uint8_t LEVELS[5] = { 35, 85, 140, 200, 255 };
  hwSetBrightness(LEVELS[brightLevel % 5]);
}

// ─── Zvuky ────────────────────────────────────────────────────────────────
// Každá událost má krátkou melodii. Tóny přehrává úloha na pozadí (hwBeep),
// hlavní smyčka nečeká. Kategorie řídí, co zazní při vypnutém cvakání a v noci.
struct Note { uint16_t f; uint16_t ms; };   // f = 0 je pauza
enum SoundCat : uint8_t {
  CAT_CLICK,      // odezva ovládání (lze vypnout položkou Zvuk ovládání)
  CAT_INFO,       // informace (v nočním klidu se vynechá)
  CAT_IMPORTANT   // dotazy, rozhodnutí, varování (v nočním klidu potichu)
};

static const Note SND_REQUEST[]    = { {1400, 55}, {0, 15}, {1900, 65} };            // žádost o schválení
static const Note SND_QUESTION[]   = { {1320, 60}, {0, 20}, {1760, 80} };            // otázka s volbami
static const Note SND_DANGER[]     = { {2400, 70}, {0, 40}, {2400, 70}, {0, 40}, {2400, 90} };
static const Note SND_APPROVE[]    = { {1800, 45}, {2400, 65} };
static const Note SND_DENY[]       = { {600, 70}, {440, 110} };
static const Note SND_ANSWER[]     = { {1600, 40}, {2000, 55} };
static const Note SND_DONE[]       = { {1047, 90}, {1319, 90}, {1568, 140} };        // C E G
static const Note SND_TIMEOUT[]    = { {1200, 80}, {900, 80}, {700, 120} };
static const Note SND_CONNECT[]    = { {1200, 50}, {1800, 70} };
static const Note SND_DISCONNECT[] = { {1600, 50}, {1000, 80} };
static const Note SND_PAIR_OK[]    = { {1319, 70}, {1568, 70}, {2093, 120} };
static const Note SND_PAIR_FAIL[]  = { {400, 150}, {0, 40}, {400, 150} };
static const Note SND_BAT_LOW[]    = { {880, 120}, {0, 80}, {880, 120} };
static const Note SND_BAT_CRIT[]   = { {660, 150}, {0, 60}, {660, 150}, {0, 60}, {660, 150} };
static const Note SND_CHARGE_ON[]  = { {1500, 40}, {2200, 60} };
static const Note SND_CHARGE_OFF[] = { {2200, 40}, {1500, 60} };
static const Note SND_SLEEP[]      = { {900, 80}, {600, 120} };
static const Note SND_WAKE[]       = { {600, 60}, {900, 90} };
static const Note SND_LEVEL[]      = { {1568, 70}, {2093, 70}, {2637, 140} };
static const Note SND_DIZZY[]      = { {900, 40}, {700, 40}, {900, 40} };
static const Note SND_BOOT[]       = { {1047, 60}, {1568, 90} };
static const Note SND_PIN[]        = { {1800, 60}, {0, 40}, {1800, 60} };
static const Note SND_MENU[]       = { {1000, 40} };
static const Note SND_PET[]        = { {2300, 30}, {2600, 40} };
static const Note SND_RESET_ARM[]  = { {1400, 60} };
static const Note SND_RESET_DO[]   = { {700, 180} };

#define PLAY(melody, cat) playMelody(melody, sizeof(melody) / sizeof(melody[0]), cat)

// Noční klid 22–7 h podle RTC (jen když je čas platný)
static bool nightNow() {
  if (!settings().nightQuiet || !dataRtcValid()) return false;
  return _clkDt.hour >= 22 || _clkDt.hour < 7;
}

static void playMelody(const Note* notes, uint8_t count, SoundCat cat) {
  const Settings& s = settings();
  if (!s.sound) return;
  if (cat == CAT_CLICK && !s.clicks) return;
  bool night = nightNow();
  if (night && cat == CAT_INFO) return;
  static const uint16_t AMP[3] = { 1500, 5500, 14000 };   // tichý, střední, hlasitý
  uint16_t amp = AMP[s.volume % 3];
  if (night && amp > AMP[0]) amp = AMP[0];
  for (uint8_t i = 0; i < count; i++) hwBeep(notes[i].f, notes[i].ms, amp);
}

// Krátké cvaknutí při ovládání
static void click(uint16_t freq) {
  Note n = { freq, 20 };
  playMelody(&n, 1, CAT_CLICK);
}

// Zpráva i s koncem řádku jde jedním zápisem (po BLE jinak "\n" jako zvláštní notifikace).
// Odpověď jde jen kanálem, ze kterého dotaz přišel: most na USB jinak dostával
// i odpovědi na dotazy Claude Desktopu a jeho záporné potvrzení je „potvrzovalo“.
static void sendCmd(const char* json, uint8_t ch) {
  char buf[720];
  size_t n = strlen(json);
  if (n + 1 > sizeof(buf)) return;
  memcpy(buf, json, n);
  buf[n] = '\n';
  if (ch != CH_BLE) Serial.write((const uint8_t*)buf, n + 1);
  if (ch != CH_USB) bleWrite((const uint8_t*)buf, n + 1);
}

static void fullClearCanvas() {
  Arduino_Canvas* cv = hwCanvas();
  if (cv) cv->fillScreen(characterPalette().bg);
}

static void applyDisplayMode() {
  if (displayMode != DISP_NORMAL) chatExpanded = false;
  buddySetMode((displayMode != DISP_NORMAL) ? CLAWD_PEEK : CLAWD_FULL);
  fullClearCanvas();
  buddyInvalidate();
}

// Zhasnutí displeje: do paměti panelu se nejdřív pošle černý snímek, aby po
// probuzení nebliklo staré (třeba už vyřízený příkaz). Procesor mezitím jede na 80 MHz.
static void screenOffNow() {
  if (screenOff) return;
  Arduino_Canvas* cv = hwCanvas();
  if (cv) {
    cv->fillScreen(COL_BG);
    hwFlush();
  }
  hwSetDisplayPower(false);
  screenOff = true;
  setCpuFrequencyMhz(80);
}

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    setCpuFrequencyMhz(240);
    hwSetDisplayPower(true);
    if (napping) hwSetBrightness(10);   // spánek displejem dolů trvá dál
    else         applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 6000;
    fullClearCanvas();
    buddyInvalidate();
  }
  if (dimmed && !napping) {
    applyBrightness();
    dimmed = false;
  }
}

// Dotaz na displej: hlavní obrazovka, zavřené nabídky, nová ochranná doba.
// Volá se při příchodu dotazu i když se tlačítka vrací po neúspěšném odeslání
// (jinak by dotaz platil pod jinou obrazovkou a klepnutí na „Akce“ by schválilo).
static void presentPrompt() {
  promptShownMs = millis();
  holdStartMs = 0;
  // Případný stisk BOOT z předchozí obrazovky po puštění nic neodešle
  if (hwBtnBoot().isPressed()) swallowBtnA = true;
  clockForced = false;
  wake();
  displayMode = DISP_NORMAL;
  chatExpanded = false;
  menuOpen = settingsOpen = resetOpen = false;
  applyDisplayMode();
}

static void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

static PersonaState derive(const TamaState& s) {
  if (!s.connected)           return P_SLEEP;
  if (s.sessionsWaiting > 0)  return P_ATTENTION;
  if (s.recentlyCompleted)    return P_CELEBRATE;
  if (s.sessionsRunning >= 1) return P_BUSY;
  return P_IDLE;
}

static bool isFaceDown() {
  float ax, ay, az;
  if (!hwGetAccel(ax, ay, az)) return false;
  return az < -0.75f && fabsf(ax) < 0.45f && fabsf(ay) < 0.45f;
}

static bool checkShake() {
  float ax, ay, az;
  if (!hwGetAccel(ax, ay, az)) return false;
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.92f + mag * 0.08f;
  return delta > 0.85f;
}

static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = hwPower().usbPresent;
  hwRtcGet(_clkDt);
}

// ─── Approval / Denial Action ──────────────────────────────────────────────
// Rozhodnutí se skládá přes ArduinoJson, aby uvozovky nebo zpětná lomítka v textu
// volby (u otázek se posílá text zvolené možnosti) nerozbily JSON pro bridge.
// V demo režimu se nic neposílá: prompt je jen ukázkový a protistrana ho nezná.
// U otázek se posílá i pořadí volby (opt, od 0): text může být zkrácený a pořadí
// je jednoznačné. Vícenásobný výběr posílá decision "multi" a pořadí zaškrtnutých
// voleb v poli opts (mask = bitová maska). Vrací false, když se odpověď
// nepodařilo sestavit.
static bool sendDecision(const char* decision, int opt = -1, int mask = -1) {
  if (dataDemo()) return true;
  JsonDocument d;
  d["cmd"]      = "permission";
  d["id"]       = tama.promptId;
  d["decision"] = decision;
  if (opt >= 0) d["opt"] = opt;
  if (mask >= 0) {
    JsonArray a = d["opts"].to<JsonArray>();
    for (int i = 0; i < PROMPT_MAX_OPTS; i++) {
      if (mask & (1 << i)) a.add(i);
    }
  }
  char cmd[640];
  size_t n = serializeJson(d, cmd, sizeof(cmd));
  if (n == 0 || n >= sizeof(cmd) - 1) return false;
  sendCmd(cmd, tama.promptCh);
  return true;
}

static void markResponseSent() {
  responseSent = true;
  responseSentMs = millis();
  responseAcked = false;
  holdStartMs = 0;
}

// Otázka s volbami (posílá jen bridge pro AGY): podpora 2 až PROMPT_MAX_OPTS možností
static uint8_t promptOptionCount() {
  if (tama.promptOptCount >= 2) return tama.promptOptCount;
  if (tama.promptOptA[0] && tama.promptOptB[0]) return 2;
  return 0;
}

static bool promptIsQuestion() {
  return promptOptionCount() >= 2;
}

// Riziková akce: schvaluje se podržením. Most ji označí příznakem "danger"
// (starší most pro AGY názvem nástroje), u Claude Desktopu ji podle vzorů
// v celém příkazu označí data.h.
static bool promptIsDanger() {
  // Obrazovka otázky nemá podržení; most rizikovou akci posílá vždy jako SCHVÁLIT / ZAMÍTNOUT
  if (tama.promptQ) return false;
  if (tama.promptDanger) return true;   // i s popisky voleb: podržení nejde obejít
  if (promptIsQuestion()) return false;
  return strcmp(tama.promptTool, "Kritická akce") == 0;
}

static bool promptGuardActive() {
  return (millis() - promptShownMs) < PROMPT_GUARD_MS;
}

// Dotaz z mostu (AGY i Claude Code v terminálu): most posílá ttl a odpověď
// potvrzuje. Claude Desktop ttl ani potvrzení neposílá.
static bool promptFromBridge() {
  return tama.agyClient || tama.promptTtl >= 0;
}

// Zbývající sekundy do vypršení dotazu podle mostu, -1 = neznámé
static int32_t promptRemainingS() {
  if (tama.promptTtl < 0) return -1;
  int32_t r = tama.promptTtl - (int32_t)((millis() - tama.promptRxMs) / 1000);
  return r < 0 ? 0 : r;
}

// Čas připomínky n (od 1) v sekundách od vzniku dotazu. Když most řekne,
// kolik je na odpověď času, zazní připomínky v polovině a ve 4/5 té doby.
// Jinak (Claude) po 30, 60 a 90 s.
static uint32_t reminderAtS(uint8_t n) {
  if (promptTotalS > 0) {
    if (n == 1) return promptTotalS / 2;
    if (n == 2) return promptTotalS * 4 / 5;
    return 0xFFFFFFFF;
  }
  return n <= 3 ? 30u * n : 0xFFFFFFFF;
}

// Od 5 voleb (a na obrazovce otázky vždy) se maskot nekreslí: karta s otázkou
// je hned pod horní lištou a řádky voleb vyplní zbytek displeje
static bool promptHidesBuddy() {
  return tama.promptId[0] && (tama.promptQ || promptOptionCount() >= 5);
}

static const char* promptOptionLabel(uint8_t i) {
  if (i < PROMPT_MAX_OPTS && tama.promptOpts[i][0]) return tama.promptOpts[i];
  if (i == 0) return tama.promptOptA;
  if (i == 1) return tama.promptOptB;
  return "";
}

static void getOptionBtnRect(uint8_t idx, uint8_t totalOpts, int& x, int& y, int& w, int& h) {
  if (totalOpts <= 2) {
    w = 206; h = 74;
    y = 384;
    x = (idx == 0) ? 24 : 250;
  } else if (totalOpts == 3) {
    x = 24; w = 432; h = 44;
    y = 320 + idx * 50;
  } else if (totalOpts == 4) {
    x = 24; w = 432; h = 36;
    y = 310 + idx * 40;
  } else {
    // 5 až 8 voleb bez maskota: řádky rovnoměrně od 162 px po spodní okraj
    // (5 voleb = 57 px, 8 voleb = 34 px na řádek)
    const int areaY = 162;
    const int areaH = LCD_HEIGHT - 12 - areaY;
    int pitch = areaH / totalOpts;
    x = 24; w = 432; h = pitch - 4;
    y = areaY + idx * pitch;
  }
}

// Zvuk dotazu podle druhu: otázka, riziková akce z AGY, běžná žádost o schválení
static void playPromptSound() {
  if (tama.promptQ) {
    // Obrazovka otázky slouží i pro povolení s volbou „Ano, už se neptat“
    if (isQuestionTool(tama.promptTool)) PLAY(SND_QUESTION, CAT_IMPORTANT);
    else                                 PLAY(SND_REQUEST, CAT_IMPORTANT);
    return;
  }
  if (promptIsQuestion())                            PLAY(SND_QUESTION, CAT_IMPORTANT);
  else if (promptIsDanger())                         PLAY(SND_DANGER, CAT_IMPORTANT);
  else                                               PLAY(SND_REQUEST, CAT_IMPORTANT);
}

// Odpověď na otázku: vlastní počítadlo, neutrální tón pro obě volby
static void applyStat(PendingStat kind, uint32_t tookS) {
  if (kind == PS_APPROVE)     statsOnApproval(tookS);
  else if (kind == PS_DENY)   statsOnDenial();
  else if (kind == PS_ANSWER) statsOnAnswer(tookS);
}

static void recordStat(PendingStat kind, uint32_t tookS) {
  if (dataDemo()) return;
  if (promptFromBridge()) { pendingStat = kind; pendingStatTook = tookS; }
  else                applyStat(kind, tookS);
}

static void answerQuestion(uint8_t idx) {
  if (!tama.promptId[0] || responseSent) return;
  if (!sendDecision(promptOptionLabel(idx), idx)) return;
  markResponseSent();
  uint32_t tookS = (millis() - promptShownMs) / 1000;   // od zobrazení, ne od vzniku
  recordStat(PS_ANSWER, tookS);
  PLAY(SND_ANSWER, CAT_IMPORTANT);
  if (tookS < 8) triggerOneShot(P_HEART, 2500);
}

// Demo režim statistiky nemění, jen posune ukázku dál
static void doApprove() {
  if (!tama.promptId[0] || responseSent) return;
  if (promptIsQuestion()) { answerQuestion(0); return; }
  if (!sendDecision("once")) return;
  markResponseSent();
  uint32_t tookS = (millis() - promptShownMs) / 1000;   // od zobrazení, ne od vzniku
  recordStat(PS_APPROVE, tookS);
  PLAY(SND_APPROVE, CAT_IMPORTANT);
  if (tookS < 8) triggerOneShot(P_HEART, 2500);
  if (dataDemo()) dataAdvanceDemo();
}

static void doDeny() {
  if (!tama.promptId[0] || responseSent) return;
  if (promptIsQuestion()) { answerQuestion(1); return; }
  if (!sendDecision("deny")) return;
  markResponseSent();
  recordStat(PS_DENY, 0);
  PLAY(SND_DENY, CAT_IMPORTANT);
  if (dataDemo()) dataAdvanceDemo();
}

// ─── UTF-8 Word Wrap Helper (preserves Czech multi-byte glyphs intact) ─────
static uint8_t wrapUtf8(const char* rawIn, char out[][96], uint8_t maxRows, uint8_t maxCols) {
  if (!rawIn || maxRows == 0 || maxCols == 0) return 0;
  if (maxCols > 44) maxCols = 44;

  uint8_t row = 0;
  uint8_t colChars = 0;
  size_t  rowBytes = 0;

  auto finishRow = [&]() {
    out[row][rowBytes] = 0;
    row++;
    colChars = 0;
    rowBytes = 0;
  };

  const char* p = rawIn;
  while (*p && row < maxRows) {
    while (*p && (uint8_t)*p <= 0x20) {
      if (*p == '\n' && colChars > 0) {
        finishRow();
        if (row >= maxRows) return row;
      }
      p++;
    }
    if (!*p) break;

    const char* wStart = p;
    uint16_t wChars = 0;
    while (*p && (uint8_t)*p > 0x20) {
      hwNextUtf8(p);
      wChars++;
    }
    const char* wEnd = p;
    if (wChars == 0) break;

    if (colChars > 0 && (colChars + 1 + wChars) > maxCols) {
      finishRow();
      if (row >= maxRows) return row;
    }

    if (colChars > 0 && rowBytes + 1 < 95) {
      out[row][rowBytes++] = ' ';
      colChars++;
    }

    const char* wp = wStart;
    while (wp < wEnd && row < maxRows) {
      const char* cpStart = wp;
      hwNextUtf8(wp);
      size_t cpBytes = (size_t)(wp - cpStart);

      if (colChars >= maxCols || (rowBytes + cpBytes) >= 95) {
        finishRow();
        if (row >= maxRows) return row;
      }
      memcpy(&out[row][rowBytes], cpStart, cpBytes);
      rowBytes += cpBytes;
      colChars++;
    }
  }

  if (colChars > 0 && row < maxRows) {
    out[row][rowBytes] = 0;
    row++;
  }
  return row;
}

// ─── UI Components (480x480 AMOLED) ────────────────────────────────────────
static void drawTopStatusBar() {
  Arduino_Canvas* cv = hwCanvas();
  const Palette& p = characterPalette();
  cv->fillRect(0, 0, LCD_WIDTH, 50, p.bg);

  // Connection pill (inset x=40 for rounded bezel corners): klient a kanál, např. "AGY·USB"
  const char* link = dataScenarioName();
  uint16_t pillCol = COL_CARD_HI;
  uint16_t txtCol  = p.textDim;
  if (strcmp(link, "BLE") == 0)       { pillCol = 0x0210; txtCol = COL_CYAN; }
  else if (strcmp(link, "USB") == 0)  { pillCol = COL_GREEN_BG; txtCol = COL_GREEN; }
  else if (strcmp(link, "DEMO") == 0) { pillCol = 0x4200; txtCol = COL_YELLOW; }

  char pillTxt[24];
  if (strcmp(link, "BLE") == 0 || strcmp(link, "USB") == 0) {
    snprintf(pillTxt, sizeof(pillTxt), "%s·%s", tama.agyClient ? "AGY" : "CLAUDE", link);
  } else {
    snprintf(pillTxt, sizeof(pillTxt), "%s", link);
  }
  int pillW = (int)hwUtf8Len(pillTxt) * 12 + 20;
  cv->fillRoundRect(40, 12, pillW, 28, 8, pillCol);
  cv->drawRoundRect(40, 12, pillW, 28, 8, txtCol);
  hwDrawTextCentered(cv, pillTxt, 40 + pillW / 2, 19, 2, txtCol, pillCol);

  // Uprostřed: čekající a běžící relace, jinak čas (i bez spojení)
  {
    char sb[40];
    if (tama.connected && tama.sessionsWaiting > 0) {
      snprintf(sb, sizeof(sb), "! %u čeká", tama.sessionsWaiting);
      hwDrawTextCentered(cv, sb, 240, 19, 2, COL_HOT, p.bg);
    } else if (tama.connected && tama.sessionsRunning > 0) {
      snprintf(sb, sizeof(sb), "* %u běží", tama.sessionsRunning);
      hwDrawTextCentered(cv, sb, 240, 19, 2, COL_CYAN, p.bg);
    } else if (dataRtcValid()) {
      snprintf(sb, sizeof(sb), "%02u:%02u", _clkDt.hour, _clkDt.minute);
      hwDrawTextCentered(cv, sb, 240, 19, 2, p.textDim, p.bg);
    }
  }

  // Battery indicator on right (inset x<=438)
  const HwPowerInfo& pw = hwPower();
  char bb[12];
  if (pw.batteryConnected) {
    snprintf(bb, sizeof(bb), "%d%%", pw.batteryPct);
  } else {
    snprintf(bb, sizeof(bb), "USB");
  }
  uint16_t batCol = pw.charging ? COL_GREEN : (pw.batteryPct <= 20 ? COL_HOT : p.textDim);
  hwDrawTextRight(cv, bb, 402, 19, 2, batCol, p.bg);

  // Battery icon (408..438)
  cv->drawRoundRect(408, 16, 26, 18, 3, batCol);
  cv->fillRect(434, 21, 3, 8, batCol);
  int fillW = pw.batteryConnected ? (pw.batteryPct * 20 / 100) : 20;
  if (fillW < 2) fillW = 2;
  cv->fillRect(411, 19, fillW, 12, batCol);

  cv->drawFastHLine(28, 49, LCD_WIDTH - 56, 0x2104);
}

// Bottom navigation bar: [Akce] [Profil] [Info] [Menu]
static const int NAV_Y = 424;
static const int NAV_H = 44;
static const int NAV_X0 = 36;
static const int NAV_W  = 96;
static const int NAV_GAP = 8;

static void drawBottomNav() {
  Arduino_Canvas* cv = hwCanvas();
  const Palette& p = characterPalette();
  cv->fillRect(0, NAV_Y - 4, LCD_WIDTH, LCD_HEIGHT - (NAV_Y - 4), p.bg);

  static const char* const LBL[4] = { "Akce", "Profil", "Info", "Menu" };
  for (int i = 0; i < 4; i++) {
    int bx = NAV_X0 + i * (NAV_W + NAV_GAP);
    bool active = (i < 3 && displayMode == i && !menuOpen && !settingsOpen && !resetOpen)
               || (i == 3 && (menuOpen || settingsOpen || resetOpen));
    uint16_t bg = active ? COL_CARD_HI : COL_CARD;
    uint16_t bd = active ? p.body : COL_BORDER;
    uint16_t fg = active ? p.text : p.textDim;
    cv->fillRoundRect(bx, NAV_Y, NAV_W, NAV_H, 10, bg);
    cv->drawRoundRect(bx, NAV_Y, NAV_W, NAV_H, 10, bd);
    hwDrawTextCentered(cv, LBL[i], bx + NAV_W / 2, NAV_Y + 15, 2, fg, bg);
  }
}

// ─── Obrazovka otázky ("q":1, most pro Claude Code v terminálu) ──────────
// Volby pod sebou v čitelné velikosti: popisek a pod ním tlumeně popis. Když
// se nevejdou, písmo se nezmenšuje, seznam se posouvá (tahem prstu, klepnutím
// na ořezanou volbu nahoře nebo dole, nebo s výběrem přes BOOT); vpravo je
// posuvník. Dole jsou podle mostu tlačítka ZPĚT, TERMINÁL a POTVRDIT.
//   jedna volba:   klepnutí vybere, druhé klepnutí odešle (IO18 odešle vybranou)
//   víc voleb:     klepnutí zaškrtne / odškrtne, odešle POTVRDIT
//   BOOT:          výběr na další volbu nebo tlačítko, IO18 ho použije
static const int Q_CARD_Y = 58;
static const int Q_BAR_Y  = 414;   // spodní tlačítka
static const int Q_BAR_H  = 54;
static const int Q_GAP    = 6;     // mezera mezi volbami
static const int Q_ROW_H  = 20;    // řádek textu velikosti 2
static const int Q_PEEK_H = 22;    // viditelný konec předchozí volby po posunutí
static const uint8_t Q_TEXT_COLS = 31;   // text v řádku volby (x 68 až 440)
static const uint8_t Q_HINT_COLS = 34;   // text otázky v kartě (x 36 až 444)
static const uint16_t Q_ACCENTS[4] = { COL_GREEN, COL_CYAN, COL_YELLOW, 0xFBE0 };

enum QBtn : uint8_t { QB_BACK, QB_TERM, QB_OK };

static uint8_t qChecked     = 0;      // zaškrtnuté volby (vícenásobný výběr, bitová maska)
static uint8_t qFirst       = 0;      // první zobrazená volba při posouvání
static bool    qFollowCursor = false; // výběr přesunutý přes BOOT: seznam se k němu posune

struct QLayout {
  uint8_t n;               // počet voleb
  uint8_t nBtn;            // počet spodních tlačítek
  uint8_t btn[3];          // druh tlačítka (QBtn) zleva
  uint8_t hintRows;
  int     cardH;
  int     optTop, optBottom;
  uint8_t descMax;         // nejvíc řádků popisu u volby
  uint8_t first, last;     // zobrazené volby (first > last = žádná)
  bool    moreAbove, moreBelow;
  int     y[PROMPT_MAX_OPTS], h[PROMPT_MAX_OPTS];
  uint8_t labelRows[PROMPT_MAX_OPTS], descRows[PROMPT_MAX_OPTS];
};
static QLayout qL;   // naposledy spočítané rozložení, podle něj se vyhodnocuje dotyk

// Zalomí text do nejvýš maxRows řádků; když se nevejde, poslední řádek končí „…“
// Pomocný buffer je statický (zásobník hlavní smyčky je malý), funkce se volá jen z ní.
static uint8_t wrapClamp(const char* text, char out[][96], uint8_t maxRows, uint8_t cols) {
  static char tmp[17][96];
  if (maxRows > 16) maxRows = 16;
  uint8_t n = wrapUtf8(text, tmp, maxRows + 1, cols);
  bool cut = n > maxRows;
  if (cut) n = maxRows;
  for (uint8_t r = 0; r < n; r++) memcpy(out[r], tmp[r], 96);
  if (cut && n > 0) {
    char* row = out[n - 1];
    size_t len = strlen(row);
    while (len > 0 && (hwUtf8Len(row) > (size_t)(cols - 1) || len > 92)) {
      do { len--; } while (len > 0 && (((unsigned char)row[len]) & 0xC0) == 0x80);
      row[len] = 0;
    }
    strcat(row, "…");
  }
  return n;
}

static uint8_t qOptionCount() {
  return tama.promptOptCount > PROMPT_MAX_OPTS ? PROMPT_MAX_OPTS : tama.promptOptCount;
}

static uint8_t qButtons(uint8_t out[3]) {
  uint8_t n = 0;
  if (tama.promptBack) out[n++] = QB_BACK;
  if (tama.promptTerm) out[n++] = QB_TERM;
  if (tama.promptMulti || qOptionCount() == 0) out[n++] = QB_OK;
  return n;
}

static void qBtnRect(uint8_t i, uint8_t n, int& x, int& y, int& w, int& h) {
  const int gap = 8;
  w = (432 - gap * (n - 1)) / n;
  x = 24 + i * (w + gap);
  y = Q_BAR_Y;
  h = Q_BAR_H;
}

// Nejmenší výška řádku volby = popisek a jeden řádek pod ním (místo pro
// „klepni znovu = odeslat“ u vybrané volby)
static int qRowHeight(uint8_t labelRows, uint8_t descRows) {
  int h = 14 + (labelRows + descRows) * Q_ROW_H;
  return h < 54 ? 54 : h;
}

// Volba vybraná klepnutím: místo popisu ukazuje „klepni znovu = odeslat“
static bool qArmed(uint8_t i) {
  return !tama.promptMulti && promptSel == i && promptSelTapped;
}

static void qMeasure(uint8_t i, uint8_t descMax, uint8_t& lr, uint8_t& dr) {
  char rows[4][96];
  lr = wrapClamp(promptOptionLabel(i), rows, 2, Q_TEXT_COLS);
  if (lr == 0) lr = 1;
  dr = tama.promptDescs[i][0] ? wrapClamp(tama.promptDescs[i], rows, descMax, Q_TEXT_COLS) : 0;
  if (qArmed(i) && dr == 0) dr = 1;
}

// Rozložení obrazovky otázky. cursorOpt >= 0: seznam se posune, aby byla
// volba vidět (výběr přes BOOT).
static void qComputeLayout(QLayout& L, int cursorOpt) {
  L.n = qOptionCount();
  L.nBtn = qButtons(L.btn);
  L.optBottom = L.nBtn ? Q_BAR_Y - 8 : LCD_HEIGHT - 12;
  L.first = 1;
  L.last = 0;
  L.moreAbove = L.moreBelow = false;
  L.descMax = 0;

  // Karta: záhlaví a text otázky (bez voleb, např. shrnutí odpovědí, až po tlačítka)
  static char rows[16][96];
  uint8_t hintMax = L.n ? 5 : (uint8_t)((L.optBottom - Q_CARD_Y - 46) / Q_ROW_H);
  L.hintRows = wrapClamp(tama.promptHint, rows, hintMax, Q_HINT_COLS);
  L.cardH = L.n ? 44 + L.hintRows * Q_ROW_H : L.optBottom - Q_CARD_Y;
  L.optTop = Q_CARD_Y + L.cardH + 8;
  if (!L.n) return;

  // Vejde se všechno s popisem na 3, nebo na 2 řádky? Jinak 2 řádky a posouvání.
  int area = L.optBottom - L.optTop;
  for (uint8_t dm = 3; dm >= 2; dm--) {
    int total = -Q_GAP;
    for (uint8_t i = 0; i < L.n; i++) {
      qMeasure(i, dm, L.labelRows[i], L.descRows[i]);
      total += qRowHeight(L.labelRows[i], L.descRows[i]) + Q_GAP;
    }
    L.descMax = dm;
    if (total <= area) {
      qFirst = 0;
      int y = L.optTop;
      for (uint8_t i = 0; i < L.n; i++) {
        L.y[i] = y;
        L.h[i] = qRowHeight(L.labelRows[i], L.descRows[i]);
        y += L.h[i] + Q_GAP;
      }
      L.first = 0;
      L.last = L.n - 1;
      return;
    }
  }

  // Posouvání: seznam začíná volbou qFirst. Nad ní je vidět konec předchozí
  // volby (Q_PEEK_H), pod poslední celou volbou začátek další, oříznutý na
  // spodním okraji. Obojí naznačuje, že seznam pokračuje, a nestojí celý řádek.
  if (qFirst >= L.n) qFirst = L.n - 1;
  if (cursorOpt >= 0 && cursorOpt < qFirst) qFirst = (uint8_t)cursorOpt;
  for (;;) {
    int y = L.optTop + (qFirst > 0 ? Q_PEEK_H + Q_GAP : 0);
    uint8_t last = qFirst;
    for (uint8_t i = qFirst; i < L.n; i++) {
      int h = qRowHeight(L.labelRows[i], L.descRows[i]);
      if (i > qFirst && y + h > L.optBottom) break;
      L.y[i] = y;
      L.h[i] = h;
      last = i;
      y += h + Q_GAP;
    }
    if (cursorOpt > (int)last && qFirst < cursorOpt) { qFirst++; continue; }
    L.first = qFirst;
    L.last = last;
    L.moreAbove = qFirst > 0;
    L.moreBelow = last < L.n - 1;
    if (L.moreAbove) {
      uint8_t a = qFirst - 1;
      L.h[a] = qRowHeight(L.labelRows[a], L.descRows[a]);
      L.y[a] = L.optTop + Q_PEEK_H - L.h[a];
    }
    if (L.moreBelow) {
      uint8_t b = last + 1;
      L.h[b] = qRowHeight(L.labelRows[b], L.descRows[b]);
      L.y[b] = y;
    }
    return;
  }
}

// Posun seznamu o volbu. Výběr pro IO18 zůstane na viditelné volbě, aby
// nešlo odeslat volbu, která po posunutí není vidět.
static void qScroll(int delta) {
  if (delta < 0 && qL.moreAbove) qFirst--;
  else if (delta > 0 && qL.moreBelow) qFirst++;
  else return;
  click(1900);
  qComputeLayout(qL, -1);
  if (promptSel < qL.n && (promptSel < qL.first || promptSel > qL.last)) {
    promptSel = promptSel < qL.first ? qL.first : qL.last;
    promptSelTapped = false;
  }
}

// Jeden řádek nejvýš cols znaků, delší text končí „…“ (out aspoň 96 B)
static void truncEll(const char* text, char* out, size_t outSize, uint8_t cols) {
  if (!text) text = "";
  if (cols < 2) cols = 2;
  const char* p = text;
  uint8_t c = 0;
  while (c < cols && hwNextUtf8(p) != 0) c++;
  if (hwNextUtf8(p) == 0) {   // celý text se vejde
    _copyUtf8(out, text, outSize);
    return;
  }
  p = text;
  for (uint8_t k = 0; k + 1 < cols && hwNextUtf8(p) != 0; k++) {}
  size_t n = (size_t)(p - text);
  if (n + 4 > outSize) n = outSize - 4;
  while (n > 0 && (((unsigned char)text[n]) & 0xC0) == 0x80) n--;
  memcpy(out, text, n);
  memcpy(out + n, "…", 3);
  out[n + 3] = 0;
}

// Jedna volba seznamu. Krajní ořezané volby (nad a pod seznamem) se kreslí celé
// a přesah se po nakreslení všech voleb přemaže pozadím.
static void qDrawOption(Arduino_Canvas* cv, const Palette& p, uint8_t i, bool guard, bool touchOk) {
  const QLayout& L = qL;
  const TouchEvent& te = hwTouch();
  const int bx = 24, bw = 432;
  int by = L.y[i], bh = L.h[i];
  bool inList  = i >= L.first && i <= L.last;
  bool cursor  = inList && promptSel == i;
  bool pressed = inList && touchOk && hitRect(te.curX, te.curY, bx, by, bw, bh);
  bool checked = tama.promptMulti && (qChecked & (1u << i));
  bool armed   = inList && qArmed(i);
  uint16_t accent = guard ? COL_BORDER : Q_ACCENTS[i % 4];
  uint16_t bg = pressed ? COL_CARD_HI : (armed ? COL_GREEN_BG : (cursor ? 0x18E4 : COL_CARD));
  uint16_t border = (cursor || armed) ? accent : (checked ? COL_GREEN : 0x2965);

  cv->fillRoundRect(bx, by, bw, bh, 10, bg);
  cv->drawRoundRect(bx, by, bw, bh, 10, border);
  if (cursor || armed || pressed) cv->drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 9, border);

  char lrows[4][96], drows[4][96];
  uint8_t lr = wrapClamp(promptOptionLabel(i), lrows, 2, Q_TEXT_COLS);
  uint8_t dr = tama.promptDescs[i][0] ? wrapClamp(tama.promptDescs[i], drows, L.descMax, Q_TEXT_COLS) : 0;
  if (armed) dr = 1;   // místo popisu výzva k potvrzení
  int contentH = (lr + dr) * Q_ROW_H - 4;
  int ty = by + (bh - contentH) / 2;

  if (tama.promptMulti) {
    // Zaškrtávací políčko
    uint16_t boxCol = checked ? COL_GREEN : accent;
    cv->drawRect(bx + 12, ty - 2, 22, 20, boxCol);
    cv->drawRect(bx + 13, ty - 1, 20, 18, boxCol);
    if (checked) {
      cv->fillRect(bx + 15, ty + 1, 16, 14, COL_GREEN);
      hwDrawUtf8(cv, "✓", bx + 17, ty, 2, COL_BG, COL_GREEN);
    }
  } else {
    char num[6];
    snprintf(num, sizeof(num), "%u.", i + 1);
    hwDrawUtf8(cv, num, bx + 12, ty, 2, accent, bg);
  }
  for (uint8_t r = 0; r < lr; r++) {
    hwDrawUtf8(cv, lrows[r], bx + 44, ty + r * Q_ROW_H, 2, cursor || armed ? 0xFFFF : p.text, bg);
  }
  if (armed) {
    hwDrawUtf8(cv, "klepni znovu = odeslat", bx + 44, ty + lr * Q_ROW_H, 2, COL_GREEN, bg);
  } else {
    for (uint8_t r = 0; r < dr; r++) {
      hwDrawUtf8(cv, drows[r], bx + 44, ty + (lr + r) * Q_ROW_H, 2, p.textDim, bg);
    }
  }
}

static void drawQuestionScreen() {
  Arduino_Canvas* cv = hwCanvas();
  const Palette& p = characterPalette();
  qComputeLayout(qL, (qFollowCursor && promptSel < qOptionCount()) ? promptSel : -1);
  qFollowCursor = false;
  const QLayout& L = qL;

  cv->fillRect(0, 50, LCD_WIDTH, LCD_HEIGHT - 50, p.bg);

  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  int32_t remaining = promptRemainingS();
  bool hot = (remaining >= 0) ? (remaining <= 15) : (waited >= 10);
  bool urgent = hot && ((millis() / 400) % 2 == 0);
  uint16_t borderCol = urgent ? COL_HOT : COL_YELLOW;

  const TouchEvent& te = hwTouch();
  bool guard = promptGuardActive();
  bool touchOk = te.touching && !guard && (int32_t)(touchDownMs - promptShownMs) >= (int32_t)PROMPT_GUARD_MS;

  // Volby včetně ořezaných krajních; karta a tlačítka se kreslí až přes ně
  if (!responseSent && L.n) {
    if (L.moreAbove) qDrawOption(cv, p, L.first - 1, guard, false);
    for (uint8_t i = L.first; i <= L.last && i < L.n; i++) qDrawOption(cv, p, i, guard, touchOk);
    if (L.moreBelow) qDrawOption(cv, p, L.last + 1, guard, false);
    cv->fillRect(0, 50, LCD_WIDTH, L.optTop - 50, p.bg);
    cv->fillRect(0, L.optBottom, LCD_WIDTH, LCD_HEIGHT - L.optBottom, p.bg);
    if (L.moreAbove || L.moreBelow) {
      // Posuvník vpravo od voleb: kde v seznamu jsme a kolik ho je vidět
      int trackH = L.optBottom - L.optTop;
      int thumbH = trackH * (L.last - L.first + 1) / L.n;
      if (thumbH < 20) thumbH = 20;
      int thumbY = L.optTop + trackH * L.first / L.n;
      if (thumbY + thumbH > L.optBottom) thumbY = L.optBottom - thumbH;
      cv->fillRoundRect(463, L.optTop, 5, trackH, 2, 0x2965);
      cv->fillRoundRect(463, thumbY, 5, thumbH, 2, p.textDim);
    }
  }

  // Karta: záhlaví, zbývající čas a text otázky
  cv->fillRoundRect(20, Q_CARD_Y, 440, L.cardH, 14, COL_AMBER_BG);
  cv->drawRoundRect(20, Q_CARD_Y, 440, L.cardH, 14, borderCol);
  cv->drawRoundRect(21, Q_CARD_Y + 1, 438, L.cardH - 2, 13, borderCol);
  char waitBuf[20];
  if (remaining >= 0) snprintf(waitBuf, sizeof(waitBuf), "zbývá %lds", (long)remaining);
  else                snprintf(waitBuf, sizeof(waitBuf), "%lus", (unsigned long)waited);
  hwDrawTextRight(cv, waitBuf, 444, Q_CARD_Y + 12, 2, hot ? COL_HOT : p.textDim, COL_AMBER_BG);
  {
    // Záhlaví končí aspoň znak před odpočtem, delší končí „…“
    const char* head = tama.promptHead[0] ? tama.promptHead
                     : (isQuestionTool(tama.promptTool) ? "OTÁZKA" : friendlyToolName(tama.promptTool));
    int headCols = (444 - (int)hwUtf8Len(waitBuf) * 12 - 12 - 36) / 12;
    char headBuf[96];
    truncEll(head, headBuf, sizeof(headBuf), (uint8_t)(headCols < 4 ? 4 : headCols));
    hwDrawUtf8(cv, headBuf, 36, Q_CARD_Y + 12, 2, hot ? COL_HOT : COL_YELLOW, COL_AMBER_BG);
  }
  {
    static char rows[16][96];
    uint8_t nRows = wrapClamp(tama.promptHint, rows, L.hintRows, Q_HINT_COLS);
    for (uint8_t r = 0; r < nRows; r++) {
      hwDrawUtf8(cv, rows[r], 36, Q_CARD_Y + 38 + r * Q_ROW_H, 2, 0xDEFB, COL_AMBER_BG);
    }
  }

  if (responseSent) {
    int top = L.n ? L.optTop : Q_BAR_Y;
    int h = LCD_HEIGHT - 12 - top;
    cv->fillRoundRect(24, top, 432, h, 14, COL_CARD_HI);
    hwDrawTextCentered(cv, "Odesláno…", 240, top + h / 2 - 10, 3, p.textDim, COL_CARD_HI);
    return;
  }

  for (uint8_t b = 0; b < L.nBtn; b++) {
    int x, y, w, h;
    qBtnRect(b, L.nBtn, x, y, w, h);
    uint8_t kind = L.btn[b];
    bool cursor   = (promptSel == L.n + b);
    bool pressed  = touchOk && hitRect(te.curX, te.curY, x, y, w, h);
    bool disabled = guard || (kind == QB_OK && tama.promptMulti && qChecked == 0);
    const char* lbl;
    uint16_t fg, bg;
    if (kind == QB_BACK)      { lbl = "← ZPĚT";   fg = COL_CYAN;   bg = pressed ? COL_CARD_HI : 0x0210; }
    else if (kind == QB_TERM) { lbl = "TERMINÁL"; fg = p.text;     bg = pressed ? COL_CARD_HI : COL_CARD; }
    else {
      lbl = tama.promptOk[0] ? tama.promptOk : "POTVRDIT";
      fg = COL_GREEN;
      bg = pressed ? COL_GREEN_HI : COL_GREEN_BG;
    }
    if (disabled) { fg = COL_BORDER; bg = COL_CARD; }
    cv->fillRoundRect(x, y, w, h, 12, bg);
    cv->drawRoundRect(x, y, w, h, 12, fg);
    if (cursor) cv->drawRoundRect(x + 1, y + 1, w - 2, h - 2, 11, fg);
    hwDrawTextCentered(cv, lbl, x + w / 2, y + (h - 16) / 2, 2, fg, bg);
  }
}

// Výchozí výběr: první předvybraná volba (návrat ZPĚT, u povolení „Ne“),
// u obrazovky bez voleb (shrnutí odpovědí) tlačítko POTVRDIT / ODESLAT
static void qInitCursor() {
  uint8_t n = qOptionCount();
  promptSel = 0;
  if (n == 0) {
    uint8_t btn[3];
    uint8_t nb = qButtons(btn);
    for (uint8_t b = 0; b < nb; b++) {
      if (btn[b] == QB_OK) promptSel = b;
    }
    return;
  }
  for (uint8_t i = 0; i < n; i++) {
    if (tama.promptSelInit & (1u << i)) { promptSel = i; break; }
  }
  qFollowCursor = true;
}

// Nový dotaz na obrazovce otázky: předvybrané volby (návrat ZPĚT) a seznam od začátku
static void qResetForPrompt() {
  qChecked = tama.promptMulti ? tama.promptSelInit : 0;
  qFirst = 0;
  qInitCursor();
  qFollowCursor = true;
  qComputeLayout(qL, promptSel < qOptionCount() ? promptSel : -1);
}

static uint8_t qItemCount() {
  return qL.n + qL.nBtn;
}

// Jedna volba odeslána. U povolení s volbou „Ano, už se neptat“ je poslední volba „Ne“.
static void qAnswerSingle(uint8_t i) {
  if (isQuestionTool(tama.promptTool)) { answerQuestion(i); return; }
  if (!sendDecision(promptOptionLabel(i), i)) return;
  markResponseSent();
  uint32_t tookS = (millis() - promptShownMs) / 1000;
  bool deny = (i + 1 == qL.n);
  recordStat(deny ? PS_DENY : PS_APPROVE, tookS);
  if (deny) PLAY(SND_DENY, CAT_IMPORTANT);
  else      PLAY(SND_APPROVE, CAT_IMPORTANT);
  if (!deny && tookS < 8) triggerOneShot(P_HEART, 2500);
}

// Použití volby nebo tlačítka (item = pořadí: volby, za nimi spodní tlačítka).
// touch: u jedné volby první klepnutí jen vybere, IO18 odešle hned.
static void qActivate(uint8_t item, bool touch) {
  if (!tama.promptId[0] || responseSent) return;
  if (item < qL.n) {
    if (tama.promptMulti) {
      qChecked ^= (uint8_t)(1u << item);
      promptSel = item;
      click((qChecked & (1u << item)) ? 2300 : 1600);
      return;
    }
    if (touch && !(promptSel == item && promptSelTapped)) {
      promptSel = item;
      promptSelTapped = true;
      // Výzva „klepni znovu“ volbu zvětší: seznam ji musí nechat celou na displeji
      qFollowCursor = true;
      click(2100);
      return;
    }
    qAnswerSingle(item);
    return;
  }
  uint8_t b = item - qL.n;
  if (b >= qL.nBtn) return;
  uint8_t kind = qL.btn[b];
  if (kind == QB_BACK || kind == QB_TERM) {
    if (!sendDecision(kind == QB_BACK ? "back" : "terminal")) return;
    markResponseSent();
    click(1900);
    return;
  }
  // POTVRDIT: zaškrtnuté volby, nebo potvrzení obrazovky bez voleb (shrnutí odpovědí)
  if (tama.promptMulti) {
    if (!qChecked) { click(900); return; }
    if (!sendDecision("multi", -1, qChecked)) return;
    markResponseSent();
    uint32_t tookS = (millis() - promptShownMs) / 1000;
    recordStat(PS_ANSWER, tookS);
  } else {
    if (!sendDecision("ok")) return;
    markResponseSent();
  }
  PLAY(SND_ANSWER, CAT_IMPORTANT);
}

static void qHandleTap(int tx, int ty) {
  for (uint8_t b = 0; b < qL.nBtn; b++) {
    int x, y, w, h;
    qBtnRect(b, qL.nBtn, x, y, w, h);
    if (hitRect(tx, ty, x, y - 4, w, h + 8)) { qActivate(qL.n + b, true); return; }
  }
  // Ořezaná volba nahoře nebo dole seznam posune
  if (qL.moreAbove && ty >= qL.optTop && ty < qL.optTop + Q_PEEK_H + Q_GAP) { qScroll(-1); return; }
  if (qL.moreBelow && ty >= qL.y[qL.last + 1] && ty < qL.optBottom + 4) { qScroll(1); return; }
  for (uint8_t i = qL.first; i <= qL.last && i < qL.n; i++) {
    if (hitRect(tx, ty, 24, qL.y[i], 432, qL.h[i])) { qActivate(i, true); return; }
  }
}

// Popisek velkého tlačítka (206 × 74 px) a pod ním drobná nápověda: do 8 znaků
// velikostí 3, jinak velikostí 2 na 1–3 řádky po 16 znacích (delší končí „…“).
// Na tři řádky se nápověda už nevejde.
static void drawOptLabel(Arduino_Canvas* cv, const char* lbl, const char* sub, int cx, int y,
                         uint16_t fg, uint16_t bg, uint16_t subFg) {
  if (hwUtf8Len(lbl) <= 8) {
    hwDrawTextCentered(cv, lbl, cx, y + 20, 3, fg, bg);
    hwDrawTextCentered(cv, sub, cx, y + 54, 1, subFg, bg);
    return;
  }
  char rows[3][96];
  uint8_t n = wrapClamp(lbl, rows, 3, 16);
  if (n <= 1) {
    hwDrawTextCentered(cv, n ? rows[0] : "", cx, y + 25, 2, fg, bg);
  } else if (n == 2) {
    hwDrawTextCentered(cv, rows[0], cx, y + 12, 2, fg, bg);
    hwDrawTextCentered(cv, rows[1], cx, y + 32, 2, fg, bg);
  } else {
    for (uint8_t r = 0; r < 3; r++) hwDrawTextCentered(cv, rows[r], cx, y + 6 + r * 20, 2, fg, bg);
    return;
  }
  hwDrawTextCentered(cv, sub, cx, y + 54, 1, subFg, bg);
}

// Approval prompt card + big touch buttons
static const int BTN_APPR_X = 24,  BTN_APPR_Y = 384, BTN_APPR_W = 206, BTN_APPR_H = 74;
static const int BTN_DENY_X = 250, BTN_DENY_Y = 384, BTN_DENY_W = 206, BTN_DENY_H = 74;

static void drawApproval() {
  if (tama.promptQ) {
    drawQuestionScreen();
    return;
  }
  Arduino_Canvas* cv = hwCanvas();
  const Palette& p = characterPalette();

  uint8_t totalOpts = promptOptionCount();
  bool isMulti = (totalOpts > 2);
  bool tall = promptHidesBuddy();   // 5+ voleb: bez maskota, karta pod horní lištou

  const int TOP = tall ? 50 : 216;
  cv->fillRect(0, TOP, LCD_WIDTH, LCD_HEIGHT - TOP, p.bg);

  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  int32_t remaining = promptRemainingS();
  // Naléhavě bliká od 10 s čekání, nebo když zbývá méně než 15 s
  bool hot = (remaining >= 0) ? (remaining <= 15) : (waited >= 10);
  bool urgent = hot && ((millis() / 400) % 2 == 0);
  uint16_t borderCol = urgent ? COL_HOT : COL_YELLOW;

  // U 4 voleb končí karta těsně nad prvním řádkem voleb (310), aby se do ní
  // vešly tři řádky otázky; u 5+ voleb končí nad prvním řádkem na 162
  int cardY = tall ? 58 : 216;
  int cardH = isMulti ? (totalOpts == 3 ? 96 : 92) : 156;

  // Prompt details card
  cv->fillRoundRect(20, cardY, 440, cardH, 14, COL_AMBER_BG);
  cv->drawRoundRect(20, cardY, 440, cardH, 14, borderCol);
  cv->drawRoundRect(21, cardY + 1, 438, cardH - 2, 13, borderCol);

  bool isQuestion = isQuestionTool(tama.promptTool) || promptIsQuestion();
  // Volba vybraná klepnutím: výzva v záhlaví karty, řádek volby zůstane celý
  bool armedRow = isMulti && promptSelTapped && !responseSent;
  const char* promptHeader = armedRow ? "KLEPNI ZNOVU = ODESLAT"
                           : (isQuestion ? "DOTAZ / ROZHODNUTÍ" : "ROZHODNOUT AKCI?");
  hwDrawUtf8(cv, promptHeader, 36, cardY + 12, 2, armedRow ? COL_GREEN : (hot ? COL_HOT : COL_YELLOW), COL_AMBER_BG);

  // AGY: kolik zbývá do vrácení dotazu terminálu; Claude: jak dlouho čeká
  char waitBuf[20];
  if (remaining >= 0) snprintf(waitBuf, sizeof(waitBuf), "zbývá %lds", (long)remaining);
  else                snprintf(waitBuf, sizeof(waitBuf), "%lus", (unsigned long)waited);
  hwDrawTextRight(cv, waitBuf, 444, cardY + 12, 2, hot ? COL_HOT : p.textDim, COL_AMBER_BG);

  if (isMulti) {
    // Pro 3 a více voleb: otázka uvnitř kompaktní karty (až 3 řádky)
    char rows[3][96];
    uint8_t nRows = wrapClamp(tama.promptHint, rows, 3, 34);
    for (uint8_t r = 0; r < nRows; r++) {
      hwDrawUtf8(cv, rows[r], 36, cardY + 34 + r * Q_ROW_H, 2, 0xDEFB, COL_AMBER_BG);
    }
  } else {
    // Friendly Czech action / tool name
    const char* toolLabel = friendlyToolName(tama.promptTool);
    size_t toolChars = hwUtf8Len(toolLabel);
    uint8_t tSize = (toolChars <= 18) ? 3 : 2;
    char toolBuf[96];
    truncEll(toolLabel, toolBuf, sizeof(toolBuf), 34);
    hwDrawUtf8(cv, toolBuf, 36, 258, tSize, p.text, COL_AMBER_BG);

    // Příkaz nebo otázka: nejvýš 4 řádky po 34 znacích, delší text končí „…“
    char rows[4][96];
    uint8_t nRows = wrapClamp(tama.promptHint, rows, 4, 34);
    for (uint8_t r = 0; r < nRows; r++) {
      hwDrawUtf8(cv, rows[r], 36, 292 + r * 20, 2, 0xDEFB, COL_AMBER_BG);
    }
  }

  if (responseSent) {
    int by = BTN_APPR_Y, bh = BTN_APPR_H;
    if (isMulti) {
      int bx, bw;
      getOptionBtnRect(0, totalOpts, bx, by, bw, bh);
      bh = LCD_HEIGHT - 12 - by;
    }
    cv->fillRoundRect(24, by, 432, bh, 14, COL_CARD_HI);
    hwDrawTextCentered(cv, "Odesláno…", 240, by + bh / 2 - 10, 3, p.textDim, COL_CARD_HI);
    return;
  }

  const TouchEvent& te = hwTouch();
  // Během ochranné doby jsou tlačítka zašedlá a nereagují
  bool guard = promptGuardActive();
  bool touchOk = te.touching && !guard && (int32_t)(touchDownMs - promptShownMs) >= (int32_t)PROMPT_GUARD_MS;

  if (!isMulti) {
    bool danger = promptIsDanger();
    // Big [ SCHVÁLIT / optA ] touch button
    bool pressAppr = touchOk && hitRect(te.curX, te.curY, BTN_APPR_X, BTN_APPR_Y, BTN_APPR_W, BTN_APPR_H);
    bool pressDeny = touchOk && hitRect(te.curX, te.curY, BTN_DENY_X, BTN_DENY_Y, BTN_DENY_W, BTN_DENY_H);

    // U kritické akce se tlačítko při dotyku nezvýrazní: barva stisku je
    // zároveň barva průběhu podržení, ten by pod prstem nebyl vidět
    uint16_t apprBg = guard ? COL_CARD : ((pressAppr && !danger) ? COL_GREEN_HI : COL_GREEN_BG);
    uint16_t denyBg = guard ? COL_CARD : (pressDeny ? COL_RED_HI   : COL_RED_BG);
    uint16_t apprFg = guard ? COL_BORDER : COL_GREEN;
    uint16_t denyFg = guard ? COL_BORDER : COL_HOT;

    cv->fillRoundRect(BTN_APPR_X, BTN_APPR_Y, BTN_APPR_W, BTN_APPR_H, 14, apprBg);
    // Průběh podržení u kritické akce (dotykem nebo tlačítkem BOOT)
    uint32_t heldMs = 0;
    if (danger && holdStartMs) heldMs = millis() - holdStartMs;
    if (danger && hwBtnBoot().isPressed() && !guard && !swallowBtnA) {
      uint32_t b = millis() - hwBtnBoot().pressStartMs;
      if (b > heldMs) heldMs = b;
    }
    if (heldMs > 0) {
      int fillW = (int)((uint32_t)BTN_APPR_W * (heldMs > DANGER_HOLD_MS ? DANGER_HOLD_MS : heldMs) / DANGER_HOLD_MS);
      cv->fillRoundRect(BTN_APPR_X, BTN_APPR_Y, fillW, BTN_APPR_H, 14, COL_GREEN_HI);
    }
    cv->drawRoundRect(BTN_APPR_X, BTN_APPR_Y, BTN_APPR_W, BTN_APPR_H, 14, apprFg);
    cv->drawRoundRect(BTN_APPR_X + 1, BTN_APPR_Y + 1, BTN_APPR_W - 2, BTN_APPR_H - 2, 13, apprFg);
    const char* btnApprLbl = tama.promptOptA[0] ? tama.promptOptA : "SCHVÁLIT";
    if (danger) {
      // Během podržení text bez pozadí (barva pozadí = barva textu), průběh je vidět i pod ním
      hwDrawTextCentered(cv, btnApprLbl, BTN_APPR_X + BTN_APPR_W / 2, BTN_APPR_Y + 12, 3, apprFg, heldMs > 0 ? apprFg : apprBg);
      hwDrawTextCentered(cv, "podržet 1 s", BTN_APPR_X + BTN_APPR_W / 2, BTN_APPR_Y + 46, 2, p.textDim, heldMs > 0 ? p.textDim : apprBg);
    } else {
      drawOptLabel(cv, btnApprLbl, "[dotyk / BOOT]", BTN_APPR_X + BTN_APPR_W / 2, BTN_APPR_Y, apprFg, apprBg, p.textDim);
    }

    // Big [ ZAMÍTNOUT / optB ] touch button
    cv->fillRoundRect(BTN_DENY_X, BTN_DENY_Y, BTN_DENY_W, BTN_DENY_H, 14, denyBg);
    cv->drawRoundRect(BTN_DENY_X, BTN_DENY_Y, BTN_DENY_W, BTN_DENY_H, 14, denyFg);
    cv->drawRoundRect(BTN_DENY_X + 1, BTN_DENY_Y + 1, BTN_DENY_W - 2, BTN_DENY_H - 2, 13, denyFg);
    const char* btnDenyLbl = tama.promptOptB[0] ? tama.promptOptB : "ZAMÍTNOUT";
    drawOptLabel(cv, btnDenyLbl, "[dotyk / IO18]", BTN_DENY_X + BTN_DENY_W / 2, BTN_DENY_Y, denyFg, denyBg, p.textDim);
  } else {
    // 3 a více voleb pod sebou: dotyková tlačítka přes celou šířku
    static const uint16_t ACCENT_COLS[4] = { COL_GREEN, COL_CYAN, COL_YELLOW, 0xFBE0 };
    for (uint8_t i = 0; i < totalOpts; i++) {
      int bx, by, bw, bh;
      getOptionBtnRect(i, totalOpts, bx, by, bw, bh);
      bool isPressed = touchOk && hitRect(te.curX, te.curY, bx, by, bw, bh);
      bool isSel = (promptSel == i);
      uint16_t accent = guard ? COL_BORDER : ACCENT_COLS[i % 4];

      bool armed = isSel && promptSelTapped;
      uint16_t btnBg = isPressed ? COL_CARD_HI : (armed ? COL_GREEN_BG : (isSel ? 0x18E4 : COL_CARD));
      uint16_t btnBorder = isSel ? accent : (isPressed ? COL_BORDER : 0x2965);

      cv->fillRoundRect(bx, by, bw, bh, 10, btnBg);
      cv->drawRoundRect(bx, by, bw, bh, 10, btnBorder);
      if (isSel || isPressed) {
        cv->drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 9, btnBorder);
      }

      const char* lbl = promptOptionLabel(i);
      int textY = by + (bh - 16) / 2;
      char numBadge[8];
      snprintf(numBadge, sizeof(numBadge), "%u.", i + 1);
      hwDrawUtf8(cv, numBadge, bx + 12, textY, 2, accent, btnBg);
      // Popisek do značky výběru (30 znaků), delší končí „…“. Výzvu „klepni
      // znovu“ u vybrané volby ukazuje záhlaví karty.
      char lblBuf[96];
      truncEll(lbl, lblBuf, sizeof(lblBuf), (uint8_t)((bw - 58) / 12 - 1));
      hwDrawUtf8(cv, lblBuf, bx + 38, textY, 2, isSel ? 0xFFFF : p.text, btnBg);
      if (isSel) hwDrawUtf8(cv, "<", bx + bw - 18, textY, 2, accent, btnBg);
    }
  }
}

static void drawHUD() {
  if (tama.promptId[0]) {
    drawApproval();
    return;
  }

  Arduino_Canvas* cv = hwCanvas();
  const Palette& p = characterPalette();
  const int CARD_X = chatExpanded ? 16 : 24;
  const int CARD_W = chatExpanded ? 448 : 432;
  const int CARD_Y = chatExpanded ? 56 : 278;
  const int CARD_H = chatExpanded ? 360 : 136;
  const uint8_t MAX_COLS = chatExpanded ? 35 : 33;
  const uint8_t SHOW     = chatExpanded ? 13 : 5;

  cv->fillRect(0, CARD_Y - 2, LCD_WIDTH, NAV_Y - CARD_Y + 2, p.bg);
  cv->fillRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 12, COL_CARD);
  cv->drawRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 12, chatExpanded ? p.body : COL_BORDER);

  int textY0 = CARD_Y + 24;
  if (chatExpanded) {
    hwDrawUtf8(cv, "CHAT / HISTORIE ZPRÁV", CARD_X + 18, CARD_Y + 14, 2, p.body, COL_CARD);
    hwDrawTextRight(cv, "[Zavřít]", CARD_X + CARD_W - 16, CARD_Y + 14, 2, p.textDim, COL_CARD);
    cv->drawFastHLine(CARD_X + 14, CARD_Y + 38, CARD_W - 28, COL_BORDER);
    textY0 = CARD_Y + 50;
  } else {
    char zoomLbl[32];
    snprintf(zoomLbl, sizeof(zoomLbl), "%s [zvětšit]", buddySpeciesName());
    hwDrawTextRight(cv, zoomLbl, CARD_X + CARD_W - 12, CARD_Y + 10, 1, p.body, COL_CARD);
  }

  if (tama.nLines == 0) {
    msgMaxScroll = 0;
    char rows[6][96];
    uint8_t n = wrapUtf8(tama.msg, rows, chatExpanded ? 6 : 3, MAX_COLS);
    for (uint8_t i = 0; i < n; i++) {
      hwDrawUtf8(cv, rows[i], CARD_X + 16, textY0 + 4 + i * 24, 2, p.textDim, COL_CARD);
    }
    drawBottomNav();
    return;
  }

  static char disp[48][96];
  static uint8_t srcOf[48];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 48; i++) {
    uint8_t got = wrapUtf8(tama.lines[i], &disp[nDisp], 48 - nDisp, MAX_COLS);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxScroll = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  msgMaxScroll = maxScroll;
  if (msgScroll > maxScroll) msgScroll = maxScroll;

  for (uint8_t i = 0; i < SHOW && (msgScroll + i) < nDisp; i++) {
    uint8_t row = msgScroll + i;
    bool newest = (srcOf[row] == 0);
    hwDrawUtf8(cv, disp[row], CARD_X + 14, textY0 + i * 22, 2, newest ? p.text : p.textDim, COL_CARD);
  }

  if (msgScroll > 0 || nDisp > SHOW) {
    char sc[20];
    snprintf(sc, sizeof(sc), "%u/%u", (unsigned)(msgScroll + 1), (unsigned)(maxScroll + 1));
    if (chatExpanded) hwDrawTextRight(cv, sc, CARD_X + CARD_W - 12, CARD_Y + CARD_H - 14, 1, p.body, COL_CARD);
    else              hwDrawUtf8(cv, sc, CARD_X + 14, CARD_Y + 10, 1, p.body, COL_CARD);
  }

  drawBottomNav();
}

static void drawHeartIcon(Arduino_Canvas* cv, int x, int y, bool filled, uint16_t col) {
  if (filled) {
    cv->fillCircle(x - 5, y, 5, col);
    cv->fillCircle(x + 5, y, 5, col);
    cv->fillTriangle(x - 10, y + 2, x + 10, y + 2, x, y + 13, col);
  } else {
    cv->drawCircle(x - 5, y, 5, col);
    cv->drawCircle(x + 5, y, 5, col);
    cv->drawLine(x - 10, y + 2, x, y + 13, col);
    cv->drawLine(x + 10, y + 2, x, y + 13, col);
  }
}

static void drawPetScreen() {
  Arduino_Canvas* cv = hwCanvas();
  const Palette& p = characterPalette();
  const int TOP = 158;
  cv->fillRect(0, TOP, LCD_WIDTH, NAV_Y - TOP, p.bg);
  cv->fillRoundRect(24, TOP, 432, 256, 14, COL_CARD);
  cv->drawRoundRect(24, TOP, 432, 256, 14, COL_BORDER);

  // Header + page switcher
  char title[64];
  if (ownerName()[0]) snprintf(title, sizeof(title), "%s: %s", ownerName(), petName());
  else snprintf(title, sizeof(title), "%s (%s)", petName(), buddySpeciesName());
  hwDrawUtf8(cv, title, 40, TOP + 16, 2, p.body, COL_CARD, 22);

  char pg[16];
  snprintf(pg, sizeof(pg), "< %u/%u >", petPage + 1, PET_PAGES);
  hwDrawTextRight(cv, pg, 440, TOP + 16, 2, p.textDim, COL_CARD);
  cv->drawFastHLine(36, TOP + 38, 408, COL_BORDER);

  if (petPage == 0) {
    int y = TOP + 52;
    hwDrawUtf8(cv, "Nálada", 40, y, 2, p.textDim, COL_CARD);
    uint8_t mood = statsMoodTier();
    uint16_t moodCol = (mood >= 3) ? COL_HOT : (mood >= 2) ? COL_YELLOW : p.textDim;
    for (int i = 0; i < 4; i++) {
      drawHeartIcon(cv, 190 + i * 34, y + 4, i < mood, moodCol);
    }

    y += 34;
    hwDrawUtf8(cv, "Nakrmeno", 40, y, 2, p.textDim, COL_CARD);
    uint8_t fed = statsFedProgress();
    for (int i = 0; i < 10; i++) {
      int px = 185 + i * 24;
      if (i < fed) cv->fillRoundRect(px, y + 2, 18, 12, 3, p.body);
      else cv->drawRoundRect(px, y + 2, 18, 12, 3, p.textDim);
    }

    y += 34;
    hwDrawUtf8(cv, "Energie", 40, y, 2, p.textDim, COL_CARD);
    uint8_t en = statsEnergyTier();
    uint16_t enCol = (en >= 4) ? COL_CYAN : (en >= 2) ? COL_YELLOW : COL_HOT;
    for (int i = 0; i < 5; i++) {
      int px = 185 + i * 44;
      if (i < en) cv->fillRoundRect(px, y + 2, 36, 12, 3, enCol);
      else cv->drawRoundRect(px, y + 2, 36, 12, 3, p.textDim);
    }

    y += 36;
    cv->fillRoundRect(40, y - 2, 92, 24, 6, p.body);
    hwPrintfUtf8(cv, 52, y + 2, 2, COL_BG, p.body, "Lv %u", stats().level);
    hwPrintfUtf8(cv, 150, y + 2, 2, COL_GREEN, COL_CARD, "OK %u", stats().approvals);
    hwPrintfUtf8(cv, 250, y + 2, 2, COL_HOT, COL_CARD, "NE %u", stats().denials);
    hwPrintfUtf8(cv, 340, y + 2, 2, COL_CYAN, COL_CARD, "Odp %u", stats().answers);

    y += 34;
    uint64_t v = stats().tokens;
    if (v >= 1000000000ULL) hwPrintfUtf8(cv, 40, y, 2, p.textDim, COL_CARD, "Tokeny celkem: %lu.%luG", (unsigned long)(v / 1000000000ULL), (unsigned long)((v / 100000000ULL) % 10));
    else if (v >= 1000000) hwPrintfUtf8(cv, 40, y, 2, p.textDim, COL_CARD, "Tokeny celkem: %lu.%luM", (unsigned long)(v / 1000000), (unsigned long)((v / 100000) % 10));
    else if (v >= 1000) hwPrintfUtf8(cv, 40, y, 2, p.textDim, COL_CARD, "Tokeny celkem: %lu.%luK", (unsigned long)(v / 1000), (unsigned long)((v / 100) % 10));
    else hwPrintfUtf8(cv, 40, y, 2, p.textDim, COL_CARD, "Tokeny celkem: %lu", (unsigned long)v);

    y += 26;
    uint32_t td = tama.tokensToday;
    if (td >= 1000) hwPrintfUtf8(cv, 40, y, 2, p.textDim, COL_CARD, "Dnes: %lu.%luK  |  Spánek: %lum", td / 1000, (td / 100) % 10, stats().napSeconds / 60);
    else hwPrintfUtf8(cv, 40, y, 2, p.textDim, COL_CARD, "Dnes: %lu  |  Spánek: %lum", td, stats().napSeconds / 60);
  } else {
    int y = TOP + 50;
    auto row = [&](uint16_t col, const char* s) {
      hwDrawUtf8(cv, s, 40, y, 2, col, COL_CARD);
      y += 24;
    };
    row(p.body,    "JAK FUNGUJE PARŤÁK:");
    row(p.textDim, "- Rychlé rozhodnutí = +nálada");
    row(p.textDim, "- Tokeny = Level Up (další dražší)");
    row(p.textDim, "- Otoč displejem dolů = spánek");
    row(p.textDim, "- Zatřes krabičkou = motání hlavy");
    row(p.textDim, "- Klepni na parťáka = pohlazení");
    row(p.textDim, "- Táhni nahoru/dolů = posun zpráv");
  }

  drawBottomNav();
}

static void drawInfoScreen() {
  Arduino_Canvas* cv = hwCanvas();
  const Palette& p = characterPalette();
  const int TOP = 158;
  cv->fillRect(0, TOP, LCD_WIDTH, NAV_Y - TOP, p.bg);
  cv->fillRoundRect(24, TOP, 432, 256, 14, COL_CARD);
  cv->drawRoundRect(24, TOP, 432, 256, 14, COL_BORDER);

  static const char* const TITLES[INFO_PAGES] = {
    "BLUETOOTH PÁROVÁNÍ",
    "SPOJENÍ CLAUDE / AGY",
    "BATERIE A SYSTÉM",
    "OVLÁDÁNÍ KRABIČKY",
    "O PROJEKTU ROZHODOVAČ"
  };

  hwDrawUtf8(cv, TITLES[infoPage], 40, TOP + 16, 2, p.body, COL_CARD);

  char pg[16];
  snprintf(pg, sizeof(pg), "< %u/%u >", infoPage + 1, INFO_PAGES);
  hwDrawTextRight(cv, pg, 440, TOP + 16, 2, p.textDim, COL_CARD);
  cv->drawFastHLine(36, TOP + 38, 408, COL_BORDER);

  int y = TOP + 50;
  auto ln = [&](uint16_t col, const char* fmt, ...) {
    char b[96];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    hwDrawUtf8(cv, b, 40, y, 2, col, COL_CARD, 33);
    y += 25;
  };

  if (infoPage == 0) {
    bool linked = bleConnected();
    ln(linked ? COL_GREEN : COL_YELLOW, "Stav BLE: %s",
       linked ? (bleSecure() ? "PŘIPOJENO (Šifrováno)" : "PŘIPOJENO") : "ČEKÁ NA PÁROVÁNÍ");
    ln(p.text, "Název: %s", btName);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln(p.textDim, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    y += 6;
    ln(COL_CYAN,  "Claude: Dev → Hardware Buddy");
    ln(COL_GREEN, "AGY: rozhodovac_ctl.py status");
  } else if (infoPage == 1) {
    ln(p.text,    "Aktivní kanál: %s", dataScenarioName());
    ln(p.text,    "Klient:        %s", !tama.connected || dataDemo() ? "-" : (tama.agyClient ? "AGY CLI" : "Claude"));
    ln(p.textDim, "Relace celkem: %u", tama.sessionsTotal);
    ln(COL_CYAN,  "Právě běží:    %u", tama.sessionsRunning);
    ln(COL_HOT,   "Čeká schvál.:  %u", tama.sessionsWaiting);
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln(p.textDim, "Poslední data: před %lus", (unsigned long)age);
    ln(p.textDim, "Stav postavy:  %s", stateNamesCz[activeState]);
  } else if (infoPage == 2) {
    const HwPowerInfo& pw = hwPower();
    ln(pw.charging ? COL_GREEN : p.text, "Baterie: %d%% (%d mV)%s",
       pw.batteryPct, pw.batteryMv, pw.charging ? " [Nabíjí]" : "");
    ln(p.textDim, "USB VBUS: %s (%d mV)", pw.usbPresent ? "Připojeno" : "Odpojeno", pw.vbusMv);
    ln(p.textDim, "Teplota PMU: %d °C", pw.tempC);
    uint32_t up = millis() / 1000;
    ln(p.textDim, "Běh systému: %luh %02lum %02lus", (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
    ln(p.textDim, "RAM: %luKB | PSRAM: %luKB", (unsigned long)(ESP.getFreeHeap() / 1024), (unsigned long)(ESP.getFreePsram() / 1024));
  } else if (infoPage == 3) {
    ln(COL_GREEN, "Dotyk: tlačítka, karty, menu");
    ln(p.text,    "BOOT (nahoře): Schválit / Další");
    ln(COL_HOT,   "Kritická akce: podržet 1 s");
    ln(p.text,    "IO18 (nahoře): Zamítnout / Strana");
    ln(p.text,    "Více voleb: vyber, potvrď znovu");
    ln(p.textDim, "Podržet BOOT:  Otevřít Menu");
    ln(p.textDim, "PWR krátce:    Vypnout displej");
    ln(p.textDim, "PWR 6 sekund:  Úplné vypnutí");
  } else {
    ln(p.text,    "Waveshare ESP32-S3 AMOLED 2.16");
    ln(p.textDim, "Displej: 480x480 QSPI CO5300");
    ln(p.textDim, "Dotyk:   CST9220 + AXP2101 PMU");
    ln(COL_CYAN,  "Protokol: Claude Buddy BLE + USB");
    ln(COL_GREEN, "Podpora:  Claude Code + AGY CLI");
  }

  drawBottomNav();
}

static void drawPasskeyScreen() {
  Arduino_Canvas* cv = hwCanvas();
  const Palette& p = characterPalette();
  cv->fillScreen(p.bg);
  cv->fillRoundRect(30, 80, 420, 320, 20, COL_CARD);
  cv->drawRoundRect(30, 80, 420, 320, 20, COL_CYAN);

  hwDrawTextCentered(cv, "BLUETOOTH PÁROVÁNÍ", 240, 118, 3, COL_CYAN, COL_CARD);
  hwDrawTextCentered(cv, "Zadejte tento PIN na PC:", 240, 168, 2, p.textDim, COL_CARD);

  char pin[12];
  snprintf(pin, sizeof(pin), "%06lu", (unsigned long)blePasskey());
  hwDrawTextCentered(cv, pin, 240, 224, 6, p.text, COL_CARD);

  hwDrawTextCentered(cv, btName, 240, 336, 2, p.body, COL_CARD);
}

// Stolní hodiny: jen čas a datum přes celou plochu, bez maskota
static void drawDeskClock() {
  Arduino_Canvas* cv = hwCanvas();
  const Palette& p = characterPalette();
  const int TOP = 50;
  cv->fillRect(0, TOP, LCD_WIDTH, NAV_Y - 4 - TOP, p.bg);

  char hms[12];
  if (dataRtcValid()) snprintf(hms, sizeof(hms), "%02u:%02u:%02u", _clkDt.hour, _clkDt.minute, _clkDt.second);
  else                snprintf(hms, sizeof(hms), "--:--:--");
  hwDrawTextCentered(cv, hms, 240, 184, 7, p.text, p.bg);

  static const char* const DOW_CZ[] = { "Neděle", "Pondělí", "Úterý", "Středa", "Čtvrtek", "Pátek", "Sobota" };
  char dl[48];
  if (dataRtcValid()) {
    snprintf(dl, sizeof(dl), "%s %u.%u.%u", DOW_CZ[_clkDt.wday % 7], _clkDt.day, _clkDt.month, _clkDt.year);
  } else {
    snprintf(dl, sizeof(dl), "Čas zatím není nastavený");
  }
  hwDrawTextCentered(cv, dl, 240, 268, 2, p.body, p.bg);

  drawBottomNav();
}

// ─── Touch-Friendly Menu / Settings / Reset Overlays ──────────────────────
static const char* const menuItems[] = {
  "Nastavení",
  "Hodiny",
  "Demo režim",
  "Zhasnout displej",
  "Vypnout krabičku",
  "Zavřít menu"
};
static const uint8_t MENU_N = 6;

static const char* const settingsItems[] = {
  "Jas displeje",
  "Zvuk",
  "Zvuk ovládání",
  "Noční klid 22–7 h",
  "Spodní panel (HUD)",
  "Tovární reset…",
  "Zpět"
};
static const uint8_t SETTINGS_N = 7;

static const char* const resetItems[] = {
  "Tovární nastavení + BLE",
  "Zpět"
};
static const uint8_t RESET_N = 2;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx   = 0xFF;

// Nabídky (Menu, Nastavení, Reset) vyplní celou plochu mezi horní lištou
// a spodní navigací, aby pod nimi nevykukoval chat ani maskot.
static const int MODAL_X = 24, MODAL_W = 432;
static const int MODAL_Y = 58;
static const int MODAL_H = NAV_Y - 10 - MODAL_Y;   // až po spodní navigaci
static const int ROW_H = 48;

// Výška řádku nabídky: 48 px, u delšího seznamu méně, aby se vešel nad nápovědu
static int modalRowH(uint8_t count) {
  int h = (MODAL_H - 52 - 34) / (count ? count : 1);
  return h < ROW_H ? h : ROW_H;
}

static void executeMenuItem(uint8_t idx) {
  switch (idx) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1:
      // Stolní hodiny hned, i na baterii; skončí dotykem, tlačítkem nebo prací agenta
      menuOpen = false;
      displayMode = DISP_NORMAL;
      chatExpanded = false;
      clockForced = true;
      applyDisplayMode();
      break;
    case 2: dataSetDemo(!dataDemo(), &tama); break;
    case 3:
      menuOpen = false;
      screenOffNow();
      break;
    case 4: hwPowerOff(); break;
    case 5:
      menuOpen = false;
      fullClearCanvas();
      buddyInvalidate();
      break;
  }
}

static void executeSettingItem(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      s.brightness = brightLevel;
      applyBrightness();
      break;
    case 1:
      // VYP -> tichý -> střední -> hlasitý -> VYP; ukázka nové hlasitosti
      if (!s.sound)           { s.sound = true; s.volume = 0; }
      else if (s.volume < 2)  { s.volume++; }
      else                    { s.sound = false; }
      settingsSave();
      PLAY(SND_CONNECT, CAT_IMPORTANT);
      return;
    case 2: s.clicks = !s.clicks; break;
    case 3: s.nightQuiet = !s.nightQuiet; break;
    case 4: s.hud = !s.hud; break;
    case 5: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 6:
      settingsOpen = false;
      fullClearCanvas();
      buddyInvalidate();
      return;
  }
  settingsSave();
}

static void executeResetItem(uint8_t idx) {
  uint32_t now = millis();
  if (idx == 1) { resetOpen = false; return; }
  bool armed = (resetConfirmIdx == idx) && ((int32_t)(now - resetConfirmUntil) < 0);
  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3500;
    PLAY(SND_RESET_ARM, CAT_IMPORTANT);
    return;
  }
  PLAY(SND_RESET_DO, CAT_IMPORTANT);
  _prefs.begin("buddy", false);
  _prefs.clear();
  _prefs.end();
  bleClearBonds();
  delay(450);   // dohrát zvuk z fronty
  ESP.restart();
}

static void drawModalList(const char* title, uint8_t count, uint8_t sel, uint16_t borderCol,
                          void (*rowFmt)(uint8_t idx, char* left, char* right, uint16_t& rCol)) {
  Arduino_Canvas* cv = hwCanvas();
  const Palette& p = characterPalette();
  const int my = MODAL_Y, mh = MODAL_H;

  cv->fillRect(0, 50, LCD_WIDTH, NAV_Y - 4 - 50, p.bg);
  cv->fillRoundRect(MODAL_X, my, MODAL_W, mh, 16, COL_CARD);
  cv->drawRoundRect(MODAL_X, my, MODAL_W, mh, 16, borderCol);
  cv->drawRoundRect(MODAL_X + 1, my + 1, MODAL_W - 2, mh - 2, 15, borderCol);

  hwDrawTextCentered(cv, title, 240, my + 18, 2, borderCol, COL_CARD);
  cv->drawFastHLine(MODAL_X + 16, my + 44, MODAL_W - 32, COL_BORDER);
  hwDrawTextCentered(cv, "BOOT = další položka   IO18 = vybrat", 240, my + mh - 26, 1, p.textDim, COL_CARD);

  const int rh = modalRowH(count);
  const int ty = (rh - 4 - 16) / 2 + 1;   // svislé vystředění textu v řádku
  for (uint8_t i = 0; i < count; i++) {
    int ry = my + 52 + i * rh;
    bool isSel = (i == sel);
    uint16_t rBg = isSel ? COL_CARD_HI : COL_CARD;
    if (isSel) cv->fillRoundRect(MODAL_X + 10, ry, MODAL_W - 20, rh - 4, 8, rBg);

    char left[64] = "", right[24] = "";
    uint16_t rCol = p.textDim;
    rowFmt(i, left, right, rCol);

    hwDrawUtf8(cv, left, MODAL_X + 24, ry + ty, 2, isSel ? p.text : p.textDim, rBg);

    if (right[0]) {
      hwDrawTextRight(cv, right, MODAL_X + MODAL_W - 24, ry + ty, 2, rCol, rBg);
    }
  }
}

static void fmtMenuRow(uint8_t i, char* left, char* right, uint16_t& rCol) {
  strncpy(left, menuItems[i], 63);
  left[63] = 0;
  if (i == 2) {
    snprintf(right, 23, "%s", dataDemo() ? "ZAP" : "VYP");
    rCol = dataDemo() ? COL_GREEN : characterPalette().textDim;
  }
}

static void fmtSettingsRow(uint8_t i, char* left, char* right, uint16_t& rCol) {
  strncpy(left, settingsItems[i], 63);
  left[63] = 0;
  Settings& s = settings();
  auto onOff = [&](bool on) {
    snprintf(right, 23, "%s", on ? "ZAP" : "VYP");
    rCol = on ? COL_GREEN : characterPalette().textDim;
  };
  if (i == 0) {
    snprintf(right, 23, "%u/4", brightLevel);
    rCol = COL_CYAN;
  } else if (i == 1) {
    static const char* const VOL[3] = { "TICHÝ", "STŘEDNÍ", "HLASITÝ" };
    snprintf(right, 23, "%s", s.sound ? VOL[s.volume % 3] : "VYP");
    rCol = s.sound ? COL_GREEN : characterPalette().textDim;
  } else if (i == 2) {
    onOff(s.clicks);
  } else if (i == 3) {
    onOff(s.nightQuiet);
  } else if (i == 4) {
    onOff(s.hud);
  }
}

static void fmtResetRow(uint8_t i, char* left, char* right, uint16_t& rCol) {
  bool armed = (i == resetConfirmIdx) && ((int32_t)(millis() - resetConfirmUntil) < 0);
  if (armed) {
    strncpy(left, "OPRAVDU POTVRDIT?", 63);
    strncpy(right, "KLEPNI", 23);
    rCol = COL_HOT;
  } else {
    strncpy(left, resetItems[i], 63);
  }
  left[63] = 0;
  right[23] = 0;
}

static bool handleModalTouch(int tx, int ty, uint8_t count, uint8_t& sel, void (*execFn)(uint8_t)) {
  const int my = MODAL_Y, mh = MODAL_H;
  if (!hitRect(tx, ty, MODAL_X, my, MODAL_W, mh)) {
    // Tapping outside closes modal
    resetOpen = settingsOpen = menuOpen = false;
    fullClearCanvas();
    buddyInvalidate();
    return true;
  }
  const int rh = modalRowH(count);
  for (uint8_t i = 0; i < count; i++) {
    int ry = my + 52 + i * rh;
    if (hitRect(tx, ty, MODAL_X + 8, ry, MODAL_W - 16, rh)) {
      sel = i;
      click(2100);
      execFn(i);
      return true;
    }
  }
  return true;
}

// ─── Arduino Setup & Main Loop ─────────────────────────────────────────────
void setup() {
  // Výchozí fronta příjmu USB má 256 B a co se nevejde, jádro tiše zahodí;
  // snímek s dotazem má přes 1 KB. Zápis bez čekání: když hostitel nečte
  // (kabel jen nabíjí, daemon neběží), jinak blokuje až 2 s na každý řádek.
  Serial.setRxBufferSize(8192);
  Serial.setTxBufferSize(4096);
  Serial.setTxTimeoutMs(0);
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Rozhodovac (Waveshare ESP32-S3 AMOLED 2.16) ===");

  if (!hwInit()) {
    Serial.println("FATAL: hwInit failed");
    while (1) delay(1000);
  }

  startBt();
  applyBrightness();

  // Čas z RTC (zálohovaného baterií) platí hned, pokud dává smysl;
  // jinak se čeká na synchronizaci z počítače
  {
    HwDateTime dt;
    if (hwRtcGet(dt) && dt.year >= 2025 && dt.year < 2100 &&
        dt.month >= 1 && dt.month <= 12 && dt.day >= 1 && dt.day <= 31) {
      dataMarkRtcValid();
    }
  }
  lastInteractMs = millis();

  statsLoad();
  settingsLoad();
  brightLevel = settings().brightness % 5;
  applyBrightness();
  petNameLoad();
  buddyInit();
  applyDisplayMode();

  // Boot splash on 480x480 AMOLED featuring Clawd!
  {
    Arduino_Canvas* cv = hwCanvas();
    const Palette& p = characterPalette();
    cv->fillScreen(p.bg);
    buddySetMode(CLAWD_FULL);
    buddyTick(P_IDLE);
    hwDrawTextCentered(cv, "ROZHODOVAČ", 240, 306, 4, p.body, p.bg);
    hwDrawTextCentered(cv, "Claude Code & Antigravity CLI", 240, 358, 2, p.text, p.bg);
    hwDrawTextCentered(cv, btName, 240, 394, 2, COL_CYAN, p.bg);
    hwDrawTextCentered(cv, "klepnutím pokračovat", 240, 440, 1, p.textDim, p.bg);
    hwFlush();
    PLAY(SND_BOOT, CAT_INFO);

    // Úvodní obrazovka zůstane 3 s, aby šla přečíst. Dotyk nebo tlačítko ji zkrátí.
    // BOOT reaguje až na puštění, to by v hlavní smyčce přepnulo obrazovku, proto
    // se spolkne. IO18 reaguje na stisk, ten už proběhl tady.
    const uint32_t splashEnd = millis() + 3000;
    while ((int32_t)(millis() - splashEnd) < 0) {
      hwUpdate();
      if (hwTouch().type == TOUCH_TAP
          || hwBtnBoot().isPressed() || hwBtnBoot().wasPressed()
          || hwBtnUser().isPressed() || hwBtnUser().wasPressed()) break;
      delay(20);
    }
    swallowBtnA = hwBtnBoot().isPressed();
    fullClearCanvas();
    buddyInvalidate();
  }

  Serial.printf("[boot] ready as '%s' (%s)\n", btName, buddySpeciesName());
}

void loop() {
  hwUpdate();
  uint32_t now = millis();

  dataPoll(&tama);   // skutečný dotaz demo sám ukončí, změna id dotazu ho pak ohlásí
  statsTick();

  // Nové zprávy v chatu: jen posun na začátek. Displej se kvůli nim nebudí,
  // na baterii by jinak při práci agenta nikdy nezhasl.
  if (tama.lineGen != lastLineGen) {
    msgScroll = 0;
    lastLineGen = tama.lineGen;
  }

  if (statsPollLevelUp()) {
    PLAY(SND_LEVEL, CAT_INFO);
    triggerOneShot(P_CELEBRATE, 3500);
  }
  baseState = derive(tama);

  // Hranice se po vypršení posouvají s časem: porovnání se znaménkem by po
  // 24,8 dne bez jednorázové animace jinak obrátilo výsledek
  if ((int32_t)(now - wakeTransitionUntil) < 0) {
    if (baseState == P_IDLE) baseState = P_SLEEP;
  } else {
    wakeTransitionUntil = now;
  }
  if ((int32_t)(now - oneShotUntil) >= 0) {
    activeState = baseState;
    oneShotUntil = now;
  }

  // Shake -> dizzy
  if (now - lastShakeCheck > 60) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2200);
      PLAY(SND_DIZZY, CAT_CLICK);
    }
  }

  // Palm cover -> turn screen off
  if (hwTakePalmCover() && !screenOff && !tama.promptId[0]) {
    screenOffNow();
  }

  // Potvrzení odpovědi od mostu se zpracuje dřív než změna dotazu: most pošle
  // potvrzení a hned snímek bez dotazu, oboje obvykle dorazí v jednom průchodu.
  // Porovnává se s dotazem, na který se odpovídalo (lastPromptId).
  {
    char ackId[48];
    bool ackOk;
    uint8_t ackCh;
    if (dataTakePermissionAck(ackId, sizeof(ackId), ackOk, ackCh) && !responseAcked
        && ackCh == lastPromptCh && lastPromptId[0] && strcmp(ackId, lastPromptId) == 0) {
      if (responseSent) {
        responseAcked = true;
        if (ackOk && pendingStat != PS_NONE) applyStat(pendingStat, pendingStatTook);
        pendingStat = PS_NONE;
      } else if (ackOk) {
        // Potvrzení dorazilo až po vrácení tlačítek: odpověď přece jen prošla
        responseSent = true;
        responseAcked = true;
        if (pendingStat != PS_NONE) applyStat(pendingStat, pendingStatTook);
        pendingStat = PS_NONE;
      }
    }
  }

  // New approval prompt arrived -> wake screen immediately, jump to normal view & chirp!
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    // Dotaz zmizel bez odpovědi z krabičky: vypršel nebo se vyřídil jinde (terminál)
    bool hadUnanswered = lastPromptId[0] && !responseSent;
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId) - 1);
    lastPromptId[sizeof(lastPromptId) - 1] = 0;
    lastPromptCh = tama.promptCh;
    responseSent = false;
    responseAcked = false;
    holdStartMs = 0;
    if (tama.promptId[0]) {
      // Most posílá stáří a zbývající čas dotazu: dotaz, který se vrací po
      // vyřízení jiného, si tak zachová odpočet i odbyté připomínky
      uint32_t ageS = tama.promptAge > 0 ? (uint32_t)tama.promptAge : 0;
      promptArrivedMs = millis() - ageS * 1000;
      promptTotalS = (tama.promptTtl >= 0) ? ageS + (uint32_t)tama.promptTtl : 0;
      promptShownMs = millis();
      promptReminders = 0;
      while (promptReminders < 3 && ageS >= reminderAtS(promptReminders + 1)) promptReminders++;
      promptSel = 0;
      promptSelTapped = false;
      pendingStat = PS_NONE;
      if (tama.promptQ) qResetForPrompt();
      presentPrompt();
      // Další krok téže otázky (další otázka, shrnutí) jen cvakne
      if (tama.promptCont) click(2000);
      else                 playPromptSound();
    } else {
      pendingStat = PS_NONE;
      if (hadUnanswered && tama.connected && !dataDemo()) PLAY(SND_TIMEOUT, CAT_IMPORTANT);
      fullClearCanvas();
      buddyInvalidate();
    }
  }

  // Odpověď odeslaná do mostu bez potvrzení (spojení spadlo cestou): tlačítka
  // se vrátí, aby šlo odpovědět znovu. Claude Desktop potvrzení neposílá.
  // Statistika čeká dál, opožděné potvrzení odpověď ještě přijme.
  {
    uint32_t resendAfter = promptFromBridge() ? ACK_TIMEOUT_MS : CLAUDE_RESEND_AFTER_MS;
    if (responseSent && !responseAcked && tama.promptId[0] && !dataDemo()
        && (millis() - responseSentMs) > resendAfter) {
      responseSent = false;
      promptSelTapped = false;
      // U povolení se výběr vrátí na „Ne“, aby IO18 omylem neschválilo
      if (tama.promptQ && !isQuestionTool(tama.promptTool)) qInitCursor();
      presentPrompt();
      PLAY(SND_PAIR_FAIL, CAT_IMPORTANT);
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Připomínky nevyřízeného dotazu (viz reminderAtS), pak už krabička mlčí
  if (inPrompt && promptReminders < 3 && (millis() - promptArrivedMs) / 1000 >= reminderAtS(promptReminders + 1)) {
    promptReminders++;
    wake();
    playPromptSound();
  }

  // Agent dokončil práci (snapshot s "completed"): zvuk a rozsvícení displeje
  static bool wasCompleted = false;
  if (tama.recentlyCompleted && !wasCompleted && !inPrompt) {
    wake();
    PLAY(SND_DONE, CAT_INFO);
  }
  wasCompleted = tama.recentlyCompleted;

  // Připojení a odpojení klienta (v demo režimu se nehlásí)
  static bool wasLinked = false;
  if (!dataDemo()) {
    if (tama.connected != wasLinked) {
      if (tama.connected) PLAY(SND_CONNECT, CAT_INFO);
      else                PLAY(SND_DISCONNECT, CAT_INFO);
    }
    wasLinked = tama.connected;
  }

  // Nabíjení a baterie: varování jen jednou na hranici, znovu až po nabití nad 25 %
  {
    const HwPowerInfo& pw = hwPower();
    static bool wasUsb = pw.usbPresent;
    if (pw.usbPresent != wasUsb) {
      if (pw.usbPresent) PLAY(SND_CHARGE_ON, CAT_INFO);
      else               PLAY(SND_CHARGE_OFF, CAT_INFO);
      wasUsb = pw.usbPresent;
    }
    static uint8_t batWarn = 0;   // 0 nic, 1 varováno při 20 %, 2 při 10 %
    if (pw.usbPresent || !pw.batteryConnected || pw.batteryPct >= 25) {
      batWarn = 0;
    } else if (pw.batteryPct <= 10 && batWarn < 2) {
      batWarn = 2;
      wake();
      PLAY(SND_BAT_CRIT, CAT_IMPORTANT);
    } else if (pw.batteryPct <= 20 && batWarn < 1) {
      batWarn = 1;
      PLAY(SND_BAT_LOW, CAT_INFO);
    }
  }

  // Power button short press toggles screen on/off
  if (hwTakePowerKeyShortPress()) {
    if (screenOff) {
      wake();
    } else {
      screenOffNow();
    }
  }

  // Physical buttons: BOOT (BtnA) and IO18 (BtnB)
  HwButton& btnA = hwBtnBoot();
  HwButton& btnB = hwBtnUser();
  if (btnA.wasPressed()) btnADownMs = millis();

  // Obrazovka s PINem pro párování překrývá všechno: tlačítka ani dotyk
  // nesmí naslepo odpovědět na dotaz pod ní
  const bool pinShown = blePasskey() != 0;
  if (pinShown) {
    if (btnA.isPressed() || btnA.wasPressed()) swallowBtnA = true;
    if (btnB.wasPressed()) swallowBtnB = true;
  }

  // Krátké cvaknutí může začít i skončit mezi dvěma průchody smyčky, pak
  // isPressed() už neplatí, ale wasPressed() ano. IO18 reaguje na stisk, proto
  // se spolkne jen nový stisk (držené tlačítko by spolklo až ten příští).
  bool aDown = btnA.isPressed() || btnA.wasPressed();
  if (aDown || btnB.isPressed() || btnB.wasPressed()) {
    if (screenOff) {
      if (aDown) swallowBtnA = true;
      if (btnB.wasPressed()) swallowBtnB = true;
    }
    wake();
  }

  // Nový stisk na hodinách je jen ukončí (reaguje se na hranu stisku, ne na držení,
  // jinak by hodiny zapnuté z menu tlačítkem IO18 hned zase zmizely)
  if (btnA.wasPressed() || btnB.wasPressed()) {
    if (clockShown) {
      if (btnA.wasPressed()) swallowBtnA = true;
      if (btnB.wasPressed()) swallowBtnB = true;
    }
    clockForced = false;
  }

  // Ochranná doba po objevení dotazu: nový stisk se spolkne
  if (inPrompt && promptGuardActive()) {
    if (btnA.wasPressed()) swallowBtnA = true;
    if (btnB.wasPressed()) swallowBtnB = true;
  }

  // Kritická akce: BOOT se musí podržet DANGER_HOLD_MS
  if (inPrompt && promptIsDanger() && !swallowBtnA && btnA.pressedFor(DANGER_HOLD_MS)) {
    btnALong = true;   // puštění už nic dalšího nedělá
    swallowBtnA = true;
    doApprove();
  }

  // Během dotazu menu neotevírá (BOOT i IO18 tam patří dotazu), podržení se
  // jen spolkne, aby po puštění neodeslalo odpověď
  if (btnA.pressedFor(650) && !btnALong && !swallowBtnA && !(inPrompt && promptIsDanger())) {
    btnALong = true;
    if (!tama.promptId[0]) {
      PLAY(SND_MENU, CAT_CLICK);
      if (resetOpen) resetOpen = false;
      else if (settingsOpen) settingsOpen = false;
      else { menuOpen = !menuOpen; menuSel = 0; }
      fullClearCanvas();
      buddyInvalidate();
    }
  }

  if (btnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt && (int32_t)(btnADownMs - promptShownMs) < (int32_t)PROMPT_GUARD_MS) {
        // Stisk začal před dotazem nebo v ochranné době: puštění nic neodešle
      } else if (tama.promptId[0] && !inPrompt) {
        // Odpověď odeslaná, čeká se na most: obrazovka dotazu zůstává
      } else if (inPrompt && tama.promptQ) {
        // Obrazovka otázky: výběr na další volbu nebo tlačítko
        uint8_t items = qItemCount();
        if (items) promptSel = (promptSel + 1) % items;
        promptSelTapped = false;
        qFollowCursor = true;
        click(1800);
      } else if (inPrompt) {
        uint8_t totalOpts = promptOptionCount();
        if (promptIsDanger()) {
          click(900);   // krátký stisk u kritické akce nestačí, je třeba podržet
        } else if (totalOpts <= 2) {
          doApprove();
        } else {
          promptSel = (promptSel + 1) % totalOpts;
          promptSelTapped = false;
          click(1800);
        }
      } else if (resetOpen) {
        click(1800);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        click(1800);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        click(1800);
        menuSel = (menuSel + 1) % MENU_N;
      } else {
        click(1800);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  if (btnB.wasPressed()) {
    if (swallowBtnB) {
      swallowBtnB = false;
    } else if (tama.promptId[0] && !inPrompt) {
      // Odpověď odeslaná, čeká se na most
    } else if (inPrompt && tama.promptQ) {
      qActivate(promptSel, false);
    } else if (inPrompt) {
      uint8_t totalOpts = promptOptionCount();
      if (totalOpts <= 2) {
        doDeny();
      } else {
        answerQuestion(promptSel);
      }
    } else if (resetOpen) {
      click(2200);
      executeResetItem(resetSel);
    } else if (settingsOpen) {
      click(2200);
      executeSettingItem(settingsSel);
    } else if (menuOpen) {
      click(2200);
      executeMenuItem(menuSel);
    } else if (displayMode == DISP_INFO) {
      click(2200);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      click(2200);
      petPage = (petPage + 1) % PET_PAGES;
    } else {
      click(2200);
      msgScroll = (msgScroll >= msgMaxScroll) ? 0 : msgScroll + 1;
    }
  }

  // ─── Capacitive Touch Input Handling ─────────────────────────────────────
  const TouchEvent& te = hwTouch();
  static bool wasTouching = false;
  if (te.touching && !wasTouching) touchDownMs = millis();
  wasTouching = te.touching;

  // Dotyk, který začal před dotazem nebo v ochranné době, na dotaz nepůsobí
  bool touchFresh = (int32_t)(touchDownMs - promptShownMs) >= (int32_t)PROMPT_GUARD_MS;

  // Kritická akce: SCHVÁLIT se drží prstem DANGER_HOLD_MS
  if (inPrompt && promptIsDanger() && !screenOff && !pinShown && te.touching && touchFresh
      && hitRect(te.curX, te.curY, BTN_APPR_X, BTN_APPR_Y, BTN_APPR_W, BTN_APPR_H)) {
    if (!holdStartMs) holdStartMs = millis();
    if (millis() - holdStartMs >= DANGER_HOLD_MS) doApprove();
  } else {
    holdStartMs = 0;
  }

  if (!pinShown && (te.type != TOUCH_NONE || te.touching)) {
    if (screenOff) {
      if (te.type == TOUCH_TAP || te.type == TOUCH_LONG_PRESS) wake();
    } else if (clockShown) {
      // Dotyk na hodinách jen vrátí hlavní obrazovku (a jas ztlumených hodin)
      lastInteractMs = millis();
      if (dimmed && !napping) { applyBrightness(); dimmed = false; }
      if (te.type != TOUCH_NONE) {
        clockForced = false;
        click(1900);
      }
    } else {
      lastInteractMs = millis();
      if (dimmed) { applyBrightness(); dimmed = false; }

      if (te.type == TOUCH_LONG_PRESS && !tama.promptId[0]) {
        PLAY(SND_MENU, CAT_CLICK);
        menuOpen = !menuOpen;
        settingsOpen = resetOpen = false;
        menuSel = 0;
        fullClearCanvas();
        buddyInvalidate();
      } else if (te.type == TOUCH_TAP) {
        int tx = te.x, ty = te.y;

        bool modalOpen = resetOpen || settingsOpen || menuOpen;
        if (modalOpen && ty < NAV_Y - 6) {
          if (resetOpen)         handleModalTouch(tx, ty, RESET_N, resetSel, executeResetItem);
          else if (settingsOpen) handleModalTouch(tx, ty, SETTINGS_N, settingsSel, executeSettingItem);
          else                   handleModalTouch(tx, ty, MENU_N, menuSel, executeMenuItem);
        } else if (tama.promptId[0] && !modalOpen) {
          uint8_t totalOpts = promptOptionCount();
          if (!inPrompt || !touchFresh) {
            // odpověď už odešla, nebo klepnutí mířené na předchozí obrazovku
          } else if (tama.promptQ) {
            qHandleTap(tx, ty);
          } else if (totalOpts <= 2) {
            if (hitRect(tx, ty, BTN_APPR_X, BTN_APPR_Y, BTN_APPR_W, BTN_APPR_H)) {
              if (promptIsDanger()) click(900);   // klepnutí nestačí, je třeba podržet
              else doApprove();
            } else if (hitRect(tx, ty, BTN_DENY_X, BTN_DENY_Y, BTN_DENY_W, BTN_DENY_H)) {
              doDeny();
            }
          } else {
            // 3+ voleb: první klepnutí vybere, druhé na stejnou volbu odešle
            for (uint8_t i = 0; i < totalOpts; i++) {
              int bx, by, bw, bh;
              getOptionBtnRect(i, totalOpts, bx, by, bw, bh);
              if (hitRect(tx, ty, bx, by, bw, bh)) {
                if (promptSel == i && promptSelTapped) {
                  answerQuestion(i);
                } else {
                  promptSel = i;
                  promptSelTapped = true;
                  click(2100);
                }
                break;
              }
            }
          }
        } else if (ty >= NAV_Y - 6) {
          // Bottom navigation bar tap
          for (int i = 0; i < 4; i++) {
            int bx = NAV_X0 + i * (NAV_W + NAV_GAP);
            if (hitRect(tx, ty, bx - 4, NAV_Y - 6, NAV_W + 8, NAV_H + 12)) {
              click(1900);
              if (i < 3) {
                // Akce / Profil / Info zavře případnou nabídku a přepne obrazovku
                resetOpen = settingsOpen = menuOpen = false;
                displayMode = (uint8_t)i;
                chatExpanded = false;
                applyDisplayMode();
              } else if (modalOpen) {
                // Menu při otevřené nabídce ji zavře
                resetOpen = settingsOpen = menuOpen = false;
                fullClearCanvas();
                buddyInvalidate();
              } else {
                menuOpen = true;
                menuSel = 0;
              }
              break;
            }
          }
        } else if (displayMode == DISP_INFO && ty >= 158) {
          click(2100);
          if (tx < 240) infoPage = (infoPage + INFO_PAGES - 1) % INFO_PAGES;
          else          infoPage = (infoPage + 1) % INFO_PAGES;
        } else if (displayMode == DISP_PET && ty >= 158) {
          click(2100);
          petPage = (petPage + 1) % PET_PAGES;
        } else if (displayMode == DISP_NORMAL && chatExpanded && ty >= 54) {
          // Tap anywhere on fullscreen chat -> collapse back to Clawd!
          click(1900);
          chatExpanded = false;
          fullClearCanvas();
          buddyInvalidate();
        } else if (displayMode == DISP_NORMAL && !chatExpanded && settings().hud && ty >= 278) {
          // Tap compact chat box -> expand chat to full screen (hide Clawd)!
          click(2100);
          chatExpanded = true;
          msgScroll = 0;
          fullClearCanvas();
          buddyInvalidate();
        } else if (ty >= 54 && ty < 276) {
          // Tap Clawd -> happy hearts!
          PLAY(SND_PET, CAT_CLICK);
          triggerOneShot(P_HEART, 2200);
        }
      } else if (inPrompt && tama.promptQ && !menuOpen && !settingsOpen && !resetOpen) {
        // Tah nahoru ukáže další volby, dolů předchozí
        if (te.type == TOUCH_SWIPE_UP)        qScroll(1);
        else if (te.type == TOUCH_SWIPE_DOWN) qScroll(-1);
      } else if (!menuOpen && !settingsOpen && !resetOpen && !tama.promptId[0]) {
        if (te.type == TOUCH_SWIPE_LEFT) {
          click(1900);
          if (displayMode == DISP_INFO) infoPage = (infoPage + 1) % INFO_PAGES;
          else if (displayMode == DISP_PET) petPage = (petPage + 1) % PET_PAGES;
          else { displayMode = (displayMode + 1) % DISP_COUNT; applyDisplayMode(); }
        } else if (te.type == TOUCH_SWIPE_RIGHT) {
          click(1900);
          if (displayMode == DISP_INFO) infoPage = (infoPage + INFO_PAGES - 1) % INFO_PAGES;
          else if (displayMode == DISP_PET) petPage = (petPage + 1) % PET_PAGES;
          else { displayMode = (displayMode + DISP_COUNT - 1) % DISP_COUNT; applyDisplayMode(); }
        } else if (te.type == TOUCH_SWIPE_UP && displayMode == DISP_NORMAL) {
          if (msgScroll < msgMaxScroll) msgScroll++;
        } else if (te.type == TOUCH_SWIPE_DOWN && displayMode == DISP_NORMAL) {
          if (msgScroll > 0) msgScroll--;
        }
      }
    }
  }

  // ─── Charging Desk Clock State ───────────────────────────────────────────
  clockRefreshRtc();
  // Hodiny: automaticky na USB po 20 s nečinnosti, nebo ručně z menu.
  // Práce agenta nebo dotaz je vždy ukončí.
  bool agentIdle = !tama.promptId[0] && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0;
  if (clockForced && !agentIdle) clockForced = false;
  bool clockScreenFree = (displayMode == DISP_NORMAL) && !chatExpanded
                      && !menuOpen && !settingsOpen && !resetOpen && agentIdle;
  bool autoClock = !dataDemo() && dataRtcValid() && _onUsb && (millis() - lastInteractMs > 20000);
  bool clocking = clockScreenFree && (clockForced || autoClock);
  clockShown = clocking;

  static bool wasClocking = false;
  if (clocking != wasClocking) {
    fullClearCanvas();
    buddyInvalidate();
    wasClocking = clocking;
  }

  buddySetCharacter(tama.connected && tama.agyClient && !dataDemo() ? CHAR_AGY : CHAR_CLAWD);

  ClawdSizeMode desiredMode = tama.promptId[0]
    ? CLAWD_PROMPT
    : ((clocking || displayMode != DISP_NORMAL) ? CLAWD_PEEK : CLAWD_FULL);
  buddySetMode(desiredMode);

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  static bool pairingShown = false;
  if (pk && !lastPasskey) { wake(); pairingShown = true; PLAY(SND_PIN, CAT_IMPORTANT); }
  // Úspěch hlásit jen po zobrazeném PINu (ne při každém znovupřipojení spárovaného PC)
  uint8_t auth = bleTakeAuthResult();
  if (auth == 1 && pairingShown) { PLAY(SND_PAIR_OK, CAT_IMPORTANT); pairingShown = false; }
  else if (auth == 2) { PLAY(SND_PAIR_FAIL, CAT_IMPORTANT); pairingShown = false; }
  if (!pk && lastPasskey) { fullClearCanvas(); buddyInvalidate(); }
  lastPasskey = pk;

  // ─── Frame Render into PSRAM Canvas & QSPI Flush ─────────────────────────
  // Celý snímek je 460 KB přes QSPI. Při dotyku, stisku a chvíli po interakci se
  // kreslí plnou rychlostí, jinak po 140 ms, kdy maskot mění pózu (buddy.cpp TICK_MS).
  // Bez maskota (hodiny, celoobrazovkový chat, obrazovka otázky, 5+ voleb)
  // se mění jen sekundy a blikání po 400 ms: stačí snímek na začátku každých 200 ms.
  static uint32_t lastFrameMs = 0;
  static uint32_t lastFrameSlot = 0;
  bool lively = te.touching || btnA.isPressed() || btnB.isPressed() || holdStartMs != 0
             || (millis() - lastInteractMs) < 1500;
  bool drawBuddy = !clocking && !(displayMode == DISP_NORMAL && chatExpanded && !inPrompt) && !promptHidesBuddy();
  uint32_t frameSlot = millis() / 200;
  bool frameDue = lively || (drawBuddy ? (millis() - lastFrameMs) >= 140   // = TICK_MS maskota
                                       : frameSlot != lastFrameSlot);
  if (!napping && !screenOff && frameDue) {
    lastFrameMs = millis();
    lastFrameSlot = frameSlot;
    if (pk) {
      drawPasskeyScreen();
    } else {
      // Maskot se nekreslí v celoobrazovkovém chatu, na stolních hodinách
      // ani u otázky s 5 a více volbami (místo zabírají řádky voleb). Kreslí
      // se před horní lištou, ta překryje, co z něj přesahuje nahoru.
      if (drawBuddy) buddyTick(activeState);
      drawTopStatusBar();

      if (clocking) drawDeskClock();
      else if (displayMode == DISP_INFO) drawInfoScreen();
      else if (displayMode == DISP_PET) drawPetScreen();
      else if (settings().hud || tama.promptId[0]) drawHUD();   // dotaz i „Odesláno…“ vždy
      else drawBottomNav();

      if (resetOpen)         drawModalList("RESET ZAŘÍZENÍ", RESET_N, resetSel, COL_HOT, fmtResetRow);
      else if (settingsOpen) drawModalList("NASTAVENÍ", SETTINGS_N, settingsSel, COL_CYAN, fmtSettingsRow);
      else if (menuOpen)     drawModalList("MENU ROZHODOVAČE", MENU_N, menuSel, characterPalette().body, fmtMenuRow);
    }
    hwFlush();
  }

  // Face-down nap detection. Čekající rozhodnutí spánek ukončí (jinak by se
  // během spánku nepřekreslovalo a na displeji by zůstal starý obsah) a nový
  // spánek během něj nezačne.
  static int8_t faceDownFrames = 0;
  if (isFaceDown()) { if (faceDownFrames < 20) faceDownFrames++; }
  else              { if (faceDownFrames > -10) faceDownFrames--; }
  if (!napping && !inPrompt && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    hwSetBrightness(10);
    dimmed = true;
    PLAY(SND_SLEEP, CAT_INFO);
  } else if (napping && (inPrompt || faceDownFrames <= -8)) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
    if (!inPrompt) PLAY(SND_WAKE, CAT_INFO);
  }

  // Na baterii po 45 s nečinnosti zhasnout (ne během dotazu ani na USB).
  // Hodiny zapnuté z menu nezhasnou, jen se ztlumí na nejnižší jas.
  if (!screenOff && !inPrompt && !_onUsb && (millis() - lastInteractMs > SCREEN_OFF_MS)) {
    if (clockShown && clockForced) {
      if (!dimmed && !napping) {
        hwSetBrightness(35);
        dimmed = true;
      }
    } else {
      screenOffNow();
    }
  }

  // Dotaz, který dlouho čeká (Claude Desktop nemá časový limit), na baterii po
  // 2 min a minutě bez dotyku ztlumí displej. Dotyk nebo tlačítko jas vrátí.
  if (inPrompt && !_onUsb && !screenOff && !dimmed && !napping
      && (millis() - promptArrivedMs) > 120000 && (millis() - lastInteractMs) > 60000) {
    hwSetBrightness(35);
    dimmed = true;
  }

  delay(screenOff ? 80 : 16);
}
