#!/usr/bin/env python
"""
Project-local PreToolUse hook (matcher: Bash). Opt-in per project — copy this
file to <project>/.claude/hooks/qa-gate.py, copy
templates/qa-gate/qa-gate.config.example.json to
<project>/.claude/qa-gate.config.json (and edit it for the project's real
stack), then merge templates/qa-gate/settings.snippet.json's hooks block into
the project's own .claude/settings.json.

Not installed globally on purpose: it needs a project-specific test/lint
command and assumes git. A global hook can't make either assumption for
every project.

What it actually gates, and how:

1. `git commit` is blocked unless the project's real test command (and lint
   command, if configured) are re-run right now, by this script, and exit 0.
   This does NOT trust any agent's self-reported "tests passed" -- it
   recomputes the signal itself, which is the only way to make it a real
   gate instead of a trust-based one.

2. If the staged diff touches non-meta files (anything outside `.claude/`),
   commit is blocked unless a `hattori` review-log entry exists under
   `.claude/review-log/` with `verdict: PASS`, fresher (by mtime) than every
   staged file. This closes a gap an earlier version of this script had:
   `hattori`'s BLOCKER verdict was never actually checked by anything, only
   `yagyu`'s -- so a BLOCKER on readability/error-handling/duplication never
   stopped a commit. Once a project has ever produced a `hattori` review-log,
   every subsequent commit touching non-meta files is held to the same
   freshness bar `yagyu` was already held to below. Turn off via
   `require_hattori: false` in the config if this is too strict for a given
   project (e.g. a throwaway prototype).

3. If the staged diff touches auth-shaped content (heuristic: a fixed
   keyword list against `git diff --cached`), commit is also blocked unless
   a `yagyu` review-log entry exists with `verdict: PASS`, fresh by the same
   rule. Set `require_yagyu_always: true` in the config to hold every commit
   (not just auth-shaped ones) to this bar, same as `require_hattori`.

Configuration lives in `.claude/qa-gate.config.json` (see
qa-gate.config.example.json for the schema/defaults) rather than only in env
vars. A hardcoded `npm test`/`npm run lint` default silently assumed every
project is Node, which is wrong more often than right (e.g. a Python/Flask
backend with no lint tool configured at all). If no config file exists and
no `QA_GATE_TEST_CMD` env var is set, the gate blocks with an explicit
"configure me" message instead of silently running a command that doesn't
apply to this project. `lint_cmd` is optional: leave it unset/null to skip
the lint step entirely (many projects have no separate lint command).
`QA_GATE_TEST_CMD`/`QA_GATE_LINT_CMD` env vars, if set, override the config
file -- handy for a one-off manual check without editing the file.

Known limitations, stated plainly rather than silently assumed away:
- The auth heuristic is a keyword match, not semantic understanding. It can
  miss auth logic in a generically-named file, and can false-positive on
  unrelated code that happens to mention one of the words.
- Freshness is mtime-based. It can be fooled by touching the review-log file
  without re-reading the diff, and is unreliable across machines with clock
  skew (e.g. a repo synced via a network drive with a different clock).
- Anyone can delete/edit the review-log file directly to fake a PASS. This
  is the same trust boundary as any local pre-commit hook -- it stops
  accidental gaps, not a deliberately adversarial actor with write access to
  the repo.
- `require_hattori`/`require_yagyu_always` only look at whether a review-log
  file exists and is fresh -- they cannot force an agent to have been
  invoked before the very first one ever runs. The bar only becomes "must be
  fresh for this exact commit" once at least one review-log for that agent
  exists in the project. First commit after installing the gate on a brand
  new project is not held to a freshness bar it has no way to satisfy yet.

Exit code 0 -> allow. Exit code 2 -> block (stderr shown to Claude/user).
"""
import json
import os
import re
import subprocess
import sys

CONFIG_FILENAME = "qa-gate.config.json"
DEFAULT_CONFIG = {
    "test_cmd": None,
    "lint_cmd": None,
    "require_hattori": True,
    "require_yagyu_always": False,
}
AUTH_KEYWORDS = re.compile(
    r"\b(auth|login|logout|session|token|password|permission|credential)\b", re.I
)


def run(cmd, cwd):
    return subprocess.run(
        cmd, cwd=cwd, shell=True, capture_output=True, text=True
    )


def normalize_path(path):
    """Git Bash reports cwd as /c/Users/... ; Windows subprocess needs
    C:\\Users\\... . Without this, git detection silently fails and the gate
    no-ops (fail-open) instead of enforcing anything -- found by testing this
    script from Git Bash, where payload["cwd"] came through POSIX-style."""
    match = re.match(r"^/([a-zA-Z])/(.*)$", path)
    if match and os.name == "nt":
        drive, rest = match.groups()
        return "{}:\\{}".format(drive.upper(), rest.replace("/", "\\"))
    return path


