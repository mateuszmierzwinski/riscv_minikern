#!/usr/bin/env python3
"""The image the board is about to run, built and staged.

**Where this belongs, learned twice.** `bazel cquery --output=files` prints
an output's path *without building it*, so staging that way copies whatever
the last build left behind. That served a stale image during the
`MADV_DONTNEED` work and produced a board result for code that was not on
the board; the suite grew a check, and then a single `board.py` boot — which
had no check — did it again, reporting that a self-test had not run when the
truth was that the image predated it.

So the check lives with the **choice of image**, not with one of its
callers: every boot builds the target, hashes the result, compares it against
the file the web server will hand out, and re-stages when they differ. A
board result from an unknown binary is worse than no board result, because
it is read as evidence.
"""
import hashlib
import os
import shutil
import subprocess

BOARD_TARGET = "//:minikern_board"
SERVED = os.path.expanduser(os.environ.get("MINIKERN_SERVED", "~/vf2/minikern.bin"))


def stage(project_dir, quiet=False):
    """Builds the board image and makes the served file be that image.

    Returns the digest now in place, so a caller can print what the board is
    about to run — or None when staging was skipped because the tree does not
    build, which is the caller's to report rather than this one's to hide."""
    built = subprocess.run(["bazel", "build", BOARD_TARGET], cwd=project_dir)
    if built.returncode != 0:
        return None
    path = subprocess.run(["bazel", "cquery", "--output=files", BOARD_TARGET],
                          cwd=project_dir, check=True, capture_output=True,
                          text=True).stdout.split()[0]
    # Workspace-relative, not project-relative: `bazel info workspace` is the
    # only thing that knows where the root is.
    workspace = subprocess.run(["bazel", "info", "workspace"], cwd=project_dir,
                               check=True, capture_output=True, text=True).stdout.strip()
    path = os.path.join(workspace, path)
    fresh = hashlib.md5(open(path, "rb").read()).hexdigest()
    served = (hashlib.md5(open(SERVED, "rb").read()).hexdigest()
              if os.path.exists(SERVED) else None)
    if fresh != served:
        shutil.copyfile(path, SERVED)
        if not quiet:
            print(f"hil: staged a new image at {SERVED}")
    return fresh
