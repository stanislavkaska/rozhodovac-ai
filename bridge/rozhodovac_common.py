"""
Sdílené konstanty a pomocné funkce mostu Rozhodovače (hook, ovládání, daemon).

Režim rozhodování se ukládá do bridge/state.json, aby ho `rozhodovac_ctl.py mode`
nastavil trvale pro všechny další spuštění hooku. Proměnná prostředí
ROZHODOVAC_MODE (dříve SCHVALOVAC_GATE) má přednost před souborem.
"""

import json
import os
import secrets
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Optional, Tuple

BRIDGE_DIR = Path(__file__).resolve().parent
REPO_DIR = BRIDGE_DIR.parent
DAEMON_SCRIPT = BRIDGE_DIR / "agy_buddy_daemon.py"
HOOK_SCRIPT = BRIDGE_DIR / "agy_hook.py"
STATE_FILE = BRIDGE_DIR / "state.json"
FLASHING_LOCK = BRIDGE_DIR / ".flashing"

DAEMON_URL = (
    os.environ.get("ROZHODOVAC_DAEMON_URL")
    or os.environ.get("SCHVALOVAC_DAEMON_URL")
    or "http://127.0.0.1:18765"
)

# Lokální HTTP daemonu chrání token v profilu uživatele: bez něj by krabičce mohl
# poslat snímek (třeba s podvrženým textem čekajícího dotazu) jakýkoli program na PC.
TOKEN_FILE = Path.home() / ".rozhodovac" / "token"
TOKEN_HEADER = "X-Rozhodovac-Token"

MODES = ("decide", "write", "all")
DEFAULT_MODE = "decide"


def setup_utf8_stdio() -> None:
    for stream in (sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, "reconfigure"):
            try:
                stream.reconfigure(encoding="utf-8", errors="replace")
            except Exception:
                pass


def daemon_port() -> int:
    """Port z ROZHODOVAC_DAEMON_URL, aby ho daemon spuštěný z hooku poslouchal taky."""
    try:
        return urllib.parse.urlsplit(DAEMON_URL).port or 18765
    except ValueError:
        return 18765


def read_token() -> str:
    try:
        return TOKEN_FILE.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def write_token() -> str:
    """Vygeneruje nový token (volá daemon po úspěšném obsazení portu)."""
    token = secrets.token_hex(16)
    TOKEN_FILE.parent.mkdir(parents=True, exist_ok=True)
    atomic_write_text(TOKEN_FILE, token + "\n")
    return token


def atomic_write_text(path: Path, text: str) -> None:
    """Zápis přes dočasný soubor a přejmenování: čtenář nikdy neuvidí rozepsaný soubor."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        # Na Windows přejmenování selže, když soubor zrovna čte jiný proces
        # (hook čte state.json při každém volání): krátce to zkusit znovu
        for attempt in range(10):
            try:
                os.replace(tmp, path)
                break
            except PermissionError:
                if attempt == 9:
                    raise
                time.sleep(0.05)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


# Bez proxy: HTTP_PROXY z prostředí by jinak poslal požadavky na 127.0.0.1
# (i s tokenem) přes proxy
_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def http_json(path: str, payload=None, timeout: float = 5.0) -> dict:
    """GET (payload=None) nebo POST JSON na lokální daemon, vrací dekódovanou odpověď."""
    url = f"{DAEMON_URL}{path}"
    headers = {TOKEN_HEADER: read_token()}
    if payload is None:
        req = urllib.request.Request(url, headers=headers, method="GET")
    else:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    with _OPENER.open(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def read_state() -> dict:
    try:
        data = json.loads(STATE_FILE.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def write_state(**changes) -> None:
    """Změní klíče ve state.json; hodnota None klíč odebere."""
    data = read_state()
    for k, v in changes.items():
        if v is None:
            data.pop(k, None)
        else:
            data[k] = v
    atomic_write_text(STATE_FILE, json.dumps(data, indent=2, ensure_ascii=False) + "\n")


def env_mode() -> str:
    return (os.environ.get("ROZHODOVAC_MODE") or os.environ.get("SCHVALOVAC_GATE") or "").strip().lower()


def resolve_mode() -> Tuple[str, str]:
    """Vrátí (režim, zdroj), kde zdroj je 'env', 'state' nebo 'default'."""
    env = env_mode()
    if env in MODES:
        return env, "env"
    mode = str(read_state().get("mode") or "").strip().lower()
    if mode in MODES:
        return mode, "state"
    return DEFAULT_MODE, "default"


def background_python() -> str:
    """Na Windows pythonw.exe (bez okna konzole), jinde aktuální interpret."""
    py = Path(sys.executable)
    pyw = py.parent / "pythonw.exe"
    if sys.platform == "win32" and pyw.is_file():
        return str(pyw)
    return sys.executable


def spawn_daemon() -> bool:
    """Spustí daemon odpojeně na pozadí. Nečeká na jeho start."""
    if not DAEMON_SCRIPT.is_file():
        return False
    args = ["--http-port", str(daemon_port())]
    try:
        if sys.platform == "win32":
            flags = (
                subprocess.DETACHED_PROCESS
                | subprocess.CREATE_NEW_PROCESS_GROUP
                | subprocess.CREATE_NO_WINDOW
            )
            # Mimo úlohu (job) volajícího: když AGY spouští hooky v jobu, který se
            # při skončení hooku zabije, skončil by s ním i daemon. Kde job
            # vystoupení nedovolí, spustí se daemon bez tohoto příznaku.
            breakaway = getattr(subprocess, "CREATE_BREAKAWAY_FROM_JOB", 0x01000000)
            for extra in (breakaway, 0):
                try:
                    subprocess.Popen(
                        [background_python(), str(DAEMON_SCRIPT), *args],
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        creationflags=flags | extra,
                        close_fds=True,
                    )
                    break
                except OSError:
                    if not extra:
                        raise
        else:
            subprocess.Popen(
                [sys.executable, str(DAEMON_SCRIPT), *args],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
                close_fds=True,
            )
        return True
    except Exception:
        return False
