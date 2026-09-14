#!/usr/bin/env python3
"""Regression gate for the SetThreadContext trampoline in win32_process.cpp.

The trampoline is hand-encoded x86-64 machine code. Its interesting failure
modes are invisible to the compiler and to a normal test run:

  1. stack misalignment -- a Windows x64 callee requires rsp % 16 == 8 on entry
     and takes an access violation on its first aligned SSE spill if it is not.
     Whether that happens depends on where the victim thread was suspended, so
     it presents as an intermittent "sometimes the target process dies".
  2. an unbalanced push/pop, which silently walks the victim's stack pointer and
     corrupts it on return.
  3. freeing the code page the victim thread is about to return into, which
     kills the target deterministically.

This script parses the sc_push8/sc_push32/sc_push64 calls straight out of the
C++ function (so it cannot drift from a hand-maintained copy), rebuilds the byte
stream, disassembles it, and replays the stack effects of the real instruction
sequence for both possible incoming stack parities.

Run:  python tools/check_hijack_shellcode.py
"""

import os
import re
import sys

try:
    import capstone
    HAVE_CAPSTONE = True
except ImportError:
    HAVE_CAPSTONE = False

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "renderdoc", "os", "win32", "win32_process.cpp")

FAILED = []


def fail(msg):
    FAILED.append(msg)
    print("  [FAIL] " + msg)


def ok(msg):
    print("  [ ok ] " + msg)


def extract_builders(text):
    """Return (x64_body, x86_body)."""
    guard = "#if defined(_M_X64) || defined(__x86_64__)"
    g = text.find(guard)
    if g < 0:
        raise SystemExit("could not find the x64 guard in " + SOURCE)
    s64 = text.find("static rdcarray<uint8_t> BuildHijackShellcode", g)
    e64 = text.find("\n#else", s64)
    s86 = e64
    e86 = text.find("\n#endif", s86)
    if min(s64, e64, s86, e86) < 0:
        raise SystemExit("could not isolate the two BuildHijackShellcode bodies")
    return text[s64:e64], text[s86:e86]


# Symbolic operands the builder passes through, replaced with marker values that
# are recognisable in the disassembly.
SYMBOLS = {
    "arg": 0x000001D2_00001000,
    "funcAddr": 0x00007FF8_12345678,
    "doneFlagAddr": 0x000001D2_00002008,
    "savedRspSlot": 0x000001D2_00002000,
    "origRip": 0x00007FF8_ABCDEF01,
    "origEip": 0x00007FF8_ABCDEF01,
}

PUSH8 = re.compile(r"sc_push8\(sc,\s*(0x[0-9A-Fa-f]{2})\s*\)\s*;")
PUSH32_SYM = re.compile(r"sc_push32\(sc,\s*\(uint32_t\)\s*(\w+)\s*\)\s*;")
PUSH32_SHIFT = re.compile(r"sc_push32\(sc,\s*\(uint32_t\)\s*\(\s*(\w+)\s*>>\s*(\d+)\s*\)\s*\)\s*;")
PUSH32_LIT = re.compile(r"sc_push32\(sc,\s*(0x[0-9A-Fa-f]+)\s*\)\s*;")
PUSH64_SYM = re.compile(r"sc_push64\(sc,\s*\(uint64_t\)\s*(\w+)\s*\)\s*;")


