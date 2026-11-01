#!/usr/bin/env python3
"""
Ovládací utilita pro Rozhodovač + Antigravity CLI (AGY).

Příklady použití:
  python bridge/rozhodovac_ctl.py status            # Stav spojení, baterie a statistik krabičky
  python bridge/rozhodovac_ctl.py start             # Spustí daemon na pozadí
  python bridge/rozhodovac_ctl.py stop              # Zastaví daemon na pozadí
  python bridge/rozhodovac_ctl.py test              # Pošle zkušební žádost o akci na displej
  python bridge/rozhodovac_ctl.py test-question [2-8]  # Pošle zkušební dotaz s volbami na displej
  python bridge/rozhodovac_ctl.py mode [decide|write|all]  # Zobrazí / trvale nastaví režim
  python bridge/rozhodovac_ctl.py name "Rozhodovac" # Nastaví jméno parťáka v krabičce
  python bridge/rozhodovac_ctl.py unpair            # Smaže v krabičce spárovaná BLE zařízení
  python bridge/rozhodovac_ctl.py forget-device     # Daemon zapomene krabičku, příště se spojí s jinou
  python bridge/rozhodovac_ctl.py enable-local      # Zapne Rozhodovač pro projekt v aktuální složce
  python bridge/rozhodovac_ctl.py enable-global     # Zapne Rozhodovač pro VŠECHNY projekty v AGY
  python bridge/rozhodovac_ctl.py disable-global    # Vypne globální hook v AGY
  python bridge/rozhodovac_ctl.py enable-claude     # Zapne Rozhodovač pro Claude Code v terminálu (všechny projekty)
  python bridge/rozhodovac_ctl.py disable-claude    # Vypne ho (s --dir jen v projektu)
"""

import argparse
import json
import shutil
import sys
import time
from pathlib import Path
from typing import Optional

from rozhodovac_common import (
    BRIDGE_DIR,
    DEFAULT_MODE,
    HOOK_SCRIPT,
    MODES,
    atomic_write_text,
    env_mode,
    http_json,
    read_state,
    resolve_mode,
    setup_utf8_stdio,
    spawn_daemon,
    write_state,
)

setup_utf8_stdio()

GLOBAL_HOOKS_JSON = Path.home() / ".gemini" / "config" / "hooks.json"
HOOK_KEY = "rozhodovac"
LEGACY_HOOK_KEYS = ("schvalovac",)

# Claude Code v terminálu: hooky v ~/.claude/settings.json (pro všechny projekty)
# nebo v .claude/settings.local.json projektu. Své záznamy pozná podle skriptu.
CLAUDE_HOOK_SCRIPT = BRIDGE_DIR / "claude_hook.py"
CLAUDE_GLOBAL_SETTINGS = Path.home() / ".claude" / "settings.json"
# Hook PermissionRequest čeká na krabičku nejvýš 9 min (otázky po krocích,
# claude_hook.QUESTIONS_TOTAL_S) + rezerva
CLAUDE_PERMISSION_HOOK_TIMEOUT_S = 600
CLAUDE_ASYNC_EVENTS = (
    "SessionStart", "UserPromptSubmit", "PreToolUse", "Notification",
    "Stop", "SubagentStop", "SessionEnd",
)
CLAUDE_MATCHER_EVENTS = ("PreToolUse", "PermissionRequest")

MODE_DESCRIPTIONS = {
    "decide": "Rutina jde podle politiky AGY, krabička jen u otázek s volbami (2 až 8 možností) a rizikových akcí",
    "write": "Krabička navíc u všech zápisů do souborů a spouštěných příkazů",
    "all": "Krabička u každého volání nástroje",
}
MODE_SOURCES = {
    "env": "z proměnné prostředí ROZHODOVAC_MODE",
    "state": "uloženo v bridge/state.json",
    "default": "výchozí",
}


def _health(timeout: float = 1.5) -> Optional[dict]:
    try:
        h = http_json("/health", timeout=timeout)
        return h if h.get("ok") else None
    except Exception:
        return None


def _win_cmd_safe_path(p: str) -> str:
    """Vrátí cestu bezpečnou pro Windows `cmd /c` volané z Go bez vnitřních uvozovek.

    Cestu s mezerou nahradí krátkým názvem 8.3. Když ho systém nemá (8.3 názvy
    vypnuté), dá cestu do uvozovek: bez nich by se hook nespustil vůbec.
    """
    if sys.platform != "win32":
        return f'"{p}"' if " " in p else p
    norm = str(Path(p).resolve())
    if " " not in norm:
        return norm
    try:
        import ctypes
        buf = ctypes.create_unicode_buffer(512)
        if ctypes.windll.kernel32.GetShortPathNameW(norm, buf, 512) and " " not in buf.value:
            return buf.value
    except Exception:
        pass
    print(f"POZOR: cesta {norm} obsahuje mezeru a nemá krátký název 8.3, zapisuji ji v uvozovkách.", file=sys.stderr)
    return f'"{norm}"'


