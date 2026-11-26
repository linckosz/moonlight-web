#!/usr/bin/env python3
# MoonlightWeb — browser-based Sunshine/GameStream client.
# Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every program the server starts receives its standard channels and nothing else.

Read over the server's sources (backend/src, backend/native-host/src and the Linux
launcher). Comments and string literals are left out. Three rules:

1. QProcess::startDetached and QProcess::execute are never called directly:
   mw::startDetached (common/DetachedProcess.h) is the one detached start.
2. A system call that starts a program (CreateProcess*, ShellExecute*, popen,
   system, posix_spawn*, fork, exec*, NSTask, QDesktopServices::openUrl) appears
   only where scripts/ci/child-launch-allowlist.txt says so, as many times as it
   says, with what the launch does and why nothing of ours passes.
3. A QProcess built in these sources goes through mw::passOnlyStdio (or the
   detached helper) before it is started.
   The rule follows a variable's name, not the object: a QProcess it cannot
   follow that way (never started under its name in the file, or built without
   a name) fails rather than passes.

Run from the repository root: `python3 scripts/ci/check-child-launch.py`.
`--self-test` runs the rules on small samples first, so a rule that stopped
firing is seen.
"""

import os
import re
import subprocess
import sys

ROOTS = ("backend/src/", "backend/native-host/src/", "backend/packaging/linux/")
EXTENSIONS = (".c", ".cpp", ".h", ".hpp", ".m", ".mm")
ALLOWLIST = "scripts/ci/child-launch-allowlist.txt"

# The two helpers build and start their QProcess themselves.
HELPERS = ("backend/src/common/ChildStdio.cpp", "backend/src/common/DetachedProcess.h")

FORBIDDEN = [
    ("QProcess::startDetached", re.compile(r"\bQProcess\s*::\s*startDetached\s*\(")),
    ("QProcess::execute", re.compile(r"\bQProcess\s*::\s*execute\s*\(")),
]

LAUNCHES = [
    ("CreateProcess", re.compile(r"\bCreateProcess(?:AsUser|WithLogon|WithToken)?[AW]?\s*\(")),
    ("ShellExecute", re.compile(r"\bShellExecute(?:Ex)?[AW]?\s*\(")),
    ("WinExec", re.compile(r"\bWinExec\s*\(")),
    ("popen", re.compile(r"(?<![\w.>])(?:::)?_?w?popen\s*\(")),
    ("system", re.compile(r"(?:(?<![\w.>:])_?w?system|(?<!\w)(?:std)?::_?w?system)\s*\(")),
    ("_spawn", re.compile(r"\b_w?spawn[lv]p?e?\s*\(")),
    ("posix_spawn", re.compile(r"\bposix_spawnp?\s*\(")),
    ("fork", re.compile(r"(?<![\w.>])(?:::)?(?:_Fork|vfork|fork)\s*\(")),
    ("exec", re.compile(r"(?<![\w.>])(?:::)?f?exec(?:l|le|lp|v|ve|vp|vpe|veat)\s*\(")),
    ("NSTask", re.compile(r"\bNSTask\b")),
    ("NSWorkspace open", re.compile(r"\bopen(?:URL|URLs|ApplicationAtURL)\s*:")),
    ("QDesktopServices::openUrl", re.compile(r"\bQDesktopServices\s*::\s*openUrl\s*\(")),
]

BUILT = [
    re.compile(r"\bQProcess\s+([A-Za-z_]\w*)\s*[;({]"),
    re.compile(r"\b([A-Za-z_][\w.>-]*)\s*=\s*new\s+QProcess\b"),
    re.compile(r"\b([A-Za-z_][\w.>-]*)\s*=\s*std\s*::\s*make_(?:unique|shared)\s*<\s*QProcess\s*>"),
    re.compile(r"\bunique_ptr\s*<\s*QProcess\s*>\s*([A-Za-z_]\w*)\s*[({]"),
]
ANY_BUILT = re.compile(r"\bnew\s+QProcess\b|\bmake_(?:unique|shared)\s*<\s*QProcess\s*>")


def blank(text):
    """The source with comments and string or character literals turned into spaces.

    Line breaks are kept, so a position still gives its line.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == "R" and text[i + 1:i + 2] == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            m = re.match(r'R"([^()\\\s]{0,16})\(', text[i:])
            if not m:
                out.append(c)
                i += 1
                continue
            end = text.find(")" + m.group(1) + '"', i)
            j = n if end < 0 else end + len(m.group(1)) + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r"[^\n]", " ", text[i + 1:j - 1]) + (c if j - 1 > i else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def check_text(path, text, allowed):
    """Problems in one file, as lines. @p allowed is the allowlist's count for it, or None."""
    problems = []
    code = blank(text)

    for name, rx in FORBIDDEN:
        for m in rx.finditer(code):
            problems.append("%s:%d: %s: use mw::startDetached (common/DetachedProcess.h)"
                            % (path, line_of(code, m.start()), name))

    found = []
    for name, rx in LAUNCHES:
        for m in rx.finditer(code):
            found.append((line_of(code, m.start()), name))
    found.sort()
    if found and allowed is None:
        for line, name in found:
            problems.append("%s:%d: %s starts a program: add the file to %s with what it starts "
                            "and why nothing of ours passes" % (path, line, name, ALLOWLIST))
    elif allowed is not None and len(found) != allowed:
        where = ", ".join("%d (%s)" % f for f in found) or "none"
        problems.append("%s: %d launch(es) found, %s lists %d: %s; update the entry and its "
                        "reason" % (path, len(found), ALLOWLIST, allowed, where))

    if path not in HELPERS:
        seen = set()
        for rx in BUILT:
            for m in rx.finditer(code):
                seen.add(m.start())
                problems += follow(path, code, m.group(1), m.end())
        for m in ANY_BUILT.finditer(code):
            if not any(abs(m.start() - s) < 200 for s in seen):
                problems.append("%s:%d: a QProcess this check cannot follow: keep it in a named "
                                "variable" % (path, line_of(code, m.start())))
    return problems


def follow(path, code, var, after):
    """A QProcess called @p var, built at @p after: it must meet passOnlyStdio before its start."""
    name = re.escape(var.split(".")[-1].split(">")[-1])
    start = re.compile(r"(?<![\w])" + name + r"\s*(?:\.|->)\s*(?:start|startDetached|open)\s*\(")
    helper = re.compile(r"\b(?:passOnlyStdio|closeInheritedDescriptors)\s*\(\s*\*?\s*" + name + r"\b")
    s = start.search(code, after)
    if not s:
        return ["%s:%d: QProcess '%s' is never started in this file: this check cannot follow it"
                % (path, line_of(code, after), var)]
    if not helper.search(code, after, s.start()):
        return ["%s:%d: QProcess '%s' is started without mw::passOnlyStdio (common/ChildStdio.h)"
                % (path, line_of(code, s.start()), var)]
    return []


def read_allowlist(path):
    entries, problems = {}, []
    with open(path, encoding="utf-8") as f:
        for number, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split("|", 2)]
            if len(parts) != 3 or not parts[1].isdigit() or not parts[2]:
                problems.append("%s:%d: expected 'path | count | what it starts and why nothing "
                                "passes'" % (path, number))
                continue
            if parts[0] in entries:
                problems.append("%s:%d: %s listed twice" % (path, number, parts[0]))
            entries[parts[0]] = int(parts[1])
    return entries, problems


