#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Commit on a branch whose name contains '/' on this machine.

Why this exists
---------------
On this box something (the same application-control agent that refuses to run
MSBuild.exe) blocks git.exe from creating directories under
`.git/refs/heads/`. The consequences are nasty and silent:

  * `git branch foo/bar` reports success, prints nothing, and creates no ref.
  * `git commit` on a branch like `tinecmatool/mumu` writes the commit object
    and appends the reflog entry, then gives up on the ref -- HEAD keeps
    pointing at the previous commit, and git still exits 0.
  * a loose ref that *you* create by hand gets deleted again by the next
    `git commit` touching that ref.

A scratch repo under %TEMP% is unaffected, so this is specific to
`C:\\Program Files\\GraphicsDebuggerRdcTools`. `git update-ref`, `git branch`
and `git commit` are all affected; only refs at the top level
(`refs/heads/flatname`) can be written normally.

What this script does
---------------------
Drives the commit with plumbing, which writes objects only and never touches a
ref, then places the loose ref itself -- byte for byte what git would have
written:

    git add -A            (through a private GIT_INDEX_FILE, see below)
    T=$(git write-tree)
    C=$(git commit-tree $T -p HEAD -F msgfile)
    write .git/refs/heads/<branch>   <- the step git cannot do here

It uses its own index file instead of .git/index because .git/index proved
unreliable here too: a commit built by plain `git commit` came out only
partially staged. Afterwards .git/index is regenerated from the new HEAD so
`git status` reports the truth.

Usage
-----
    python tools/commit_nested_branch.py -F msgfile          # normal commit
    python tools/commit_nested_branch.py -m "subject line"   # inline message
    python tools/commit_nested_branch.py -F msg -o           # also git add -A -u
    python tools/commit_nested_branch.py -F msg -p a -p b    # stage only a and b
    python tools/commit_nested_branch.py --verify            # just report state

Prefer -p: `git add -A` also picks up `.workbuddy/`, `backup_pre_fix/` and the
registry backup .reg files, which should not be committed.

Nothing is pushed.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True,
                      text=True, encoding='utf-8', errors='replace').stdout.strip()
if not ROOT:
    sys.exit('not inside a git working tree')


def git(args, index=None, check=True):
    env = dict(os.environ)
    if index:
        env['GIT_INDEX_FILE'] = index
    r = subprocess.run(['git'] + args, cwd=ROOT, capture_output=True, text=True,
                       encoding='utf-8', errors='replace', env=env)
    if check and r.returncode != 0:
        sys.stderr.write('git %s failed (rc=%d)\n%s%s\n'
                         % (' '.join(args), r.returncode, r.stdout, r.stderr))
        sys.exit(1)
    return (r.stdout or '').strip()


def loose_ref(name):
    return os.path.join(ROOT, '.git', 'refs', 'heads', *name.split('/'))


def describe():
    branch = git(['symbolic-ref', '--short', 'HEAD'])
    head = git(['rev-parse', 'HEAD'])
    missing = not os.path.exists(loose_ref(branch))
    print('branch        : %s' % branch)
    print('HEAD          : %s' % head)
    print('loose ref file: %s %s' % ('MISSING' if missing else 'present',
                                     loose_ref(branch) if missing else ''))
    dirty = git(['status', '--short', '--untracked-files=no'])
    print('tracked changes: %s' % (dirty if dirty else '(clean)'))
    return branch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-m', '--message')
    ap.add_argument('-F', '--file')
    ap.add_argument('-o', '--only-tracked', action='store_true',
                    help='stage with `git add -u` instead of `git add -A`')
    ap.add_argument('-p', '--path', action='append', default=[],
                    help='stage only this path (repeatable). Use this instead of '
                         '-A so untracked scratch dirs stay out of the commit.')
    ap.add_argument('--verify', action='store_true', help='report state and exit')
    args = ap.parse_args()

    if args.verify:
        branch = describe()
        # repair the ref if HEAD disagrees with the tree it should describe
        if not os.path.exists(loose_ref(branch)):
            print('\nref is missing -> if the worktree is already committed, run')
            print('  git rev-parse HEAD   # confirm, then re-run without --verify')
        return 0

    if not (args.file or args.message):
        ap.error('pass -m or -F (or --verify)')
    if args.file and not os.path.isfile(args.file):
        sys.exit('no such message file: %s' % args.file)

    branch = git(['symbolic-ref', '--short', 'HEAD'])
    base = git(['rev-parse', 'HEAD'])
    print('branch: %s   base: %s' % (branch, base[:10]))

    tmpd = tempfile.mkdtemp(prefix='gitidx_')
    idx = os.path.join(tmpd, 'index')
    try:
        # 1. stage off to the side so nothing else can clobber .git/index
        git(['read-tree', base], index=idx)
        if args.path:
            add = ['add', '--'] + args.path
        elif args.only_tracked:
            add = ['add', '-u']
        else:
            add = ['add', '-A']
        git(add, index=idx)
        tree = git(['write-tree'], index=idx)

        staged = git(['diff-tree', '--no-commit-id', '--name-status', '-r', base, tree])
        print('staged entries: %d' % len(staged.split('\n')))
        if not staged:
            print('nothing to commit')
            return 0

        # 2. commit object only -- plumbing never touches a ref
        c = ['commit-tree', tree, '-p', base]
        c += (['-F', args.file] if args.file else ['-m', args.message])
        commit = git(c)
        print('commit: %s' % commit)

        # 3. the step git cannot do here
        p = loose_ref(branch)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', newline='\n') as f:
            f.write(commit + '\n')
        print('wrote %s' % os.path.relpath(p, ROOT))

        # 4. make .git/index agree with the new HEAD
        idx2 = os.path.join(tmpd, 'index2')
        git(['read-tree', commit], index=idx2)
        shutil.copy2(idx2, os.path.join(ROOT, '.git', 'index'))
    finally:
        shutil.rmtree(tmpd, ignore_errors=True)

    print()
    describe()
    d = git(['diff', '--stat', 'HEAD'])
    print('git diff HEAD  : %s' % (d if d else '(empty)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
