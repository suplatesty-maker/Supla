"""v7 source contracts and GP8403 packet reference model, no compiler/hardware.

These checks prevent structural regressions; they do NOT prove C++ compilation,
FreeRTOS timing, GPIO behaviour, or library integration on a real ESP32.
Run: py -3 <absolute path to this file>
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent
SKETCH = (ROOT / "grzalki-pv-sigenergy_v7.ino").read_text(encoding="utf-8")
CONTROL = (ROOT / "controller.h").read_text(encoding="utf-8")
LOGIC = (ROOT / "control_logic.h").read_text(encoding="utf-8")
SETTINGS = (ROOT / "settings.h").read_text(encoding="utf-8")
DAC = (ROOT / "gp8403.h").read_text(encoding="utf-8")


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
        prior = (ROOT.parent / "grzalki-pv-sigenergy_v6" / "settings.h").read_text(
            encoding="utf-8")
        self.assertEqual(code(SETTINGS).strip(), code(prior).strip())

    def test_production_logic_contains_regression_assertions(self):
        # Presence check only: static_asserts have NOT been executed.
        self.assertEqual(code(LOGIC).count("static_assert("), 31)
        for name in ("cancelRejectedOn", "timedOut", "sameCommand"):
            self.assertIn(f"Heating::{name}(", code(SKETCH + CONTROL))

    def test_gp8403_protocol_constants(self):
        for name, value in (("ADDRESS", "0x58"), ("RANGE_REG", "0x01"),
                            ("RANGE_10V", "0x11"), ("CHANNEL0_REG", "0x02"),
                            ("MAX_CODE", "0x0FFF"), ("DATA_SHIFT", "4")):
            self.assertRegex(code(DAC), rf"\b{name}\s*=\s*{value}\s*;")
        begin = body(CONTROL, "void begin()")
        self.assertIn("zero[] = {0, 0, 0, 0}", begin)
        self.assertIn("range = Gp8403::RANGE_10V", begin)
        self.assertIn("dacWrite(Gp8403::CHANNEL0_REG, zero, sizeof(zero))", begin)
        self.assertIn("dacWrite(Gp8403::RANGE_REG, &range, 1)", begin)
        self.assertLess(begin.index("dacWrite(Gp8403::CHANNEL0_REG"),
                        begin.index("dacWrite(Gp8403::RANGE_REG"))

    def test_gp8403_production_encoding_path(self):
        write = body(CONTROL, "bool writeSsr")
        self.assertIn("Gp8403::wordForFraction(watts / cfg.maximum)", write)
        self.assertIn("{Gp8403::low8(word), Gp8403::high8(word)}", write)
        self.assertIn("dacWrite(Gp8403::CHANNEL0_REG, bytes, sizeof(bytes))", write)
        self.assertIn("dacFault = true", write)
        transport = body(CONTROL, "bool dacWrite")
        self.assertIn("Wire.beginTransmission(Gp8403::ADDRESS)", transport)
        self.assertIn("Wire.endTransmission() == 0 && queued", transport)
        self.assertNotIn("32767", code(CONTROL + DAC))
        self.assertIn("(code > MAX_CODE ? MAX_CODE : code) << DATA_SHIFT",
                      body(DAC, "constexpr uint16_t encodeCode"))
        self.assertIn("uint16_t(fraction * MAX_CODE)",
                      body(DAC, "constexpr uint16_t wordForFraction"))
        self.assertIn("uint8_t(word)", body(DAC, "constexpr uint8_t low8"))
        self.assertIn("uint8_t(word >> 8)", body(DAC, "constexpr uint8_t high8"))
        self.assertEqual(code(DAC).count("static_assert("), 10)

    def test_gp8403_reference_packets(self):
        # Independent Python model of DFRobot's 12-bit packing, NOT C++ execution.
        # Extract production constants so a wrong bit width/shift fails vectors.
        def constant(name):
            value = re.search(rf"\b{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+);", DAC)
            return int(value.group(1), 0)

        maximum, shift = constant("MAX_CODE"), constant("DATA_SHIFT")
        previous = -1
        for raw in range(maximum + 1):
            word = raw << shift
            self.assertEqual(word & 0xF, 0)
            self.assertGreater(word, previous)
            self.assertLessEqual(word, 0xFFF0)
            previous = word
        for fraction, expected in ((-1, (0, 0)), (0, (0, 0)),
                                   (0.25, (0xF0, 0x3F)), (0.5, (0xF0, 0x7F)),
                                   (0.75, (0xF0, 0xBF)), (1, (0xF0, 0xFF)),
                                   (2, (0xF0, 0xFF))):
            with self.subTest(fraction=fraction):
                word = int(max(0, min(1, fraction)) * maximum) << shift
                self.assertEqual((word & 0xFF, word >> 8), expected)

    def test_control_logic_unchanged_from_v6(self):
        prior = (ROOT.parent / "grzalki-pv-sigenergy_v6" / "control_logic.h").read_text(
            encoding="utf-8")
        self.assertEqual(code(LOGIC).strip(), code(prior).strip())


if __name__ == "__main__":
    unittest.main(verbosity=2)