def load_config(repo_root):
    config = dict(DEFAULT_CONFIG)
    cfg_path = os.path.join(repo_root, ".claude", CONFIG_FILENAME)
    if os.path.isfile(cfg_path):
        try:
            with open(cfg_path, "r", encoding="utf-8") as fh:
                user_cfg = json.load(fh)
        except (json.JSONDecodeError, OSError):
            user_cfg = {}
        for key in DEFAULT_CONFIG:
            if key in user_cfg:
                config[key] = user_cfg[key]

    # Env vars override the config file -- for a one-off manual check
    # without editing the file, not the primary configuration path.
    if os.environ.get("QA_GATE_TEST_CMD"):
        config["test_cmd"] = os.environ["QA_GATE_TEST_CMD"]
    if os.environ.get("QA_GATE_LINT_CMD"):
        config["lint_cmd"] = os.environ["QA_GATE_LINT_CMD"]

    return config


def is_meta_path(path):
    norm = path.replace("\\", "/")
    if norm.startswith("./"):
        norm = norm[2:]
    return norm.startswith(".claude/")


def block(reason):
    sys.stderr.write("qa-gate: commit bloqueado.\n{}\n".format(reason))
    return 2


def check_reviewer(agent, repo_root, staged_files, why_required):
    """Returns a block reason string if `agent`'s latest review-log is
    missing, not PASS, or stale relative to the staged files; None if OK."""
    review_dir = os.path.join(repo_root, ".claude", "review-log")
    reports = []
    if os.path.isdir(review_dir):
        reports = sorted(
            f for f in os.listdir(review_dir) if f.endswith("-{}.md".format(agent))
        )
    if not reports:
        return (
            "{} Nenhum review-log de {} em .claude/review-log/. Rode o "
            "subagent {} antes de commitar.".format(why_required, agent, agent)
        )

    latest = reports[-1]
    latest_path = os.path.join(review_dir, latest)
    with open(latest_path, "r", encoding="utf-8") as fh:
        content = fh.read()
    verdict_match = re.search(r"^verdict:\s*(\w+)", content, re.M)
    verdict = verdict_match.group(1) if verdict_match else None

    if verdict != "PASS":
        return (
            "Ultimo review-log de {} ({}) tem verdict={}, nao PASS. "
            "Resolva os achados BLOCKER antes de commitar.".format(
                agent, latest, verdict
            )
        )

    review_mtime = os.path.getmtime(latest_path)
    staged_mtimes = [
        os.path.getmtime(os.path.join(repo_root, f))
        for f in staged_files
        if os.path.exists(os.path.join(repo_root, f))
    ]
    if staged_mtimes and review_mtime < max(staged_mtimes):
        return (
            "Review-log de {} ({}) e mais antigo que a ultima edicao de um "
            "arquivo staged -- pode ter revisado uma versao anterior do "
            "diff. Rode {} de novo antes de commitar.".format(
                agent, latest, agent
            )
        )

    return None


def main():
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0

    if payload.get("tool_name") != "Bash":
        return 0

    command = (payload.get("tool_input") or {}).get("command") or ""
    if not re.search(r"\bgit\s+commit\b", command, re.I):
        return 0

    cwd = normalize_path(payload.get("cwd") or os.getcwd())

    root = run("git rev-parse --show-toplevel", cwd)
    if root.returncode != 0:
        return 0  # not a git repo -- nothing for this hook to gate
    repo_root = root.stdout.strip()

    staged = run("git diff --cached --name-only", repo_root)
    staged_files = [f for f in staged.stdout.splitlines() if f.strip()]
    if not staged_files:
        return 0  # nothing staged -- let git report "nothing to commit"

    config = load_config(repo_root)

    if not config["test_cmd"]:
        return block(
            "Nenhum test_cmd configurado. Crie .claude/{} a partir de "
            "templates/qa-gate/qa-gate.config.example.json (ou defina "
            "QA_GATE_TEST_CMD) com o comando de teste real deste "
            "projeto.".format(CONFIG_FILENAME)
        )

    test_result = run(config["test_cmd"], repo_root)
    if test_result.returncode != 0:
        return block(
            "Suite de teste falhou (`{}`):\n{}\n{}".format(
                config["test_cmd"], test_result.stdout[-2000:], test_result.stderr[-2000:]
            )
        )

    if config["lint_cmd"]:
        lint_result = run(config["lint_cmd"], repo_root)
        if lint_result.returncode != 0:
            return block(
                "Lint falhou (`{}`):\n{}\n{}".format(
                    config["lint_cmd"], lint_result.stdout[-2000:], lint_result.stderr[-2000:]
                )
            )

    non_meta_staged = [f for f in staged_files if not is_meta_path(f)]

    if non_meta_staged and config["require_hattori"]:
        reason = check_reviewer(
            "hattori", repo_root, staged_files,
            why_required="Diff toca arquivo(s) fora de .claude/.",
        )
        if reason:
            return block(reason)

    diff_content = run("git diff --cached", repo_root).stdout
    touches_auth = bool(AUTH_KEYWORDS.search(diff_content)) or any(
        AUTH_KEYWORDS.search(f) for f in staged_files
    )

    if non_meta_staged and (touches_auth or config["require_yagyu_always"]):
        why = (
            "Diff toca padroes de auth (heuristica por palavra-chave)."
            if touches_auth
            else "require_yagyu_always esta ativo neste projeto."
        )
        reason = check_reviewer("yagyu", repo_root, staged_files, why_required=why)
        if reason:
            return block(reason)

    return 0


if __name__ == "__main__":
    sys.exit(main())
