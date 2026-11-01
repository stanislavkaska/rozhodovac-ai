#!/usr/bin/env python3
"""
Lifecycle hook Rozhodovače pro Antigravity CLI (AGY).

V režimu 'decide' (výchozí) nechává běžné akce projít výchozí politice AGY
a na krabičce žádá rozhodnutí jen když:
  1) se agent ptá (ask_question) a otázka má 2 až 8 možností s jedinou volbou,
  2) jde o destruktivní příkaz podle vzorů níže (git push --force, rm -rf,
     Remove-Item -Recurse, drop table apod.) nebo o delete_file.
Režim 'write' přidává všechny zápisy a příkazy, režim 'all' každý nástroj.

Výstup pro PreToolUse (viz dokumentace hooků AGY):
  allow        běžná akce, rozhoduje výchozí politika AGY,
  auto_approve schváleno na krabičce, AGY už se v terminálu neptá,
  deny+reason  zamítnuto na krabičce, nebo odpověď na otázku pro agenta,
  ask          krabička není k dispozici nebo nikdo nerozhodl, ptá se terminál.
"""

import json
import os
import re
import sys
import time
from pathlib import Path

from rozhodovac_common import (
    BRIDGE_DIR,
    FLASHING_LOCK,
    http_json,
    read_state,
    resolve_mode,
    setup_utf8_stdio,
    spawn_daemon,
)

setup_utf8_stdio()

SPAWN_MARK = BRIDGE_DIR / ".spawn_attempt"
SPAWN_BACKOFF_S = 60

QUESTION_TOOLS = {"ask_question", "question", "AskUserQuestion"}

# Zápisové nástroje pro režim 'write'
GATED_TOOLS = {
    "run_command",
    "write_file",
    "edit_file",
    "replace_file_content",
    "multi_replace_file_content",
    "delete_file",
}

# Destruktivní příkazy. Jde o seznam vzorů, ne o sandbox: co sem nepatří,
# v režimu 'decide' projde bez dotazu na krabičce.
_DANGEROUS_I = [re.compile(p, re.IGNORECASE) for p in (
    r"\bgit\b.*\bpush\b.*(\s--force(-with-lease|-if-includes)?\b|\s-[a-z]*f[a-z]*\b|\s\+\S|\s--delete\b|\s-d\b|\s--mirror\b|\s--prune\b|\s:\S)",
    r"\bgit\b.*\breset\b.*\s--hard\b",
    r"\bgit\b.*\bclean\b.*(\s-\w*f|\s--force\b)",
    r"\bgit\b.*\bbranch\b.*\s--delete\b.*\s--force\b",
    r"\bgit\b.*\bbranch\b.*\s--force\b.*\s--delete\b",
    r"\bgit\b.*\b(checkout|restore)\b.*\s\.(\s|$)",
    r"\bgit\b.*\bstash\s+(drop|clear)\b",
    r"\brm\s+(-\w*\s+)*-\w*r",
    r"\brm\b.*\s--recursive\b",
    r"\b(rmdir|rd)\b.*\s/s\b",
    r"(^|[\s;&|])(del|erase)\b.*\s/[sfq]\b",
    r"\bfind\b.*\s-delete\b",
    r"\bfind\b.*\s-exec(dir)?\s+rm\b",
    r"\brimraf\b",
    r"\b(remove-item|ri|rm|del|erase|rmdir)\b.*\s-r\w*",
    r"\bremove-item\b.*\s-recurse\b",
    r"\bformat\s+[a-z]:",
    r"\b(format-volume|clear-disk|diskpart)\b",
    r"\bmkfs(\.\w+)?\b",
    r"\bdd\b.*\bof=",
    r"\bdrop\s+(database|table|schema)\b",
    r"\btruncate\s+table\b",
    r"\b(shutdown|restart-computer|stop-computer)\b",
)]
# git branch -D (vynucené smazání) se liší od bezpečného -d jen velikostí písmene
_DANGEROUS_CS = [re.compile(r"\bgit\b.*\bbranch\b.*\s-[a-zA-Z]*D[a-zA-Z]*\b")]

