#!/usr/bin/env python3
"""Trusted helpers for the vetting and packaging workflows (vet.yml, vet-label.yml, package.yml).

Everything here comes from main. A pull request's files are only read as data: git objects, the API's file list. Nothing
from the pull request is imported or run, except its ActionScript sources, which `compile` hands to the pinned Flex
compiler. Standard library only; run it with `python3 -I`.

  flags FILES.jsonl [--comment FILE]   the API's file list -> {"needs_vetting": ..., "reasons": [...]}
  inspect --repo DIR --base SHA --head SHA --api-files FILES.jsonl --out FILE
  toolchain --work DIR [--unpack DIR] [--strict] [--claims-repo DIR --claims-rev REV] --out FILE
  compile --flex DIR --ui DIR --out DIR --report FILE
  compare --built DIR --ui DIR --report FILE
  assemble --out FILE                  the final report, from the jobs' outputs in the environment

The toolchain's pins come from the environment, set in the workflow files on main: FLEX_URL, FLEX_SHA256,
FLEX_KEY_FINGERPRINT, FLEX_KEYS_URLS (space-separated), PLAYERGLOBAL_URL, PLAYERGLOBAL_SHA256.
"""

import argparse
import base64
import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request

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
# repository trusts. The first group is the maintainer's list; the rest are kin of it.
SENSITIVE = [
    (re.compile(r"(^|/)xmake\.lua$"), "xmake.lua (build script)"),
    (re.compile(r"(^|/)(?!xmake\.lua$)[^/]+\.lua$"), "Lua (xmake script)"),
    (re.compile(r"^\.github/"), ".github (workflows, CI)"),
    (re.compile(r"^tools/"), "tools/ (scripts)"),
    (re.compile(r"(^|/)\.gitattributes$"), ".gitattributes"),
    (re.compile(r"(^|/)\.gitmodules$"), ".gitmodules (submodules)"),
    (re.compile(r"(^|/)(\.githooks|\.husky|git-hooks|hooks)/"), "git hooks"),
    (re.compile(r"^lib/"), "lib/ (submodules)"),
    (re.compile(r"(^|/)(AGENTS|CLAUDE)(\.[\w-]+)?\.md$|^\.claude/"), "AI agents' instructions"),
    (re.compile(r"^\.(vscode|devcontainer|idea|vs)/"), "editor config (can run tasks on open)"),
    (re.compile(r"^\.oss-scanner/"), ".oss-scanner (scanner build)"),
]

# Text whose URLs are left out: prose, and the players' translations.
PROSE = re.compile(r"\.(md|txt)$|^dist/lang/", re.IGNORECASE)
URL = re.compile(r"""(?i)\b(?:https?|ftps?|wss?|git|ssh|file|s3|gs|sftp)://[^\s"'<>`\\)\]}|]+|\bgit@[\w.-]+:[\w./~-]+""")
LONG_LINE = 1000  # an added line this long may be data in disguise (base64, a minified blob)
REPORT_LIMIT = 400  # rows per list, so the job summary stays far below its 1 MiB


def clean(text):
    """PR-controlled text made safe for a Markdown code block: no control characters, no line breaks."""
    return "".join(ch if 32 <= ord(ch) != 127 else "?" for ch in str(text))


def block(lines, limit=REPORT_LIMIT):
    """Lines as an indented Markdown code block (nothing in it is rendered), or '(none)'."""
    lines = list(lines)
    if not lines:
        return "    (none)\n"
    shown = lines[:limit]
    out = "".join("    " + clean(line) + "\n" for line in shown)
    if len(lines) > limit:
        out += f"    ... and {len(lines) - limit} more\n"
    return out


def extension(path):
    name = path.rsplit("/", 1)[-1]
    return name.rsplit(".", 1)[-1].lower() if "." in name else ""


def sensitive_reasons(path):
    return [reason for pattern, reason in SENSITIVE if pattern.search(path)]


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


def api_flags(files):
    """Why a PR needs vetting, from the API's file list alone (vet-label.yml reads nothing else)."""
    reasons = []
    for f in files:
        path, status = f.get("filename", ""), f.get("status", "")
        if status != "removed":
            if extension(path) in BINARY_EXTENSIONS:
                reasons.append(f"binary ({status}): {path}")
            elif "patch" not in f and f.get("changes", 0) == 0 and status in ("added", "modified", "copied", "changed"):
                reasons.append(f"no text diff, likely binary ({status}): {path}")
        if re.search(r"^[-+]Subproject commit", f.get("patch", ""), re.MULTILINE):
            reasons.append(f"submodule pointer ({status}): {path}")
        for old_or_new in {path, f.get("previous_filename") or path}:
            for reason in sensitive_reasons(old_or_new):
                reasons.append(f"{reason} ({status}): {old_or_new}")
    return reasons


