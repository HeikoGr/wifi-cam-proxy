# PlatformIO pre script: compile through ccache if it is installed. pioarduino keeps a
# single custom build of ESP-IDF and the Arduino core (custom_sdkconfig) for all
# environments and rebuilds it (~4 min) whenever the build switches between the ESP32 and
# the ESP32-S3; with ccache the files compiled before come from the cache. Symlinks named
# like the compilers go in front of the toolchain on PATH (ccache's masquerade mode), so
# they also apply to the build environments the framework scripts clone.
import os
import shutil

Import("env")

ccache = shutil.which("ccache")
if ccache:
    bindir = os.path.join(env.subst("$PROJECT_DIR"), ".pio", "ccache-bin")
    os.makedirs(bindir, exist_ok=True)
    for chip in ("esp32", "esp32s3"):
        for tool in ("gcc", "g++"):
            link = os.path.join(bindir, "xtensa-%s-elf-%s" % (chip, tool))
            if not os.path.islink(link):
                os.symlink(ccache, link)
    env.PrependENVPath("PATH", bindir)