# Kolik voleb krabička zobrazí (stejný limit má daemon a firmware, PROMPT_MAX_OPTS)
DEVICE_MAX_OPTS = 8
# Délka popisku volby: u dvou voleb velká tlačítka vedle sebe (zhruba 17 znaků
# ve velikosti 2), od tří voleb řádky přes celou šířku displeje (31 znaků)
OPT_MAX_CHARS = 18
OPT_MAX_CHARS_ROW = 30
# Tyto hodnoty daemon chápe jako schválení / zamítnutí, volba se jimi jmenovat nesmí
# a hodnoty, kterými daemon hlásí, že nikdo nerozhodl
_RESERVED_DECISIONS = {"once", "allow", "deny", "fallback", "timeout"}


def _command_of(args: dict) -> str:
    return str(args.get("CommandLine") or args.get("command") or "")


def _danger_match_end(cmd: str) -> int:
    """Konec první nalezené rizikové části příkazu (u gitu až za přepínačem), -1 když žádná."""
    ends = [m.end() for p in (*_DANGEROUS_I, *_DANGEROUS_CS) for m in [p.search(cmd)] if m]
    return min(ends) if ends else -1


def is_dangerous_action(tool_name: str, args: dict) -> bool:
    if tool_name == "delete_file":
        return True
    if tool_name == "run_command":
        cmd = " ".join(_command_of(args).split())
        return any(p.search(cmd) for p in _DANGEROUS_I) or any(p.search(cmd) for p in _DANGEROUS_CS)
    return False


def _parse_questions(args: dict) -> list:
    questions = args.get("questions")
    if questions is None and args.get("question"):
        questions = [args]
    if isinstance(questions, str):
        try:
            questions = json.loads(questions)
        except Exception:
            questions = []
    if not isinstance(questions, list):
        return []
    return [q for q in questions if isinstance(q, dict)]


def _option_label(opt) -> str:
    if isinstance(opt, dict):
        return str(opt.get("label") or opt.get("text") or opt.get("value") or "").strip()
    return str(opt).strip()


def _clean_label(label: str) -> str:
    return label.replace("(Recommended)", "").replace("(Doporučeno)", "").strip()


def question_spec(args: dict) -> dict:
    """Popis otázky pro krabičku.

    Na krabičce jdou zodpovědět otázky se 2 až DEVICE_MAX_OPTS možnostmi a jedinou
    volbou. Ostatní (více voleb, multi-select, volný text) se ptají v terminálu.
    """
    questions = _parse_questions(args)
    if not questions:
        return {"text": "", "options": [], "short": [], "device": False}
    q0 = questions[0]
    text = str(q0.get("question") or q0.get("header") or "").strip()
    options = [o for o in (_clean_label(_option_label(o)) for o in (q0.get("options") or [])) if o]
    max_chars = OPT_MAX_CHARS if len(options) <= 2 else OPT_MAX_CHARS_ROW
    short = [o[:max_chars].strip() for o in options]
    if len(short) >= 2 and len(set(short)) < len(short):
        # Stejný začátek dlouhých možností: odliší číslice
        short = [f"{i + 1}. {o[:max_chars - 3].strip()}" for i, o in enumerate(options)]
    device = (
        len(questions) == 1
        and 2 <= len(options) <= DEVICE_MAX_OPTS
        and not q0.get("multiSelect")
        and len(set(s.lower() for s in short)) == len(short)
        and not any(s.lower() in _RESERVED_DECISIONS for s in short)
    )
    return {"text": text, "options": options, "short": short, "device": device}


def _fix_cz(s: str) -> str:
    """Opraví text, který přišel jako UTF-8 přečtené v cp1250 (např. 'Ä›' místo 'ě')."""
    if not s or not any(ch in s for ch in ("Ă", "Ä", "Ĺ")):
        return s
    try:
        return s.encode("cp1250").decode("utf-8")
    except Exception:
        return s