def _warn_venv() -> None:
    """Hook se spouští interpretem, kterým běží tento skript; venv se do hooks.json zafixuje."""
    if sys.prefix != getattr(sys, "base_prefix", sys.prefix):
        print(f"POZOR: běžíš ve virtuálním prostředí ({sys.prefix}). Hook bude volat jeho Python;"
              " po smazání venv přestane fungovat. Pro trvalé nastavení spusť skript systémovým Pythonem.",
              file=sys.stderr)


def build_hook_entry(enabled: bool = True) -> dict:
    py = _win_cmd_safe_path(sys.executable)
    hs = _win_cmd_safe_path(str(HOOK_SCRIPT))

    def cmd(event: str, timeout: int) -> dict:
        return {"type": "command", "command": f"{py} {hs} {event}", "timeout": timeout}

    return {
        "enabled": enabled,
        "SessionStart": [cmd("SessionStart", 5)],
        "PreInvocation": [cmd("PreInvocation", 5)],
        # Hook čeká na rozhodnutí nejvýš 95 s, timeout AGY musí být delší
        "PreToolUse": [{"matcher": "*", "hooks": [cmd("PreToolUse", 180)]}],
        "PostToolUse": [{"matcher": "*", "hooks": [cmd("PostToolUse", 5)]}],
        "Stop": [cmd("Stop", 5)],
    }


def start_daemon(quiet: bool = False) -> Optional[dict]:
    h = _health(timeout=0.6)
    if h:
        if not quiet:
            print(f"Daemon už běží (připojeno: {h.get('connected')}, kanál: {h.get('transport')}, zařízení: {h.get('device')}).")
        return h
    if not spawn_daemon():
        print("Daemon se nepodařilo spustit (chybí bridge/agy_buddy_daemon.py nebo Python).", file=sys.stderr)
        return None
    for _ in range(20):
        time.sleep(0.25)
        h = _health(timeout=0.5)
        if h:
            break
    if not quiet:
        cmd_status()
    return h


def _claude_hooks_on() -> bool:
    """Jsou hooky Rozhodovače v globálním nastavení Claude Code?"""
    try:
        text = CLAUDE_GLOBAL_SETTINGS.read_text(encoding="utf-8-sig")
    except OSError:
        return False
    return CLAUDE_HOOK_SCRIPT.name in text


def cmd_status():
    h = _health()
    mode, source = resolve_mode()
    claude = ("zapnuto (~/.claude/settings.json)" if _claude_hooks_on()
              else "vypnuto (zapnutí: python bridge/rozhodovac_ctl.py enable-claude)")
    if not h:
        if read_state().get("stopped"):
            print("Stav daemona: ZASTAVENÝ příkazem stop, hooky ho nespouští (zapnutí: python bridge/rozhodovac_ctl.py start)")
        else:
            print("Stav daemona: VYPNUTÝ (spusť: python bridge/rozhodovac_ctl.py start)")
        print(f"  Režim:      {mode.upper()} ({MODE_SOURCES[source]})")
        print(f"  Claude Code: {claude}")
        return

    print("=== Rozhodovač Bridge Daemon ===")
    print(f"  Připojeno:  {'ANO' if h.get('connected') else 'NE (hledám USB / BLE...)'}")
    print(f"  Přenos:     {h.get('transport') or '-'}")
    print(f"  Zařízení:   {h.get('device') or '-'}")
    print(f"  Režim AGY:  {mode.upper()} ({MODE_SOURCES[source]}) – {MODE_DESCRIPTIONS[mode]}")
    print(f"  Claude Code: {claude} – krabička u otázek a žádostí o povolení")
    print(f"  Relace:     {h.get('sessions', 0)} (čeká na rozhodnutí: {h.get('waiting', 0)})")
    print(f"  Tokeny:     {h.get('tokens', 0)} od spuštění mostu, dnes {h.get('tokensToday', 0)}")
    ds = h.get("deviceStatus") or {}
    if ds:
        bat = ds.get("bat") or {}
        st = ds.get("stats") or {}
        print(f"  Parťák:     {ds.get('name', '-')} (vlastník: {ds.get('owner', '-')}, Level {st.get('lvl', 0)})")
        print(f"  Baterie:    {bat.get('pct', '?')}% ({bat.get('mV', '?')} mV, USB={'ANO' if bat.get('usb') else 'NE'})")
        print(f"  Statistiky: schváleno={st.get('appr', 0)}, zamítnuto={st.get('deny', 0)}, "
              f"odpovědi na otázky={st.get('answ', 0)}")


