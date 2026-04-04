import sys
import sysconfig
from setuptools import setup, Extension

header_dir = sysconfig.get_path("include")
if header_dir is None or not __import__("os").path.isfile(__import__("os").path.join(header_dir, "Python.h")):
    v = f"{sys.version_info.major}.{sys.version_info.minor}"
    sys.exit(
        f"error: Python.h not found for Python {v}.\n"
        f"Install the development headers, e.g.:  sudo apt install python{v}-dev"
    )

setup(
    ext_modules=[
        Extension(
            "aleff._aleff",
            sources=["src/aleff/_aleff.c"],
            extra_compile_args=["-std=c2x"],
        ),
    ],
)
