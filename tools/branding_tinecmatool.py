#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
branding_tinecmatool.py -- rebrand the RenderDoc detection surface to TinecmaTool.

Why this exists
---------------
Anti-cheat on the capture target (CrashSight, ACE, ...) fingerprints RenderDoc by
things that are NOT the C API:

  * the module name loaded into the process          (renderdoc.dll)
  * the helper exe it launches                       (renderdoccmd.exe, qrenderdoc.exe)
  * the AppInit / global-hook shim DLL               (renderdocshim64/32.dll)
  * the replay marker export                         (renderdoc__replay__marker)
  * named kernel objects                             (RENDERDOC_CRASHHANDLE, file mapping)
  * window class + window title                      (renderdocGLclass, "RenderDoc ...")
  * on-disk paths it creates                         (%TEMP%\\RenderDoc\\, registry keys)
  * PE version resources                             (ProductName / OriginalFilename)

This script rewrites exactly those. It deliberately does NOT touch:

  * the C API surface           -- RENDERDOC_GetAPI / renderdoc_app.h stay, so
                                  existing in-app integrations keep working
  * the Python module names     -- `import renderdoc` / `import qrenderdoc` stay,
                                  so the RenderDoc MCP and user scripts keep working
  * the .rdc file format        -- unchanged
  * Android (layer .so, package)-- MuMu capture is host-side, so the device side
                                  is out of scope and renaming it would only add risk

Naming convention (matches BRANDING_TINECMATOOL.md so this fork has one brand):
  PascalCase  TinecmaTool          file names, ProjectName/TargetName, exports, display
  ALLCAPS     TINECMATOOL_*        macros, env vars, kernel object names
  lowercase   tinecmatool*         strings that get strlower()'d before comparison

Usage
-----
  python branding_tinecmatool.py --check     # dry run, print every substitution
  python branding_tinecmatool.py --apply     # rewrite the files in place