def ensure_daemon_running() -> bool:
    if FLASHING_LOCK.is_file():
        return False
    try:
        return bool(http_json("/health", None, timeout=0.4).get("ok"))
    except Exception:
        pass
    # Zastavený příkazem stop: hooky ho znovu nespouští, až příkaz start
    if read_state().get("stopped"):
        return False

    # Když se daemon nedaří spustit (chybí bleak, port obsazený…), nezdržuj
    # každé volání nástroje dalšími pokusy. Nový pokus nejdřív po SPAWN_BACKOFF_S.
    # Značka se zakládá výlučně (O_EXCL): ze souběžně spuštěných hooků spouští
    # daemon jen jeden, ostatní nečekají
    try:
        if SPAWN_MARK.is_file():
            if time.time() - SPAWN_MARK.stat().st_mtime < SPAWN_BACKOFF_S:
                return False
            SPAWN_MARK.unlink()
        os.close(os.open(str(SPAWN_MARK), os.O_CREAT | os.O_EXCL | os.O_WRONLY))
    except FileExistsError:
        return False
    except Exception:
        pass

    if not spawn_daemon():
        return False
    for _ in range(8):
        time.sleep(0.15)
        try:
            if http_json("/health", None, timeout=0.3).get("ok"):
                return True
        except Exception:
            pass
    return False


def _short_path(p: str, max_chars: int = 60) -> str:
    """Celá cesta, u dlouhé konec (složka a jméno souboru) s „…“ na začátku."""
    p = str(p)
    return p if len(p) <= max_chars else "…" + p[-(max_chars - 1):]


def format_tool_hint(tool_name: str, args: dict, decision: bool = False) -> str:
    """Popis pro krabičku. U rozhodování (decision=True) jde vždy o skutečný příkaz
    nebo celou cestu: popis toolSummary píše agent a nemusí odpovídat tomu, co proběhne."""
    if not isinstance(args, dict):
        return ""
    if tool_name == "run_command":
        cmd = _fix_cz(" ".join(_command_of(args).split()))
        summary = _fix_cz(str(args.get("toolSummary") or ""))
        if decision:
            # Krabička ukáže zhruba 300 bajtů od začátku. U dlouhého příkazu by
            # riziková část na konci (… && rm -rf ~) zmizela: ukázat výřez, který ji obsahuje.
            # Karta krabičky ukáže zhruba 130 znaků: riziková část musí být
            # v prvních dvou až třech řádcích výřezu
            end = _danger_match_end(cmd)
            if end > 100:
                cmd = "…" + cmd[end - 80:]
            if cmd and summary:
                return f"{cmd} | {summary}"
            return cmd or summary
        if summary and cmd:
            return summary if len(cmd) > 48 else f"{summary} ({cmd})"
        return summary or cmd
    if tool_name in ("write_file", "edit_file", "read_file", "view_file", "delete_file"):
        p = args.get("Path") or args.get("AbsolutePath") or args.get("TargetFile") or ""
        if not p:
            return ""
        return _short_path(p) if decision else Path(str(p)).name
    if tool_name in QUESTION_TOOLS:
        return question_spec(args)["text"][:80]
    for k in ("Path", "CommandLine", "query", "toolSummary"):
        if args.get(k):
            return str(args[k])
    return ""


def _emit(obj: dict) -> None:
    print(json.dumps(obj, ensure_ascii=False))


def _post_event(event: str, timeout: float = 0.6, **fields) -> None:
    try:
        http_json("/event", {"event": event, **fields}, timeout=timeout)
    except Exception:
        pass


