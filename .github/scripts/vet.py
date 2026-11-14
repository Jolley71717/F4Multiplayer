#!/usr/bin/env python3
"""Trusted helpers for the vetting and packaging workflows (vet.yml, vet-label.yml, package.yml).

Everything here comes from main. A pull request's files are only read as data: git objects, the API's file list. Nothing
from the pull request is imported or run, except its ActionScript sources, which `compile` hands to the pinned Flex
compiler. Standard library only; run it with `python3 -I`.

  flags FILES.jsonl [--owner] [--changed-files N] [--comment FILE]
                                       the API's file list -> {"needs_vetting": ..., "reasons": [...]}
  inspect --repo DIR --base SHA --head SHA --api-files FILES.jsonl --out FILE
  toolchain --work DIR [--unpack DIR] [--strict] [--claims-repo DIR --claims-rev REV] --out FILE
  java --work DIR --home DIR --report FILE   the pinned Amazon Corretto, checked and unpacked
  compile --java-home DIR --flex DIR --ui DIR --out DIR --report FILE
  compare --built DIR --ui DIR --report FILE
  assemble --parts DIR --out FILE      the final report, from the parts the report job wrote; exit 1 if the vet failed

The toolchain's pins are below (PINS), in this file on main: vet.yml refuses to run unless its copy of this file is
main's, byte for byte.
"""

import argparse
import base64
import gzip
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import traceback
import unicodedata
import urllib.error
import urllib.request

# The toolchain the menu movies are built with, pinned here only (vet.yml and package.yml both use these). The Flex
# tarball is signed by an Apache release manager whose key is in Apache's KEYS file; Adobe signs nothing. Amazon
# Corretto 8 and 11 both compile the movies with no warning and give the same bytes; 11 is supported longer.
PINS = {
    "FLEX_URL": "https://archive.apache.org/dist/flex/4.16.1/binaries/apache-flex-sdk-4.16.1-bin.tar.gz",
    "FLEX_SHA256": "17fda7ac8d3e476cad3127f345455ef316acfb87c6f4322e5897bd8d9b09388e",
    "FLEX_KEY_FINGERPRINT": "44998F3E242727E94C4BADEB6B0A7EC905061FC8",  # the key that signed 4.16.1, in Apache's KEYS
    "FLEX_KEYS_URLS": "https://archive.apache.org/dist/flex/KEYS https://downloads.apache.org/flex/KEYS",
    "PLAYERGLOBAL_URL": "https://fpdownload.macromedia.com/get/flashplayer/updaters/32/playerglobal32_0.swc",
    "PLAYERGLOBAL_SHA256": "7d4d6168d27603cfb3b750302448e354e0bbc1bdd58f5d101c3dcf6891e9bb65",
    # https://github.com/corretto/corretto-11/releases/tag/11.0.33.0.10 publishes this checksum.
    "CORRETTO_URL": "https://corretto.aws/downloads/resources/11.0.33.0.10/amazon-corretto-11.0.33.0.10-linux-x64.tar.gz",
    "CORRETTO_SHA256": "f368ac379f3b20e266080e80fcc1f5e8567d044a4f895c8786220cab5b580c15",
}

# What counts as a binary by its name, whatever git makes of its bytes.
BINARY_EXTENSIONS = {
    # code and libraries
    "swf", "swc", "dll", "exe", "pdb", "lib", "obj", "o", "a", "so", "dylib", "jar", "class", "pyc", "wasm", "node",
    "msi", "msix", "appx", "sys", "scr", "com", "efi", "bin", "dat",
    # archives
    "zip", "7z", "rar", "gz", "tgz", "bz2", "xz", "zst", "tar", "cab", "iso", "img", "vhd", "vhdx", "deb", "rpm", "apk",
    # images, fonts, media, documents
    "png", "jpg", "jpeg", "gif", "bmp", "tga", "dds", "ico", "webp", "psd", "tif", "tiff", "ttf", "otf", "woff", "woff2",
    "fla", "wav", "mp3", "ogg", "flac", "mp4", "webm", "bk2", "pdf",
    # the game's own formats
    "nif", "ba2", "bsa", "esp", "esm", "esl", "pex", "xwm", "fuz", "lip", "hkx",
}

# Paths where a change can run code on a machine that builds or works on the repository, or changes what the
# repository trusts: (pattern, reason, also for the owner's own PRs). Matched without regard to case, as Windows checks
# files out: `claude.md` or `.VSCode/` lands on the same file as `CLAUDE.md` or `.vscode/`.
SENSITIVE = [(re.compile(pattern, re.IGNORECASE), reason, owner) for pattern, reason, owner in [
    (r"(^|/)xmake\.lua$", "xmake.lua (build script)", False),
    (r"(^|/)(?!xmake\.lua$)[^/]+\.lua$", "Lua (xmake script)", False),
    (r"^\.github/", ".github (workflows, CI)", True),
    (r"^tools/", "tools/ (scripts)", False),
    (r"(^|/)\.gitattributes$", ".gitattributes", False),
    (r"(^|/)\.gitmodules$", ".gitmodules (submodules)", True),
    (r"(^|/)(\.githooks|\.husky|\.lefthook|git-hooks|hooks)/|(^|/)(\.pre-commit-config|\.?lefthook)\.ya?ml$",
     "git hooks", True),
    (r"^lib/", "lib/ (submodules)", False),
    # Anything an agent loads when it starts in the repository: instructions, settings, hooks, MCP servers, skills.
    (r"(^|/)(AGENTS|CLAUDE|GEMINI)(\.[\w-]+)?\.md$|(^|/)\.(claude|codex|cursor|gemini|agents|windsurf|continue)(/|$)"
     r"|(^|/)\.mcp\.json$|(^|/)\.(cursorrules|windsurfrules)$", "AI agents' config (can run commands)", True),
    (r"(^|/)\.(vscode|devcontainer|idea|vs)(/|$)", "editor config (can run tasks on open)", True),
    # Build and environment files that IDEs, MSBuild, CMake, direnv or git LFS load and act on by themselves.
    (r"(^|/)(Directory\.Build\.(props|targets)|CMakeLists\.txt|CMake(User)?Presets\.json)$|\.(sln|vcxproj|props|targets)$",
     "MSBuild, CMake or Visual Studio files (run on open or build)", True),
    (r"(^|/)\.(envrc|lfsconfig)$", ".envrc or .lfsconfig (direnv, git LFS)", True),
    (r"^\.oss-scanner/", ".oss-scanner (scanner build)", False),
]]

