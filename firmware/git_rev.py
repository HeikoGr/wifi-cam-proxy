# PlatformIO post script: commit of the build as GIT_REV (e.g. "a8d098b" or
# "a8d098b-dirty" with uncommitted changes), for /status and the boot message.
# Only for the project's own sources (projenv), so a new commit does not rebuild the
# framework.
import subprocess

Import("projenv")


def git_rev():
    try:
        out = subprocess.check_output(
            ["git", "describe", "--always", "--dirty", "--abbrev=7"],
            cwd=projenv.subst("$PROJECT_DIR"), stderr=subprocess.DEVNULL)
        return out.decode().strip() or "unknown"
    except Exception:
        return "unknown"


projenv.Append(CPPDEFINES=[("GIT_REV", projenv.StringifyMacro(git_rev()))])