def cmd_mode(val: Optional[str]):
    if val:
        write_state(mode=val)
        print(f"Režim nastaven na: {val.upper()} (platí pro další volání hooku, uloženo v bridge/state.json)")
        env = env_mode()
        if env and env != val:
            print(f"POZOR: proměnná prostředí ROZHODOVAC_MODE={env} má přednost. Zruš ji, jinak se změna neprojeví.")
        return
    mode, source = resolve_mode()
    print(f"Aktuální režim: {mode.upper()} ({MODE_SOURCES[source]})")
    print("Dostupné režimy:")
    for m in MODES:
        print(f"  {m:<6} - {MODE_DESCRIPTIONS[m]}{' (výchozí)' if m == DEFAULT_MODE else ''}")


def _load_hooks(path: Path) -> Optional[dict]:
    """Načte hooks.json. Při chybě vrátí None a soubor se nesmí přepsat."""
    if not path.is_file():
        return {}
    # utf-8-sig: PowerShell a Poznámkový blok ukládají s BOM
    text = path.read_text(encoding="utf-8-sig")
    if not text.strip():
        return {}
    try:
        data = json.loads(text)
    except json.JSONDecodeError as e:
        print(f"CHYBA: {path} není platný JSON ({e}).", file=sys.stderr)
        print("Soubor nechávám beze změny, aby se neztratily ostatní hooky. Oprav ho a spusť příkaz znovu.", file=sys.stderr)
        return None
    if not isinstance(data, dict):
        print(f"CHYBA: {path} nemá na nejvyšší úrovni objekt. Soubor nechávám beze změny.", file=sys.stderr)
        return None
    return data


def _write_hooks(path: Path, data: dict) -> None:
    text = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
    if path.is_file():
        old = path.read_text(encoding="utf-8-sig")
        if old == text:
            return
        # Záloha jen při skutečné změně: opakované spuštění nepřepíše původní stav
        shutil.copy2(path, path.with_name(path.name + ".bak"))
    # Přes dočasný soubor: AGY nikdy nenačte rozepsaný hooks.json
    atomic_write_text(path, text)


def set_hooks(target_file: Path, enabled: bool) -> bool:
    data = _load_hooks(target_file)
    if data is None:
        return False
    if enabled:
        _warn_venv()
    for key in LEGACY_HOOK_KEYS:
        data.pop(key, None)
    data[HOOK_KEY] = build_hook_entry(enabled=enabled)
    _write_hooks(target_file, data)
    print(f"{'Zapnuto' if enabled else 'Vypnuto'} v: {target_file}")
    return True


def local_hooks_json(directory: Optional[str]) -> Path:
    """hooks.json projektu: zadaná složka, jinak aktuální (projekt, ve kterém pracuješ)."""
    base = Path(directory).expanduser() if directory else Path.cwd()
    return base.resolve() / ".agents" / "hooks.json"


def _claude_cmd_path(p: str) -> str:
    """Cesta pro příkaz hooku Claude Code. Ten na Windows běží v Git Bash, kde je
    zpětné lomítko escape znak: lomítka dopředu fungují v bashi i v PowerShellu."""
    posix = Path(p).resolve().as_posix()
    return f'"{posix}"' if " " in posix else posix


def build_claude_hooks() -> dict:
    """Záznamy hooků pro settings.json Claude Code (klíč "hooks")."""
    command = f"{_claude_cmd_path(sys.executable)} {_claude_cmd_path(str(CLAUDE_HOOK_SCRIPT))}"
    hooks = {}
    for event in CLAUDE_ASYNC_EVENTS:
        # Průběh práce: na pozadí, Claude Code na hook nečeká
        group = {"hooks": [{"type": "command", "command": command, "async": True, "timeout": 15}]}
        if event in CLAUDE_MATCHER_EVENTS:
            group = {"matcher": "*", **group}
        hooks[event] = [group]
    hooks["PermissionRequest"] = [{
        "matcher": "*",
        "hooks": [{
            "type": "command",
            "command": command,
            "timeout": CLAUDE_PERMISSION_HOOK_TIMEOUT_S,
            "statusMessage": "Čekám na odpověď z Rozhodovače…",
        }],
    }]
    return hooks