# Text whose URLs are left out: prose, and the players' translations.
PROSE = re.compile(r"\.(md|txt)$|^dist/lang/", re.IGNORECASE)
URL = re.compile(r"""(?i)\b(?:https?|ftps?|wss?|git|ssh|file|s3|gs|sftp)://[^\s"'<>`\\)\]}|]+|\bgit@[\w.-]+:[\w./~-]+""")
LONG_LINE = 1000  # an added line this long may be data in disguise (base64, a minified blob)
REPORT_LIMIT = 400  # rows per list
LINE_LIMIT = 300  # characters per row; a longer one is cut and says so
# GitHub drops a job summary over 1 MiB without failing the step, so a report that big could vanish from a green run.
# inspect and assemble fail instead, with a short report saying why.
REPORT_BYTES_LIMIT = 900_000
# What vet.yml's inspect job sends in place of a trusted part too long for a job output; assemble fails the vet on it.
TRUSTED_STAND_IN = "## Trusted inspection\n\nToo long to pass on"


# Invisible or blank characters that are not format characters: the combining grapheme joiner, the variation
# selectors, the Hangul fillers, the Khmer inherent vowels, the Mongolian variation selectors and the braille blank.
INVISIBLE = re.compile("[\u034f\u115f\u1160\u17b4\u17b5\u180b-\u180f\u2800\u3164\ufe00-\ufe0f\uffa0"
                       "\U000e0100-\U000e01ef]")


def clean(text):
    """PR-controlled text made safe for a Markdown code block: no control characters (C0 as '?', C1 as <U+XXXX>), no
    line breaks, and invisible characters (format characters such as a right-to-left override, private use,
    separators, spaces other than U+0020, the INVISIBLE ones, and a file name's bytes that aren't UTF-8, which
    surrogateescape keeps as U+DC80 to U+DCFF) shown as <U+XXXX>."""
    out = []
    for ch in str(text):
        if ord(ch) < 32 or ord(ch) == 127:
            out.append("?")
        elif (unicodedata.category(ch) in ("Cc", "Cf", "Co", "Cs", "Cn", "Zl", "Zp")
              or (unicodedata.category(ch) == "Zs" and ch != " ") or INVISIBLE.match(ch)):
            out.append(f"<U+{ord(ch):04X}>")
        else:
            out.append(ch)
    return "".join(out)


def neutral(text):
    """One line for a job's log. The runner obeys a workflow command found anywhere in a log line ("##[add-mask]...",
    "::stop-commands::..."), so after clean() no two colons and no two hashes stay side by side."""
    return re.sub(r"#(?=#)", "# ", re.sub(r":(?=:)", ": ", clean(text)))


def log(text):
    """Write to stderr, neutralized line by line: what goes to a log may hold a PR's file names."""
    for line in str(text).splitlines() or [""]:
        print(neutral(line), file=sys.stderr)


def cut(text, limit=LINE_LIMIT):
    """One cleaned row of at most `limit` characters, marked when cut."""
    shown = clean(text)
    if len(shown) <= limit:
        return shown
    return shown[:limit] + f" ... (cut: {len(str(text))} characters)"


def block(lines, limit=REPORT_LIMIT):
    """Lines as an indented Markdown code block (nothing in it is rendered), or '(none)'."""
    lines = list(lines)
    if not lines:
        return "    (none)\n"
    shown = lines[:limit]
    out = "".join("    " + cut(line) + "\n" for line in shown)
    if len(lines) > limit:
        out += f"    ... and {len(lines) - limit} more\n"
    return out


def write_report(path, text, title):
    """Write a report for a job summary; when it is over REPORT_BYTES_LIMIT, write a short one saying so and fail."""
    size = len(text.encode("utf-8"))
    if size <= REPORT_BYTES_LIMIT:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"{title}\n\n**Vet FAILED: this report came to {size} bytes, over the {REPORT_BYTES_LIMIT} a job summary "
                "can be trusted to hold (GitHub drops one over 1 MiB without a word).** A PR can make it that big on "
                "purpose; read its changes another way before going on.\n\n")
    sys.exit(f"report too large: {size} bytes")


def extension(path):
    name = path.rsplit("/", 1)[-1]
    return name.rsplit(".", 1)[-1].lower() if "." in name else ""


def sensitive_reasons(path, owner=False):
    """Why `path` is sensitive; for the owner's own PR, only the reasons that count there."""
    return [reason for pattern, reason, for_owner in SENSITIVE if pattern.search(path) and (for_owner or not owner)]


def read_json_stream(path):
    """JSON values one after another (what `gh api --paginate --jq '.[]'` writes)."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    decoder = json.JSONDecoder()
    at, values = 0, []
    while True:
        while at < len(text) and text[at].isspace():
            at += 1
        if at >= len(text):
            return values
        value, at = decoder.raw_decode(text, at)
        values.append(value)


def run(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, **kwargs).stdout


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


# ---------------------------------------------------------------------------------------------------------- flags


# In xmake.lua, the lines that fetch something: package requirements, package repositories and definitions, includes.
XMAKE_FETCH = re.compile(r"^[-+].*\b(add_requires|add_requireconfs|add_repositories|package|add_urls|set_urls|add_versions|includes)\s*\(",
                         re.MULTILINE)


def patch_urls(patch):
    """URLs on the lines a patch adds, less those on the lines it removes (a URL moved within the file is not new)."""
    added, removed = set(), set()
    for line in patch.splitlines():
        if line.startswith("+") and not line.startswith("+++"):
            added.update(url.rstrip(".,;:") for url in URL.findall(line))
        elif line.startswith("-") and not line.startswith("---"):
            removed.update(url.rstrip(".,;:") for url in URL.findall(line))
    return sorted(added - removed)


FILES_API_LIMIT = 3000  # the most files the API lists for a pull request


def count(f, key):
    """An entry's count field; anything but a whole number counts as 1, so a missing diff is still flagged."""
    value = f.get(key, 0)
    return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else 1


def api_flags(files, owner=False, changed_files=None):
    """Why a PR needs vetting, from the API's file list alone (vet-label.yml reads nothing else).

    An outside PR is labelled whatever this says (vet-label.yml); for it, these are what its report will look at: any
    binary, submodule change or sensitive path. The owner's own PR is flagged only for new or changed binaries, new URLs
    in code, scripts or build files, changes under .github/, submodules, agents' and editors' config and git hooks, and
    xmake.lua lines that fetch packages; plain edits under tools/ and the like don't count."""
    reasons = []
    if changed_files is not None and (len(files) >= FILES_API_LIMIT or len(files) < changed_files):
        reasons.append(f"the API listed {len(files)} of the PR's {changed_files} changed files: the rest can't be read here")
    for f in files:
        if not isinstance(f, dict):
            reasons.append("an entry in the API's file list that isn't an object")
            continue
        path, status = str(f.get("filename") or ""), str(f.get("status") or "")
        previous = str(f.get("previous_filename") or "")
        has_patch = isinstance(f.get("patch"), str)
        patch = f["patch"] if has_patch else ""
        names = sorted({path, previous} - {""})
        if status != "removed":
            for name in names:
                if extension(name) in BINARY_EXTENSIONS:
                    reasons.append(f"binary ({status}): {name}")
            if not has_patch and count(f, "changes") == 0 and status in ("added", "modified", "copied", "changed", "renamed"):
                reasons.append(f"no text diff, likely binary ({status}): {path}")
        if re.search(r"^[-+]Subproject commit", patch, re.MULTILINE):
            reasons.append(f"submodule pointer ({status}): {path}")
        for name in names:
            for reason in sensitive_reasons(name, owner=owner):
                reasons.append(f"{reason} ({status}): {name}")
        if not owner:
            continue
        if re.search(r"(^|/)xmake\.lua$", path, re.IGNORECASE):
            if not has_patch and count(f, "changes") > 0:
                reasons.append(f"xmake.lua with no diff to read ({status}): {path}")
            elif XMAKE_FETCH.search(patch):
                reasons.append(f"xmake.lua package requirements ({status}): {path}")
        if not PROSE.search(path):
            if not has_patch and count(f, "additions") > 0:
                reasons.append(f"no diff to read for URLs ({status}): {path}")
            for url in patch_urls(patch):
                reasons.append(f"new URL: {path}: {url}")
    return reasons


