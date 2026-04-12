"""
Demo: Record / Replay

Record effect results and replay the same business logic without side effects.

Use cases:
  - Testing: record a production run, replay in tests to verify results
  - Debugging: reproduce effect sequences from an incident for step-by-step analysis
  - Caching: reuse results of expensive external calls
"""

import json
from dataclasses import dataclass
from typing import Any

from aleff import (
    effect,
    Effect,
    Resume,
    create_handler,
)


# ---------------------------------------------------------------------------
# Effects
# ---------------------------------------------------------------------------

get_temperature: Effect[[str], float] = effect("get_temperature")
get_humidity: Effect[[str], float] = effect("get_humidity")
log: Effect[[str], None] = effect("log")


# ---------------------------------------------------------------------------
# Business logic
# ---------------------------------------------------------------------------


@dataclass
class WeatherReport:
    city: str
    temperature: float
    humidity: float
    comfort: str


def generate_report(city: str) -> WeatherReport:
    log(f"fetching weather for {city}")

    temp = get_temperature(city)
    humidity = get_humidity(city)

    if temp < 15:
        comfort = "cold"
    elif temp > 30:
        comfort = "hot"
    elif humidity > 80:
        comfort = "humid"
    else:
        comfort = "comfortable"

    log(f"report: {temp}°C, {humidity}%, {comfort}")

    return WeatherReport(city=city, temperature=temp, humidity=humidity, comfort=comfort)


# ---------------------------------------------------------------------------
# Record / Replay infrastructure
# ---------------------------------------------------------------------------


@dataclass
class EffectEntry:
    effect_name: str
    args: tuple[Any, ...]
    result: Any

    def to_dict(self) -> dict[str, Any]:
        return {"effect": self.effect_name, "args": list(self.args), "result": self.result}

    @staticmethod
    def from_dict(d: dict[str, Any]) -> "EffectEntry":
        return EffectEntry(effect_name=d["effect"], args=tuple(d["args"]), result=d["result"])


class EffectLog:
    """Recorded log of effect invocations."""

    def __init__(self):
        self._entries: list[EffectEntry] = []
        self._cursor: int = 0

    def record(self, effect_name: str, args: tuple[Any, ...], result: Any) -> None:
        self._entries.append(EffectEntry(effect_name, args, result))

    def next(self) -> EffectEntry:
        if self._cursor >= len(self._entries):
            raise RuntimeError("replay exhausted: no more recorded entries")
        entry = self._entries[self._cursor]
        self._cursor += 1
        return entry

    @property
    def entries(self) -> list[EffectEntry]:
        return list(self._entries)

    def to_json(self) -> str:
        return json.dumps([e.to_dict() for e in self._entries], indent=2)

    @staticmethod
    def from_json(s: str) -> "EffectLog":
        log = EffectLog()
        for d in json.loads(s):
            entry = EffectEntry.from_dict(d)
            log._entries.append(entry)
        return log


# ---------------------------------------------------------------------------
# Handler: Record (wraps real implementation + records results)
# ---------------------------------------------------------------------------


def run_with_recording(city: str) -> tuple[WeatherReport, EffectLog]:
    """Execute effects with a real handler and record the results."""

    effect_log = EffectLog()

    # Simulated external API responses
    weather_data: dict[str, dict[str, float]] = {
        "Tokyo": {"temp": 28.5, "humidity": 65.0},
        "London": {"temp": 12.3, "humidity": 88.0},
        "Dubai": {"temp": 42.1, "humidity": 30.0},
    }

    h = create_handler(get_temperature, get_humidity, log)

    @h.on(get_temperature)
    def _get_temp(k: Resume[float, WeatherReport], city: str):
        result = weather_data[city]["temp"]
        print(f"  [API] GET temperature({city}) -> {result}")
        effect_log.record("get_temperature", (city,), result)
        return k(result)

    @h.on(get_humidity)
    def _get_humidity(k: Resume[float, WeatherReport], city: str):
        result = weather_data[city]["humidity"]
        print(f"  [API] GET humidity({city}) -> {result}")
        effect_log.record("get_humidity", (city,), result)
        return k(result)

    @h.on(log)
    def _log(k: Resume[None, WeatherReport], msg: str):
        print(f"  [LOG] {msg}")
        effect_log.record("log", (msg,), None)
        return k(None)

    report = h(lambda: generate_report(city))
    return report, effect_log


# ---------------------------------------------------------------------------
# Handler: Replay (no side effects, uses recorded data)
# ---------------------------------------------------------------------------


def run_with_replay(city: str, effect_log: EffectLog) -> WeatherReport:
    """Re-execute business logic without side effects using the recorded effect log."""

    h = create_handler(get_temperature, get_humidity, log)

    @h.on(get_temperature)
    def _get_temp(k: Resume[float, WeatherReport], city: str):
        entry = effect_log.next()
        assert entry.effect_name == "get_temperature"
        assert entry.args == (city,)
        print(f"  [REPLAY] get_temperature({city}) -> {entry.result}")
        return k(entry.result)

    @h.on(get_humidity)
    def _get_humidity(k: Resume[float, WeatherReport], city: str):
        entry = effect_log.next()
        assert entry.effect_name == "get_humidity"
        assert entry.args == (city,)
        print(f"  [REPLAY] get_humidity({city}) -> {entry.result}")
        return k(entry.result)

    @h.on(log)
    def _log(k: Resume[None, WeatherReport], msg: str):
        entry = effect_log.next()
        assert entry.effect_name == "log"
        print(f"  [REPLAY] log({msg!r})")
        return k(None)

    return h(lambda: generate_report(city))


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main():
    cities = ["Tokyo", "London", "Dubai"]

    for city in cities:
        # Phase 1: Record
        print(f"=== Record: {city} ===")
        report, effect_log = run_with_recording(city)
        print(f"  -> {report}")
        print()

        # Serialize / deserialize (simulates saving to disk)
        json_data = effect_log.to_json()
        print(f"=== Recorded log ({city}) ===")
        print(json_data)
        print()

        restored_log = EffectLog.from_json(json_data)

        # Phase 2: Replay
        print(f"=== Replay: {city} ===")
        replayed_report = run_with_replay(city, restored_log)
        print(f"  -> {replayed_report}")
        print()

        # Verify
        assert report == replayed_report, f"mismatch for {city}"
        print(f"  [OK] {city}: record and replay match")
        print()

    print("All record/replay demos passed.")


if __name__ == "__main__":
    main()
