#!/usr/bin/env python3
"""
Rozhodovač — bridge daemon pro Antigravity CLI (AGY) a Claude Code v terminálu.

Automaticky se připojí ke krabičce Waveshare ESP32-S3-Touch-AMOLED-2.16:
  1) přes USB-C sériovou linku (ESP32-S3 USB VID 0x303A), pokud je připojená kabelem,
  2) nebo bezdrátově přes Bluetooth Low Energy (Nordic UART Service, zařízení 'Claude-*').
     Bez knihovny bleak jen přes USB.

Mluví stejným řádkovým JSON protokolem jako Claude Desktop Hardware Buddy, takže
v krabičce běží jediný firmware pro Claude i pro AGY. Hooky (agy_hook.py pro AGY,
claude_hook.py pro Claude Code) s daemonem mluví přes lokální HTTP na 127.0.0.1:18765.
"""

import argparse
import asyncio
import datetime
import getpass
import json
import logging
import logging.handlers
import math
import os
import socket
import sys
import threading
import time
from collections import defaultdict, deque
from pathlib import Path
from typing import Dict, List, Optional, Set

import serial
import serial.tools.list_ports

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    # Bez knihovny pro Bluetooth most funguje jen přes USB kabel
    BleakClient = BleakScanner = None

from rozhodovac_common import TOKEN_HEADER, read_state, write_state, write_token

NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX_UUID      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
ESP32_USB_VID    = 0x303A

# Firmware čte jeden JSON řádek do 4 KB (starší verze do 2 KB) a delší řádek
# zahodí celý, i s případným promptem. Snapshot proto držíme pod touto mezí.
# Firmware s obrazovkou otázky hlásí ve statusu "ui": 2 a bere řádek do 4 KB.
MAX_LINE_BYTES = 1900
MAX_LINE_BYTES_UI2 = 3800

# Limity bufferů firmwaru v bajtech (src/data.h, TamaState) bez koncové nuly.
# Text se zkracuje po celých znacích UTF-8, firmware by jinak rozřízl češtinu.
FW_TOOL_BYTES = 63
FW_HINT_BYTES = 319
FW_OPT_BYTES = 63
# Obrazovka otázky (firmware "ui": 2): popisek volby na 2 řádky, popis, záhlaví
FW_QOPT_BYTES = 95
FW_DESC_BYTES = 191
FW_HEAD_BYTES = 63
FW_OK_BYTES = 23
FW_MAX_OPTS = 8

# Otázky Claude Code (AskUserQuestion) jdou na krabičku po jedné, na konci
# shrnutí. Na každý krok je doba z hooku (60 s), celé dotazování nejvýš:
QUESTIONS_TOTAL_MAX_S = 540

# Spojení s krabičkou platí, jen když z ní něco chodí: na status odpovídá
# s každým heartbeatem (8 s), po 40 s ticha (5 ztracených odpovědí) se
# spojení zavře a hledá znovu.
RX_SILENCE_S = 40.0
# Port, na kterém Rozhodovač neodpověděl nebo který nešel otevřít, se na tuto
# dobu vynechá (krabička mohla teprve startovat, port mohl držet monitor)
BAD_PORT_RETRY_S = 30.0
# Zápis na USB nesmí viset: zamrzlý firmware přestane číst a Windows by zápis
# blokovaly navždy (i kontrolu ticha v heartbeatu)
USB_WRITE_TIMEOUT_S = 2.0
# Po výpadku spojení se čekající dotazy vrátí terminálu, pokud se spojení
# do této doby neobnoví (jinak by hook čekal celý limit 45/90 s).
RECONNECT_GRACE_S = 5.0

# AGY hook nedodává skutečnou spotřebu tokenů, počítá se hrubý odhad na nástroj.
# U Claude Code se tokeny čtou ze záznamu relace (transcript_path).
TOKENS_PER_TOOL_ESTIMATE = 800

# Relace, ze které 15 min nepřišla žádná událost, se nepočítá jako pracující:
# Claude Code po přerušení (Esc) událost Stop nepošle
RUNNING_STALE_S = 900
# Kolik záznamů relací (transcriptů) si daemon pamatuje kvůli počítání tokenů
MAX_TRANSCRIPTS = 64

TOOL_LABELS_CZ = {
    "run_command": "Příkaz",
    "write_file": "Zápis",
    "edit_file": "Úprava",
    "replace_file_content": "Úprava",
    "multi_replace_file_content": "Úprava",
    "delete_file": "Smazání",
    "read_file": "Čtení",
    "view_file": "Čtení",
    "manage_task": "Úloha",
    "search_web": "Hledání",
}

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 18765
MAX_BODY_BYTES = 64 * 1024
ALLOWED_HOSTNAMES = {"127.0.0.1", "localhost", "[::1]"}

BRIDGE_DIR = Path(__file__).resolve().parent
LOG_PATH = BRIDGE_DIR / "daemon.log"
PID_PATH = BRIDGE_DIR / "daemon.pid"

_handlers = [logging.handlers.RotatingFileHandler(LOG_PATH, maxBytes=1_000_000, backupCount=2, encoding="utf-8")]
if sys.stderr is not None:  # pod pythonw.exe stderr neexistuje
    _handlers.append(logging.StreamHandler(sys.stderr))
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s", handlers=_handlers)
log = logging.getLogger("rozhodovac")


def utf8_trunc(text: str, max_bytes: int) -> str:
    """Zkrátí text na nejvýš max_bytes bajtů UTF-8 bez rozříznutí znaku."""
    raw = text.encode("utf-8")
    if len(raw) <= max_bytes:
        return text
    return raw[:max_bytes].decode("utf-8", errors="ignore")


def utf8_trunc_ell(text: str, max_bytes: int) -> str:
    """Jako utf8_trunc, zkrácený text končí „…“."""
    if len(text.encode("utf-8")) <= max_bytes:
        return text
    if max_bytes < 4:
        return ""
    return utf8_trunc(text, max_bytes - 3).rstrip() + "…"