def cmd_flags(args):
    reasons = api_flags(read_json_stream(args.files), owner=args.owner, changed_files=args.changed_files)
    needs = bool(reasons) or not args.owner  # every outside PR is vetted, whatever its files
    json.dump({"needs_vetting": needs, "reasons": reasons}, sys.stdout)
    sys.stdout.write("\n")
    if args.comment:
        if args.owner:
            intro = ("This pull request adds or changes binary files, download URLs, workflows, submodules, agents' or "
                     "editors' config, git hooks or xmake.lua's packages, so it is labelled `needs-vetting`")
        else:
            intro = "Every pull request from outside the project is labelled `needs-vetting`"
        with open(args.comment, "w", encoding="utf-8") as f:
            f.write(
                intro + ": before anything from it is built or run on the maintainer's machines, the owner runs the "
                "vetting workflow on its head (it builds the code on GitHub's throwaway machines) and adds `vetted` "
                "after reading the report. Nothing committed in binary form is shipped; it is built from source "
                "(CONTRIBUTING.md, \"Binaries and new dependencies\").\n\n"
                + ("What the vetting will look at first:\n\n" + block(reasons, 60) if reasons else "")
            )


# -------------------------------------------------------------------------------------------------------- inspect


def git(repo, *args):
    # --literal-pathspecs: a PR's file name such as ':(exclude)*' is a name, not pathspec magic.
    return run(["git", "--literal-pathspecs", "-C", repo, "-c", "core.quotePath=false", *args])


def added_lines(repo, old_blob, new_blob):
    """(line number, text) of the lines `new_blob` adds to `old_blob`. Blob ids, not paths: no file name goes back to
    git, so a name git can't be handed exactly (bytes that aren't UTF-8) can't hide its lines. Git's default context, as
    --numstat counts with: its hunks depend on the context asked for, and -U0's held a few lines fewer than the count in one
    file of PR 379 (2442 against 2445), which failed the vet over lines no one could have hidden."""
    out = git(repo, "diff", "--no-color", "--no-ext-diff", "--no-textconv", old_blob, new_blob)
    lines, line_no, in_hunk = [], 0, False
    for raw in out.split(b"\n"):
        if raw.startswith(b"@@"):
            match = re.match(rb"@@ -\S+ \+(\d+)", raw)
            line_no, in_hunk = (int(match.group(1)) if match else 0), True
        elif in_hunk and raw.startswith(b"+"):
            lines.append((line_no, raw[1:].decode("utf-8", "replace")))
            line_no += 1
        elif in_hunk and raw.startswith(b" "):
            line_no += 1
    return lines


def cmd_inspect(args):
    repo, head = args.repo, args.head
    merge_base = git(repo, "merge-base", args.base, head).decode().strip()
    api = read_json_stream(args.api_files)

    # Names stay bytes as git gives them (-z: no quoting). Shown, each is decoded with surrogateescape, so a byte that
    # isn't UTF-8 shows as <U+DCxx>; nothing hands a name back to git.
    raw = git(repo, "diff", "--raw", "-z", "--no-abbrev", "--no-renames", "--no-ext-diff", "--no-textconv", merge_base, head).split(b"\0")
    entries = []  # (old mode, new mode, old blob, new blob, status, raw path)
    for meta, raw_path in zip(raw[0::2], raw[1::2]):
        if not meta:
            continue
        old_mode, new_mode, old_blob, new_blob, status = meta.decode().lstrip(":").split()
        entries.append((old_mode, new_mode, old_blob, new_blob, status[0], raw_path))
    numstat = {}
    for item in git(repo, "diff", "--numstat", "-z", "--no-renames", "--no-ext-diff", "--no-textconv", merge_base, head).split(b"\0"):
        if item:
            added, deleted, raw_path = item.split(b"\t", 2)
            numstat[raw_path] = (added.decode(), deleted.decode())
    names_only = [n for n in git(repo, "diff", "--name-only", "-z", "--no-renames", merge_base, head).split(b"\0") if n]
    empty_blob = run(["git", "-C", repo, "hash-object", "-w", "--stdin"], input=b"").decode().strip()

    problems, bad_names, unread = [], [], []
    binaries, modes, sensitive, urls, long_lines, configs = [], [], [], [], [], []
    url_files = set()
    lines_read = 0
    for old_mode, new_mode, old_blob, blob, status, raw_path in entries:
        path = raw_path.decode("utf-8", "surrogateescape")
        try:
            raw_path.decode("utf-8")
        except UnicodeDecodeError as error:
            bad_names.append(f"{status}  not UTF-8 at byte {error.start}  {path}")
        for reason in sensitive_reasons(path):
            sensitive.append(f"{status}  {reason:42}  {path}")
        if new_mode == "160000" or old_mode == "160000":
            modes.append(f"{status}  submodule pointer  {path}")
        if status == "D":
            continue
        if new_mode == "120000":
            target = git(repo, "cat-file", "blob", blob).decode("utf-8", "replace")
            modes.append(f"{status}  symlink -> {target}  {path}")
            continue
        if new_mode == "160000":
            continue
        if new_mode == "100755" and old_mode != "100755":
            modes.append(f"{status}  executable bit  {path}")
        git_added = numstat.get(raw_path, ("?", ""))[0]
        is_binary = git_added == "-"
        data = git(repo, "cat-file", "blob", blob)
        why = "git: binary" if is_binary else f"by name (.{extension(path)})" if extension(path) in BINARY_EXTENSIONS else ""
        if not why:
            # git looks for a NUL byte only in the first 8000 bytes: read the whole file.
            if b"\0" in data:
                why = "NUL past 8000 bytes"
            else:
                try:
                    data.decode("utf-8")
                except UnicodeDecodeError as error:
                    why = f"not UTF-8 at byte {error.start}"
        if why:
            binaries.append(f"{status}  {len(data):>9} B  sha256 {hashlib.sha256(data).hexdigest()}  {why:20}  {path}")
        if is_binary:
            continue
        if re.match(r"^ui/.*-config\.xml$|(^|/)flex-config\.xml$", path, re.IGNORECASE):
            configs.append(f"{status}  {path}")
        # A submodule pointer replaced by a file: the old side is a commit of another repository, no blob here, so every
        # line of the file is new (git's numstat counts it so too).
        added = added_lines(repo, empty_blob if set(old_blob) == {"0"} or old_mode == "160000" else old_blob, blob)
        lines_read += len(added)
        if not git_added.isdigit() or int(git_added) > len(added):
            unread.append(f"{status}  git counts {git_added} added lines, {len(added)} read  {path}")
        for line_no, text in added:
            if len(text) >= LONG_LINE:
                long_lines.append(f"{path}:{line_no}  {len(text)} characters")
            if not PROSE.search(path):
                for url in URL.findall(text):
                    urls.append(f"{path}:{line_no}  {url.rstrip('.,;:')}")
                    url_files.add(path)

    git_added_total = sum(int(a) for a, _ in numstat.values() if a.isdigit())
    if len(names_only) > len(entries) or len(numstat) > len(entries):
        problems.append(f"git lists {max(len(names_only), len(numstat))} changed files, and only {len(entries)} were read")
    if unread:
        problems.append(f"fewer added lines were read than git counts in {len(unread)} file(s)")
    if bad_names:
        problems.append(f"{len(bad_names)} file name(s) aren't UTF-8")
    git_paths = {e[5].decode("utf-8", "surrogateescape") for e in entries}
    api_paths = {f.get("filename") for f in api} | {f.get("previous_filename") for f in api if f.get("previous_filename")}
    unseen = sorted(p for p in api_paths - git_paths if p)

    out = [
        "## Trusted inspection\n\n",
        (f"**PROBLEMS (the vet fails):** " + "; ".join(problems) + ".\n\n") if problems else "",
        f"The PR's commits were fetched as git objects and read as data; nothing from them ran here. Diff: merge base "
        f"`{merge_base}` to head `{head}`, {len(entries)} files ({len(api)} in the API's list); {lines_read} added lines "
        f"read of the {git_added_total} git counts in text files.\n\n",
        f"### File names that aren't UTF-8 ({len(bad_names)}; bytes that aren't show as <U+DCxx>)\n\n", block(bad_names), "\n",
        f"### Binary or non-text files added or changed ({len(binaries)}; text that isn't UTF-8 shows here too)\n\n", block(binaries), "\n",
        f"### Symlinks, submodule pointers, executable bits ({len(modes)})\n\n", block(modes), "\n",
        f"### Sensitive paths ({len(sensitive)})\n\n", block(sensitive), "\n",
        f"### URLs in added lines ({len(urls)} in {len(url_files)} files; prose and translations left out)\n\n",
        block(urls), "\n",
        f"### Added lines of {LONG_LINE}+ characters, possibly encoded data ({len(long_lines)})\n\n", block(long_lines), "\n",
    ]
    if configs:
        out += ["### Compiler configuration files mxmlc reads by itself\n\n", block(configs), "\n"]
    if unread:
        out += ["### Files whose added lines were not all read\n\n", block(unread), "\n"]
    if unseen:
        out += ["### In the API's file list but not in the git diff\n\n", block(unseen), "\n"]
    write_report(args.out, "".join(out), "## Trusted inspection")
    if problems:
        sys.exit("inspect: " + "; ".join(problems))


