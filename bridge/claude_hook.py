#!/usr/bin/env python3
"""
Hook Rozhodovače pro Claude Code v terminálu (CLI).

Claude Code ho volá při událostech relace, do ~/.claude/settings.json ho zapíše
`python bridge/rozhodovac_ctl.py enable-claude`:
  - SessionStart, UserPromptSubmit, PreToolUse, Notification, Stop, SubagentStop
    a SessionEnd běží na pozadí (async) a jen hlásí krabičce průběh práce,
  - PermissionRequest čeká na rozhodnutí. Dialog, který by se jinak ukázal
    v terminálu, se nejdřív ukáže na krabičce:
      povolení nástroje: Ano / Ano, už se neptat (když ho terminál nabízí) / Ne,
      otázky AskUserQuestion: všechny po jedné, i s vícenásobným výběrem,
      u více otázek na konci shrnutí s ODESLAT (jako v terminálu).
    Když na krabičce nikdo do 30 s (u otázek 60 s na každý krok) nerozhodne,
    zvolí TERMINÁL nebo krabička není připojená, hook nic nevrátí a Claude Code
    se zeptá v terminálu jako obvykle.

Rozhodnutí z krabičky vrací hook ve tvaru hookSpecificOutput.decision:
  allow                      schváleno, Claude Code se v terminálu neptá,
  allow + updatedPermissions schváleno a už se neptat (pravidlo navržené terminálem),
  allow + updatedInput       odpovědi na otázky (answers: znění otázky -> volba,
                             u vícenásobného výběru volby oddělené „, “),
  deny + message             zamítnuto, zpráva jde modelu.
"""

import json
import os
import sys
import time
from pathlib import Path
from urllib.parse import urlsplit

from agy_hook import (
    SPAWN_MARK,
    _clean_label,
    _danger_match_end,
    _fix_cz,
    _short_path,
    ensure_daemon_running,
)
from rozhodovac_common import BRIDGE_DIR, FLASHING_LOCK, http_json, setup_utf8_stdio

setup_utf8_stdio()

# Jak dlouho čeká krabička na rozhodnutí, než se dialog vrátí do terminálu.
# U otázek platí doba pro každý krok (otázku, shrnutí), celé dotazování nejvýš
# QUESTIONS_TOTAL_S. Hook má v settings.json delší limit (rozhodovac_ctl.py,
# CLAUDE_PERMISSION_HOOK_TIMEOUT_S).
PERMISSION_TIMEOUT_S = 30
QUESTION_TIMEOUT_S = 60
QUESTIONS_TOTAL_S = 540
# Jak dlouho čeká událost na pozadí na právě spouštěný most (hook má timeout 15 s)
DAEMON_START_WAIT_S = 8.0
# Text zadání a poslední odpovědi pro historii na krabičce (ukáže 2 řádky);
# celé by mohlo přesáhnout limit těla požadavku mostu (64 KB)
EVENT_TEXT_MAX = 400
# Popis žádosti o povolení (krabička ukáže nejvýš 319 bajtů)
HINT_MAX = 2000

# Nástroje, jejichž dialog krabička nenahradí: schválení plánu má v terminálu
# víc možností (s automatickým schvalováním úprav, ručně, upravit plán)
SKIP_TOOLS = {"ExitPlanMode"}

COMMAND_TOOLS = {"Bash", "PowerShell"}

TOOL_LABELS_CZ = {
    "Bash": "Příkaz",
    "PowerShell": "Příkaz",
    "Read": "Čtení",
    "Write": "Zápis",
    "Edit": "Úprava",
    "MultiEdit": "Úprava",
    "NotebookEdit": "Úprava",
    "Glob": "Hledání",
    "Grep": "Hledání",
    "WebFetch": "Web",
    "WebSearch": "Hledání na webu",
    "Agent": "Agent",
    "Task": "Agent",
    "TodoWrite": "Úkoly",
    "AskUserQuestion": "Otázka",
    "Skill": "Skill",
    "ExitPlanMode": "Plán",
    "Workflow": "Workflow",
}

DENY_MESSAGE = "Uživatel akci zamítl na hardwarovém Rozhodovači."

# Návrhy „už se neptat“ z terminálu (permission_suggestions), které krabička nabídne
_DEST_CZ = {
    "session": "v této relaci",
    "cliArg": "v této relaci",
    "localSettings": "v tomto projektu",
    "projectSettings": "v projektu (sdílené nastavení)",
    "userSettings": "ve všech projektech",
}
_MODE_CZ = {
    "acceptEdits": "povolit všechny úpravy souborů",
    "bypassPermissions": "povolit vše bez ptaní",
    "default": "běžné ptaní",
    "plan": "režim plánování",
}


