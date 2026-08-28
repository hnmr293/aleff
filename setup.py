import os
import platform
import sys
import sysconfig
from setuptools import setup, Extension, find_packages  # pyright: ignore[reportMissingModuleSource]
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
                ext.extra_compile_args = (
                    ["/std:c++20"] if any(source.endswith(".cpp") for source in ext.sources) else ["/std:c17"]
                )
        else:
            for ext in self.extensions:
                ext.extra_compile_args = (
                    ["-std=c++17"] if any(source.endswith(".cpp") for source in ext.sources) else ["-std=c2x"]
                )
        build_ext.build_extensions(self)


vendored_greenlet_supported = (
    sys.platform == "linux"
    and sys.implementation.name == "cpython"
    and (3, 12) <= sys.version_info[:2] < (3, 15)
    and platform.machine() == "x86_64"
)

packages = find_packages("src")
package_dir = {"": "src"}
extensions = [
    Extension(
        "aleff._multishot.v1._aleff",
        sources=["src/aleff/_multishot/v1/_aleff.c"],
    ),
]
if vendored_greenlet_supported:
    packages.append("greenlet")
    packages.append("greenlet.platform")
    package_dir["greenlet"] = "vendor/greenlet"
    package_dir["greenlet.platform"] = "vendor/greenlet/platform"
    extensions.append(
        Extension(
            "greenlet._greenlet",
            sources=["vendor/greenlet/greenlet.cpp"],
            include_dirs=["vendor/greenlet"],
        )
    )


setup(
    packages=packages,
    package_dir=package_dir,
    package_data={
        "greenlet": ["*.cpp", "*.hpp", "*.h", "*.pyi", "py.typed", "LICENSE", "LICENSE.PSF"],
        "greenlet.platform": ["*.h"],
    },
    ext_modules=extensions,
    cmdclass={"build_ext": BuildExt},
)