# ------------------------------------------------------------------------------------------------------ toolchain


def download(url, path):
    """(HTTP status, final URL); status 0 when the server can't be reached."""
    last = None
    for attempt in range(3):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "F4Multiplayer-vetting"})
            with urllib.request.urlopen(request, timeout=600) as response, open(path, "wb") as f:
                shutil.copyfileobj(response, f, 1 << 20)
                return response.status, response.geturl()
        except urllib.error.HTTPError as error:
            if error.code in (403, 404, 410):
                return error.code, url
            last = error
        except (urllib.error.URLError, TimeoutError, ConnectionError) as error:
            last = error
        time.sleep(10 * (attempt + 1))
    log(f"download of {url} failed: {last}")
    return 0, url


def gpg_verify(keys, signature, data):
    """Verify `signature` of `data` against only the keys in the file `keys`. Only the key's fingerprint is reported,
    never its user id, which names a person (AGENTS.md)."""
    home = tempfile.mkdtemp(prefix="vet-gpg-")
    os.chmod(home, 0o700)
    try:
        base = ["gpg", "--homedir", home, "--batch", "--no-tty"]
        subprocess.run(base + ["--import", keys], capture_output=True)
        verify = subprocess.run(base + ["--status-fd", "1", "--verify", signature, data], capture_output=True, text=True)
        result = {"good": False, "fingerprint": "", "listed": False, "date": "", "status": []}
        for line in verify.stdout.splitlines():
            parts = line.split()
            if len(parts) < 2 or parts[0] != "[GNUPG:]":
                continue
            if parts[1] in ("GOODSIG", "BADSIG", "EXPSIG", "EXPKEYSIG", "REVKEYSIG", "ERRSIG", "NO_PUBKEY", "KEYEXPIRED", "KEYREVOKED"):
                result["status"].append(parts[1])
            if parts[1] == "VALIDSIG" and len(parts) >= 12:
                result["fingerprint"], result["date"] = parts[11], parts[3]
        result["good"] = "GOODSIG" in result["status"] and bool(result["fingerprint"]) and verify.returncode == 0
        if result["fingerprint"]:
            listing = subprocess.run(base + ["--with-colons", "--list-keys", result["fingerprint"]], capture_output=True, text=True)
            result["listed"] = listing.returncode == 0 and any(
                line.startswith("fpr:") and line.split(":")[9] == result["fingerprint"] for line in listing.stdout.splitlines())
        return result
    finally:
        shutil.rmtree(home, ignore_errors=True)


def pr_claims(repo, rev):
    """The pins the PR's own fetch scripts name, read as text."""
    patterns = {
        "tools/flash/fetch.sh": {
            "version": r"^FLEX_VERSION=(\S+)",
            "flex_url": r"^FLEX_URL=(\S+)",
            "flex_sha256": r"^FLEX_SHA256=([0-9A-Fa-f]{64})\b",
            "playerglobal_url": r"^PLAYERGLOBAL_URL=(\S+)",
            "playerglobal_sha256": r"^PLAYERGLOBAL_SHA256=([0-9A-Fa-f]{64})\b",
        },
        "tools/flash/fetch.ps1": {
            "version": r"\$flexVersion\s*=\s*'([^']+)'",
            "flex_url": r"\$flexUrl\s*=\s*\"([^\"]+)\"",
            "flex_sha256": r"\$flexSha256\s*=\s*'([0-9A-Fa-f]{64})'",
            "playerglobal_url": r"\$playerglobalUrl\s*=\s*'([^']+)'",
            "playerglobal_sha256": r"\$playerglobalSha256\s*=\s*'([0-9A-Fa-f]{64})'",
        },
    }
    claims = {}
    for path, keys in patterns.items():
        try:
            text = git(repo, "show", f"{rev}:{path}").decode("utf-8", "replace")
        except subprocess.CalledProcessError:
            continue
        found = {}
        for key, pattern in keys.items():
            match = re.search(pattern, text, re.MULTILINE)
            if match:
                found[key] = match.group(1)
        version = found.get("version", "")
        for key in ("flex_url", "playerglobal_url"):
            if key in found:
                found[key] = found[key].replace("$FLEX_VERSION", version).replace("$flexVersion", version)
        found["other_sha256"] = sorted(set(re.findall(r"\b[0-9a-f]{64}\b", text)) - {found.get("flex_sha256"), found.get("playerglobal_sha256")})
        claims[path] = found
    return claims