def tool_label(name: str) -> str:
    """Český název nástroje, u MCP nástroje jeho vlastní jméno bez serveru."""
    if name.startswith("mcp__"):
        parts = name.split("__")
        return parts[-1] if len(parts) >= 3 and parts[-1] else name
    return TOOL_LABELS_CZ.get(name, name)


def _command(inp: dict) -> str:
    return " ".join(str(inp.get("command") or "").split())


def is_dangerous(name: str, inp: dict) -> bool:
    """Riziková akce se na krabičce schvaluje podržením (stejné vzory jako u AGY)."""
    return name in COMMAND_TOOLS and _danger_match_end(_command(inp)) >= 0


def _first_question(inp: dict) -> dict:
    questions = inp.get("questions")
    if isinstance(questions, list) and questions and isinstance(questions[0], dict):
        return questions[0]
    return {}


def activity_summary(name: str, inp: dict) -> str:
    """Krátký popis nástroje pro historii na krabičce."""
    if not isinstance(inp, dict):
        return ""
    if name in COMMAND_TOOLS:
        return _fix_cz(str(inp.get("description") or "").strip()) or _command(inp)
    for key in ("file_path", "notebook_path"):
        if inp.get(key):
            return Path(str(inp[key])).name
    if name == "AskUserQuestion":
        return str(_first_question(inp).get("question") or "")
    if name == "WebFetch" and inp.get("url"):
        return urlsplit(str(inp["url"])).netloc or str(inp["url"])
    if name == "Skill" and inp.get("skill"):
        return str(inp["skill"])
    for key in ("description", "pattern", "query", "url", "path", "prompt"):
        val = inp.get(key)
        if isinstance(val, str) and val.strip():
            return val
    # Ostatní (např. nástroje MCP): první krátký textový parametr
    for val in inp.values():
        if isinstance(val, str) and 0 < len(val.strip()) <= 120:
            return val
    return ""


def decision_hint(name: str, inp: dict) -> str:
    """Popis pro rozhodnutí: vždy skutečný příkaz nebo celá cesta, popis až za ním."""
    if name in COMMAND_TOOLS:
        cmd = _fix_cz(_command(inp))
        # Karta krabičky ukáže zhruba 130 znaků od začátku: u dlouhého příkazu
        # výřez, ve kterém je riziková část v prvních dvou až třech řádcích
        end = _danger_match_end(cmd)
        if end > 100:
            cmd = "…" + cmd[end - 80:]
        desc = _fix_cz(str(inp.get("description") or "").strip())
        if cmd and desc:
            return f"{cmd} | {desc}"
        return cmd or desc
    for key in ("file_path", "notebook_path"):
        if inp.get(key):
            return _short_path(inp[key])
    if name == "WebFetch" and inp.get("url"):
        return str(inp["url"])
    try:
        return json.dumps(inp, ensure_ascii=False)[:300]
    except Exception:
        return ""


def describe_suggestions(suggestions) -> str:
    """Český popis návrhu „Ano, už se neptat“, nebo "" když ho krabička nenabídne
    (neznámý druh návrhu: raději se zeptá terminál)."""
    if not isinstance(suggestions, list) or not suggestions:
        return ""
    parts = []
    for sug in suggestions:
        if not isinstance(sug, dict):
            return ""
        kind = sug.get("type")
        where = _DEST_CZ.get(str(sug.get("destination") or ""), "")
        if kind == "addRules" and sug.get("behavior", "allow") == "allow":
            rules = []
            for r in sug.get("rules") or []:
                if not isinstance(r, dict) or not r.get("toolName"):
                    return ""
                name = tool_label(str(r["toolName"]))
                rules.append(f"{name} {r['ruleContent']}" if r.get("ruleContent") else name)
            if not rules:
                return ""
            parts.append(f"{', '.join(rules)} {where}".strip())
        elif kind == "setMode":
            mode = str(sug.get("mode") or "")
            parts.append(f"{_MODE_CZ.get(mode, mode)} {where}".strip())
        elif kind == "addDirectories":
            dirs = [_short_path(d, 40) for d in sug.get("directories") or [] if isinstance(d, str)]
            if not dirs:
                return ""
            parts.append(f"přístup do {', '.join(dirs)} {where}".strip())
        else:
            return ""
    return "; ".join(parts)


