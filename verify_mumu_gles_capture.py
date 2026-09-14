#!/usr/bin/env python3
"""Verdict tool for the MuMu + TinecmaTool GLES capture fix.

Run this after a capture session. It reads the newest TinecmaTool UI session log and
answers the only question that matters:

    did TinecmaTool actually attach a GL/GLES device inside MuMuNxDevice.exe?

Background: MuMu Player 6.7 (nx_device\\15.0) runs the VM and the UI inside a single
process, MuMuNxDevice.exe. Its GLES stack is a forked ANGLE shipped under private
names - libEGL_nemu.dll / libGLESv2_nemu.dll / libNGL.dll - which are resolved
dynamically and therefore never show up in an import table. Until those names were
registered as hook targets the emulator's GLES layer was invisible and a capture
only held the ~8-draw presentation frame on Vulkan.

Usage:
    python verify_mumu_gles_capture.py               # newest session log
    python verify_mumu_gles_capture.py <logfile>     # a specific log
    python verify_mumu_gles_capture.py --all         # newest 5 sessions, compact
"""

import glob
import os
import re
import sys

LOGDIR = os.path.join(os.environ.get("TEMP", os.path.expanduser("~")), "TinecmaTool")

LINE_RE = re.compile(
    r"RDOC (?P<pid>\d+): \[(?P<time>\d\d:\d\d:\d\d)\]\s*"
    r"(?P<file>[^()]+)\(\s*(?P<line>\d+)\) - (?P<level>\w+)\s+- (?P<msg>.*)"
)

HANDSHAKE_RE = re.compile(r"Got remote handshake: (?P<name>\S+) \[(?P<pid>\d+)\]")
LOADING_RE = re.compile(r"Loading into (?P<path>.+)$")
DEVCAP_RE = re.compile(r"Adding (?P<api>[\w]+) device frame capturer for (?P<ptr>0x[0-9A-Fa-f]+)")
PORT_RE = re.compile(r"Listening for target control on (?P<port>\d+)")

TARGET_EXE = "mumunxdevice.exe"