def cmd_toolchain(args):
    pins = {name: PINS.get(name, "").strip() for name in
            ("FLEX_URL", "FLEX_SHA256", "FLEX_KEY_FINGERPRINT", "FLEX_KEYS_URLS", "PLAYERGLOBAL_URL", "PLAYERGLOBAL_SHA256")}
    for name, value in pins.items():
        if not value:
            sys.exit(f"{name} is not pinned (PINS in this file)")
    os.makedirs(args.work, exist_ok=True)
    flex = os.path.join(args.work, "flex.tar.gz")
    asc = os.path.join(args.work, "flex.tar.gz.asc")
    swc = os.path.join(args.work, "playerglobal.swc")
    problems, lines = [], []

    status, _ = download(pins["FLEX_URL"], flex)
    flex_sha = sha256_file(flex) if status == 200 else ""
    lines.append(f"Flex SDK      {pins['FLEX_URL']}")
    lines.append(f"  HTTP {status}, {os.path.getsize(flex) if status == 200 else 0} bytes, sha256 {flex_sha or '-'}")
    lines.append(f"  pinned       {pins['FLEX_SHA256']}  {'MATCH' if flex_sha == pins['FLEX_SHA256'] else 'MISMATCH'}")
    if flex_sha != pins["FLEX_SHA256"]:
        problems.append("the Flex SDK's sha256 is not the pinned one")

    asc_status, _ = download(pins["FLEX_URL"] + ".asc", asc)
    lines.append(f"Signature     {pins['FLEX_URL']}.asc  HTTP {asc_status}")
    good_somewhere = False
    for keys_url in pins["FLEX_KEYS_URLS"].split():
        keys = os.path.join(args.work, "KEYS-" + hashlib.sha256(keys_url.encode()).hexdigest()[:8])
        keys_status, _ = download(keys_url, keys)
        if keys_status != 200:
            lines.append(f"KEYS          {keys_url}  HTTP {keys_status}: not published there")
            continue
        lines.append(f"KEYS          {keys_url}  HTTP 200, sha256 {sha256_file(keys)}")
        if asc_status != 200 or status != 200:
            continue
        result = gpg_verify(keys, asc, flex)
        verdict = "GOOD" if result["good"] else "NOT GOOD (" + (" ".join(result["status"]) or "no signature status") + ")"
        lines.append(f"  gpg --verify against it: {verdict}")
        if result["fingerprint"]:
            pinned = pins["FLEX_KEY_FINGERPRINT"].replace(" ", "").upper()
            same = result["fingerprint"].upper() == pinned
            lines.append(f"  signing key  {result['fingerprint']}  signed {result['date']}, "
                         + ("listed in this KEYS file" if result["listed"] else "NOT listed in this KEYS file"))
            lines.append(f"  pinned key   {pinned}  {'MATCH' if same else 'MISMATCH'}")
            if result["good"] and result["listed"] and same:
                good_somewhere = True
    if not good_somewhere:
        problems.append("no KEYS file gave a good signature from the pinned key")

    pg_status, _ = download(pins["PLAYERGLOBAL_URL"], swc)
    pg_sha = sha256_file(swc) if pg_status == 200 else ""
    lines.append(f"playerglobal  {pins['PLAYERGLOBAL_URL']}  (Adobe publishes no signature)")
    lines.append(f"  HTTP {pg_status}, {os.path.getsize(swc) if pg_status == 200 else 0} bytes, sha256 {pg_sha or '-'}")
    lines.append(f"  pinned       {pins['PLAYERGLOBAL_SHA256']}  {'MATCH' if pg_sha == pins['PLAYERGLOBAL_SHA256'] else 'MISMATCH'}")
    if pg_sha != pins["PLAYERGLOBAL_SHA256"]:
        problems.append("playerglobal.swc's sha256 is not the pinned one")

    claim_lines = []
    if args.claims_repo:
        expected = {"flex_url": pins["FLEX_URL"], "flex_sha256": pins["FLEX_SHA256"],
                    "playerglobal_url": pins["PLAYERGLOBAL_URL"], "playerglobal_sha256": pins["PLAYERGLOBAL_SHA256"]}
        claims = pr_claims(args.claims_repo, args.claims_rev)
        if not claims:
            claim_lines.append("The PR has no tools/flash/fetch.sh or fetch.ps1.")
        for path, found in claims.items():
            for key, want in expected.items():
                got = found.get(key)
                mark = "not found" if got is None else "matches main's pin" if got.lower() == want.lower() else "MISMATCH with main's pin"
                claim_lines.append(f"{path}  {key:20} {got or '-'}  {mark}")
                if got is not None and got.lower() != want.lower():
                    problems.append(f"{path} claims a different {key} than main pins")
            for other in found.get("other_sha256", []):
                claim_lines.append(f"{path}  other sha256-like   {other}")

    if args.unpack and not problems:
        unpack(flex, swc, args.unpack)
        lines.append(f"Unpacked to   {args.unpack}")

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("## Flash toolchain (trusted pins)\n\n")
        f.write(("All checks passed.\n\n" if not problems else "**PROBLEMS:** " + "; ".join(problems) + ".\n\n"))
        f.write(block(lines))
        if args.claims_repo:
            f.write("\nWhat the PR's own scripts pin:\n\n" + block(claim_lines))
        f.write("\n")
    if problems and args.strict:
        sys.exit("toolchain: " + "; ".join(problems))


def extract_tar(archive, home):
    """A .tar.gz into `home`, its top folder dropped; the "data" filter refuses links out of `home` and odd files."""
    if os.path.exists(home):
        shutil.rmtree(home)
    os.makedirs(home)
    with tarfile.open(archive, "r:gz") as tar:
        members = []
        for member in tar.getmembers():
            parts = member.name.split("/", 1)
            if len(parts) < 2 or not parts[1]:
                continue
            member.name = parts[1]
            if member.islnk():
                member.linkname = member.linkname.split("/", 1)[-1]
            members.append(member)
        tar.extractall(home, members=members, filter="data")