def join_answers(labels: list) -> str:
    """Odpověď s více volbami stejně jako terminál: oddělené „, “, volba
    obsahující „, “ nebo uvozovky je v uvozovkách JSON."""
    return ", ".join(json.dumps(x, ensure_ascii=False) if (", " in x or '"' in x) else x for x in labels)


def _emit_decision(decision: dict) -> None:
    print(json.dumps({
        "hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": decision},
    }, ensure_ascii=False))


def _approve_raw(req: dict, wait_s: float) -> dict:
    """Pošle dotaz na krabičku. Vrací odpověď mostu, {} když rozhodne terminál."""
    try:
        res = http_json("/approve", req, timeout=wait_s + 10.0)
    except Exception:
        return {}
    dec = res.get("decision")
    if not res.get("connected") or dec in ("fallback", "timeout", "terminal", None):
        return {}
    return res


def _approve(req: dict) -> str:
    """Rozhodnutí z krabičky, nebo "" (rozhodne terminál)."""
    return str(_approve_raw(req, req["timeout"]).get("decision") or "")


def handle_question(inp: dict, workspace: str) -> None:
    """Otázky AskUserQuestion na krabičku: všechny po jedné, i vícenásobný výběr.
    Odpověď je původní popisek volby (i s „(Recommended)“), jako v terminálu."""
    questions = inp.get("questions")
    if not isinstance(questions, list) or not questions or not all(isinstance(q, dict) for q in questions):
        return
    spec = []
    raw = []   # původní popisky voleb ke každé otázce
    for q in questions:
        text = _fix_cz(" ".join(str(q.get("question") or "").split()))
        opts = [o for o in (q.get("options") or []) if isinstance(o, dict)]
        labels = [str(o.get("label") or "") for o in opts]
        if not text or len(opts) < 2 or not all(_clean_label(lbl) for lbl in labels):
            return   # nečitelná otázka: odpoví se v terminálu
        items = []
        for o, lbl in zip(opts, labels):
            desc = _fix_cz(" ".join(str(o.get("description") or "").split()))
            if "(Recommended)" in lbl or "(Doporučeno)" in lbl:
                desc = f"Doporučeno. {desc}".strip()
            items.append({"label": _fix_cz(_clean_label(lbl)), "desc": desc})
        spec.append({
            "header": _fix_cz(str(q.get("header") or "")),
            "question": text,
            "multi": bool(q.get("multiSelect")),
            "options": items,
        })
        raw.append(labels)

    res = _approve_raw({
        "client": "claude",
        "kind": "questions",
        "questions": spec,
        "timeout": QUESTION_TIMEOUT_S,
        "total": QUESTIONS_TOTAL_S,
    }, QUESTIONS_TOTAL_S)
    answers = res.get("answers")
    if res.get("decision") != "answers" or not isinstance(answers, list) or len(answers) != len(questions):
        return
    out = {}
    for q, labels, picked in zip(questions, raw, answers):
        if not isinstance(picked, list) or not picked:
            return
        chosen = [labels[i] for i in picked if isinstance(i, int) and 0 <= i < len(labels)]
        if len(chosen) != len(picked):
            return
        out[str(q.get("question") or "")] = join_answers(chosen) if q.get("multiSelect") else chosen[0]
    updated = dict(inp)
    updated["answers"] = out
    _emit_decision({"behavior": "allow", "updatedInput": updated})


def handle_permission(ctx: dict, workspace: str) -> None:
    name = str(ctx.get("tool_name") or "")
    inp = ctx.get("tool_input") if isinstance(ctx.get("tool_input"), dict) else {}
    if not name or name in SKIP_TOOLS:
        return
    if not ensure_daemon_running():
        return
    if name == "AskUserQuestion":
        handle_question(inp, workspace)
        return

    danger = is_dangerous(name, inp)
    label = "Kritická akce" if danger else tool_label(name)
    suggestions = ctx.get("permission_suggestions")
    # „Ano, už se neptat“ jen u běžné akce a jen se srozumitelným návrhem
    always = "" if danger else describe_suggestions(suggestions)
    req = {
        "client": "claude",
        "tool": f"{label} · {workspace}" if workspace else label,
        "hint": (decision_hint(name, inp) or name)[:HINT_MAX],
        "danger": danger,
        "timeout": PERMISSION_TIMEOUT_S,
    }
    if always:
        req["always"] = always
    dec = _approve(req)
    if dec == "allow":
        _emit_decision({"behavior": "allow"})
    elif dec == "always":
        _emit_decision({"behavior": "allow", "updatedPermissions": suggestions})
    elif dec:
        _emit_decision({"behavior": "deny", "message": DENY_MESSAGE})


