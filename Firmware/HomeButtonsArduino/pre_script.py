#!/usr/bin/env python

import os
import subprocess

Import("env")

sdkconfig_files = ["sdkconfig.original_release", "sdkconfig.original_debug"]

def delete_sdkconfig_files():
    print("Deleting sdkconfig files...")

    for file in sdkconfig_files:
        if os.path.isfile(file):
            try:
                os.remove(file)
            except:
                print("Failed to delete {}".format(file))
            else:
                print("Deleted {}".format(file))


print("#### PRE SCRIPT ####")
delete_sdkconfig_files()
print("#### PRE SCRIPT DONE ####")


def build_id():
    """Short commit sha, marked when the tree has uncommitted changes.

    Flashed into the firmware and written into the SPIFFS image so a device
    can say exactly what it is running - "is this even the build I flashed"
    is otherwise unanswerable without a diff of the binaries.
    """
    # __file__ is not reliably defined inside a PlatformIO extra script, so
    # take the project directory from SCons rather than from this module.
    root = env.subst("$PROJECT_DIR")
    try:
        sha = subprocess.check_output(
            ["git", "rev-parse", "--short=8", "HEAD"],
            stderr=subprocess.DEVNULL, cwd=root,
        ).decode().strip()
    except Exception:
        return "nogit"
    try:
        dirty = subprocess.call(
            ["git", "diff", "--quiet", "--ignore-submodules", "HEAD"],
            stderr=subprocess.DEVNULL, cwd=root,
        ) != 0
    except Exception:
        dirty = False
    return sha + ("+dirty" if dirty else "")


BUILD_ID = build_id()
print("#### BUILD ID: {} ####".format(BUILD_ID))
env.Append(CPPDEFINES=[("BUILD_SHA", env.StringifyMacro(BUILD_ID))])
