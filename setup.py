import os
import sys
import sysconfig
from setuptools import setup, Extension  # pyright: ignore[reportMissingModuleSource]
from setuptools.command.build_ext import build_ext  # pyright: ignore[reportMissingModuleSource]

header_dir = sysconfig.get_path("include")
if not os.path.isfile(os.path.join(header_dir, "Python.h")):
    v = f"{sys.version_info.major}.{sys.version_info.minor}"
    sys.exit(
        f"error: Python.h not found for Python {v}.\n"
        f"Install the development headers, e.g.:  sudo apt install python{v}-dev"
    )


class BuildExt(build_ext):
    def build_extensions(self):
        if self.compiler.compiler_type == "msvc":
            for ext in self.extensions:
                ext.extra_compile_args = ["/std:c17"]
        else:
            for ext in self.extensions:
                ext.extra_compile_args = ["-std=c2x"]
        build_ext.build_extensions(self)


setup(
    ext_modules=[
        Extension(
            "aleff._multishot.v1._aleff",
            sources=["src/aleff/_multishot/v1/_aleff.c"],
        ),
    ],
    cmdclass={"build_ext": BuildExt},
)
