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
            sources=[
                "src/aleff/_multishot/v1/_aleff.c",
                "src/aleff/_multishot/v1/adapters/framework.c",
                "src/aleff/_multishot/v1/adapters/sort_engine.c",
                "src/aleff/_multishot/v1/adapters/iterators.c",
                "src/aleff/_multishot/v1/adapters/itertools.c",
                "src/aleff/_multishot/v1/adapters/builtins.c",
                "src/aleff/_multishot/v1/adapters/protocols.c",
                "src/aleff/_multishot/v1/adapters/containers.c",
                "src/aleff/_multishot/v1/adapters/mappings.c",
                "src/aleff/_multishot/v1/adapters/sets.c",
                "src/aleff/_multishot/v1/adapters/text.c",
                "src/aleff/_multishot/v1/adapters/operator.c",
                "src/aleff/_multishot/v1/adapters/functools.c",
                "src/aleff/_multishot/v1/adapters/adapters_bootstrap.c",
            ],
            depends=[
                "src/aleff/_multishot/v1/adapters/api.h",
                "src/aleff/_multishot/v1/adapters/internal.h",
                "src/aleff/_multishot/v1/adapters/builtins.h",
                "src/aleff/_multishot/v1/adapters/functools.h",
                "src/aleff/_multishot/v1/adapters/itertools.h",
                "src/aleff/_multishot/v1/adapters/operator.h",
                "src/aleff/_multishot/v1/adapters/protocols.h",
                "src/aleff/_multishot/v1/adapters/containers.h",
                "src/aleff/_multishot/v1/adapters/iterators.h",
                "src/aleff/_multishot/v1/adapters/mappings.h",
                "src/aleff/_multishot/v1/adapters/sets.h",
                "src/aleff/_multishot/v1/adapters/text.h",
                "src/aleff/_multishot/v1/adapters/sort_engine.h",
            ],
        ),
    ],
    cmdclass={"build_ext": BuildExt},
)