def handle_pre_tool_use(ctx: dict, conv_id: str, daemon_up: bool) -> None:
    tc = ctx.get("toolCall") or {}
    tool_name = str(tc.get("name") or "tool").split(":")[-1]
    args = tc.get("args") or {}
    if not isinstance(args, dict):
        args = {}
    hint = format_tool_hint(tool_name, args)
    mode, _ = resolve_mode()

    is_question = tool_name in QUESTION_TOOLS
    qspec = question_spec(args) if is_question else None
    is_danger = is_dangerous_action(tool_name, args)

    if is_question:
        # Otázku, kterou krabička neumí zodpovědět (5+ voleb, více otázek,
        # vícenásobný výběr), nemá smysl „schvalovat“ ani v režimu all
        needs_decision = qspec["device"]
    elif mode == "all":
        needs_decision = True
    elif mode == "write":
        needs_decision = is_danger or tool_name in GATED_TOOLS
    else:
        needs_decision = is_danger

    # Bez odpovědi z krabičky: otázka jde do terminálu (allow, AGY ji položí),
    # ostatní akce rozhodne terminál (ask)
    no_answer = {"decision": "allow"} if is_question else {"decision": "ask"}

    if not needs_decision:
        if daemon_up:
            if is_question:
                _post_event("QuestionInTerminal", conversationId=conv_id, summary=hint)
            else:
                _post_event("PreToolUse", conversationId=conv_id, tool=tool_name, summary=hint)
        _emit({"decision": "allow"})
        return

    if not daemon_up:
        _emit(no_answer)
        return

    req = {"timeout": 90 if is_question else 45}
    if is_question and qspec["device"]:
        req.update(
            tool="Otázka",
            hint=qspec["text"] or hint or tool_name,
            options=qspec["short"],
            optA=qspec["short"][0] if len(qspec["short"]) > 0 else "",
            optB=qspec["short"][1] if len(qspec["short"]) > 1 else "",
        )
    else:
        req.update(
            tool="Kritická akce" if is_danger else tool_name,
            hint=format_tool_hint(tool_name, args, decision=True) or tool_name,
            danger=is_danger,
        )

    try:
        res = http_json("/approve", req, timeout=req["timeout"] + 5.0)
    except Exception:
        _emit(no_answer)
        return

    dec = res.get("decision")
    if not res.get("connected") or dec in ("fallback", "timeout", None):
        # Krabička není připojená nebo nikdo nerozhodl -> rozhodne terminál AGY
        _emit(no_answer)
        return

    if is_question and qspec["device"]:
        # AGY neumí hookem dodat odpověď do ask_question. Otázku proto zamítneme
        # a zvolenou možnost předáme agentovi v důvodu zamítnutí.
        try:
            chosen = qspec["options"][qspec["short"].index(dec)]
        except ValueError:
            if dec in qspec["options"]:
                chosen = dec
            elif str(dec).isdigit() and 1 <= int(dec) <= len(qspec["options"]):
                chosen = qspec["options"][int(dec) - 1]
            else:
                _emit(no_answer)
                return
        _emit({
            "decision": "deny",
            "reason": (
                f"Uživatel odpověděl na hardwarovém Rozhodovači: zvolil možnost „{chosen}“. "
                "Otázku znovu nepokládej a pokračuj podle této volby."
            ),
        })
        return

    if dec == "allow":
        _emit({"decision": "auto_approve"})
    else:
        _emit({
            "decision": "deny",
            "reason": f"Akce '{tool_name}' byla zamítnuta na hardwarovém Rozhodovači.",
        })


def main() -> None:
    event = sys.argv[1] if len(sys.argv) > 1 else "PreToolUse"
    try:
        raw_bytes = sys.stdin.buffer.read()
        try:
            raw = raw_bytes.decode("utf-8")
        except UnicodeDecodeError:
            raw = raw_bytes.decode("cp1250", errors="replace")
        ctx = json.loads(raw) if raw.strip() else {}
        if not isinstance(ctx, dict):
            ctx = {}
    except Exception:
        ctx = {}

    conv_id = ctx.get("conversationId") or "agy"
    ws_paths = ctx.get("workspacePaths") or []
    ws_name = Path(ws_paths[0]).name if ws_paths else "AGY"

    daemon_up = ensure_daemon_running()

    if event == "PreToolUse":
        handle_pre_tool_use(ctx, conv_id, daemon_up)
        return

    # Ostatní události (SessionStart, PreInvocation, PostToolUse, Stop)
    if daemon_up:
        tc = ctx.get("toolCall") or {}
        tool_name = str(tc.get("name") or "").split(":")[-1]
        _post_event(
            event,
            timeout=0.8,
            conversationId=conv_id,
            workspace=ws_name,
            lastUserInput=_fix_cz(str(ctx.get("lastUserInput") or "")),
            tool=tool_name,
            summary=format_tool_hint(tool_name, tc.get("args") or {}),
        )

    if event == "Stop":
        # Cokoli jiného než "continue" nechá agenta normálně skončit
        _emit({"decision": "stop"})
    else:
        _emit({})


if __name__ == "__main__":
    main()