def cmd_flags(args):
    reasons = api_flags(read_json_stream(args.files))
    json.dump({"needs_vetting": bool(reasons), "reasons": reasons}, sys.stdout)
    sys.stdout.write("\n")
    if args.comment:
        with open(args.comment, "w", encoding="utf-8") as f:
            f.write(
                "This pull request adds binary files or changes build, tooling or repository files, so it is labelled "
                "`needs-vetting`: before anything from it is built or run on the maintainer's machines, the owner runs "
                "the vetting workflow on it (it builds the code on GitHub's throwaway machines) and adds `vetted` "
                "after reading the report. Nothing committed in binary form is shipped; it is built from source "
                "(CONTRIBUTING.md, \"Binaries and new dependencies\").\n\n"
                + block(reasons, 60)
            )


# -------------------------------------------------------------------------------------------------------- inspect


def git(repo, *args):
    return run(["git", "-C", repo, "-c", "core.quotePath=false", *args])


def added_lines(repo, base, head, path):
    """(line number, text) of the lines `head` adds to `path`."""
    out = git(repo, "diff", "-U0", "--no-color", "--no-ext-diff", "--no-textconv", base, head, "--", path)
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

    raw = git(repo, "diff", "--raw", "-z", "--no-renames", "--no-ext-diff", "--no-textconv", merge_base, head).split(b"\0")
    entries = []  # (old mode, new mode, new blob, status, path)
    for meta, path in zip(raw[0::2], raw[1::2]):
        if not meta:
            continue
        old_mode, new_mode, _old_blob, new_blob, status = meta.decode().lstrip(":").split()
        entries.append((old_mode, new_mode, new_blob, status[0], path.decode("utf-8", "replace")))
    numstat = {}
    for item in git(repo, "diff", "--numstat", "-z", "--no-renames", "--no-ext-diff", "--no-textconv", merge_base, head).split(b"\0"):
        if item:
            added, deleted, path = item.decode("utf-8", "replace").split("\t", 2)
            numstat[path] = (added, deleted)

    binaries, modes, sensitive, urls, long_lines, configs = [], [], [], [], [], []
    url_files = set()
    for old_mode, new_mode, blob, status, path in entries:
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
        is_binary = numstat.get(path, ("", ""))[0] == "-"
        if is_binary or extension(path) in BINARY_EXTENSIONS:
            data = git(repo, "cat-file", "blob", blob)
            why = "git: binary" if is_binary else f"by name (.{extension(path)})"
            binaries.append(f"{status}  {len(data):>9} B  sha256 {hashlib.sha256(data).hexdigest()}  {why:20}  {path}")
        if is_binary:
            continue
        if re.match(r"^ui/.*-config\.xml$|(^|/)flex-config\.xml$", path):
            configs.append(f"{status}  {path}")
        for line_no, text in added_lines(repo, merge_base, head, path):
            if len(text) >= LONG_LINE:
                long_lines.append(f"{path}:{line_no}  {len(text)} characters")
            if not PROSE.search(path):
                for url in URL.findall(text):
                    urls.append(f"{path}:{line_no}  {url.rstrip('.,;:')}")
                    url_files.add(path)

    git_paths = {e[4] for e in entries}
    api_paths = {f.get("filename") for f in api} | {f.get("previous_filename") for f in api if f.get("previous_filename")}
    unseen = sorted(p for p in api_paths - git_paths if p)

    out = [
        "## Trusted inspection\n\n",
        f"The PR's commits were fetched as git objects and read as data; nothing from them ran here. Diff: merge base "
        f"`{merge_base}` to head `{head}`, {len(entries)} files ({len(api)} in the API's list).\n\n",
        f"### Binary or non-text files added or changed ({len(binaries)})\n\n", block(binaries), "\n",
        f"### Symlinks, submodule pointers, executable bits ({len(modes)})\n\n", block(modes), "\n",
        f"### Sensitive paths ({len(sensitive)})\n\n", block(sensitive), "\n",
        f"### URLs in added lines ({len(urls)} in {len(url_files)} files; prose and translations left out)\n\n",
        block(urls), "\n",
        f"### Added lines of {LONG_LINE}+ characters, possibly encoded data ({len(long_lines)})\n\n", block(long_lines), "\n",
    ]
    if configs:
        out += ["### Compiler configuration files mxmlc reads by itself\n\n", block(configs), "\n"]
    if unseen:
        out += ["### In the API's file list but not in the git diff\n\n", block(unseen), "\n"]
    with open(args.out, "w", encoding="utf-8") as f:
        f.writelines(out)


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
    print(f"download of {url} failed: {last}", file=sys.stderr)
    return 0, url