def sources():
    listed = subprocess.run(["git", "ls-files", "--", *ROOTS], check=True, capture_output=True,
                            text=True).stdout.split("\n")
    return [p for p in listed if p.endswith(EXTENSIONS)]


def self_test():
    cases = [
        ("QProcess::startDetached(a, {b});", None, 1),
        ("QProcess::execute(a);", None, 1),
        ("// QProcess::startDetached(a);\nauto s = \"system(x)\";", None, 0),
        ("::CreateProcessW(a);", None, 1),
        ("::CreateProcessW(a);", 1, 0),
        ("::CreateProcessW(a); ShellExecuteExW(&i);", 1, 1),
        ("std::system(\"x\"); ::system(y); system(z);", None, 3),
        ("QRandomGenerator::system()->bounded(4); obj.system(1);", None, 0),
        ("p.setCreateProcessArgumentsModifier(f); QProcess::CreateProcessArguments* a;", None, 0),
        ("posix_spawnp(&c, a, &f, nullptr, v, e);", None, 1),
        ("pid_t p = _Fork(); execve(a, b, c);", None, 2),
        ("QDesktopServices::openUrl(u);", None, 1),
        ("QProcess p; p.setProgram(a); p.start();", None, 1),
        ("QProcess p; mw::passOnlyStdio(p); p.start();", None, 0),
        ("auto* q = new QProcess(this); q->start(a); mw::passOnlyStdio(*q);", None, 1),
        ("m_Proc = new QProcess(this); mw::passOnlyStdio(*m_Proc); m_Proc->start();", None, 0),
        ("run(new QProcess(this));", None, 1),
        ("QProcess p;", None, 1),
        ("const char* s = R\"(QProcess::execute(a))\";", None, 0),
    ]
    bad = 0
    for i, (text, allowed, want) in enumerate(cases, 1):
        got = check_text("sample%d.cpp" % i, text, allowed)
        if len(got) != want:
            bad += 1
            print("self-test %d: %d problem(s), expected %d: %s" % (i, len(got), want, got))
    print("self-test: %d case(s), %d wrong" % (len(cases), bad))
    return bad == 0


def main():
    if "--self-test" in sys.argv:
        return 0 if self_test() else 1
    entries, problems = read_allowlist(ALLOWLIST)
    files = sources()
    for path in files:
        with open(path, encoding="utf-8", errors="replace") as f:
            problems += check_text(path, f.read(), entries.get(path))
    for path in sorted(set(entries) - set(files)):
        problems.append("%s: %s is not a source this check reads" % (ALLOWLIST, path))
    for p in problems:
        print(p)
    print("child launches: %d file(s) read, %d allowlist entr(ies), %d problem(s)"
          % (len(files), len(entries), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    sys.exit(main())
