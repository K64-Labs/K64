#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def git(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args],
        cwd=ROOT,
        text=True,
    ).strip()


def git_lines(*args: str) -> list[str]:
    out = git(*args)
    return [line for line in out.splitlines() if line.strip()]


@dataclass
class Commit:
    sha: str
    subject: str
    files: list[str]


SECTION_RULES = [
    ("Kernel and Runtime", ("k64_kernel.c", "k64_sched", "k64_vmm", "k64_pmm", "k64_idt", "k64_irq", "k64_isr", "boot.s", "longmode.s", "k64_hotreload", "k64_reload")),
    ("Services and Drivers", ("k64_system", "k64_modules", "k64s/", "k64m/", "k64s_def/", "k64m_def/")),
    ("Filesystem and Loader", ("k64_fs", "grub/", "tools/mk_k64fs.py", "tools/build_k64x.py", "k64_elf", "ex/")),
    ("Shell and User Space", ("k64_shell", "k64_user", "k64_keyboard", "k64_terminal", "k64_serial")),
    ("Build, Tooling, and Docs", ("Makefile", ".github/", "tools/", "README.md", ".gitignore")),
    ("Tests", ("tests/",)),
]


def previous_release_tag() -> str | None:
    try:
        return git("describe", "--tags", "--abbrev=0", "--match", "v*", "HEAD^")
    except subprocess.CalledProcessError:
        return None


def collect_commits(base_ref: str | None) -> list[Commit]:
    rev = "HEAD" if not base_ref else f"{base_ref}..HEAD"
    rows = git_lines("log", "--reverse", "--format=%H%x1f%s", rev)
    commits: list[Commit] = []
    for row in rows:
        sha, subject = row.split("\x1f", 1)
        files = git_lines("diff-tree", "--no-commit-id", "--name-only", "-r", sha)
        commits.append(Commit(sha=sha, subject=subject, files=files))
    return commits


def classify_commit(commit: Commit) -> str:
    for section, markers in SECTION_RULES:
        for path in commit.files:
            if any(path == marker or path.startswith(marker) for marker in markers):
                return section
    return "Miscellaneous"


def build_notes(version: str) -> str:
    prev_tag = previous_release_tag()
    commits = collect_commits(prev_tag)
    if not commits:
        return f"# K64 v{version}\n\nNo code changes since the previous release.\n"

    grouped: dict[str, list[Commit]] = {}
    for commit in commits:
        grouped.setdefault(classify_commit(commit), []).append(commit)

    lines: list[str] = []
    lines.append(f"# K64 v{version}")
    lines.append("")
    if prev_tag:
        lines.append(f"Changes since `{prev_tag}`")
    else:
        lines.append("Initial tagged release notes")
    lines.append("")
    lines.append("## Highlights")
    lines.append("")
    lines.append(f"- {len(commits)} commit(s) are included in this release.")
    lines.append(f"- Kernel artifact: `k64-kernel-v{version}.elf`")
    lines.append("- Boot artifact: `k64.iso`")
    lines.append("- Validation: `make test` in CI before publishing")
    lines.append("")

    for section, _ in SECTION_RULES + [("Miscellaneous", tuple())]:
        section_commits = grouped.get(section, [])
        if not section_commits:
            continue
        lines.append(f"## {section}")
        lines.append("")
        for commit in section_commits:
            lines.append(f"- {commit.subject} (`{commit.sha[:7]}`)")
        lines.append("")

    lines.append("## Validation")
    lines.append("")
    lines.append("- Host-side unit tests: shell command parser, string helpers, filesystem core")
    lines.append("- Packaging check: GRUB config validation")
    lines.append("- Runtime check: QEMU boot smoke test")
    lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[1] != "write":
        print("usage: gen_release_notes.py write <version>", file=sys.stderr)
        return 2

    version = sys.argv[2]
    sys.stdout.write(build_notes(version))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
