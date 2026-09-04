from __future__ import annotations

import os
import runpy
from pathlib import Path
from typing import Any
from unittest.mock import Mock

import pytest
import setuptools
from setuptools import Distribution


ROOT = Path(__file__).resolve().parents[1]


class FakeMsvcCompiler:
    compiler_type = "msvc"

    def __init__(self, *, initialized: bool) -> None:
        self.initialized = initialized
        self.calls: list[str] = []

    def initialize(self) -> None:
        self.calls.append("initialize")
        self.initialized = True

    def spawn(self, cmd: list[str]) -> None:
        assert self.initialized
        self.calls.append("spawn")


def load_build_ext(monkeypatch: pytest.MonkeyPatch) -> type[Any]:
    monkeypatch.setattr(setuptools, "setup", Mock())
    return runpy.run_path(str(ROOT / "setup.py"))["BuildExt"]


def make_build_ext(monkeypatch: pytest.MonkeyPatch, tmp_path: Path, *, initialized: bool) -> Any:
    build_ext = load_build_ext(monkeypatch)(Distribution())
    build_ext.build_temp = str(tmp_path / "build")
    build_ext.compiler = FakeMsvcCompiler(initialized=initialized)
    monkeypatch.setattr(build_ext, "_find_msvc_assembler", lambda: "ml64.exe")
    return build_ext


def test_msvc_asm_compile_initializes_compiler_before_spawn(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    build_ext = make_build_ext(monkeypatch, tmp_path, initialized=False)

    output = build_ext._compile_msvc_asm(os.path.join("src", "backend.asm"))

    assert output == os.path.join(str(tmp_path / "build"), "src", "backend.obj")
    assert build_ext.compiler.calls == ["initialize", "spawn"]


def test_msvc_asm_compile_does_not_reinitialize_compiler(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    build_ext = make_build_ext(monkeypatch, tmp_path, initialized=True)

    build_ext._compile_msvc_asm(os.path.join("src", "backend.asm"))

    assert build_ext.compiler.calls == ["spawn"]


def test_msvc_asm_compile_requires_assembler(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    build_ext = make_build_ext(monkeypatch, tmp_path, initialized=False)
    monkeypatch.setattr(build_ext, "_find_msvc_assembler", lambda: None)

    with pytest.raises(RuntimeError, match="ml64\\.exe is required"):
        build_ext._compile_msvc_asm(os.path.join("src", "backend.asm"))

    assert build_ext.compiler.calls == []