def _is_claude_hook(hook) -> bool:
    return isinstance(hook, dict) and CLAUDE_HOOK_SCRIPT.name in str(hook.get("command") or "")


def set_claude_hooks(target_file: Path, enabled: bool) -> bool:
    """Přidá nebo odebere hooky Rozhodovače v settings.json Claude Code.

    Ostatní nastavení a cizí hooky zůstávají beze změny, záznamy Rozhodovače
    se poznají podle claude_hook.py v příkazu (opakované zapnutí je nezdvojí).
    """
    data = _load_hooks(target_file)
    if data is None:
        return False
    hooks = data.get("hooks")
    if hooks is not None and not isinstance(hooks, dict):
        print(f"CHYBA: {target_file} má klíč \"hooks\", který není objekt. Soubor nechávám beze změny.", file=sys.stderr)
        return False
    hooks = dict(hooks or {})

    for event, groups in list(hooks.items()):
        if not isinstance(groups, list):
            continue
        kept = []
        for group in groups:
            inner = group.get("hooks") if isinstance(group, dict) else None
            if isinstance(inner, list) and any(_is_claude_hook(h) for h in inner):
                rest = [h for h in inner if not _is_claude_hook(h)]
                if rest:
                    kept.append({**group, "hooks": rest})
                continue
            kept.append(group)
        if kept:
            hooks[event] = kept
        else:
            hooks.pop(event)

    if enabled:
        _warn_venv()
        for event, groups in build_claude_hooks().items():
            hooks.setdefault(event, []).extend(groups)

    if hooks:
        data["hooks"] = hooks
    else:
        data.pop("hooks", None)
    _write_hooks(target_file, data)
    print(f"{'Zapnuto' if enabled else 'Vypnuto'} pro Claude Code v: {target_file}")
    if enabled:
        print("Projeví se v nově spuštěném Claude Code (v běžící relaci případně přes /hooks).")
    return True


def claude_settings_file(directory: Optional[str]) -> Path:
    """Bez --dir globální ~/.claude/settings.json, s --dir .claude/settings.local.json projektu."""
    if not directory:
        return CLAUDE_GLOBAL_SETTINGS
    return Path(directory).expanduser().resolve() / ".claude" / "settings.local.json"


def _send_test(payload: dict) -> None:
    h = start_daemon(quiet=True)
    if not h:
        print("Daemon neběží, test nelze odeslat.")
        return
    if not h.get("connected"):
        print("Daemon běží, ale krabička není připojená (USB ani BLE). Zkontroluj: python bridge/rozhodovac_ctl.py status")
        return
    try:
        res = http_json("/approve", payload, timeout=payload["timeout"] + 5.0)
    except Exception as e:
        print(f"Test se nepodařil: {e}")
        return
    print(f"Výsledek z Rozhodovače: {res}")


def _device_cmd(payload: dict, ack: str) -> None:
    if not _health(timeout=0.6):
        print("Daemon neběží (spusť: python bridge/rozhodovac_ctl.py start).")
        return
    try:
        print(http_json("/cmd", {"action": "raw", "payload": payload, "ack": ack}, timeout=8.0))
    except Exception as e:
        print(f"Příkaz se nepodařil: {e}")


