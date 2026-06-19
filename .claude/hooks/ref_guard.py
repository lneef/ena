#!/usr/bin/env python3
# .claude/hooks/refs-guard.py
#
# Policy:
#   - amzn-drivers/ is readable ONLY by the ena-inspector subagent
#     (main session and all other agents are redirected to delegate)
#   - ena-inspector may write ONLY under docs/wiki/
#
# Registered in .claude/settings.json as a PreToolUse hook with
# matcher: "Read|Grep|Glob|Bash|Write|Edit"
#
# Input:  JSON on stdin (tool_name, tool_input, agent_type when in a subagent)
# Output: deny decision JSON on stdout, or exit 0 with no output
#         (= no decision, normal permission flow applies)
#
# No external dependencies (deliberately no jq — a missing dependency
# would make a bash/jq version fail open).

import json
import re
import sys

INSPECTOR = "inspector"
VERIFIER = "wiki_verifier"
SPEC_VERIFIER = "spec-verifier"
REFS_MARKER = "amzn-drivers"
WIKI_MARKER = "docs/wiki"

# Bash constructs that can write files
BASH_WRITE_RE = re.compile(
    r"(>|>>|\btee\b|\bcp\b|\bmv\b|\binstall\b|\bsed\b[^|;]*-i|\brm\b|\btruncate\b|\bdd\b)"
)


def deny(reason: str) -> None:
    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": reason,
        }
    }))
    sys.exit(0)


def main() -> None:
    try:
        data = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        # Unparseable input: fail closed rather than open.
        deny("refs-guard hook could not parse tool input; blocking as a precaution.")
        return

    print(data)
    tool = data.get("tool_name", "") or ""
    agent = data.get("agent_type") or "main"
    tool_input = data.get("tool_input") or {}

    if tool == "Bash":
        paths = tool_input.get("command", "") or ""
    else:
        paths = " ".join(
            str(tool_input.get(k))
            for k in ("file_path", "path", "pattern", "notebook_path")
            if tool_input.get(k)
        )

    touches_refs = REFS_MARKER in paths

    # -----------------------------------------------------------------
    # ena-inspector: read anywhere (incl. refs/), write only docs/wiki/
    # -----------------------------------------------------------------
    if agent in (INSPECTOR, VERIFIER, SPEC_VERIFIER):
        if tool in ("Write", "Edit"):
            fp = tool_input.get("file_path", "") or ""
            if WIKI_MARKER not in fp:
                deny(
                        "you may only write or edit in docs/wiki"
                        )
        if tool == "Bash":
            cmd = tool_input.get("command", "") or ""
            if BASH_WRITE_RE.search(cmd) and WIKI_MARKER not in cmd:
                deny(
                    "inspector may only write under docs/wiki/. "
                    "Use Read/Grep for research; record findings in the wiki."
                )
        sys.exit(0)

    # -----------------------------------------------------------------
    # Everyone else (main session, other subagents): refs/ is sealed
    # -----------------------------------------------------------------
    if touches_refs:
        deny(
            "refs/ is delegated context. Spawn the ena-inspector subagent "
            "with a specific question instead of reading reference sources "
            "directly. Settled facts live in docs/wiki/ and the contract "
            "docs in docs/."
        )

    sys.exit(0)


if __name__ == "__main__":
    main()