def gpg_verify(keys, signature, data):
    """Verify `signature` of `data` against only the keys in the file `keys`."""
    home = tempfile.mkdtemp(prefix="vet-gpg-")
    os.chmod(home, 0o700)
    try:
        base = ["gpg", "--homedir", home, "--batch", "--no-tty"]
        subprocess.run(base + ["--import", keys], capture_output=True)
        verify = subprocess.run(base + ["--status-fd", "1", "--verify", signature, data], capture_output=True, text=True)
        result = {"good": False, "fingerprint": "", "uids": [], "date": "", "status": []}
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
            for line in listing.stdout.splitlines():
                fields = line.split(":")
                if fields[0] == "uid" and len(fields) > 9:
                    result["uids"].append(fields[9].replace("\\x3a", ":"))
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
    pins = {name: os.environ.get(name, "").strip() for name in
            ("FLEX_URL", "FLEX_SHA256", "FLEX_KEY_FINGERPRINT", "FLEX_KEYS_URLS", "PLAYERGLOBAL_URL", "PLAYERGLOBAL_SHA256")}
    for name in ("FLEX_URL", "FLEX_SHA256", "FLEX_KEYS_URLS", "PLAYERGLOBAL_URL", "PLAYERGLOBAL_SHA256"):
        if not pins[name]:
            sys.exit(f"{name} is not set (the workflow pins it)")
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
            lines.append(f"  signing key  {result['fingerprint']}  signed {result['date']}")
            lines.append(f"  pinned key   {pinned or '(none pinned yet)'}  {'MATCH' if same else 'MISMATCH' if pinned else ''}")
            for uid in result["uids"]:
                lines.append(f"  uid          {uid}")
            if result["good"] and (same or not pinned):
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