class Session(object):
    def __init__(self, path):
        self.path = path
        self.names = {}        # pid -> process image basename
        self.egl_on = {}       # pid -> True/False/None
        self.devices = {}      # pid -> {api: count}
        self.handshakes = {}   # pid -> reported name
        self.ports = []        # ports bound by the UI
        self.socket_errors = 0
        self.ident_errors = 0
        self.parse()

    def name_of(self, pid):
        return self.names.get(pid, self.handshakes.get(pid, "pid %d" % pid))

    def parse(self):
        try:
            fh = open(self.path, encoding="utf-8", errors="replace")
        except IOError as exc:
            print("cannot read %s: %s" % (self.path, exc))
            return

        with fh:
            for raw in fh:
                m = LINE_RE.search(raw)
                if not m:
                    continue
                pid = int(m.group("pid"))
                msg = m.group("msg")
                f = m.group("file")

                pm = PORT_RE.search(msg)
                if pm:
                    self.ports.append(int(pm.group("port")))

                if "win32_libentry" in f:
                    lm = LOADING_RE.search(msg)
                    if lm:
                        self.names[pid] = os.path.basename(lm.group("path").strip())
                    continue

                if "egl_hooks" in f:
                    if "EGL hooks disabled by RENDERDOC_HOOK_EGL" in msg:
                        self.egl_on[pid] = False
                    elif "Registering EGL hooks" in msg:
                        self.egl_on.setdefault(pid, True)
                    continue

                if "core.cpp" in f:
                    dm = DEVCAP_RE.search(msg)
                    if dm:
                        self.devices.setdefault(pid, {})
                        self.devices[pid][dm.group("api")] = \
                            self.devices[pid].get(dm.group("api"), 0) + 1
                    if "Couldn't open socket for target control" in msg:
                        self.socket_errors += 1
                    if "returned invalid ident" in msg:
                        self.ident_errors += 1
                    continue

                if "target_control.cpp" in f:
                    hm = HANDSHAKE_RE.search(msg)
                    if hm:
                        self.handshakes[int(hm.group("pid"))] = hm.group("name")
                    continue

    def target_pid(self):
        for pid, name in self.names.items():
            if name.lower() == TARGET_EXE:
                return pid
        return None

    def report(self):
        tpid = self.target_pid()
        used = sorted(set(self.ports))
        print("log      : %s" % self.path)
        print("bound ports: %s%s" % (
            used and ("%d..%d (%d distinct)" % (min(used), max(used), len(used)))
            or "none",
            "   socket errors: %d   invalid-ident errors: %d"
            % (self.socket_errors, self.ident_errors)))

        if self.names:
            print("injected processes:")
            for pid in sorted(self.names, key=lambda p: self.name_of(p).lower()):
                print("   %-28s [%d]" % (self.names[pid], pid))

        if not tpid:
            print("\nRESULT: FAIL - MuMuNxDevice.exe was never injected in this session.")
            print("        Check the target path and that 'hook into children' is on.")
            return False

        print("\ntarget: %s [%d]" % (self.names.get(tpid, "?"), tpid))
        egl = self.egl_on.get(tpid)
        print("   EGL hooks    : %s" % (
            "ENABLED" if egl else
            "DISABLED by RENDERDOC_HOOK_EGL" if egl is False else "not logged"))
        devs = self.devices.get(tpid, {})
        if devs:
            for api in sorted(devs):
                print("   device: %-10s x%d" % (api, devs[api]))
        else:
            print("   device:      none - no graphics device was created in this process")

        gls = [a for a in devs if a.upper().startswith("OPENGL")]
        vks = [a for a in devs if a.upper() == "VULKAN"]
        if not vks and not gls:
            print("   handshake    : %s" % ("yes" if tpid in self.handshakes else "NO"))

        ok = bool(gls)
        print("\nRESULT: %s" % ("PASS" if ok else "FAIL"))
        if ok:
            print("        GL/GLES device attached inside MuMuNxDevice - the ANGLE fork")
            print("        (libEGL_nemu / libGLESv2_nemu) is now being captured.")
            print("        Now grab a frame while the game is animating and check the")
            print("        Event Browser drawcall count is in the hundreds, not 8.")
        else:
            print("        No GL/GLES device inside MuMuNxDevice. Either the rebuilt")
            print("        TinecmaTool.dll is not the one being injected, or the emulator")
            print("        is rendering purely through Vulkan/present.")
            if self.egl_on.get(tpid) is False:
                print("        NOTE: EGL hooks were disabled for this process by the")
                print("        RENDERDOC_HOOK_EGL environment variable.")
        return ok


def newest_logs(n):
    files = glob.glob(os.path.join(LOGDIR, "TinecmaTool_2*.log"))
    files.sort(key=os.path.getmtime, reverse=True)
    return files[:n]


def main():
    args = [a for a in sys.argv[1:]]
    if not args:
        files = newest_logs(1)
        if not files:
            print("no session logs found in %s" % LOGDIR)
            return 2
        return 0 if Session(files[0]).report() else 1

    if args[0] == "--all":
        files = newest_logs(40)
        if not files:
            print("no session logs found in %s" % LOGDIR)
            return 2
        shown = 0
        skipped = 0
        for f in files:
            s = Session(f)
            tpid = s.target_pid()
            if not tpid:
                skipped += 1
                continue
            devs = s.devices.get(tpid, {})
            print("%s  target=yes  devices=%-22s  GLES=%s" % (
                os.path.basename(f),
                ",".join("%s x%d" % (a, n) for a, n in sorted(devs.items())) or "none",
                "YES" if any(a.upper().startswith("OPENGL") for a in devs) else "no"))
            shown += 1
            if shown >= 5:
                break
        print("\n%s session(s) with a MuMuNxDevice injection, %d newer log(s) skipped "
              "(no target)" % (shown, skipped))
        return 0

    if not os.path.isfile(args[0]):
        print("no such log: %s" % args[0])
        return 2
    return 0 if Session(args[0]).report() else 1


if __name__ == "__main__":
    sys.exit(main())