def unpack(flex, swc, home):
    """The SDK into `home`, with playerglobal where mxmlc looks for it."""
    extract_tar(flex, home)
    player = os.path.join(home, "frameworks", "libs", "player", "32.0")
    os.makedirs(player, exist_ok=True)
    shutil.copyfile(swc, os.path.join(player, "playerglobal.swc"))
    if not os.path.isfile(os.path.join(home, "lib", "mxmlc.jar")):
        sys.exit("the Flex SDK has no lib/mxmlc.jar")


def cmd_java(args):
    """The pinned Amazon Corretto: downloaded, checked against its pinned sha256, unpacked into --home."""
    os.makedirs(args.work, exist_ok=True)
    archive = os.path.join(args.work, "corretto.tar.gz")
    status, _ = download(PINS["CORRETTO_URL"], archive)
    got = sha256_file(archive) if status == 200 else ""
    lines = [f"Amazon Corretto  {PINS['CORRETTO_URL']}",
             f"  HTTP {status}, sha256 {got or '-'}",
             f"  pinned       {PINS['CORRETTO_SHA256']}  {'MATCH' if got == PINS['CORRETTO_SHA256'] else 'MISMATCH'}"]
    ok = got == PINS["CORRETTO_SHA256"]
    if ok:
        extract_tar(archive, args.home)
        java = os.path.join(args.home, "bin", "java")
        version = subprocess.run([java, "-version"], capture_output=True, text=True)
        lines += [f"  {line}" for line in (version.stderr or version.stdout).splitlines()]
        ok = version.returncode == 0
    with open(args.report, "w", encoding="utf-8") as f:
        f.write("## Java (trusted pin)\n\n" + ("" if ok else "**PROBLEM:** Corretto is not the pinned one or does not run.\n\n")
                + block(lines) + "\n")
    if not ok:
        sys.exit("java: Corretto is not the pinned one or does not run")


# ------------------------------------------------------------------------------------------------- compile/compare


def clear_compile_time(data):
    """Zero the compile date mxmlc writes into the ProductInfo tag (its last 8 bytes), as tools/flash/build.ps1 does.
    Returns False when the movie is compressed or has no ProductInfo tag."""
    if data[:3] != b"FWS" or len(data) < 9:
        return False
    bits = data[8] >> 3
    at = 8 + (5 + bits * 4 + 7) // 8 + 4
    stamped = False
    while at + 2 <= len(data):
        header = int.from_bytes(data[at:at + 2], "little")
        at += 2
        code, length = header >> 6, header & 0x3F
        if length == 0x3F:
            length = int.from_bytes(data[at:at + 4], "little")
            at += 4
        if code == 41 and length >= 26 and at + length <= len(data):
            data[at + length - 8:at + length] = bytes(8)
            stamped = True
        at += length
        if code == 0:
            break
    return stamped


METADATA_DATE = re.compile(rb"<dc:date>([^<]*)</dc:date>")


def normalize_movie(movie):
    """The movie without what mxmlc stamps into every build: the compile time in its ProductInfo tag (as
    tools/flash/build.ps1 clears it) and the date in its Metadata tag's RDF (<dc:date>, which build.ps1 leaves; it is
    written in the compiling machine's locale). Two builds of the same sources then give the same bytes, so a movie
    can be checked by its hash. Returns (movie, what was cleared); a compressed movie comes back unchanged."""
    data = bytearray(movie)
    cleared = ["compile time"] if clear_compile_time(data) else []
    if data[:3] != b"FWS" or len(data) < 9:
        return bytes(data), cleared
    at = 8 + (5 + (data[8] >> 3) * 4 + 7) // 8 + 4
    out = bytearray(data[:at])
    while at + 2 <= len(data):
        start = at
        value = int.from_bytes(data[at:at + 2], "little")
        at += 2
        code, length = value >> 6, value & 0x3F
        long_form = length == 0x3F
        if long_form:
            length = int.from_bytes(data[at:at + 4], "little")
            at += 4
        body = bytes(data[at:at + length])
        at += length
        dated = METADATA_DATE.sub(b"<dc:date></dc:date>", body) if code == 77 else body
        if dated != body:
            cleared.append("Metadata date")
            if long_form or len(dated) >= 0x3F:
                out += ((code << 6) | 0x3F).to_bytes(2, "little") + len(dated).to_bytes(4, "little")
            else:
                out += ((code << 6) | len(dated)).to_bytes(2, "little")
            out += dated
        else:
            out += data[start:at]
        if code == 0:
            break
    out += data[at:]
    out[4:8] = len(out).to_bytes(4, "little")
    return bytes(out), cleared


def metadata_date(movie):
    match = METADATA_DATE.search(movie)
    return match.group(1).decode("latin-1") if match else None


TAG_NAMES = {0: "End", 1: "ShowFrame", 9: "SetBackgroundColor", 24: "Protect", 41: "ProductInfo", 43: "FrameLabel",
             56: "ExportAssets", 58: "EnableDebugger", 63: "DebugID", 64: "EnableDebugger2", 65: "ScriptLimits",
             69: "FileAttributes", 76: "SymbolClass", 77: "Metadata", 82: "DoABC", 86: "DefineSceneAndFrameLabelData",
             87: "DefineBinaryData"}


def swf_tags(data):
    """The header (everything before the first tag) and the tags of an uncompressed movie, as (code, body)."""
    if data[:3] != b"FWS" or len(data) < 9:
        return None, []
    at = 8 + (5 + (data[8] >> 3) * 4 + 7) // 8 + 4
    header, tags = bytes(data[:at]), []
    while at + 2 <= len(data):
        value = int.from_bytes(data[at:at + 2], "little")
        at += 2
        code, length = value >> 6, value & 0x3F
        if length == 0x3F:
            length = int.from_bytes(data[at:at + 4], "little")
            at += 4
        tags.append((code, bytes(data[at:at + length])))
        at += length
        if code == 0:
            break
    return header, tags


def describe_difference(ours, theirs):
    """Where two movies differ: the header, then tag by tag, with the text found in only one of them."""
    lines = []
    ours_header, ours_tags = swf_tags(ours)
    theirs_header, theirs_tags = swf_tags(theirs)
    if ours_header is None or theirs_header is None:
        return lines
    if ours_header[:4] + ours_header[8:] != theirs_header[:4] + theirs_header[8:]:
        lines.append(f"header differs: {ours_header.hex()} vs {theirs_header.hex()}")
    if len(ours_tags) != len(theirs_tags):
        lines.append(f"{len(ours_tags)} tags vs {len(theirs_tags)}")
    differing = 0
    for index, ((code_a, body_a), (code_b, body_b)) in enumerate(zip(ours_tags, theirs_tags)):
        if code_a == code_b and body_a == body_b:
            continue
        name = TAG_NAMES.get(code_a, str(code_a)) if code_a == code_b else f"{TAG_NAMES.get(code_a, code_a)} vs {TAG_NAMES.get(code_b, code_b)}"
        first = next((i for i, (a, b) in enumerate(zip(body_a, body_b)) if a != b), min(len(body_a), len(body_b)))
        differing += 1
        lines.append(f"tag {index} ({name}): {len(body_a)} vs {len(body_b)} bytes, first difference at byte {first}")
        start = max(0, first - 40)
        lines.append("  built:     ..." + body_a[start:first + 40].decode("latin-1") + "...")
        lines.append("  committed: ..." + body_b[start:first + 40].decode("latin-1") + "...")
    if len(ours_tags) == len(theirs_tags):
        lines.append(f"the other {len(ours_tags) - differing} of {len(ours_tags)} tags are identical")
    return lines