def _daemon_starting() -> bool:
    """Most se právě spouští (pokus o spuštění před chvílí): má smysl na něj
    počkat. Při nahrávání firmwaru, po příkazu stop nebo když se spustit
    nedaří, hook nečeká a proces hned skončí."""
    if FLASHING_LOCK.is_file():
        return False
    try:
        return time.time() - SPAWN_MARK.stat().st_mtime < 10
    except OSError:
        return False


def _wait_daemon(timeout_s: float) -> bool:
    """Počká, až most naběhne (spustil ho tento nebo souběžný hook). Jen pro
    události na pozadí: Claude Code na ně nečeká, první události relace se
    tak neztratí, když se most teprve spouští."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            if http_json("/health", None, timeout=0.4).get("ok"):
                return True
        except Exception:
            pass
        time.sleep(0.3)
    return False


def _debug(text: str) -> None:
    """Ladicí výpis do bridge/claude_hook.log, jen s proměnnou ROZHODOVAC_HOOK_DEBUG=1."""
    if os.environ.get("ROZHODOVAC_HOOK_DEBUG") != "1":
        return
    try:
        with open(BRIDGE_DIR / "claude_hook.log", "a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%H:%M:%S')} {os.getpid()} {text}\n")
    except OSError:
        pass


def _post_event(fields: dict) -> None:
    try:
        http_json("/event", {"client": "claude", **fields}, timeout=1.5)
        _debug(f"{fields.get('event')} odesláno")
    except Exception as e:
        _debug(f"{fields.get('event')} neodesláno: {e}")


def main() -> None:
    raw_len = 0
    try:
        raw_bytes = sys.stdin.buffer.read()
        raw_len = len(raw_bytes)
        try:
            raw = raw_bytes.decode("utf-8")
        except UnicodeDecodeError:
            raw = raw_bytes.decode("cp1250", errors="replace")
        ctx = json.loads(raw) if raw.strip() else {}
        if not isinstance(ctx, dict):
            ctx = {}
    except Exception:
        ctx = {}

    event = str(ctx.get("hook_event_name") or (sys.argv[1] if len(sys.argv) > 1 else ""))
    cwd = str(ctx.get("cwd") or "")
    workspace = Path(cwd).name if cwd else ""
    _debug(f"{event or '(bez události)'} start, {raw_len} B vstupu")

    if event == "PermissionRequest":
        handle_permission(ctx, workspace)
        return

    if not ensure_daemon_running() and not (_daemon_starting() and _wait_daemon(DAEMON_START_WAIT_S)):
        _debug(f"{event} most neběží")
        return

    fields = {
        "event": event,
        "sessionId": ctx.get("session_id") or "",
        "workspace": workspace or "Claude Code",
        "transcript": ctx.get("transcript_path") or "",
    }
    if event == "SessionStart":
        fields["source"] = ctx.get("source") or ""
    elif event == "UserPromptSubmit":
        # Zadání od člověka do historie, automaticky vložená (probuzení smyčky,
        # oznámení úloh) jen jako práce bez textu
        if ctx.get("source") in (None, "", "user", "sdk"):
            # Krabička ukáže dva řádky: dlouhé vložené zadání se neposílá celé
            fields["prompt"] = _fix_cz(str(ctx.get("prompt") or "")[:EVENT_TEXT_MAX])
    elif event == "PreToolUse":
        name = str(ctx.get("tool_name") or "")
        inp = ctx.get("tool_input") if isinstance(ctx.get("tool_input"), dict) else {}
        fields["tool"] = tool_label(name)
        fields["summary"] = activity_summary(name, inp)
    elif event == "Notification":
        fields["notificationType"] = ctx.get("notification_type") or ""
    elif event == "Stop":
        fields["lastMessage"] = str(ctx.get("last_assistant_message") or "")[:EVENT_TEXT_MAX]
    elif event == "SubagentStop":
        fields["agentTranscript"] = ctx.get("agent_transcript_path") or ""
    elif event != "SessionEnd":
        return
    _post_event(fields)


if __name__ == "__main__":
    main()