def assemble(body):
    """Replay the builder's emissions in source order."""
    out = bytearray()
    for m in re.finditer(r"sc_push(8|32|64)\(sc,[^;]*\);", body):
        frag = m.group(0)
        kind = m.group(1)
        if kind == "8":
            mm = PUSH8.match(frag)
            if not mm:
                raise SystemExit("unparsed sc_push8: " + frag)
            out.append(int(mm.group(1), 16))
        elif kind == "32":
            mm = PUSH32_SHIFT.match(frag)
            if mm:
                v = (SYMBOLS[mm.group(1)] >> int(mm.group(2))) & 0xFFFFFFFF
            else:
                mm = PUSH32_SYM.match(frag)
                if mm:
                    v = SYMBOLS[mm.group(1)] & 0xFFFFFFFF
                else:
                    mm = PUSH32_LIT.match(frag)
                    if not mm:
                        raise SystemExit("unparsed sc_push32: " + frag)
                    v = int(mm.group(1), 16) & 0xFFFFFFFF
            out += v.to_bytes(4, "little")
        else:
            mm = PUSH64_SYM.match(frag)
            if not mm:
                raise SystemExit("unparsed sc_push64: " + frag)
            out += (SYMBOLS[mm.group(1)] & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little")
    return bytes(out)


def simulate_rsp(insns, entry_rsp):
    """Replay the stack effects of the disassembled trampoline.

    Returns (rsp_at_call, rsp_at_ret, note). `entry_rsp` is the victim's RSP at
    the moment of the hijack, which is what makes the alignment question
    interesting.
    """
    rsp = entry_rsp
    stored = None
    rsp_at_call = None
    rsp_at_ret = None
    for ins in insns:
        mn, ops = ins.mnemonic, ins.op_str
        if rsp_at_call is None and mn == "call":
            rsp_at_call = rsp
        if mn in ("push", "pushfq"):
            rsp -= 8
        elif mn in ("pop", "popfq"):
            rsp += 8
        elif mn == "movabs" and ops.startswith("rsp, "):
            rsp = int(ops.split(",")[1].strip(), 16)
        elif mn == "mov" and ops.startswith("rsp, qword ptr [rax]"):
            if stored is None:
                return None, None, "rsp is restored from a slot that was never written"
            rsp = stored
        elif mn == "mov" and ops.startswith("qword ptr [rax], rsp"):
            stored = rsp
        elif mn == "and" and ops.startswith("rsp, "):
            imm = int(ops.split(",")[1].strip(), 0)
            if imm < 0:
                imm &= (1 << 64) - 1
            rsp &= imm
        elif mn == "sub" and ops.startswith("rsp, "):
            rsp -= int(ops.split(",")[1].strip(), 0)
        elif mn == "add" and ops.startswith("rsp, "):
            rsp += int(ops.split(",")[1].strip(), 0)
        elif mn == "ret":
            # ret pops the return address, so record what the victim resumes
            # with rather than the state mid-instruction
            rsp += 8
            rsp_at_ret = rsp
    return rsp_at_call, rsp_at_ret, None


def main():
    text = open(SOURCE, "rb").read().decode("utf-8", "replace")
    print("=" * 78)
    print("SetThreadContext trampoline gate   (%s)" % os.path.relpath(SOURCE, ROOT))
    print("=" * 78)

    print("\npulling the x64 trampoline out of the C++ source")
    x64_body, x86_body = extract_builders(text)
    code = assemble(x64_body)
    print("  emitted %d bytes from %d sc_push calls"
          % (len(code), len(re.findall(r"sc_push(?:8|32|64)\(sc,", x64_body))))

    print("\nstructural")
    if b"\x48\x83\xe4\xf0" not in code:
        fail("x64 trampoline never aligns rsp (no `and rsp, -16` / 48 83 E4 F0); "
             "the call would inherit the victim's unknown stack parity")
    else:
        ok("aligns rsp explicitly instead of trusting the victim's parity")
    if b"\x48\xbc" in code:
        fail("trampoline switches to an absolute stack -- that is a stale design, "
             "the call should run on free stack below the victim's frame")
    else:
        ok("no absolute stack switch; no extra remote allocation needed")

    print("\ndisassembly (CS_MODE_64)")
    if not HAVE_CAPSTONE:
        print("  [SKIP] capstone is not installed -- byte stream not decoded, so the")
        print("         instruction-level asserts below did not run.")
        print("         python -m pip install capstone")
        insns = []
    else:
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        md.detail = False
        insns = list(md.disasm(code, 0))
        for ins in insns:
            print("  %-4x %-24s %s %s"
                  % (ins.address, ins.bytes.hex(" "), ins.mnemonic, ins.op_str))

        consumed = sum(len(i.bytes) for i in insns)
        if consumed != len(code):
            fail("disassembly consumed %d of %d bytes -- the encoding is not "
                 "self-consistent" % (consumed, len(code)))
        else:
            ok("every byte decoded as a complete instruction")

    if insns:
        call_idx = next((i for i, x in enumerate(insns) if x.mnemonic == "call"), None)
        if call_idx is None:
            fail("no call instruction in the trampoline")
        else:
            # the whole point: correct for *both* incoming parities
            results = {}
            for parity in (0, 8):
                entry = 0x7FF000 + parity
                at_call, at_ret, note = simulate_rsp(insns, entry)
                results[parity] = (at_call, at_ret, note)

            bad = [p for p, (c, _, _) in results.items() if c is None or c % 16 != 0]
            if bad:
                for p in bad:
                    c = results[p][0]
                    fail("with the victim stopped at rsp %s mod 16 the call would "
                         "execute at rsp %s (needs 0)"
                         % (p, "%#x (%d mod 16)" % (c, c % 16) if c is not None else "?"))
            else:
                ok("rsp % 16 == 0 at the call for both incoming parities, so the "
                   "callee enters with 8 as the x64 ABI requires")

            for p, (_, at_ret, note) in results.items():
                if note:
                    fail("rsp simulation with parity %d: %s" % (p, note))
                elif at_ret != 0x7FF000 + p:
                    fail("stack is not balanced: the victim was hijacked with rsp %#x "
                         "but resumes with %#x"
                         % (0x7FF000 + p, at_ret))
            if not any(n for _, _, n in results.values()) and \
               all(r[1] == 0x7FF000 + p for p, r in results.items()):
                ok("stack is exactly balanced -- every push is matched, so the "
                   "victim resumes with its original rsp")

        store_idx = next((i for i, x in enumerate(insns)
                          if x.mnemonic == "mov"
                          and x.op_str.startswith("byte ptr [rax], 1")), None)
        if store_idx is None:
            fail("the trampoline never stores the done flag")
        elif call_idx is not None and store_idx < call_idx:
            fail("the done flag is stored before the call -- the injector would "
                 "free the page while the callee is still running")
        else:
            ok("done flag is stored after the call returns")

        if len(insns) < 3:
            fail("trampoline too short to contain the return sequence")
        else:
            push, patch, ret = insns[-3], insns[-2], insns[-1]
            want_lo = SYMBOLS["origRip"] & 0xFFFFFFFF
            want_hi = (SYMBOLS["origRip"] >> 32) & 0xFFFFFFFF
            if push.mnemonic != "push" or patch.mnemonic != "mov" or ret.mnemonic != "ret":
                fail("the trampoline does not end in push <lo32> / mov [rsp+4] <hi32> / ret")
            elif "dword ptr [rsp + 4]" not in patch.op_str:
                fail("expected `mov dword ptr [rsp + 4], <hi32 of origRip>`, got "
                     "%s %s" % (patch.mnemonic, patch.op_str))
            else:
                got_lo = int(push.op_str, 0) & 0xFFFFFFFF
                got_hi = int(patch.op_str.split(",")[-1].strip(), 0) & 0xFFFFFFFF
                if got_lo != want_lo or got_hi != want_hi:
                    fail("return target rebuilds to %#x%08x, expected %#x%08x"
                         % (got_hi, got_lo, want_hi, want_lo))
                else:
                    ok("returns to the victim's original RIP (%#x)" % SYMBOLS["origRip"])

        pops = [x for x in insns if x.mnemonic in ("pop", "popfq")]
        pushes = [x for x in insns if x.mnemonic in ("push", "pushfq")]
        # push origRip is popped by ret, so it is not part of the mirrored set
        if len(pops) != 8 or len([x for x in pops if x.mnemonic == "popfq"]) != 1:
            fail("expected 7 pop + 1 popfq to mirror pushfq + 7 pushes, got %d pop, "
                 "%d popfq" % (len([x for x in pops if x.mnemonic == "pop"]),
                               len([x for x in pops if x.mnemonic == "popfq"])))
        else:
            order = [x.mnemonic + " " + x.op_str for x in pops if x.mnemonic == "pop"]
            want = ["pop r11", "pop r10", "pop r9", "pop r8", "pop rdx", "pop rcx", "pop rax"]
            if order != want:
                fail("register teardown is out of order: %s" % order)
            else:
                ok("victim r11/r10/r9/r8/rdx/rcx/rax/flags restored in reverse "
                   "push order")

    print("\nx86 path (compiled; not exercised by the x64 target)")
    if "(void)savedRspSlot" not in x86_body:
        fail("the x86 builder does not acknowledge the unused savedRspSlot parameter")
    else:
        ok("x86 builder accepts the x64-only parameter without warning")
    if "add esp, 4" not in x86_body:
        fail("the x86 builder has no caller-side cleanup for the __cdecl "
             "INTERNAL_* exports")
    else:
        ok("x86 builder carries the caller-side pop for __cdecl callees")

    print("\npage lifetime")
    fn = text[text.find("static bool ThreadHijackInvoke"):]
    fn = fn[:fn.find("\nstatic bool InjectDLL_ThreadHijack")]
    # strip comments first: the explanation of why the pages are leaked names
    # VirtualFreeEx, and a substring search must not read that as a call
    fn = re.sub(r"/\*.*?\*/", "", fn, flags=re.S)
    fn = re.sub(r"//[^\n]*", "", fn)
    tail = fn[fn.find("bool ok = false;"):]
    if re.search(r"\bVirtualFreeEx\s*\(", tail):
        fail("ThreadHijackInvoke frees remote memory after redirecting the thread; "
             "the victim returns into that page and dies")
    else:
        ok("no VirtualFreeEx after the thread is redirected (trampoline pages are "
           "intentionally leaked)")

    print("\n" + "=" * 78)
    if FAILED:
        print("RESULT: %d FAILED" % len(FAILED))
        for f in FAILED:
            print("   ! " + f)
        return 1
    print("RESULT: trampoline OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