def movie_names(ui):
    """ui/<Name>/<Name>.as, for every folder but ui/shared."""
    if not os.path.isdir(ui):
        return []
    return sorted(name for name in os.listdir(ui)
                  if name != "shared" and os.path.isfile(os.path.join(ui, name, name + ".as")))


def cmd_compile(args):
    flex, ui, out = os.path.abspath(args.flex), os.path.abspath(args.ui), os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    java_bin = os.path.join(os.path.abspath(args.java_home), "bin", "java")
    java = subprocess.run([java_bin, "-version"], capture_output=True, text=True)
    lines = [f"java -version: {line}" for line in (java.stderr or java.stdout).splitlines()]
    failed = 0
    for name in movie_names(ui):
        movie = os.path.join(out, name + ".swf")
        command = [
            java_bin, "-Dsun.io.useCanonCaches=false", "-Djava.util.Arrays.useLegacyMergeSort=true",
            "-jar", os.path.join(flex, "lib", "mxmlc.jar"), "+flexlib=" + os.path.join(flex, "frameworks"),
            "-target-player=32.0", "-swf-version=15", "-compress=false", "-static-link-runtime-shared-libraries=true",
            "-compiler.library-path=",
            "-compiler.external-library-path=" + os.path.join(flex, "frameworks", "libs", "player", "32.0", "playerglobal.swc"),
            "-compiler.source-path=" + os.path.join(ui, name), "-compiler.source-path+=" + os.path.join(ui, "shared"),
            "-default-size", "1280", "720", "-default-frame-rate", "60", "-default-background-color", "0x000000",
            "-warnings=true", "-output", movie, os.path.join(ui, name, name + ".as"),
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=600)
        output = (result.stdout + result.stderr).replace(ui + os.sep, "ui/").replace(flex + os.sep, "FLEX/")
        with open(os.path.join(out, name + ".log"), "w", encoding="utf-8") as f:
            f.write(output)
        warnings = sum(1 for line in output.splitlines() if "Warning:" in line)
        if result.returncode != 0 or not os.path.isfile(movie):
            failed += 1
            lines.append(f"{name}.swf  mxmlc FAILED (exit {result.returncode}), {warnings} warning(s); last lines:")
            lines += ["    " + line for line in output.splitlines()[-15:]]
            continue
        data, cleared = normalize_movie(open(movie, "rb").read())
        open(movie, "wb").write(data)
        missing = [what for what in ("compile time", "Metadata date") if what not in cleared]
        lines.append(f"{name}.swf  compiled, {warnings} warning(s), {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}"
                     + (f"  (no {' or '.join(missing)} found to clear)" if missing else ""))
        lines += ["    " + line for line in output.splitlines() if "Warning:" in line or "Error:" in line][:20]
    if not movie_names(ui):
        lines.append("No ui/<Name>/<Name>.as to compile.")
    with open(args.report, "w", encoding="utf-8") as f:
        f.write("## Menu movies compiled from source\n\n" + block(lines) + "\n")
    if failed:
        sys.exit(f"{failed} movie(s) failed to compile")


def cmd_compare(args):
    ui, built = args.ui, args.built
    lines = []
    names = movie_names(ui)
    committed_only = sorted(name for name in (os.listdir(ui) if os.path.isdir(ui) else [])
                            if os.path.isfile(os.path.join(ui, name, name + ".swf")) and name not in names)
    for name in names:
        ours_path, theirs_path = os.path.join(built, name + ".swf"), os.path.join(ui, name, name + ".swf")
        if not os.path.isfile(ours_path):
            lines.append(f"{name}.swf  not compiled")
            continue
        if not os.path.isfile(theirs_path):
            lines.append(f"{name}.swf  the PR commits no copy (nothing to compare; ours is the one that counts)")
            continue
        ours, _ = normalize_movie(open(ours_path, "rb").read())  # compile already did; this keeps compare honest
        theirs_raw = open(theirs_path, "rb").read()
        theirs_timed = bytearray(theirs_raw)
        uncompressed = clear_compile_time(theirs_timed)
        theirs, _ = normalize_movie(theirs_raw)
        if ours == theirs_raw:
            lines.append(f"{name}.swf  MATCHES the committed copy byte for byte")
        elif ours == bytes(theirs_timed):
            lines.append(f"{name}.swf  MATCHES the committed copy once its compile time is cleared")
        elif ours == theirs:
            lines.append(f"{name}.swf  DIFFERS from the committed copy, and only in the date mxmlc writes into its Metadata "
                         f"tag (committed: '{metadata_date(theirs_raw)}'), which tools/flash/build.ps1 does not clear; with "
                         f"that date and the compile time cleared, it is the same movie")
        else:
            differing = sum(1 for a, b in zip(ours, theirs) if a != b) + abs(len(ours) - len(theirs))
            note = "" if uncompressed else f" (committed copy starts {bytes(theirs_raw[:3])!r}: not an uncompressed movie mxmlc made)"
            lines.append(f"{name}.swf  DIFFERS from the committed copy beyond the build dates: {len(ours)} vs {len(theirs)} "
                         f"bytes once both are cleared, {differing} byte(s) differ{note}")
            lines += ["    " + line for line in describe_difference(ours, theirs)[:40]]
            if metadata_date(theirs_raw):
                lines.append(f"    and its Metadata date: '{metadata_date(theirs_raw)}'")
        lines.append(f"    built      sha256 {hashlib.sha256(ours).hexdigest()} (build dates cleared)")
        lines.append(f"    committed  sha256 {hashlib.sha256(theirs_raw).hexdigest()} as committed, "
                     f"{hashlib.sha256(theirs).hexdigest()} with its build dates cleared")
    for name in committed_only:
        lines.append(f"{name}.swf  committed with no ui/{name}/{name}.as: not built from source here")
    with open(args.report, "w", encoding="utf-8") as f:
        f.write("## Built movies against the PR's committed copies\n\n" + block(lines) + "\n")


# -------------------------------------------------------------------------------------------------------- assemble


UNTRUSTED_PART_LIMIT = 64 << 10  # bytes of an untrusted part's text kept; the rest is cut, and the report says so