def unpack(flex, swc, home):
    """The SDK into `home` (the tarball's top folder dropped), with playerglobal where mxmlc looks for it."""
    if os.path.exists(home):
        shutil.rmtree(home)
    os.makedirs(home)
    with tarfile.open(flex, "r:gz") as tar:
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
    player = os.path.join(home, "frameworks", "libs", "player", "32.0")
    os.makedirs(player, exist_ok=True)
    shutil.copyfile(swc, os.path.join(player, "playerglobal.swc"))
    if not os.path.isfile(os.path.join(home, "lib", "mxmlc.jar")):
        sys.exit("the Flex SDK has no lib/mxmlc.jar")


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
    java = subprocess.run(["java", "-version"], capture_output=True, text=True)
    lines = [f"java -version: {line}" for line in (java.stderr or java.stdout).splitlines()]
    failed = 0
    for name in movie_names(ui):
        movie = os.path.join(out, name + ".swf")
        command = [
            "java", "-Dsun.io.useCanonCaches=false", "-Djava.util.Arrays.useLegacyMergeSort=true",
            "-jar", os.path.join(flex, "lib", "mxmlc.jar"), "+flexlib=" + os.path.join(flex, "frameworks"),
            "-target-player=32.0", "-swf-version=15", "-compress=false", "-static-link-runtime-shared-libraries=true",
            "-compiler.library-path=",
            "-compiler.external-library-path=" + os.path.join(flex, "frameworks", "libs", "player", "32.0", "playerglobal.swc"),
            "-compiler.source-path=" + os.path.join(ui, name), "-compiler.source-path+=" + os.path.join(ui, "shared"),
            "-default-size", "1280", "720", "-default-frame-rate", "60", "-default-background-color", "0x000000",
            "-warnings=true", "-output", movie, os.path.join(ui, name, name + ".as"),
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=600)
        log = (result.stdout + result.stderr).replace(ui + os.sep, "ui/").replace(flex + os.sep, "FLEX/")
        with open(os.path.join(out, name + ".log"), "w", encoding="utf-8") as f:
            f.write(log)
        warnings = sum(1 for line in log.splitlines() if "Warning:" in line)
        if result.returncode != 0 or not os.path.isfile(movie):
            failed += 1
            lines.append(f"{name}.swf  mxmlc FAILED (exit {result.returncode}), {warnings} warning(s); last lines:")
            lines += ["    " + line for line in log.splitlines()[-15:]]
            continue
        data = bytearray(open(movie, "rb").read())
        stamped = clear_compile_time(data)
        open(movie, "wb").write(data)
        lines.append(f"{name}.swf  compiled, {warnings} warning(s), {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}"
                     + ("" if stamped else "  (no compile time found to clear)"))
        lines += ["    " + line for line in log.splitlines() if "Warning:" in line or "Error:" in line][:20]
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
        ours = bytearray(open(ours_path, "rb").read())
        clear_compile_time(ours)
        theirs_raw = open(theirs_path, "rb").read()
        theirs = bytearray(theirs_raw)
        cleared = clear_compile_time(theirs)
        if ours == theirs:
            how = "byte for byte" if bytes(theirs) == theirs_raw else "byte for byte once its compile time is cleared"
            lines.append(f"{name}.swf  MATCHES the committed copy {how}")
        else:
            differing = sum(1 for a, b in zip(ours, theirs) if a != b) + abs(len(ours) - len(theirs))
            first = next((i for i, (a, b) in enumerate(zip(ours, theirs)) if a != b), min(len(ours), len(theirs)))
            note = "" if cleared else f" (committed copy starts {bytes(theirs_raw[:3])!r}: not an uncompressed movie mxmlc made)"
            lines.append(f"{name}.swf  DIFFERS from the committed copy: {len(ours)} vs {len(theirs)} bytes, "
                         f"{differing} byte(s) differ, first at offset {first}{note}")
            lines += ["    " + line for line in describe_difference(ours, theirs)[:40]]
        lines.append(f"    built      sha256 {hashlib.sha256(ours).hexdigest()} (compile time cleared)")
        lines.append(f"    committed  sha256 {hashlib.sha256(theirs_raw).hexdigest()} as committed")
    for name in committed_only:
        lines.append(f"{name}.swf  committed with no ui/{name}/{name}.as: not built from source here")
    with open(args.report, "w", encoding="utf-8") as f:
        f.write("## Built movies against the PR's committed copies\n\n" + block(lines) + "\n")


# -------------------------------------------------------------------------------------------------------- assemble


def unpack_part(name):
    text = os.environ.get(name, "")
    if not text:
        return ""
    try:
        return gzip.decompress(base64.b64decode(text)).decode("utf-8", "replace")
    except Exception as error:  # an untrusted job may hand over anything
        return f"(could not read this part: {error})"


def cmd_assemble(args):
    env = os.environ.get
    out = [
        f"# Vetting report: PR {env('PR', '?')} at {env('SHA', '?')}\n\n",
        f"Run: {env('RUN_URL', '')}\n\n",
        f"Jobs: inspect {env('INSPECT_RESULT', '?')}, movies {env('MOVIES_RESULT', '?')}, "
        f"build and tests {env('BUILD_RESULT', '?')}.\n\n",
        "The trusted part was made by main's own code, which read the PR only as data, and reached this job as a job "
        "output, not an artifact the other jobs could replace. The untrusted parts come from jobs that compiled the "
        "PR's sources or ran its code: a hostile PR could forge them, so they are shown as plain text.\n\n",
        unpack_part("INSPECT_REPORT") or "## Trusted inspection\n\n(missing)\n\n",
    ]
    for title, part in (("movies", "MOVIES_REPORT"), ("build and tests", "BUILD_REPORT")):
        text = unpack_part(part)
        out.append(f"## Untrusted: {title}\n\n")
        out.append(block(text.splitlines(), 2000) if text else "    (this job did not run or reported nothing)\n")
        out.append("\n")
    with open(args.out, "w", encoding="utf-8") as f:
        f.writelines(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("flags")
    p.add_argument("files")
    p.add_argument("--comment")
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
    p = sub.add_parser("compile")
    for name in ("--flex", "--ui", "--out", "--report"):
        p.add_argument(name, required=True)
    p.set_defaults(func=cmd_compile)
    p = sub.add_parser("compare")
    for name in ("--built", "--ui", "--report"):
        p.add_argument(name, required=True)
    p.set_defaults(func=cmd_compare)
    p = sub.add_parser("assemble")
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_assemble)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
