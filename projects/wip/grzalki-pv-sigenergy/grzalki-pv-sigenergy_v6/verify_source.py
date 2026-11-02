"""Source-contract checks only. No compiler, firmware execution or hardware I/O.

These checks prevent structural regressions; they do NOT prove C++ compilation,
FreeRTOS timing, GPIO behaviour, or library integration on a real ESP32.
Run: py -3 <absolute path to this file>
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent
SKETCH = (ROOT / "grzalki-pv-sigenergy_v6.ino").read_text(encoding="utf-8")
CONTROL = (ROOT / "controller.h").read_text(encoding="utf-8")
LOGIC = (ROOT / "control_logic.h").read_text(encoding="utf-8")
SETTINGS = (ROOT / "settings.h").read_text(encoding="utf-8")


def code(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def body(text, signature):
    """Extract a braced body for structural checks (not a C++ parser)."""
    text = code(text)
    start = text.index("{", text.index(signature))
    depth = 1
    for end in range(start + 1, len(text)):
        depth += (text[end] == "{") - (text[end] == "}")
        if depth == 0:
            return text[start + 1:end]
    raise AssertionError(f"Unclosed body: {signature}")


class SourceContracts(unittest.TestCase):
    def test_network_owner_has_no_hardware_access(self):
        for token in ("digitalWrite(", "gpio_set_level(", "Wire.", "sensors."):
            self.assertNotIn(token, code(SKETCH))
        self.assertEqual(code(SKETCH).count("Controller::initializePins()"), 1)

    def test_engine_has_no_network_or_supla_calls(self):
        engine = body(CONTROL, "class Engine {")
        for token in ("SuplaDevice", "Supla::", "WiFi.", "mb.", "web."):
            self.assertNotIn(token, engine)
        self.assertIn("temperatureTask();", body(CONTROL, "void step()"))
        self.assertIn("engine.step();", body(CONTROL, "inline void taskEntry"))

    def test_gpio_latch_precedes_output_enable(self):
        init = body(CONTROL, "inline void initializePins")
        self.assertLess(init.index("pinMode(pin, INPUT)"),
                        init.index("digitalWrite(pin, RELAY_OFF)"))
        self.assertLess(init.index("digitalWrite(pin, RELAY_OFF)"),
                        init.index("pinMode(pin, OUTPUT)"))

    def test_original_freshness_deadlines(self):
        self.assertRegex(CONTROL, r"TEMP_STALE_MS\s*=\s*6000")
        self.assertRegex(CONTROL, r"GRID_STALE_MS\s*=\s*10000")
        self.assertIn("gridFresh(now)", body(CONTROL, "bool allowed"))

    def test_dac_checked_even_at_constant_power(self):
        step = body(CONTROL, "void step()")
        self.assertIn("mainOn && !dacFault && Heating::elapsed(millis(), lastDacWrite, 1000)", step)
        self.assertLess(step.index("writeSsr(ssrWatts)"), step.index("const bool fault"))
        self.assertNotIn("transition()", body(CONTROL, "bool writeSsr"))

    def test_no_volatile_as_synchronization(self):
        self.assertNotRegex(code(CONTROL + SKETCH), r"\bvolatile\b")
        self.assertIn("portENTER_CRITICAL", CONTROL)
        self.assertIn("portEXIT_CRITICAL", CONTROL)
        self.assertIn("Guard guard(*this)", body(CONTROL, "Snapshot read()"))
        self.assertNotIn("mailbox.data", code(SKETCH))

    def test_no_blocking_api_in_critical_sections(self):
        text = code(CONTROL)
        # Check the remainder of every lexical scope after its RAII guard.
        for match in re.finditer(r"(?:Mailbox::)?Guard guard\([^;]+;", text):
            remaining = text[match.end():]
            depth = 0
            end = len(remaining)
            for i, char in enumerate(remaining):
                if char == "{":
                    depth += 1
                elif char == "}":
                    if depth == 0:
                        end = i
                        break
                    depth -= 1
            locked = remaining[:end]
            for token in ("Wire.", "writeSsr(", "sensors.", "Serial.", "vTaskDelay("):
                self.assertNotIn(token, locked)

    def test_commit_rechecks_after_i2c(self):
        apply = body(CONTROL, "void apply(")
        self.assertLess(apply.index("writeSsr(nextSsr)"),
                        apply.index("Mailbox::Guard guard"))
        for token in ("Heating::sameCommand", "allowed(c, now)", "Heating::timedOut"):
            self.assertGreater(apply.index(token), apply.index("writeSsr(nextSsr)"))
        self.assertGreater(apply.index("gpio(next2"), apply.index("if (commit)"))

    def test_fault_revokes_requests_and_grid(self):
        step = body(CONTROL, "void step()")
        self.assertIn("!temperatureOk(now) || hot || dacFault || !cfg.valid", step)
        self.assertIn("if (fault && !wasFault) mailbox.revokeLocked()", step)
        revoke = body(CONTROL, "void revokeLocked()")
        for token in ("++data.command.generation", "d.on = false",
                      "d.duration = 0", "data.grid.valid = false"):
            self.assertIn(token, revoke)

    def test_countdowns_expire_without_supla(self):
        step = body(CONTROL, "void step()")
        self.assertIn("Heating::timedOut(now, d.started, d.duration)", step)
        self.assertIn("++d.revision", step)
        self.assertIn("c.modeExpired = true", step)
        self.assertIn("Heating::timedOut(now, c.modeStarted, c.modeDuration)", step)

    def test_rejected_on_does_not_erase_off_countdown(self):
        rejected = body(LOGIC, "constexpr bool cancelRejectedOn")
        self.assertIn("return !requestedOn && duration != 0;", rejected)
        turn_on = body(SKETCH, "void turnOn(")
        self.assertIn("Heating::cancelRejectedOn(isOn(), durationMs)", turn_on)
        self.assertNotIn("durationMs = 0", turn_on)
        self.assertIn("timerDispatch && timerGeneration != seenGeneration", turn_on)

    def test_revocation_before_library_timer_dispatch(self):
        iteration = body(SKETCH, "void iterateAlways()")
        self.assertLess(iteration.index("sync();"),
                        iteration.index("VirtualRelay::iterateAlways()"))
        clear = body(SKETCH, "void clearLocalRequest()")
        self.assertIn("isOn() || durationMs != 0", clear)
        self.assertIn("VirtualRelay::turnOff(0)", clear)

    def test_failed_task_creation_is_reported(self):
        start = body(CONTROL, "inline bool start()")
        self.assertIn("result != pdPASS", start)
        self.assertIn("mailbox.taskCreationFailed()", start)
        self.assertIn("return false", start)

    def test_modbus_timeout_and_ipv4_guard(self):
        text = code(SKETCH)
        self.assertLess(text.index("#define MODBUSIP_CONNECT_TIMEOUT 300"),
                        text.index("#include <ModbusIP_ESP8266.h>"))
        self.assertIn("inverter.type() == IPv4", text)
        self.assertIn("sample.at = requestAt", text)
        grid = body(CONTROL, "void grid(")
        self.assertIn("value.generation != data.command.generation", grid)
        self.assertIn("value.outputEpoch != data.status.outputEpoch", grid)
        self.assertIn("data.status.transitioning", grid)

    def test_settings_loaded_before_task_and_not_reloaded(self):
        setup = body(SKETCH, "void setup()")
        self.assertLess(setup.index("loadSettings()"), setup.index("Controller::start()"))
        self.assertNotIn("loadSettings()", body(SKETCH, "void loop()"))
        prior = (ROOT.parent / "grzalki-pv-sigenergy_v5" / "settings.h").read_text(
            encoding="utf-8")
        self.assertEqual(code(SETTINGS).strip(), code(prior).strip())

    def test_production_logic_contains_regression_assertions(self):
        # Presence check only: static_asserts have NOT been executed.
        self.assertEqual(code(LOGIC).count("static_assert("), 31)
        for name in ("cancelRejectedOn", "timedOut", "sameCommand"):
            self.assertIn(f"Heating::{name}(", code(SKETCH + CONTROL))


if __name__ == "__main__":
    unittest.main(verbosity=2)