"""

import argparse
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SCAN_DIRS = ["renderdoc", "renderdoccmd", "renderdocshim", "qrenderdoc"]
SCAN_ROOT_FILES = [
    "CMakeLists.txt",
    "build_and_deploy.cmd",
    "fix_vulkan_layer_registration.py",
]

# Never descend into these directory names (matched anywhere below SCAN_DIRS).
EXCLUDE_DIRS = {
    ".git", ".vs", ".workbuddy", "3rdparty", "docs", "build", "x64", "Win32",
    "agent-chats", "backup_pre_fix", "batch_fbx_exporter_ExtraUV", "util",
    "official", "android", "pyside", "qt", "python",
}

# Never rewrite these files, whatever a rule says. Each has a reason.
PROTECT = [
    # (substring of repo-relative path, reason)
    ("renderdoc/api/app/renderdoc_app.h", "C API surface - RENDERDOC_GetAPI must not move"),
    ("qrenderdoc/Code/pyrenderdoc",       "python module names must not move"),
    ("qrenderdoc/qrenderdoc.pro",         "qmake project for the python/linux path"),
    ("renderdoc/rdocself.version",        "self-capture build uses RDOC_BASE_NAME=rdocself"),
    # this script's whole job is to DELETE stale layer entries and to preserve the
    # OFFICIAL RenderDoc install -- the paths it compares against must keep saying
    # RenderDoc, otherwise it would start treating the official install as junk.
    ("fix_vulkan_layer_registration.py",  "references the OFFICIAL RenderDoc install"),
]

# Paths we refuse to rewrite because the brand string there is PAIRED with a file
# we are not touching, so a one-sided edit would silently break a code path that
# is out of scope for Windows/MuMu capture.  Android/Apple/Linux only.
OFF_LIMITS = [
    "renderdoc/android/",
    "renderdoccmd/android/",
    "renderdoccmd_android.cpp",
    "renderdoccmd_apple.cpp",
    "renderdoccmd_linux.cpp",
    "renderdoc/os/apple/",
    "renderdoc/driver/metal/",
    "qrenderdoc/share/",
]

# Binary/asset extensions we never touch.
SKIP_EXT = {
    ".png", ".jpg", ".jpeg", ".ico", ".bmp", ".obj", ".tlog", ".bin", ".mo",
    ".dll", ".lib", ".exe", ".pdb", ".ttf", ".pdf", ".zip", ".7z", ".so", ".dylib",
}

# ---------------------------------------------------------------------------
# The rule table.  ORDER MATTERS: longest / most specific first, so that
# "renderdocshim64.dll" is rewritten before any bare "renderdoc" rule could see it.
# Each entry: (needle, replacement, note)
# ---------------------------------------------------------------------------
RULES = [
    # --- 1. binary + project artifact names ---------------------------------
    ("renderdocshim64.dll",  "TinecmaToolshim64.dll",  "global-hook shim (64-bit)"),
    ("renderdocshim32.dll",  "TinecmaToolshim32.dll",  "global-hook shim (32-bit)"),
    ("renderdoccmd.exe",     "TinecmaToolcmd.exe",     "helper exe"),
    ("qrenderdoc.exe",       "qTinecmaTool.exe",       "UI exe"),
    ("renderdocui.exe",      "TinecmaToolui.exe",      "UI launcher stub"),
    ("renderdocui_stub",     "TinecmaToolui_stub",     "UI launcher stub project"),
    ("renderdoc.dll",        "TinecmaTool.dll",        "core DLL"),

    # --- 1b. strlower()'d haystacks need the LOWERCASE form ----------------
    # Section 1 has just rewritten "renderdoccmd.exe" -> "TinecmaToolcmd.exe"
    # everywhere, including here. But these two sites lowercase the HAYSTACK
    # before comparing (`rdcstr app = strlower(...)` in sys_win32_hooks.cpp), so a
    # PascalCase needle can never match and the hook would cheerfully inject into
    # our own cmd/UI processes. Put the lowercase form back at exactly these sites.
    ('contains("TinecmaToolcmd.exe")', 'contains("tinecmatoolcmd.exe")',
     "haystack is strlower()'d first"),
    ('contains("qTinecmaTool.exe")',   'contains("qtinecmatool.exe")',
     "haystack is strlower()'d first"),

    # --- 2. marker / named kernel objects ----------------------------------
    ("RenderDocGlobalHookData64", "TinecmaToolGlobalHookData64", "shim<->injector section"),
    ("RenderDocGlobalHookData32", "TinecmaToolGlobalHookData32", "shim<->injector section"),
    ("renderdoc__replay__marker", "TinecmaTool__replay__marker", "replay-self-detection export"),
    ("RENDERDOC_CRASHHANDLE",     "TINECMATOOL_CRASHHANDLE",     "crash server ready event"),
    ("RenderDocBreakpadServer",   "TinecmaToolBreakpadServer",   "crash server named pipe"),

    # --- 3. window class ---------------------------------------------------
    ("renderdocGLclass",     "TinecmaToolGLclass",     "WGL dummy window class"),

    # --- 4. registry, ini, on-disk paths ----------------------------------
    ("RenderDoc.RDCCapture.1",     "TinecmaTool.RDCCapture.1",     "shell integration key"),
    ("RenderDoc\\\\dumps",         "TinecmaTool\\\\dumps",         "minidump folder"),
    ("RenderDoc\\\\%ls_",          "TinecmaTool\\\\%ls_",          "capture/log folder prefix"),
    ("/RenderDoc/RemoteServer_",   "/TinecmaTool/RemoteServer_",   "remote server logs"),
    ("/RenderDoc/%s_",             "/TinecmaTool/%s_",             "posix capture/log folder"),
    ("\\\\renderdoc\\\\symbols",   "\\\\TinecmaTool\\\\symbols",   "dbghelp symbol cache"),
    # %APPDATA%\renderdoc\ -- config.ini / renderdoc.conf / analytics.json / shader_cache
    # live here. Must come AFTER the symbols rule above, which is a sub-path of it.
    ("\\\\renderdoc\\\\",          "\\\\TinecmaTool\\\\",          "per-user appdata folder"),
    ('GetPrivateProfileStringW(L"renderdoc"', 'GetPrivateProfileStringW(L"TinecmaTool"',
     "legacy config.ini section"),
    ('translate("qrenderdoc"',     'translate("qTinecmaTool"',     "Qt translation context"),

    # --- 5. self-detection / injection blacklist (compared lowercased) ----
    ('contains("renderdoccmd.exe")', 'contains("tinecmatoolcmd.exe")', "don't inject into our cmd"),
    ('contains("qrenderdoc.exe")',   'contains("qtinecmatool.exe")',   "don't inject into our UI"),
    ('exename.contains("renderdoccmd")', 'exename.contains("tinecmatoolcmd")',
     "don't spawn the crash server from our own cmd"),
    ('contains("renderdoc.")',       'contains("tinecmatool.")',
     "skip symbol lookup for our own module"),

    # --- 6. window class / process names of the helper -------------------
    ('L"renderdoccmd"',      'L"tinecmatoolcmd"',      "cmd window class"),
    ('"renderdoccmd"',       '"tinecmatoolcmd"',       "cmd program name string"),
    ('lit("renderdoccmd")',  'lit("tinecmatoolcmd")',  "cmd exe lookup"),

    # --- 7. Qt UI display strings ----------------------------------------
    ("QRenderDoc",                  "QTinecmaTool",                  "UI brand"),
    ('lit("RenderDoc ")',           'lit("TinecmaTool ")',           "main window title prefix"),
    ("Qt UI for RenderDoc",         "Qt UI for TinecmaTool",         "UI description"),
    ("Core DLL for RenderDoc",      "Core DLL for TinecmaTool",      "PE FileDescription"),
    ('"ProductName", "RenderDoc"',  '"ProductName", "TinecmaTool"',  "PE ProductName"),

    # --- 8. RDOC_BASE_NAME -- the single source of the DLL name ----------
    ('set(RDOC_BASE_NAME "renderdoc")', 'set(RDOC_BASE_NAME "TinecmaTool")',
     "CMake: base name drives <name>.dll and <name>.version"),
    ("#define RDOC_BASE_NAME renderdoc", "#define RDOC_BASE_NAME TinecmaTool",
     "fallback for configs that don't define it on the command line"),

    # --- 9. leftover plain forms ------------------------------------------
    # build_and_deploy.cmd writes the temp folder with SINGLE backslashes, unlike
    # the C++ sources which need "\\".
    ("\\RenderDoc\\", "\\TinecmaTool\\", "temp folder in the build script echo"),
]

# Per-file rules, applied BEFORE the global table.  Used where a needle is either
# too broad to run repo-wide (it would hit a CMake target name or a path that
# refers to the OFFICIAL RenderDoc install) or only exists in one file.
EXTRA_FILE_RULES = {
    # ---- vcxproj: ProjectName IS the output file name ---------------------
    "renderdoc/renderdoc.vcxproj": [
        ("<ProjectName>renderdoc</ProjectName>", "<ProjectName>TinecmaTool</ProjectName>"),
    ],
    "renderdoccmd/renderdoccmd.vcxproj": [
        ("<ProjectName>renderdoccmd</ProjectName>", "<ProjectName>TinecmaToolcmd</ProjectName>"),
    ],
    # renderdocshim has no <ProjectName> at all -> add one; that is what renames
    # TargetName $(ProjectName)32 / $(ProjectName)64 into TinecmaToolshim32/64.dll
    "renderdocshim/renderdocshim.vcxproj": [
        ("    <RootNamespace>renderdocshim</RootNamespace>\r\n",
         "    <RootNamespace>renderdocshim</RootNamespace>\r\n"
         "    <ProjectName>TinecmaToolshim</ProjectName>\r\n", "once"),
        ("    <RootNamespace>renderdocshim</RootNamespace>\n",
         "    <RootNamespace>renderdocshim</RootNamespace>\n"
         "    <ProjectName>TinecmaToolshim</ProjectName>\n", "once"),
    ],
    "qrenderdoc/qrenderdoc_local.vcxproj": [
        ("<ProjectName>qrenderdoc</ProjectName>", "<ProjectName>qTinecmaTool</ProjectName>"),
        ("<PrimaryOutput>qrenderdoc</PrimaryOutput>", "<PrimaryOutput>qTinecmaTool</PrimaryOutput>"),
        ("<TargetName>qrenderdoc</TargetName>", "<TargetName>qTinecmaTool</TargetName>"),
    ],
    "qrenderdoc/renderdocui_stub.vcxproj": [
        ("<ProjectName>renderdocui_stub</ProjectName>",
         "<ProjectName>TinecmaToolui_stub</ProjectName>"),
        ("<PrimaryOutput>renderdocui</PrimaryOutput>", "<PrimaryOutput>TinecmaToolui</PrimaryOutput>"),
        ("<TargetName>renderdocui</TargetName>", "<TargetName>TinecmaToolui</TargetName>"),
        # the UI must be elevated to reach MuMu's VMM process
        ("    <CharacterSet>Unicode</CharacterSet>\r\n",
         "    <CharacterSet>Unicode</CharacterSet>\r\n"
         "    <UACExecutionLevel>RequireAdministrator</UACExecutionLevel>\r\n", "once"),
        ("    <CharacterSet>Unicode</CharacterSet>\n",
         "    <CharacterSet>Unicode</CharacterSet>\n"
         "    <UACExecutionLevel>RequireAdministrator</UACExecutionLevel>\n", "once"),
    ],

    # ---- Vulkan layer manifest: the description is enumerable by any app ----
    "renderdoc/driver/vulkan/renderdoc.json": [
        ('"description": "Debugging capture layer for RenderDoc"',
         '"description": "Debugging capture layer for TinecmaTool"'),
    ],

    # ---- PE version resource: FileDescription leaked renderdoc.org ---------
    "renderdoccmd/renderdoccmd.rc": [
        ('"FileDescription", "renderdoccmd - https://renderdoc.org/"',
         '"FileDescription", "TinecmaToolcmd"'),
    ],

    # ---- console / dialog text: the bare token is safe only in these files --
    "renderdoccmd/renderdoccmd.cpp": [
        ("renderdoccmd ", "TinecmaToolcmd ", "console banner and usage text"),
    ],
    "qrenderdoc/Windows/Dialogs/CaptureDialog.cpp": [
        ("renderdoccmd ", "TinecmaToolcmd ", "linux elevation dialog text"),
    ],
}

# Files to rename on disk: (old, new)
FILE_RENAMES = [
    ("renderdoc/renderdoc.version", "renderdoc/TinecmaTool.version"),
]

# ---------------------------------------------------------------------------
# Final mop-up pass: replace the brand token ONLY inside double-quoted string
# literals.
#
# Why this is needed and why it is safe: "RenderDoc" is simultaneously
#   * our brand, appearing in log lines, user-facing messages and the strings the
#     target process can read back (VkPhysicalDeviceToolProperties, GL debug tool
#     name, the VkApplicationInfo we trample), and
#   * a C++ identifier (class RenderDoc, RenderDoc::Inst(), RenderDoc_FirstTarget...).
# A plain text replace would shred the source. Touching only the bytes between
# quotes gets every fingerprint and no symbol.
#
# Applied LAST, so the explicit rules above have already handled the paired
# identity strings (marker, crash event, shim names, ...).
# ---------------------------------------------------------------------------
STRING_LITERAL_RULES = [
    ("RenderDoc", "TinecmaTool"),
]

# Files the string-literal sweep must NOT touch, even though they contain
# "RenderDoc" inside quotes.  Each entry is a reason, not a preference.
SWEEP_EXCLUDE = {
    # A pre-build check greps the SOURCE for this exact needle. Rewriting it would
    # make the check fail and abort the build before it starts.
    "build_and_deploy.cmd": "contains a findstr needle that must match source",
    # STRINGISE_ENUM_CLASS_NAMED feeds ToStr/TypeOf, which the structured-data
    # serializer uses to round-trip enums by name into .rdc files and
    # renderdoc.conf. Renaming a value would strand every existing capture/config.
    "renderdoc/api/replay/renderdoc_tostr.inl": "enum display names may be serialized by name",
    "renderdoc/api/replay/replay_enums.h": "enum display names may be serialized by name",
    # External interop contracts with AMD's Radeon GPU Profiler. RGP scans the
    # command stream / UI window for these exact strings. Renaming silently breaks
    # RGP integration, and neither string is visible to the captured application.
    "renderdoc/driver/ihv/amd/amd_rgp.cpp": "RGP capture marker contract",
    "qrenderdoc/Code/RGPInterop.cpp": "RGP interop name contract",
    "qrenderdoc/Code/RGPInterop.h": "RGP interop name contract",
}

# Files where the PRE-REBRAND names appear on purpose, so the RULES pass is
# skipped for them (EXTRA_FILE_RULES and the string-literal sweep still run).
NAME_RULE_EXCLUDE = {
    # The build script probes for and warns about stale copies of the old
    # binaries: a leftover renderdoc.dll in x64\Development is still picked up by
    # the AppInit_DLLs hook, which is exactly the failure mode worth shouting
    # about. So it has to keep spelling the old names. A clean --check is only
    # meaningful if that is accounted for here rather than "fixed".
    "build_and_deploy.cmd": "deliberately names stale pre-rebrand binaries",
}


def replace_in_string_literals(text, needle, repl):
    """Replace only inside "..." literals, one line at a time.

    Splitting on the quote character makes every odd-indexed segment literal
    content, which also handles literals that C++ splits across lines.
    """
    parts_all = []
    count = 0
    for line in text.split("\n"):
        parts = line.split('"')
        for i in range(1, len(parts), 2):
            n = parts[i].count(needle)
            if n:
                count += n
                parts[i] = parts[i].replace(needle, repl)
        parts_all.append('"'.join(parts))
    return "\n".join(parts_all), count



def read_text(path):
    """Return (text, encoding, had_bom).  surrogateescape keeps odd bytes intact."""
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xff\xfe"):
        return raw[2:].decode("utf-16-le", "surrogatepass"), "utf-16-le", True
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw[3:].decode("utf-8", "surrogateescape"), "utf-8", True
    if len(raw) > 8 and raw.count(b"\x00") > len(raw) // 8:
        return raw.decode("utf-16-le", "surrogatepass"), "utf-16-le", False
    return raw.decode("utf-8", "surrogateescape"), "utf-8", False


def write_text(path, text, enc, bom):
    data = text.encode(enc, "surrogatepass" if enc == "utf-16-le" else "surrogateescape")
    if bom:
        data = (b"\xff\xfe" if enc == "utf-16-le" else b"\xef\xbb\xbf") + data
    with open(path, "wb") as f:
        f.write(data)


def is_protected(rel):
    rel = rel.replace(os.sep, "/")
    for sub, why in PROTECT:
        if rel.startswith(sub) or ("/" + sub) in rel:
            return why
    for sub in OFF_LIMITS:
        if rel.startswith(sub) or ("/" + sub) in rel:
            return "out of scope: brand string is paired across platforms"
    return None


def walk():
    out = []
    for d in SCAN_DIRS:
        base = os.path.join(REPO, d)
        for dp, dns, fns in os.walk(base):
            dns[:] = [x for x in dns if x not in EXCLUDE_DIRS]
            for fn in fns:
                if os.path.splitext(fn)[1].lower() in SKIP_EXT:
                    continue
                out.append(os.path.join(dp, fn))
    for fn in SCAN_ROOT_FILES:
        p = os.path.join(REPO, fn)
        if os.path.isfile(p):
            out.append(p)
    return sorted(set(out))


def apply_rules(text, rel, rules):
    """Return (new_text, [(needle, replacement, count)])

    A rule may carry the flag "once" as its third element.  That means: skip the
    rule if the replacement is already present.  Needed for the rules that INSERT
    a line (e.g. <UACExecutionLevel>) rather than substitute one, because their
    anchor still matches on a second run and would insert a duplicate.
    """
    hits = []
    for entry in rules:
        needle, repl = entry[0], entry[1]
        flag = entry[2] if len(entry) > 2 else None
        if flag == "once" and repl in text:
            continue
        n = text.count(needle)
        if n:
            text = text.replace(needle, repl)
            hits.append((needle, repl, n))
    return text, hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not (args.check or args.apply):
        ap.error("pass --check or --apply")

    files = walk()
    total_hits = 0
    changed_files = []
    skipped = []
    per_rule = {}

    for path in files:
        rel = os.path.relpath(path, REPO)
        relu = rel.replace(os.sep, "/")

        why = is_protected(relu)
        if why:
            txt, _, _ = read_text(path)
            if any(txt.count(e[0]) for e in RULES):
                skipped.append((relu, why))
            continue

        txt, enc, bom = read_text(path)

        rules = [] if relu in NAME_RULE_EXCLUDE else list(RULES)
        if relu in EXTRA_FILE_RULES:
            rules = EXTRA_FILE_RULES[relu] + rules

        new, hits = apply_rules(txt, relu, rules)

        # last pass: brand token inside string literals only
        if relu not in SWEEP_EXCLUDE:
            for needle, repl in STRING_LITERAL_RULES:
                new, n = replace_in_string_literals(new, needle, repl)
                if n:
                    hits.append((needle + " (in string literal)", repl, n))

        if not hits:
            continue

        changed_files.append((relu, sum(h[2] for h in hits)))
        total_hits += sum(h[2] for h in hits)
        for needle, repl, n in hits:
            per_rule[needle] = per_rule.get(needle, 0) + n

        if args.check:
            if args.verbose:
                needles = [h[0].replace(" (in string literal)", "") for h in hits]
                for i, line in enumerate(txt.split("\n"), 1):
                    if any(n in line for n in needles):
                        print("  %s:%d" % (relu, i))
                        print("      - %s" % line.strip()[:150])
        else:
            write_text(path, new, enc, bom)

    print("--- branding_tinecmatool: %s ---" % ("APPLY" if args.apply else "CHECK"))
    print("scanned %d files, %d need changes, %d substitutions"
          % (len(files), len(changed_files), total_hits))
    print()
    for rel, n in sorted(changed_files, key=lambda x: -x[1]):
        print("  %4d  %s" % (n, rel))
    print()
    print("rule breakdown:")
    rule_map = dict((e[0], e[1]) for e in RULES)
    for needle, n in sorted(per_rule.items(), key=lambda x: -x[1]):
        print("  %4d  %-32s -> %s" % (n, needle, rule_map.get(needle, "?")))
    if skipped:
        print()
        print("protected files that would otherwise have matched (left alone):")
        for rel, why in skipped:
            print("  %s   [%s]" % (rel, why))

    if NAME_RULE_EXCLUDE:
        print()
        print("files where the pre-rebrand names are intentional (name rules off):")
        for rel, why in sorted(NAME_RULE_EXCLUDE.items()):
            print("  %s   [%s]" % (rel, why))

    if args.apply:
        print()
        print("renaming files on disk:")
        for old, new in FILE_RENAMES:
            op = os.path.join(REPO, old.replace("/", os.sep))
            np_ = os.path.join(REPO, new.replace("/", os.sep))
            if os.path.isfile(op):
                os.replace(op, np_)
                print("  %s -> %s" % (old, new))
            elif os.path.isfile(np_):
                print("  %s already renamed" % new)
            else:
                print("  MISSING: %s" % old)

    return 0


if __name__ == "__main__":
    sys.exit(main())
