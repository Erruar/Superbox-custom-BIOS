#!/usr/bin/env python
"""edk2_patch_buildpy.py - make silent build.py failures verbose.

The stock edk2 build.py swallows OSError/FatalError/Warning exceptions in
Main() and prints nothing, which makes " - Failed - " with no diagnostics.
This script patches the workspace copy of build.py (a disposable mirror,
see tools/build.ps1) so the tracebacks are printed. Re-applied on every
build because the mirror refreshes vendor files.
"""
import sys

path = sys.argv[1]
s = open(path, encoding="utf-8").read()

REPL = [
    ("""    except OSError as X:
        ReturnCode = RESOURCE_UNKNOWN_ERROR""",
     """    except OSError as X:
        import traceback; traceback.print_exc()
        ReturnCode = RESOURCE_UNKNOWN_ERROR"""),
    ("""    except FatalError as X:
        if MyBuild is not None:""",
     """    except FatalError as X:
        import traceback; traceback.print_exc()
        if MyBuild is not None:"""),
    ("""    except Warning as X:
        # error from Fdf parser
        if MyBuild is not None:""",
     """    except Warning as X:
        import traceback; traceback.print_exc()
        # error from Fdf parser
        if MyBuild is not None:"""),
    ("""    except:
        if MyBuild is not None:
            # for multi-thread build exits safely
            MyBuild.Relinquish()

        # try to get the meta-file""",
     """    except:
        import traceback; traceback.print_exc()
        if MyBuild is not None:
            # for multi-thread build exits safely
            MyBuild.Relinquish()

        # try to get the meta-file"""),
]

changed = 0
for old, new in REPL:
    if new in s:
        continue  # already patched
    if old in s:
        s = s.replace(old, new)
        changed += 1

if changed:
    open(path, "w", encoding="utf-8").write(s)
print("edk2_patch_buildpy: %d handler(s) patched in %s" % (changed, path))