def read_part(parts, name, limit=UNTRUSTED_PART_LIMIT):
    """(text, problem) of the part the report job wrote to `parts`/`name` from a job output (gzip, then base64). Text
    past `limit` bytes is cut, with a last line saying so."""
    path = os.path.join(parts, name)
    if not os.path.isfile(path):
        return "", "could not be taken from the job's output (too large to pass on, or missing)"
    raw = open(path, "rb").read().strip()
    if not raw:
        return "", "empty"
    try:
        with gzip.GzipFile(fileobj=io.BytesIO(base64.b64decode(raw, validate=True))) as packed:
            data = packed.read(limit + 1)
    except Exception as error:  # an untrusted job may hand over anything
        return "", f"unreadable ({type(error).__name__})"
    text = data[:limit].decode("utf-8", "replace")
    if len(data) > limit:
        text += f"\n... (cut at {limit} bytes)\n"
    return text, ""


def cmd_assemble(args):
    env = os.environ.get
    problems = []
    jobs = {"inspect": env("INSPECT_RESULT", "?"), "movies": env("MOVIES_RESULT", "?"), "build": env("BUILD_RESULT", "?")}
    has_ui = env("HAS_UI", "") == "true"
    if jobs["inspect"] != "success":
        problems.append(f"the inspect job's result is {jobs['inspect']}")
    if has_ui and jobs["movies"] != "success":
        problems.append(f"the movies job's result is {jobs['movies']} (a toolchain check or a compile failed)")
    if jobs["build"] != "success":
        problems.append(f"the build and tests job's result is {jobs['build']}")

    trusted, trouble = read_part(args.parts, "inspect", limit=REPORT_BYTES_LIMIT)
    if trusted.endswith(f"(cut at {REPORT_BYTES_LIMIT} bytes)\n"):
        trouble = "is over the size limit"
    if trusted.startswith(TRUSTED_STAND_IN):
        trouble = "was too long to pass on"
    if trouble:
        problems.append(f"the trusted part {trouble}: read the inspect job's summary")
    caches = "## Caches\n\n"
    if not os.path.isfile(os.path.join(args.parts, "caches-checked")):
        problems.append("the cache check did not run")
        caches += "Not checked: the step that lists them failed.\n\n"
    else:
        # id, key, ref, created, bytes per line, from the API; a key may have been written by a PR's code.
        listed = {}
        for scope in ("main", "runner"):
            path = os.path.join(args.parts, f"caches-{scope}.tsv")
            text = open(path, encoding="utf-8", errors="replace").read() if os.path.isfile(path) else ""
            listed[scope] = [line.replace("\t", "  ") for line in text.splitlines() if line.strip()]
        if listed["main"]:
            problems.append(f"main's scope holds {len(listed['main'])} cache entr{'y' if len(listed['main']) == 1 else 'ies'}")
            caches += (f"**Main's scope holds {len(listed['main'])} cache entries.** Every branch's builds can restore "
                       "them, and no workflow saves one there any more: one may have been written by a PR's code. "
                       "Delete them (`gh cache delete --all --ref refs/heads/main`), then vet again.\n\n"
                       + block(listed["main"]) + "\n")
        else:
            caches += "Main's scope holds no cache entries.\n\n"
        caches += (f"The vet-runner scope, which only vet-runner runs can read and none restores from, holds "
                   f"{len(listed['runner'])}" + (" (`gh cache delete --all --ref refs/heads/vet-runner` clears them):\n\n"
                                                 + block(listed["runner"]) if listed["runner"] else ".\n") + "\n")

    untrusted = []
    for title, name, needed in (("movies", "movies", has_ui), ("build and tests", "build", True)):
        text, trouble = read_part(args.parts, name)
        if trouble and needed:
            problems.append(f"the {title} part {trouble}")
        untrusted.append((title, text, trouble if needed else "not needed: the PR has no ui/"))

    head = env("SHA", "?")
    if problems:
        verdict = f"**Vet FAILED for `{head}`:** " + "; ".join(problems) + ". `vetted` would be taken off again.\n\n"
    else:
        verdict = (f"**Vet PASSED for `{head}`.** Every job succeeded and every part of the report was read; the owner "
                   "may add `vetted` once the report is read.\n\n")
    out = [
        f"# Vetting report: PR {env('PR', '?')} at {head}\n\n",
        verdict,
        f"Run: {env('RUN_URL', '')}\n\n",
        f"Jobs: inspect {jobs['inspect']}, movies {jobs['movies']}, build and tests {jobs['build']}.\n\n",
        "This job summary is the report. The trusted part was made by main's own code, which read the PR only as data, "
        "and reached this job as a job output, which no other job can write. The untrusted parts come from jobs that "
        "compiled the PR's sources or ran its code: a hostile PR could forge them, so they are shown as plain text. "
        "The run's report artifact is only a convenience copy: code in an untrusted job could replace it.\n\n",
        trusted or "## Trusted inspection\n\n(missing: read the inspect job's summary)\n\n",
        caches,
    ]
    for title, text, trouble in untrusted:
        out.append(f"## Untrusted: {title}\n\n")
        out.append(block(text.splitlines()) if text else f"    (none: {trouble or 'this job reported nothing'})\n")
        out.append("\n")
    write_report(args.out, "".join(out), f"# Vetting report: PR {env('PR', '?')} at {head}")
    if problems:
        sys.exit("vet failed: " + "; ".join(problems))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("flags")
    p.add_argument("files")
    p.add_argument("--comment")
    p.add_argument("--owner", action="store_true", help="the repository owner's own PR: the narrower rule")
    p.add_argument("--changed-files", type=int, help="the PR's changed_files, to catch a file list the API cut short")
    p.set_defaults(func=cmd_flags)
    p = sub.add_parser("inspect")
    for name in ("--repo", "--base", "--head", "--api-files", "--out"):
        p.add_argument(name, required=True)
    p.set_defaults(func=cmd_inspect)
    p = sub.add_parser("toolchain")
    p.add_argument("--work", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--unpack")
    p.add_argument("--strict", action="store_true")
    p.add_argument("--claims-repo")
    p.add_argument("--claims-rev", default="HEAD")
    p.set_defaults(func=cmd_toolchain)
    p = sub.add_parser("java")
    for name in ("--work", "--home", "--report"):
        p.add_argument(name, required=True)
    p.set_defaults(func=cmd_java)
    p = sub.add_parser("compile")
    for name in ("--java-home", "--flex", "--ui", "--out", "--report"):
        p.add_argument(name, required=True)
    p.set_defaults(func=cmd_compile)
    p = sub.add_parser("compare")
    for name in ("--built", "--ui", "--report"):
        p.add_argument(name, required=True)
    p.set_defaults(func=cmd_compare)
    p = sub.add_parser("assemble")
    p.add_argument("--parts", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_assemble)
    args = parser.parse_args()
    # Nothing raw reaches the log: an error message or a traceback (a failed git command's arguments, say) may hold a
    # PR's file names, and a trusted job's log must not carry a workflow command a PR wrote.
    try:
        args.func(args)
    except SystemExit as error:
        if error.code is None or isinstance(error.code, int):
            raise
        log(error.code)
        sys.exit(1)
    except BaseException:
        log(traceback.format_exc())
        sys.exit(1)


if __name__ == "__main__":
    main()