def main():
    ap = argparse.ArgumentParser(description="Ovládání hardwarového Rozhodovače")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="Zobrazí stav daemona a připojené krabičky")
    sub.add_parser("start", help="Spustí bridge daemon na pozadí")
    p_stop = sub.add_parser("stop", help="Zastaví bridge daemon (hooky ho znovu nespustí, až příkaz start)")
    p_stop.add_argument("--keep-autostart", action="store_true",
                        help="Hooky most při další události zase spustí (používá flash.ps1)")
    sub.add_parser("test", help="Odešle testovací žádost o akci na displej krabičky")
    p_tq = sub.add_parser("test-question", help="Odešle testovací otázku s volbami (2 až 8) na displej")
    p_tq.add_argument("count", nargs="?", type=int, default=3, choices=range(2, 9), help="Počet voleb (2 až 8, výchozí 3)")

    p_mode = sub.add_parser("mode", help="Zobrazí nebo trvale nastaví režim Rozhodovače (decide, write, all)")
    p_mode.add_argument("val", nargs="?", choices=list(MODES))

    p_name = sub.add_parser("name", help="Nastaví jméno parťáka v krabičce")
    p_name.add_argument("value")

    sub.add_parser("unpair", help="Smaže v krabičce uložená BLE párování")
    sub.add_parser("forget-device", help="Daemon zapomene zapamatovanou krabičku (BLE), příště se spojí s první nalezenou")

    for name, hlp in (("enable-local", "Zapne AGY hooks v projektu (.agents/hooks.json v aktuální nebo zadané složce)"),
                      ("disable-local", "Vypne AGY hooks v projektu (aktuální nebo zadaná složka)")):
        p_loc = sub.add_parser(name, help=hlp)
        p_loc.add_argument("--dir", help="Složka projektu (výchozí aktuální)")
    sub.add_parser("enable-global", help="Zapne AGY hooks globálně pro všechny projekty (~/.gemini/config/hooks.json)")
    sub.add_parser("disable-global", help="Vypne globální AGY hooks")
    for name, hlp in (("enable-claude", "Zapne hooky pro Claude Code v terminálu (~/.claude/settings.json, s --dir jen projekt)"),
                      ("disable-claude", "Vypne hooky pro Claude Code (bez --dir globálně, s --dir v projektu)")):
        p_cc = sub.add_parser(name, help=hlp)
        p_cc.add_argument("--dir", help="Složka projektu: zápis do jeho .claude/settings.local.json místo globálního nastavení")

    args = ap.parse_args()

    if args.cmd == "status":
        cmd_status()
    elif args.cmd == "start":
        write_state(stopped=None)
        start_daemon()
    elif args.cmd == "stop":
        # Bez --keep-autostart zůstane most vypnutý: hooky ho znovu nespustí,
        # až příkaz start (jinak by ho další událost hned zase spustila)
        if not args.keep_autostart:
            write_state(stopped=True)
        try:
            http_json("/cmd", {"action": "shutdown"}, timeout=2.0)
        except Exception:
            print("Daemon neběžel.")
            return
        for _ in range(12):
            time.sleep(0.25)
            if not _health(timeout=0.3):
                break
        print("Daemon ukončen.")
    elif args.cmd == "mode":
        cmd_mode(args.val)
    elif args.cmd == "test":
        print("Odesílám testovací požadavek na displej Rozhodovače — SCHVÁLIT se drží 1 s, ZAMÍTNOUT stačí klepnout...")
        _send_test({"tool": "Kritická akce", "hint": "git push --force origin main", "danger": True, "timeout": 60})
    elif args.cmd == "test-question":
        count = getattr(args, "count", 3) or 3
        sample_opts = [
            "1. Modulární", "2. Monolit", "3. Serverless", "4. Hybridní",
            "5. Mikroslužby", "6. Událostmi řízená", "7. Vrstvená", "8. Zatím nerozhodovat",
        ][:count]
        print(f"Odesílám testovací otázku se {count} volbami na displej Rozhodovače — vyber volbu na displeji...")
        _send_test({
            "tool": "Otázka",
            "hint": "Jaký přístup zvolíme pro novou architekturu?",
            "options": sample_opts,
            "optA": sample_opts[0],
            "optB": sample_opts[1] if len(sample_opts) > 1 else "",
            "timeout": 60,
        })
    elif args.cmd == "name":
        _device_cmd({"cmd": "name", "name": args.value}, "name")
    elif args.cmd == "unpair":
        _device_cmd({"cmd": "unpair"}, "unpair")
        print("Ve Windows odeber krabičku i v Nastavení → Bluetooth (Odebrat zařízení), jinak se znovu nespáruje.")
    elif args.cmd == "forget-device":
        write_state(ble_address=None)
        print("Zapamatovaná krabička smazána. Daemon se příště spojí s první nalezenou krabičkou Claude-*.")
    elif args.cmd == "enable-local":
        set_hooks(local_hooks_json(args.dir), True)
    elif args.cmd == "disable-local":
        set_hooks(local_hooks_json(args.dir), False)
    elif args.cmd == "enable-global":
        # Lokální nastavení projektů (i vědomé vypnutí) zůstává beze změny
        set_hooks(GLOBAL_HOOKS_JSON, True)
    elif args.cmd == "disable-global":
        set_hooks(GLOBAL_HOOKS_JSON, False)
    elif args.cmd == "enable-claude":
        set_claude_hooks(claude_settings_file(args.dir), True)
    elif args.cmd == "disable-claude":
        set_claude_hooks(claude_settings_file(args.dir), False)


if __name__ == "__main__":
    main()