def encode_line(obj: dict) -> bytes:
    """JSON řádek v UTF-8. Bez \\uXXXX escapů je česká zpráva až třikrát kratší."""
    return (json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


# Šířka řádku v kompaktním okně chatu na krabičce (MAX_COLS v drawHUD firmwaru).
# Celoobrazovkový chat je širší, co se vejde sem, vejde se i tam.
CHAT_COLS = 33


def wrapped_rows(text: str, cols: int = CHAT_COLS) -> int:
    """Počet řádků po zalomení po slovech stejně jako wrapUtf8 ve firmwaru."""
    rows = 0
    col = 0
    for word in text.split():
        if col > 0 and col + 1 + len(word) > cols:
            rows += 1
            col = 0
        if col > 0:
            col += 1
        for _ in word:
            if col >= cols:
                rows += 1
                col = 0
            col += 1
    return rows + (1 if col > 0 else 0)


def fit_rows(text: str, max_rows: int = 2, cols: int = CHAT_COLS) -> str:
    """Zkrátí text tak, aby po zalomení na krabičce zabral nejvýš max_rows řádků."""
    text = " ".join(text.split())
    if wrapped_rows(text, cols) <= max_rows:
        return text
    text = text[: max_rows * cols]
    for cut in range(len(text) - 1, 0, -1):
        candidate = text[:cut].rstrip() + "…"
        if wrapped_rows(candidate, cols) <= max_rows:
            return candidate
    return text[: cols - 1] + "…"


class BuddyDaemon:
    def __init__(self, prefer_port: Optional[str] = None, ble_only: bool = False, usb_only: bool = False):
        self.prefer_port = prefer_port
        self.ble_only = ble_only
        self.usb_only = usb_only

        self.transport: Optional[str] = None  # "usb" | "ble" | None
        self.device_label: Optional[str] = None
        self._ser: Optional[serial.Serial] = None
        self._ble: Optional[BleakClient] = None
        self._write_lock = asyncio.Lock()
        self._tasks: Set[asyncio.Task] = set()
        # Snímky se slučují (viz push_snapshot): požadovaná a odeslaná generace
        self._snap_req = 0
        self._snap_done = 0
        self._snap_task: Optional[asyncio.Task] = None
        self._snap_evt: Optional[asyncio.Event] = None

        # Session tracking
        self.sessions: Dict[str, dict] = {}
        self.entries: deque = deque(maxlen=16)
        self.msg: str = "Most připraven"
        self.tokens_total: int = 0
        self.tokens_today: int = 0
        self._tokens_day = datetime.date.today()
        self.recently_completed: bool = False

        # Pending approval prompts: id -> {"created", "deadline", "tool", "hint", "options", "optA", "optB", "future"}
        self.pending_prompts: Dict[str, dict] = {}
        self.active_prompt_id: Optional[str] = None
        self._prompt_seq = 0
        self._approve_lock = asyncio.Lock()

        # Ack / status tracking from device
        self.device_status: dict = {}
        self._ack_waiters: Dict[str, asyncio.Future] = {}
        self._ack_locks: Dict[str, asyncio.Lock] = defaultdict(asyncio.Lock)
        self._rx_buf = bytearray()

        # Spojení je ověřené, až když krabička odpoví na status (na USB se
        # jinak tváří jako připojená jakákoli deska ESP32-S3)
        self._verified = False
        self._last_rx = 0.0
        # Porty, které nejsou Rozhodovač nebo nejdou otevřít: port -> čas, do kdy
        # se vynechá (BAD_PORT_RETRY_S, nebo dřív, když port zmizí)
        self._bad_ports: Dict[str, float] = {}
        self._fallback_task: Optional[asyncio.Task] = None

        # Token lokálního HTTP (nastaví main_async po obsazení portu)
        self.token: str = ""

        # Klient, jehož maskot a štítek krabička ukazuje: "agy" nebo "claude"
        # (Claude Code v terminálu). Bere se z dotazu na krabičce, jinak z naposledy
        # aktivní relace.
        self.last_client: str = "claude" if read_state().get("client") == "claude" else "agy"
        # Počítání tokenů Claude Code ze záznamů relací: cesta -> {"offset", "msgs"}
        self._transcripts: Dict[str, dict] = {}
        self._tx_lock = threading.Lock()
        self._event_seq = 0

    def _next_seq(self) -> int:
        self._event_seq += 1
        return self._event_seq

    def _set_client(self, client: str) -> None:
        """Zapamatuje klienta i pro příští start mostu (maskot hned po připojení)."""
        if client != self.last_client:
            self.last_client = client
            # Zápis v jiném vlákně: při souběžném čtení hookem na Windows
            # se přejmenování opakuje a smyčka událostí by stála. Zapisuje se
            # vždy aktuální hodnota, takže pozdější zápis nepřepíše novější stav.
            self._spawn(asyncio.to_thread(self._write_client))

    def _write_client(self) -> None:
        try:
            write_state(client=self.last_client)
        except Exception as e:
            log.debug("state.json: %s", e)

    def _spawn(self, coro) -> None:
        """Spustí úlohu na pozadí a drží na ni referenci, aby ji nesebral GC."""
        task = asyncio.create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    def _roll_day(self) -> None:
        """Po půlnoci začne denní počítadlo od nuly (i když zatím nic nepřibylo)."""
        today = datetime.date.today()
        if today != self._tokens_day:
            self._tokens_day = today
            self.tokens_today = 0

    def _add_tokens(self, n: int) -> None:
        self._roll_day()
        self.tokens_total += n
        self.tokens_today += n

    # ─── Outgoing JSON Wire Transport ─────────────────────────────────────────
    async def send_json(self, obj: dict) -> bool:
        line = encode_line(obj)
        async with self._write_lock:
            if self.transport == "usb" and self._ser and self._ser.is_open:
                try:
                    # Bez flush: na Windows čeká na odvysílání bez časového limitu
                    await asyncio.to_thread(self._ser.write, line)
                    return True
                except Exception as e:
                    log.warning("USB write error: %s", e)
                    self._close_usb()
                    return False
            elif self.transport == "ble" and self._ble and self._ble.is_connected:
                try:
                    mtu = getattr(self._ble, "mtu_size", 185) or 185
                    chunk_sz = max(20, min(180, mtu - 3))
                    for i in range(0, len(line), chunk_sz):
                        # Zaseknutý zápis by držel zámek a zablokoval všechno ostatní
                        await asyncio.wait_for(
                            self._ble.write_gatt_char(NUS_RX_UUID, line[i:i + chunk_sz], response=False),
                            timeout=2.0)
                        if len(line) > chunk_sz:
                            await asyncio.sleep(0.008)
                    return True
                except Exception as e:
                    log.warning("BLE write error: %s", e)
                    return False
        return False

    def _link_open(self) -> bool:
        if self.transport == "usb":
            return bool(self._ser and self._ser.is_open)
        if self.transport == "ble":
            return bool(self._ble and self._ble.is_connected)
        return False

    def is_connected(self) -> bool:
        """Spojení je otevřené, krabička se ozvala a nemlčí déle než RX_SILENCE_S."""
        return self._link_open() and self._verified and (time.time() - self._last_rx) < RX_SILENCE_S

    # ─── Incoming Line Handling from Device ───────────────────────────────────
    def _feed_bytes(self, data: bytes):
        for b in data:
            if b in (10, 13):  # \n or \r
                if self._rx_buf:
                    raw = bytes(self._rx_buf).decode("utf-8", errors="replace").strip()
                    self._rx_buf.clear()
                    if raw:
                        self._last_rx = time.time()
                        self._handle_device_line(raw)
            else:
                if len(self._rx_buf) < 8192:
                    self._rx_buf.append(b)

    def _handle_device_line(self, line: str):
        if not line.startswith("{"):
            log.debug("[dev] %s", line)
            return
        try:
            doc = json.loads(line)
        except Exception:
            log.debug("[dev non-json] %s", line)
            return
        if not isinstance(doc, dict):
            return

        # Rozhodnutí z dotykového displeje nebo tlačítek. U otázek je decision
        # text zvolené možnosti a opt její pořadí (od 0), jinak "once" (schválit)
        # nebo "deny". Krabička čeká na potvrzení, bez něj tlačítka vrátí.
        if doc.get("cmd") == "permission":
            pid = str(doc.get("id", ""))
            dec = str(doc.get("decision", "deny"))
            opt = doc.get("opt")
            log.info("Decision from Rozhodovac: id=%s decision=%s opt=%s", pid, dec, opt)
            item = self.pending_prompts.pop(pid, None)
            accepted = bool(item and not item["future"].done())
            if item and not accepted:
                self.pending_prompts[pid] = item   # už vyřízený jinde, úklid udělá jeho čekatel
            chain = bool(item and item.get("chain"))
            if accepted:
                options = item.get("options") or []
                if item.get("q"):
                    # Obrazovka otázky: rozhodnutí, pořadí volby, u vícenásobného
                    # výběru pořadí zaškrtnutých voleb (vyhodnotí čekající dotaz)
                    opts = doc.get("opts")
                    res = {
                        "decision": dec,
                        "opt": opt if isinstance(opt, int) and not isinstance(opt, bool) else None,
                        "opts": [o for o in opts if isinstance(o, int) and not isinstance(o, bool)]
                                if isinstance(opts, list) else None,
                    }
                elif isinstance(opt, int) and not isinstance(opt, bool) and 0 <= opt < len(options):
                    # Pořadí je jednoznačné i u zkrácených textů voleb
                    res = options[opt]
                    item["chosen"] = opt
                else:
                    res = "allow" if dec in ("once", "allow") else dec
                item["future"].set_result(res)
            self._spawn(self.send_json({"ack": "permission", "ok": accepted, "id": pid}))
            if accepted and chain:
                # Krok dotazování: další krok (nebo konec) ukáže čekající dotaz sám,
                # mezitím se nesmí ukázat jiný dotaz z fronty
                if self.active_prompt_id == pid:
                    self.active_prompt_id = None
                return
            if self.active_prompt_id == pid:
                self.active_prompt_id = next(iter(self.pending_prompts), None)
            self._spawn(self.push_snapshot())
            return

        # Command acknowledgements / status telemetry
        ack = doc.get("ack")
        if ack:
            if ack == "status" and isinstance(doc.get("data"), dict):
                self.device_status = doc["data"]
            fut = self._ack_waiters.pop(ack, None)
            if fut and not fut.done():
                fut.set_result(doc)

    # ─── Snapshot & Initial Handshake ─────────────────────────────────────────
    def _build_snapshot(self) -> dict:
        now = time.time()
        # Expire stale sessions (>30 min inactive)
        stale = [k for k, v in self.sessions.items() if now - v.get("updated", now) > 1800]
        for k in stale:
            self.sessions.pop(k, None)

        total = len(self.sessions)
        running = sum(1 for v in self.sessions.values()
                      if v.get("running") and now - v.get("updated", now) < RUNNING_STALE_S)
        waiting = len(self.pending_prompts)

        entries = list(self.entries)
        self._roll_day()
        snap = {
            "total": max(total, 1 if waiting > 0 else 0),
            "running": running,
            "waiting": waiting,
            "completed": self.recently_completed,
            "msg": self.msg,
            "entries": entries,
            "tokens": self.tokens_total,
            "tokens_today": self.tokens_today,
        }
        if self._current_client() == "agy":
            # Firmware podle "src" pozná, že je připojený AGY (maskot Agy, štítek AGY).
            # Bez něj ukáže Claude (maskot Clawd), stejně jako u Claude Desktopu.
            snap["src"] = "agy"

        if self.active_prompt_id and self.active_prompt_id in self.pending_prompts:
            pr = self.pending_prompts[self.active_prompt_id]
            pr_data = {
                "id": self.active_prompt_id,
                "tool": pr["tool"],
                "hint": pr["hint"],
                # Krabička ukazuje odpočet a připomínky rozvrhne podle doby na odpověď;
                # dotaz, který se vrací po vyřízení jiného, si podle age zachová stáří
                "ttl": max(0, int(pr["deadline"] - now)),
                "age": max(0, int(now - pr["created"])),
            }
            if pr.get("danger"):
                # Krabička kritickou akci schvaluje podržením
                pr_data["danger"] = True
            if pr.get("options"):
                pr_data["options"] = pr["options"]
            if pr.get("optA"):
                pr_data["optA"] = pr["optA"]
            if pr.get("optB"):
                pr_data["optB"] = pr["optB"]
            if pr.get("q"):
                pr_data["q"] = 1
                for key in ("head", "ok"):
                    if pr.get(key):
                        pr_data[key] = pr[key]
                for key in ("multi", "back", "term", "cont"):
                    if pr.get(key):
                        pr_data[key] = True
                if pr.get("sel"):
                    pr_data["sel"] = pr["sel"]
                if any(pr.get("descs") or []):
                    pr_data["descs"] = list(pr["descs"])
            snap["prompt"] = pr_data

        # Přednost má prompt; kvůli limitu řádku firmwaru ubíráme nejstarší zprávy,
        # u otázky s dlouhými popisy voleb nakonec zkracujeme popisy.
        limit = MAX_LINE_BYTES_UI2 if self._fw_ui2() else MAX_LINE_BYTES
        while entries and len(encode_line(snap)) > limit:
            entries.pop()
        descs = snap.get("prompt", {}).get("descs")
        for max_b in (120, 60, 0):
            if not descs or len(encode_line(snap)) <= limit:
                break
            descs[:] = [utf8_trunc(d, max_b) for d in descs]
        return snap

    def _fw_ui2(self) -> bool:
        """Firmware umí obrazovku otázky (status "ui": 2)."""
        try:
            return int(self.device_status.get("ui", 0)) >= 2
        except (TypeError, ValueError):
            return False

    def _current_client(self) -> str:
        """Klient pro maskot a štítek: dotaz na krabičce, jinak naposledy aktivní relace."""
        if self.active_prompt_id and self.active_prompt_id in self.pending_prompts:
            return self.pending_prompts[self.active_prompt_id].get("client", "agy")
        if self.sessions:
            # Pořadí události, ne čas: na Windows mají dvě rychlé události stejné razítko
            latest = max(self.sessions.values(), key=lambda v: v.get("seq", 0))
            return latest.get("client", "agy")
        return self.last_client

    async def push_snapshot(self, wait: float = 6.0):
        """Pošle krabičce aktuální stav. Žádosti se slučují: snímky posílá jedna
        úloha a vždy s nejnovějším stavem, takže se při rychlých událostech
        (po BLE trvá velký snímek desítky ms) nehromadí fronta zastaralých
        snímků před dotazem a potvrzením. Volající počká (nejvýš wait s), až
        odejde snímek sestavený po jeho žádosti."""
        if not self._link_open():
            return
        self._snap_req += 1
        mine = self._snap_req
        if self._snap_evt is None:
            self._snap_evt = asyncio.Event()
        if self._snap_task is None or self._snap_task.done():
            # Vlastní úloha: zrušení volajícího (hook se odpojil) neusekne řádek v půlce
            self._snap_task = asyncio.create_task(self._snap_sender())
        deadline = time.monotonic() + wait
        while self._snap_done < mine and not self._snap_task.done():
            left = deadline - time.monotonic()
            if left <= 0:
                return
            try:
                await asyncio.wait_for(asyncio.shield(self._snap_evt.wait()), timeout=left)
            except asyncio.TimeoutError:
                return

    async def _snap_sender(self) -> None:
        while self._snap_done < self._snap_req and self._link_open():
            gen = self._snap_req
            try:
                snap = self._build_snapshot()
                sent = await self.send_json(snap)
            except Exception as e:
                log.warning("Snímek se nepodařilo odeslat: %s", e)
                break
            # Příznak „hotovo“ se smaže až po odeslání, jinak by se při chybě zápisu ztratil
            if sent and snap.get("completed"):
                self.recently_completed = False
            self._snap_done = gen
            evt, self._snap_evt = self._snap_evt, asyncio.Event()
            if evt:
                evt.set()
        # Spojení spadlo: čekající volající se uvolní
        self._snap_done = self._snap_req
        evt, self._snap_evt = self._snap_evt, asyncio.Event()
        if evt:
            evt.set()

    async def _verify_device(self) -> bool:
        """Krabička musí odpovědět na status. Jiná deska na USB nebo zamrzlý firmware ne."""
        for _ in range(2):
            res = await self._send_cmd_wait_ack({"cmd": "status"}, "status", timeout=2.5)
            if res.get("ok"):
                self._verified = True
                self._last_rx = time.time()
                return True
        return False

    def _on_link_lost(self) -> None:
        """Spojení spadlo: když se do RECONNECT_GRACE_S neobnoví, čekající dotazy
        se vrátí terminálu, ať hook nečeká celý limit."""
        self._verified = False
        self.device_status = {}
        if self.pending_prompts and (self._fallback_task is None or self._fallback_task.done()):
            self._fallback_task = asyncio.create_task(self._fallback_after_grace())

    async def _fallback_after_grace(self) -> None:
        await asyncio.sleep(RECONNECT_GRACE_S)
        if self.is_connected():
            return
        self._resolve_all_pending("fallback")

    def _resolve_all_pending(self, decision: str) -> None:
        for item in list(self.pending_prompts.values()):
            if not item["future"].done():
                item["future"].set_result(decision)

    async def _on_connected_handshake(self):
        # Sync local clock + timezone offset (vč. letního času)
        epoch = int(time.time())
        tz_off = time.localtime(epoch).tm_gmtoff
        await self.send_json({"time": [epoch, tz_off]})
        await asyncio.sleep(0.08)
        owner = os.environ.get("ROZHODOVAC_OWNER") or os.environ.get("SCHVALOVAC_OWNER") or getpass.getuser()
        await self.send_json({"cmd": "owner", "name": owner})
        await asyncio.sleep(0.08)
        await self.send_json({"cmd": "status"})
        await asyncio.sleep(0.08)
        await self.push_snapshot()

    # ─── USB & BLE Connection Management ──────────────────────────────────────
    def _mark_bad_port(self, port: str) -> None:
        self._bad_ports[port] = time.time() + BAD_PORT_RETRY_S

    def _find_usb_port(self) -> Optional[str]:
        ports = [p.device for p in serial.tools.list_ports.comports()
                 if p.vid == ESP32_USB_VID or p.device == self.prefer_port]
        now = time.time()
        # Port, který zmizel nebo kterému vypršelo vynechání, se zkusí znovu
        self._bad_ports = {p: t for p, t in self._bad_ports.items() if p in ports and t > now}
        # Zadaný port (--port) jen když opravdu existuje, jinak se zkusí BLE
        candidates = [self.prefer_port] if self.prefer_port else ports
        for dev in candidates:
            if dev in ports and dev not in self._bad_ports:
                return dev
        return None

    def _close_usb(self):
        was_open = self._ser is not None
        if self._ser:
            try:
                self._ser.close()
            except Exception:
                pass
        self._ser = None
        if self.transport == "usb":
            self.transport = None
            self.device_label = None
            if was_open:
                self._on_link_lost()

    async def _run_usb_session(self, port_name: str):
        log.info("Connecting to Rozhodovac over USB Serial (%s)...", port_name)
        ser = serial.Serial()
        ser.port = port_name
        ser.baudrate = 115200
        ser.timeout = 0.2
        ser.write_timeout = USB_WRITE_TIMEOUT_S
        ser.dtr = True
        ser.rts = False
        try:
            await asyncio.to_thread(ser.open)
        except Exception as e:
            # Port drží jiný program (sériový monitor, PlatformIO): zkusí se BLE
            log.warning("USB port %s nejde otevřít (%s), zkouším BLE", port_name, e)
            self._mark_bad_port(port_name)
            return
        self._ser = ser
        self.transport = "usb"
        self.device_label = port_name
        self._rx_buf.clear()
        self._verified = False

        # Čtení v jednom vlákně s blokujícím read(timeout), data jdou do smyčky asyncio
        loop = asyncio.get_running_loop()
        ended = asyncio.Event()
        stop = threading.Event()

        def _reader():
            try:
                while not stop.is_set():
                    data = ser.read(ser.in_waiting or 1)
                    if data:
                        loop.call_soon_threadsafe(self._feed_bytes, data)
            except Exception as e:
                log.warning("USB session ended (%s): %s", port_name, e)
            finally:
                loop.call_soon_threadsafe(ended.set)

        threading.Thread(target=_reader, name="usb-reader", daemon=True).start()
        try:
            await asyncio.sleep(0.2)
            if not await self._verify_device():
                log.warning("Na %s neodpovídá Rozhodovač (status), port %.0f s vynechávám", port_name, BAD_PORT_RETRY_S)
                self._mark_bad_port(port_name)
                return
            log.info("Connected over USB (%s)", port_name)
            await self._on_connected_handshake()
            await ended.wait()
        finally:
            stop.set()
            self._close_usb()

    async def _run_ble_session(self):
        log.debug("Scanning BLE for Claude-* / NUS...")
        devices = await BleakScanner.discover(timeout=3.5, return_adv=True)
        # Po prvním ověřeném spojení se daemon připojuje jen ke stejné krabičce:
        # cizí zařízení se stejným jménem v dosahu by jinak mohlo schvalovat.
        # Jinou krabičku povolí `rozhodovac_ctl.py forget-device`.
        known = str(read_state().get("ble_address") or "").upper()
        target_dev = None
        for dev, adv in devices.values():
            name = dev.name or adv.local_name or ""
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            if known:
                if dev.address.upper() == known:
                    target_dev = dev
                    break
            elif name.startswith("Claude") or NUS_SERVICE_UUID in uuids:
                target_dev = dev
                break

        if not target_dev:
            return

        log.info("Connecting to Rozhodovac over BLE: %s (%s)...", target_dev.name, target_dev.address)
        disc_event = asyncio.Event()

        def _on_disc(_client):
            disc_event.set()

        async with BleakClient(target_dev, disconnected_callback=_on_disc, timeout=15.0) as client:
            self._ble = client
            self.transport = "ble"
            self.device_label = f"{target_dev.name or 'Claude'} ({target_dev.address})"
            self._rx_buf.clear()
            self._verified = False

            def _notify_cb(_sender, data: bytearray):
                self._feed_bytes(bytes(data))

            try:
                # Notifikace vyžadují šifrované spojení: odpověď na status
                # znamená spárovanou krabičku
                await client.start_notify(NUS_TX_UUID, _notify_cb)
                if not await self._verify_device():
                    log.warning("BLE zařízení %s neodpovídá jako Rozhodovač", self.device_label)
                    return
                if not known:
                    write_state(ble_address=target_dev.address)
                    log.info("Krabička %s zapamatována", target_dev.address)
                log.info("Connected over BLE to %s", self.device_label)
                await self._on_connected_handshake()

                ticks = 0
                while client.is_connected and not disc_event.is_set():
                    await asyncio.sleep(0.5)
                    ticks += 1
                    # USB má přednost: po zapojení kabelu se přepne
                    if not self.ble_only and ticks % 10 == 0 and self._find_usb_port():
                        log.info("Krabička je na USB, přepínám z BLE")
                        break
                    if not self.is_connected() and self._verified is False:
                        break
            finally:
                self._ble = None
                if self.transport == "ble":
                    self.transport = None
                    self.device_label = None
                    self._on_link_lost()

        log.info("BLE disconnected.")

    async def connection_loop(self):
        if BleakScanner is None and not self.usb_only:
            log.warning("Knihovna bleak není nainstalovaná, krabičku hledám jen na USB "
                        "(Bluetooth: python -m pip install bleak)")
        while True:
            try:
                if not self.ble_only:
                    usb_port = self._find_usb_port()
                    if usb_port:
                        await self._run_usb_session(usb_port)
                        await asyncio.sleep(1.0)
                        continue
                if not self.usb_only and BleakScanner is not None:
                    await self._run_ble_session()
            except Exception as e:
                log.debug("Connection attempt failed: %s", e)
            await asyncio.sleep(2.5)

    async def heartbeat_loop(self):
        ticks = 0
        while True:
            await asyncio.sleep(8.0)
            if self._link_open() and self._verified and time.time() - self._last_rx >= RX_SILENCE_S:
                # Krabička mlčí (zamrzlý firmware, výpadek): spojení zavřít a hledat znovu
                log.warning("Krabička %.0f s neodpovídá, zavírám spojení", time.time() - self._last_rx)
                self._verified = False
                if self.transport == "usb":
                    self._close_usb()
                elif self._ble:
                    try:
                        await asyncio.wait_for(self._ble.disconnect(), timeout=3.0)
                    except Exception:
                        pass
                continue
            if self.is_connected():
                # Na pozadí: zaseknuté odesílání nesmí zastavit kontrolu ticha
                self._spawn(self.push_snapshot())
                ticks += 1
                # Status s každým heartbeatem: jedna ztracená odpověď neznamená odpojení
                self._spawn(self.send_json({"cmd": "status"}))

    async def _send_cmd_wait_ack(self, payload: dict, ack_name: str, timeout: float = 5.0) -> dict:
        # Stejné příkazy po sobě: ack nemá id, souběžný druhý by přepsal čekatele prvního
        async with self._ack_locks[ack_name]:
            loop = asyncio.get_running_loop()
            fut = loop.create_future()
            self._ack_waiters[ack_name] = fut
            ok = await self.send_json(payload)
            if not ok:
                self._ack_waiters.pop(ack_name, None)
                return {"ok": False, "error": "not connected"}
            try:
                return await asyncio.wait_for(fut, timeout=timeout)
            except asyncio.TimeoutError:
                self._ack_waiters.pop(ack_name, None)
                return {"ok": False, "error": f"timeout waiting for ack:{ack_name}"}

    # ─── Local HTTP Server (127.0.0.1:18765) ──────────────────────────────────
    def _token_ok(self, headers: Dict[str, str]) -> bool:
        return bool(self.token) and secrets_compare(headers.get(TOKEN_HEADER.lower(), ""), self.token)

    @staticmethod
    def _request_allowed(headers: Dict[str, str]) -> bool:
        """Pustí jen lokální klienty (hook, ovládací skript).

        Prohlížeč posílá u požadavků z webové stránky hlavičku Origin, takže ji
        odmítáme: jinak by libovolná stránka mohla daemonu posílat příkazy
        (CSRF). Kontrola Host brání DNS rebindingu.
        """
        if "origin" in headers:
            return False
        host = headers.get("host", "")
        hostname = host.rsplit(":", 1)[0] if not host.endswith("]") else host
        return hostname in ALLOWED_HOSTNAMES

    async def handle_http(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        status = "200 OK"
        try:
            req_line = await asyncio.wait_for(reader.readline(), timeout=5.0)
            if not req_line:
                return
            parts = req_line.decode("utf-8", errors="replace").strip().split(" ")
            method = parts[0] if len(parts) > 0 else "GET"
            path = parts[1] if len(parts) > 1 else "/"

            headers: Dict[str, str] = {}
            while True:
                hline = await asyncio.wait_for(reader.readline(), timeout=5.0)
                if not hline or hline in (b"\r\n", b"\n"):
                    break
                name, _, value = hline.decode("utf-8", errors="replace").partition(":")
                headers[name.strip().lower()] = value.strip()

            if not self._request_allowed(headers):
                status, resp = "403 Forbidden", {"ok": False, "error": "forbidden"}
            elif path != "/health" and not self._token_ok(headers):
                # /health bez tokenu jen řekne, že daemon běží (hook i ctl token posílají)
                status, resp = "401 Unauthorized", {"ok": False, "error": "bad token"}
            else:
                content_len = int(headers.get("content-length", "0") or 0)
                if content_len > MAX_BODY_BYTES:
                    status, resp = "413 Payload Too Large", {"ok": False, "error": "body too large"}
                else:
                    body = {}
                    if content_len > 0:
                        raw_body = await asyncio.wait_for(reader.readexactly(content_len), timeout=5.0)
                        body = json.loads(raw_body.decode("utf-8", errors="replace"))
                        if not isinstance(body, dict):
                            body = {}
                    if path == "/health" and not self._token_ok(headers):
                        resp = {"ok": True}
                    elif path == "/approve":
                        # Hook, který skončí dřív (AGY zrušil nástroj, zabil hook),
                        # zavře spojení: dotaz se pak z krabičky hned stáhne
                        work = asyncio.create_task(self._dispatch_api(method, path, body))
                        gone = asyncio.create_task(reader.read(1))
                        done, _ = await asyncio.wait({work, gone}, return_when=asyncio.FIRST_COMPLETED)
                        if work not in done:
                            work.cancel()
                            try:
                                await work
                            except asyncio.CancelledError:
                                pass
                            log.info("Hook se odpojil, dotaz stažen z krabičky")
                            return
                        gone.cancel()
                        resp = work.result()
                    else:
                        resp = await self._dispatch_api(method, path, body)

            payload = json.dumps(resp, ensure_ascii=False).encode("utf-8")
            header = (
                f"HTTP/1.1 {status}\r\n"
                "Content-Type: application/json; charset=utf-8\r\n"
                f"Content-Length: {len(payload)}\r\n"
                "Connection: close\r\n\r\n"
            ).encode("ascii")
            writer.write(header + payload)
            await writer.drain()
        except Exception as e:
            log.debug("HTTP handler error: %s", e)
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass

    async def _dispatch_api(self, method: str, path: str, body: dict) -> dict:
        if path == "/health":
            return {
                "ok": True,
                "connected": self.is_connected(),
                "transport": self.transport,
                "device": self.device_label,
                "sessions": len(self.sessions),
                "waiting": len(self.pending_prompts),
                "tokens": self.tokens_total,
                "tokensToday": self.tokens_today,
                "deviceStatus": self.device_status,
            }

        if path == "/event" and method == "POST":
            return await self._handle_event(body)

        if path == "/approve" and method == "POST":
            return await self._handle_approve(body)

        if path == "/cmd" and method == "POST":
            action = body.get("action")
            if action == "raw":
                raw = body.get("payload", {})
                if not isinstance(raw, dict):
                    return {"ok": False, "error": "payload must be an object"}
                ack = body.get("ack")
                if ack:
                    return await self._send_cmd_wait_ack(raw, str(ack), 5.0)
                ok = await self.send_json(raw)
                return {"ok": ok}
            if action == "shutdown":
                # Čekající dotazy vrátit terminálu, spojení s krabičkou řádně zavřít
                self._resolve_all_pending("fallback")
                await asyncio.sleep(0.1)
                self._close_usb()
                if self._ble:
                    try:
                        await asyncio.wait_for(self._ble.disconnect(), timeout=2.0)
                    except Exception:
                        pass
                PID_PATH.unlink(missing_ok=True)
                asyncio.get_running_loop().call_later(0.1, os._exit, 0)
                return {"ok": True}

        return {"ok": False, "error": "unknown endpoint"}

    async def _handle_event(self, body: dict) -> dict:
        if body.get("client") == "claude":
            return await self._handle_claude_event(body)
        cid = body.get("conversationId") or "default"
        ev = body.get("event") or ""
        now = time.time()
        sess = self.sessions.setdefault(cid, {"running": False, "updated": now})
        sess["updated"] = now
        sess["client"] = "agy"
        sess["seq"] = self._next_seq()
        self._set_client("agy")

        stamp = time.strftime("%H:%M")
        if ev == "SessionStart":
            sess["running"] = False
            ws = body.get("workspace") or "AGY"
            self.msg = f"AGY: {ws}"
            self.entries.appendleft(f"{stamp} nová relace ({ws})")
        elif ev == "PreInvocation":
            sess["running"] = True
            user_in = (body.get("lastUserInput") or "").strip().replace("\n", " ")
            if user_in:
                # Zadání vyplní právě dva řádky okna chatu, delší dostane „…“
                self.entries.appendleft(fit_rows(f"{stamp} > {user_in}"))
            self.msg = "AGY přemýšlí…"
        elif ev == "PreToolUse":
            # Nástroj, který prošel bez rozhodování: do historie jednou, před během
            sess["running"] = True
            tool = body.get("tool") or "tool"
            t_cz = TOOL_LABELS_CZ.get(tool, tool)
            summary = body.get("summary") or ""
            line = f"{stamp} {t_cz}: {summary}" if summary else f"{stamp} {t_cz}"
            self.entries.appendleft(line[:90])
            self.msg = line[:64]
        elif ev == "QuestionInTerminal":
            sess["running"] = True
            summary = (body.get("summary") or "").strip()
            line = f"{stamp} Otázka v terminálu: {summary}" if summary else f"{stamp} Otázka v terminálu"
            self.entries.appendleft(line[:90])
            self.msg = "Agent se ptá v terminálu"
        elif ev == "PostToolUse":
            sess["running"] = True
            self._add_tokens(TOKENS_PER_TOOL_ESTIMATE)
        elif ev == "Stop":
            sess["running"] = False
            self.recently_completed = True
            self.msg = "Hotovo – čeká na zadání"
        # Hook AGY čeká na odpověď synchronně: snímek odejde na pozadí
        self._spawn(self.push_snapshot())
        return {"ok": True, "connected": self.is_connected()}

    # ─── Claude Code v terminálu ───────────────────────────────────────────────
    def _scan_transcript(self, path: str, from_start: bool = False) -> int:
        """Přírůstek výstupních tokenů ze záznamu relace Claude Code (JSONL).

        Záznam, který daemon vidí poprvé, se přeskočí (obnovená relace by jinak
        přičetla celou historii), kromě záznamu subagenta (from_start), ten je nový.
        Záznam, který ještě neexistuje, patří nové relaci: počítá se od začátku.
        Jedna odpověď modelu má v záznamu víc řádků se stejným message.id a jejich
        usage roste, počítá se proto jen nárůst nejvyšší hodnoty.
        """
        with self._tx_lock:
            try:
                size = os.path.getsize(path)
            except OSError:
                size = -1
            st = self._transcripts.get(path)
            if st is None:
                if len(self._transcripts) >= MAX_TRANSCRIPTS:
                    self._transcripts.pop(next(iter(self._transcripts)))
                st = {"offset": 0 if from_start or size < 0 else size, "msgs": {}}
                self._transcripts[path] = st
            if size < 0:
                return 0
            if size < st["offset"]:
                st["offset"] = size   # soubor se zkrátil: znovu nepočítat
            if size == st["offset"]:
                return 0
            try:
                with open(path, "rb") as f:
                    f.seek(st["offset"])
                    data = f.read(size - st["offset"])
            except OSError:
                return 0
            # Jen celé řádky, rozepsaný poslední řádek se přečte příště
            end = data.rfind(b"\n")
            if end < 0:
                return 0
            st["offset"] += end + 1
            delta = 0
            msgs = st["msgs"]
            for raw in data[:end].split(b"\n"):
                if b'"usage"' not in raw or b'"assistant"' not in raw:
                    continue
                try:
                    doc = json.loads(raw)
                except Exception:
                    continue
                if not isinstance(doc, dict) or doc.get("type") != "assistant":
                    continue
                msg = doc.get("message")
                if not isinstance(msg, dict):
                    continue
                mid = msg.get("id")
                out = (msg.get("usage") or {}).get("output_tokens")
                if not mid or not isinstance(out, int):
                    continue
                prev = msgs.get(mid, 0)
                if out > prev:
                    delta += out - prev
                    msgs[mid] = out
            while len(msgs) > 400:
                msgs.pop(next(iter(msgs)))
            return delta

    async def _claude_tokens(self, path: str, from_start: bool = False) -> None:
        if not path:
            return
        try:
            delta = await asyncio.to_thread(self._scan_transcript, path, from_start)
        except Exception as e:
            log.debug("Transcript %s: %s", path, e)
            return
        if delta > 0:
            self._add_tokens(delta)

    async def _handle_claude_event(self, body: dict) -> dict:
        """Události z hooku Claude Code (claude_hook.py): průběh práce pro krabičku."""
        cid = "cc:" + str(body.get("sessionId") or "default")
        ev = body.get("event") or ""
        now = time.time()
        stamp = time.strftime("%H:%M")
        ws = str(body.get("workspace") or "Claude Code")
        transcript = str(body.get("transcript") or "")
        self._set_client("claude")
        if ev in ("SessionStart", "Stop", "SessionEnd"):
            log.info("Claude Code: %s (%s, relace %s)", ev, ws, cid[3:11])

        if ev == "SessionEnd":
            self.sessions.pop(cid, None)
            self._spawn(self.push_snapshot())
            return {"ok": True, "connected": self.is_connected()}

        sess = self.sessions.setdefault(cid, {"running": False, "updated": now})
        sess["updated"] = now
        sess["client"] = "claude"
        sess["seq"] = self._next_seq()

        if ev == "SessionStart":
            sess["running"] = False
            self.msg = f"Claude: {ws}"
            if body.get("source") in (None, "", "startup", "resume", "fork"):
                self.entries.appendleft(f"{stamp} nová relace ({ws})"[:90])
            # Jen zapamatovat konec záznamu, historie obnovené relace se nepočítá
            await self._claude_tokens(transcript)
        elif ev == "UserPromptSubmit":
            sess["running"] = True
            user_in = " ".join(str(body.get("prompt") or "").split())
            if user_in:
                self.entries.appendleft(fit_rows(f"{stamp} > {user_in}"))
            self.msg = "Claude přemýšlí…"
            await self._claude_tokens(transcript)
        elif ev == "PreToolUse":
            sess["running"] = True
            tool = str(body.get("tool") or "Nástroj")
            summary = " ".join(str(body.get("summary") or "").split())
            line = f"{stamp} {tool}: {summary}" if summary else f"{stamp} {tool}"
            self.entries.appendleft(line[:90])
            self.msg = line[:64]
            await self._claude_tokens(transcript)
        elif ev == "Notification":
            if body.get("notificationType") == "permission_prompt":
                # Dotaz, na který krabička nestihla odpovědět, čeká v terminálu
                self.entries.appendleft(f"{stamp} Povolení čeká v terminálu")
                self.msg = "Povolení čeká v terminálu"
        elif ev == "Stop":
            sess["running"] = False
            self.recently_completed = True
            self.msg = "Hotovo – čeká na zadání"
            last = " ".join(str(body.get("lastMessage") or "").split())
            if last:
                self.entries.appendleft(fit_rows(f"{stamp} {last}"))
            await self._claude_tokens(transcript)
        elif ev == "SubagentStop":
            await self._claude_tokens(str(body.get("agentTranscript") or ""), from_start=True)

        self._spawn(self.push_snapshot())
        return {"ok": True, "connected": self.is_connected()}

    def _drop_prompt(self, pid: str) -> None:
        self.pending_prompts.pop(pid, None)
        if self.active_prompt_id == pid:
            self.active_prompt_id = next(iter(self.pending_prompts), None)

    def _activate_next(self) -> None:
        """Na krabičku další čekající dotaz, když se zrovna žádný neukazuje."""
        if not self.active_prompt_id or self.active_prompt_id not in self.pending_prompts:
            self.active_prompt_id = next(iter(self.pending_prompts), None)

    @staticmethod
    def _timeout_arg(value, default: float) -> float:
        try:
            timeout_s = float(value if value is not None else default)
            if not math.isfinite(timeout_s):
                raise ValueError("timeout")
            return min(max(timeout_s, 5.0), 600.0)
        except (TypeError, ValueError):
            return default

    async def _open_prompt(self, client: str, data: dict, timeout_s: float, front: bool = False):
        """Založí dotaz pro krabičku. Vrací (pid, future), nebo None bez spojení.

        Zámek jen na založení dotazu. Na odpověď se čeká mimo něj, takže dotazy
        z více relací běží souběžně a krabička je ukáže po sobě. front=True:
        další krok dotazování se ukáže hned, ne až za dotazy ve frontě."""
        async with self._approve_lock:
            if not self.is_connected():
                return None
            self._set_client(client)
            self._prompt_seq += 1
            pid = f"{'cc' if client == 'claude' else 'agy'}_{int(time.time())}_{self._prompt_seq}"
            fut = asyncio.get_running_loop().create_future()
            created = time.time()
            self.pending_prompts[pid] = {
                "created": created,
                "deadline": created + timeout_s,
                "client": client,
                "future": fut,
                **data,
            }
            # Nový dotaz se ukáže, jen když krabička zrovna žádný nemá; jinak
            # počká ve frontě a ukáže se po vyřízení předchozího
            if front or not self.active_prompt_id or self.active_prompt_id not in self.pending_prompts:
                self.active_prompt_id = pid
                self.msg = f"Rozhodnout: {data.get('tool') or 'Rozhodnutí'}"
        return pid, fut

    async def _await_prompt(self, pid: str, fut: asyncio.Future, timeout_s: float):
        """Ukáže dotaz a čeká na odpověď. Vrací výsledek z krabičky, "timeout",
        nebo "fallback" (spojení spadlo). Po návratu dotaz už nečeká."""
        # Od založení dotazu je všechno v try: zrušení (hook se odpojil) kdekoli,
        # i během odesílání snímku, dotaz zahodí, jinak by zablokoval frontu
        try:
            # Odpočet podle termínu dotazu (ten ukazuje i krabička), zdržené
            # odesílání snímku ho neprodlouží
            item = self.pending_prompts.get(pid) or {}
            deadline = float(item.get("deadline") or (time.time() + timeout_s))
            await self.push_snapshot()
            return await asyncio.wait_for(fut, timeout=max(0.0, deadline - time.time()))
        except asyncio.TimeoutError:
            return "timeout"
        except asyncio.CancelledError:
            self._drop_prompt(pid)
            self._activate_next()
            self._spawn(self.push_snapshot())
            raise
        finally:
            self.pending_prompts.pop(pid, None)

    async def _finish_prompt(self) -> None:
        """Po vyřízení dotazu (i celého dotazování) ukázat další z fronty."""
        self._activate_next()
        await self.push_snapshot()

    async def _handle_approve(self, body: dict) -> dict:
        if not self.is_connected():
            return {"ok": True, "connected": False, "decision": "fallback"}
        client = "claude" if body.get("client") == "claude" else "agy"
        if body.get("kind") == "questions":
            return await self._handle_questions(body, client)
        if body.get("always") and not body.get("danger") and self._fw_ui2():
            return await self._handle_permission_choice(body, client)
        return await self._handle_simple_prompt(body, client)

    async def _handle_simple_prompt(self, body: dict, client: str) -> dict:
        """Schválení / zamítnutí, nebo otázka s jedinou volbou (AGY a starší firmware)."""
        tool = utf8_trunc(str(body.get("tool") or "Rozhodnutí"), FW_TOOL_BYTES)
        hint = utf8_trunc(str(body.get("hint") or ""), FW_HINT_BYTES)
        raw_opts = body.get("options")
        if isinstance(raw_opts, list) and raw_opts:
            # Limity firmwaru: PROMPT_MAX_OPTS voleb, 63 bajtů na volbu
            options = [utf8_trunc(str(o)[:30], FW_OPT_BYTES) for o in raw_opts][:FW_MAX_OPTS]
            opt_a = options[0] if len(options) > 0 else ""
            opt_b = options[1] if len(options) > 1 else ""
        else:
            opt_a = utf8_trunc(str(body.get("optA") or "")[:24], 47)
            opt_b = utf8_trunc(str(body.get("optB") or "")[:24], 47)
            options = [o for o in (opt_a, opt_b) if o] if (opt_a or opt_b) else None
        timeout_s = self._timeout_arg(body.get("timeout"), 180.0)

        opened = await self._open_prompt(client, {
            "tool": tool,
            "hint": hint,
            "options": options,
            "optA": opt_a,
            "optB": opt_b,
            "danger": bool(body.get("danger")),
        }, timeout_s)
        if not opened:
            return {"ok": True, "connected": False, "decision": "fallback"}
        pid, fut = opened
        item = self.pending_prompts.get(pid) or {}
        decision = await self._await_prompt(pid, fut, timeout_s)
        await self._finish_prompt()
        if decision == "timeout":
            return {"ok": True, "connected": True, "decision": "timeout"}
        if decision == "fallback":
            return {"ok": True, "connected": False, "decision": "fallback"}

        stamp = time.strftime("%H:%M")
        if opt_a or opt_b:
            self.entries.appendleft(f"{stamp} [VOLBA] {decision}"[:90])
        else:
            mark = "SCHVÁLENO" if decision == "allow" else "ZAMÍTNUTO"
            t_cz = TOOL_LABELS_CZ.get(tool, tool)
            self.entries.appendleft(f"{stamp} [{mark}] {t_cz}")
        res = {"ok": True, "connected": True, "decision": decision}
        if isinstance(item.get("chosen"), int):
            res["opt"] = item["chosen"]   # pořadí zvolené volby (texty jsou zkrácené)
        return res

    async def _handle_permission_choice(self, body: dict, client: str) -> dict:
        """Povolení jako v terminálu: Ano / Ano, už se neptat / Ne (obrazovka otázky).
        Vrací decision allow, always, deny, terminal, timeout nebo fallback."""
        tool = utf8_trunc(str(body.get("tool") or "Rozhodnutí"), FW_TOOL_BYTES)
        timeout_s = self._timeout_arg(body.get("timeout"), 180.0)
        options = ["Ano", "Ano, už se neptat", "Ne"]
        opened = await self._open_prompt(client, {
            "tool": tool,
            "hint": utf8_trunc(str(body.get("hint") or ""), FW_HINT_BYTES),
            "options": options,
            "q": True,
            "head": utf8_trunc(str(body.get("head") or tool), FW_HEAD_BYTES),
            "descs": ["", utf8_trunc_ell(str(body.get("always") or ""), FW_DESC_BYTES), ""],
            # Výběr začíná na „Ne“: IO18 tak dál zamítá jako u SCHVÁLIT / ZAMÍTNOUT
            "sel": [2],
            "term": True,
        }, timeout_s)
        if not opened:
            return {"ok": True, "connected": False, "decision": "fallback"}
        pid, fut = opened
        res = await self._await_prompt(pid, fut, timeout_s)
        await self._finish_prompt()
        if res in ("timeout", "fallback"):
            return {"ok": True, "connected": res != "fallback", "decision": res}
        dec = res.get("decision") if isinstance(res, dict) else str(res)
        opt = res.get("opt") if isinstance(res, dict) else None
        if dec == "terminal":
            return {"ok": True, "connected": True, "decision": "terminal"}
        if opt not in (0, 1, 2) and dec in options:
            opt = options.index(dec)
        decision = {0: "allow", 1: "always", 2: "deny"}.get(opt, "deny")
        mark = {"allow": "SCHVÁLENO", "always": "SCHVÁLENO TRVALE", "deny": "ZAMÍTNUTO"}[decision]
        self.entries.appendleft(f"{time.strftime('%H:%M')} [{mark}] {tool}"[:90])
        return {"ok": True, "connected": True, "decision": decision}

    @staticmethod
    def _parse_questions(raw) -> list:
        """Otázky z hooku: [{header, question, multi, options: [{label, desc}]}]."""
        out = []
        if not isinstance(raw, list):
            return out
        for q in raw[:4]:
            if not isinstance(q, dict):
                return []
            opts = []
            for o in (q.get("options") or [])[:FW_MAX_OPTS]:
                if not isinstance(o, dict) or not str(o.get("label") or "").strip():
                    return []
                opts.append({
                    "label": utf8_trunc_ell(" ".join(str(o["label"]).split()), FW_QOPT_BYTES),
                    "desc": utf8_trunc_ell(" ".join(str(o.get("desc") or "").split()), FW_DESC_BYTES),
                })
            question = " ".join(str(q.get("question") or "").split())
            if len(opts) < 2 or not question:
                return []
            out.append({
                "header": " ".join(str(q.get("header") or "").split())[:20],
                "question": utf8_trunc_ell(question, FW_HINT_BYTES),
                "multi": bool(q.get("multi")),
                "options": opts,
            })
        return out

    async def _handle_questions(self, body: dict, client: str) -> dict:
        """Dotazování jako v terminálu: otázky po jedné (ZPĚT na předchozí), u více
        otázek na konci shrnutí s ODESLAT. Vrací decision "answers" s pořadím
        zvolených voleb ke každé otázce, nebo terminal / timeout / fallback."""
        qs = self._parse_questions(body.get("questions"))
        if not qs:
            return {"ok": True, "connected": True, "decision": "terminal"}
        step_s = self._timeout_arg(body.get("timeout"), 60.0)
        total_s = min(self._timeout_arg(body.get("total"), QUESTIONS_TOTAL_MAX_S), QUESTIONS_TOTAL_MAX_S)

        if not self._fw_ui2():
            # Starší firmware: jen jedna otázka s jedinou volbou, jako dřív
            q = qs[0]
            if len(qs) != 1 or q["multi"]:
                return {"ok": True, "connected": True, "decision": "terminal"}
            labels = [utf8_trunc(o["label"], FW_OPT_BYTES) for o in q["options"]]
            res = await self._handle_simple_prompt({
                "tool": "Otázka", "hint": q["question"], "options": labels, "timeout": step_s,
            }, client)
            # Podle pořadí volby: text z krabičky je zkrácený na 30 znaků
            opt = res.get("opt")
            if isinstance(opt, int) and 0 <= opt < len(labels):
                return {**res, "decision": "answers", "answers": [[opt]]}
            return res

        n = len(qs)
        review = n > 1
        answers: List[Optional[List[int]]] = [None] * n
        idx = 0
        cont = False
        deadline = time.time() + total_s
        try:
            while True:
                if idx < n:
                    q = qs[idx]
                    head = f"{idx + 1}/{n} · {q['header']}" if n > 1 else (q["header"] or "Otázka")
                    data = {
                        "tool": "Otázka",
                        "q": True,
                        "head": utf8_trunc(head, FW_HEAD_BYTES),
                        "hint": q["question"],
                        "options": [o["label"] for o in q["options"]],
                        "descs": [o["desc"] for o in q["options"]],
                        "multi": q["multi"],
                        "sel": list(answers[idx] or []),
                        "back": idx > 0,
                        "term": True,
                        "ok": ("DALŠÍ" if review else "ODESLAT") if q["multi"] else "",
                        "cont": cont,
                        "chain": True,
                    }
                else:
                    data = {
                        "tool": "Otázka",
                        "q": True,
                        "head": "Shrnutí odpovědí",
                        "hint": self._answers_summary(qs, answers),
                        "options": [],
                        "back": True,
                        "term": True,
                        "ok": "ODESLAT",
                        "cont": True,
                        "chain": True,
                    }
                wait_s = min(step_s, deadline - time.time())
                if wait_s <= 0:
                    return {"ok": True, "connected": True, "decision": "timeout"}
                opened = await self._open_prompt(client, data, wait_s, front=cont)
                if not opened:
                    return {"ok": True, "connected": False, "decision": "fallback"}
                pid, fut = opened
                res = await self._await_prompt(pid, fut, wait_s)
                if res in ("timeout", "fallback"):
                    return {"ok": True, "connected": res != "fallback", "decision": res}
                dec = res.get("decision") if isinstance(res, dict) else str(res)
                cont = True
                if dec == "terminal":
                    return {"ok": True, "connected": True, "decision": "terminal"}
                if dec == "back":
                    idx = max(0, idx - 1)
                    continue
                if idx >= n:
                    if dec == "ok":
                        break
                    continue
                n_opts = len(qs[idx]["options"])
                if not isinstance(res, dict):
                    sel = []
                elif qs[idx]["multi"]:
                    sel = sorted({o for o in (res.get("opts") or []) if 0 <= o < n_opts})
                else:
                    opt = res.get("opt")
                    sel = [opt] if isinstance(opt, int) and 0 <= opt < n_opts else []
                if not sel:
                    continue   # neplatná odpověď: stejnou otázku znovu
                answers[idx] = sel
                idx += 1
                if idx >= n and not review:
                    break
        finally:
            await self._finish_prompt()

        stamp = time.strftime("%H:%M")
        for i in reversed(range(n)):
            picked = ", ".join(qs[i]["options"][o]["label"] for o in answers[i] or [])
            self.entries.appendleft(f"{stamp} [VOLBA] {picked}"[:90])
        return {"ok": True, "connected": True, "decision": "answers", "answers": answers}

    @staticmethod
    def _answers_summary(qs: list, answers: list) -> str:
        """Shrnutí na krabičku: řádek na otázku (záhlaví: volby)."""
        lines = []
        for i, q in enumerate(qs):
            name = q["header"] or f"Otázka {i + 1}"
            picked = ", ".join(q["options"][o]["label"] for o in answers[i] or []) or "–"
            lines.append(f"{name}: {picked}")
        budget = FW_HINT_BYTES
        out = []
        for i, line in enumerate(lines):
            # Každý řádek dostane podíl ze zbývajícího místa ve firmwaru
            share = budget // (len(lines) - i) - 1
            line = utf8_trunc_ell(line, max(share, 0))
            out.append(line)
            budget -= len(line.encode("utf-8")) + 1
        return "\n".join(out)


def secrets_compare(a: str, b: str) -> bool:
    import hmac
    return hmac.compare_digest(a.encode("utf-8"), b.encode("utf-8"))


async def main_async(args):
    daemon = BuddyDaemon(prefer_port=args.port, ble_only=args.ble, usb_only=args.usb)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    sock.bind((args.host, args.http_port))
    # Token až po obsazení portu: druhý daemon, kterému bind selže, ho nepřepíše
    daemon.token = write_token()
    server = await asyncio.start_server(daemon.handle_http, sock=sock)
    log.info("Rozhodovac daemon listening on http://%s:%d", args.host, args.http_port)

    PID_PATH.write_text(str(os.getpid()), encoding="utf-8")

    try:
        await asyncio.gather(
            server.serve_forever(),
            daemon.connection_loop(),
            daemon.heartbeat_loop(),
        )
    finally:
        PID_PATH.unlink(missing_ok=True)


def main():
    ap = argparse.ArgumentParser(description="AGY / Claude Code <-> Rozhodovač bridge daemon")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--http-port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--port", help="Explicitní COM port (např. COM7)")
    ap.add_argument("--ble", action="store_true", help="Pouze Bluetooth LE")
    ap.add_argument("--usb", action="store_true", help="Pouze USB Serial")
    args = ap.parse_args()
    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
