"""Contract tests for the vendored suspended-greenlet clone operation."""

from contextvars import ContextVar, copy_context
import gc
import sys

import pytest

greenlet = pytest.importorskip("greenlet")
gil_enabled = getattr(sys, "_is_gil_enabled", lambda: True)
if not hasattr(greenlet.greenlet, "clone") or not gil_enabled():
    pytest.skip(
        "suspended greenlet cloning requires a supported GIL-enabled build",
        allow_module_level=True,
    )


def test_clone_preserves_suspended_python_and_native_state() -> None:
    """A clone resumes at the suspension point and never invokes the callable."""
    parent = greenlet.getcurrent()
    calls: list[str] = []
    context_value: ContextVar[str] = ContextVar("clone_context")

    def run() -> tuple[str, object, str]:
        calls.append("run")
        context_value.set("inside")
        return ("left", parent.switch("ready"), "right")

    source = greenlet.greenlet(run)
    source.gr_context = copy_context()

    assert source.switch() == "ready"
    assert calls == ["run"]
    assert context_value.get("outside") == "outside"

    template = source.clone()
    assert calls == ["run"]
    assert template is not source
    assert template.gr_frame is not None

    assert template.switch("cloned") == ("left", "cloned", "right")
    assert calls == ["run"]


def test_suspended_clone_can_be_used_for_sequential_resumes() -> None:
    """Keeping a suspended template permits independent, sequential shots."""
    parent = greenlet.getcurrent()

    def run() -> tuple[str, int]:
        return ("answer", parent.switch("suspended"))

    source = greenlet.greenlet(run)
    assert source.switch() == "suspended"
    template = source.clone()

    first = template.clone()
    second = template.clone()
    assert first.switch(1) == ("answer", 1)
    assert second.switch(2) == ("answer", 2)
    assert not template.dead


def test_suspended_clone_has_an_independent_context_object() -> None:
    parent = greenlet.getcurrent()
    context_value: ContextVar[str] = ContextVar("clone_context_independent")

    def run() -> str:
        context_value.set("source")
        return parent.switch("suspended")

    source = greenlet.greenlet(run)
    source.gr_context = copy_context()
    assert source.switch() == "suspended"

    clone = source.clone()
    assert clone.gr_context is not source.gr_context
    assert clone.gr_context is not None
    assert clone.gr_context.get(context_value) == "source"
    clone.gr_context.run(context_value.set, "clone")
    assert source.gr_context is not None
    assert source.gr_context.get(context_value) == "source"
    assert clone.switch("result") == "result"
    assert clone.dead


def test_unresumed_clones_can_be_discarded_repeatedly() -> None:
    parent = greenlet.getcurrent()

    def run() -> object:
        return parent.switch("suspended")

    source = greenlet.greenlet(run)
    assert source.switch() == "suspended"

    for _ in range(100):
        clone = source.clone()
        del clone
    gc.collect()

    assert source.switch("result") == "result"


@pytest.mark.parametrize("state", ["new", "dead", "current"])
def test_clone_rejects_non_suspended_greenlets(state: str) -> None:
    parent = greenlet.getcurrent()

    def run() -> None:
        parent.switch("suspended")

    if state == "new":
        candidate = greenlet.greenlet(run)
    elif state == "dead":
        candidate = greenlet.greenlet(lambda: None)
        candidate.switch()
    else:
        candidate = parent

    with pytest.raises(greenlet.error, match="suspended"):
        candidate.clone()


def test_clone_rejects_a_greenlet_owned_by_another_thread() -> None:
    """The API must not turn a thread-local native stack into a shared one."""
    parent = greenlet.getcurrent()

    def run() -> None:
        parent.switch("suspended")

    source = greenlet.greenlet(run)
    assert source.switch() == "suspended"

    threading = pytest.importorskip("threading")
    outcome: list[BaseException] = []

    def attempt() -> None:
        try:
            source.clone()
        except BaseException as exc:  # noqa: BLE001 - assert the public error below
            outcome.append(exc)

    thread = threading.Thread(target=attempt)
    thread.start()
    thread.join()
    assert len(outcome) == 1
    assert isinstance(outcome[0], greenlet.error)
