"""v12 source contracts, no compiler/hardware.

These checks prevent structural regressions; they do NOT prove C++ compilation,
FreeRTOS timing, GPIO behaviour, or library integration on a real ESP32.
Run: python3 grzalki-pv-sigenergy_v12/verify_source.py
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent
SKETCH = (ROOT / "grzalki-pv-sigenergy_v12.ino").read_text(encoding="utf-8")
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
    def test_network_owner_has_no_direct_hardware_access(self):
        text = code(SKETCH)
        for token in ("digitalWrite(", "gpio_set_level(", "Wire.", "sensors."):
            self.assertNotIn(token, text)
        self.assertEqual(text.count("Controller::initializePins()"), 1)

    def test_engine_has_no_network_or_supla_calls(self):
        engine = body(CONTROL, "class Engine")
        for token in ("SuplaDevice", "Supla::", "WiFi.", "mb.", "web."):
            self.assertNotIn(token, engine)
        self.assertIn("temperatureTask();", body(CONTROL, "void step()"))
        self.assertIn("engine.step();", body(CONTROL, "inline void taskEntry"))

    def test_manual_is_independent_of_dac_and_grid(self):
        allowed = body(CONTROL, "bool allowed(")
        self.assertIn("Heating::relayPermitted(", allowed)
        self.assertNotIn("dacFault", allowed)
        self.assertNotIn("gridFresh", allowed.split("c.manual ||", 1)[0])
        self.assertIn("c.manual || Heating::autoPermitted(", allowed)
        step = body(CONTROL, "void step()")
        self.assertIn("!temperatureOk(now) || hot || !cfg.safetyValid", step)
        self.assertNotIn("dacFault ||", step)
        self.assertNotIn("!cfg.valid", step)

    def test_first_manual_press_is_not_gated_by_request_allowed(self):
        demand = body(CONTROL, "bool demand(")
        self.assertNotIn("requestAllowed", demand)
        turn_on = body(SKETCH, "void turnOn(")
        self.assertNotIn("requestAllowed", turn_on)
        self.assertIn("!snapshot.status.sensorFault", turn_on)
        self.assertIn("!snapshot.status.hot", turn_on)
        self.assertIn("!snapshot.status.taskFailed", turn_on)
        self.assertIn("cfg.safetyValid", turn_on)

    def test_dac_is_written_only_while_ssr_is_driven(self):
        step = body(CONTROL, "void step()")
        self.assertIn("ssrWatts > 0 && !dacFault", step)
        apply = body(CONTROL, "void apply(")
        self.assertIn("nextSsr != ssrWatts && !dacFault", apply)
        stop = body(CONTROL, "void stop()")
        self.assertIn("dacZeroPending", stop)
        self.assertIn("writeSsr(0)", stop)

    def test_unconfirmed_zero_is_retried_without_stopping_relays(self):
        step = body(CONTROL, "void step()")
        self.assertIn("ssrWatts == 0 && dacZeroPending", step)
        self.assertIn("writeSsr(0)", step)
        self.assertLess(step.index("ssrWatts == 0 && dacZeroPending"),
                        step.index("temperatureTask();"))

    def test_manual_uses_short_dwell_not_auto_dwell(self):
        step = body(CONTROL, "void step()")
        self.assertIn("const uint32_t dwell = input.command.manual", step)
        self.assertRegex(step, r"\? MANUAL_DWELL_MS : uint32_t\(cfg\.dwell\) \* 1000")
        self.assertRegex(CONTROL, r"MANUAL_DWELL_MS\s*=\s*1000")
        self.assertIn("Heating::manualTarget(", step)

    def test_mode_duration_cannot_expire_auto(self):
        set_mode = body(CONTROL, "void setMode(")
        self.assertIn("data.command.modeStarted = manual ? started : 0", set_mode)
        self.assertIn("data.command.modeDuration = manual ? duration : 0", set_mode)
        step = body(CONTROL, "void step()")
        self.assertIn("c.manual && !c.modeExpired", step)

    def test_mode_sync_clears_stale_mailbox_manual_flag(self):
        mode = body(SKETCH, "class ModeChannel")
        self.assertIn("Controller::mailbox.setMode(false, 0, 0)", mode)
        sync = body(SKETCH, "void syncChannels()")
        self.assertLess(sync.index("modeChannel->sync()"), sync.index("channel2->sync()"))

    def test_modbus_uses_transaction_id_and_timeout(self):
        reply = body(SKETCH, "bool modbusResponse(")
        self.assertIn("transactionId != pendingTx", reply)
        self.assertIn("pendingKind = ReqKind::None", reply)
        poll = body(SKETCH, "void pollModbus()")
        self.assertIn("handleModbusTimeout()", poll)
        self.assertRegex(SKETCH, r"MODBUS_TIMEOUT_MS\s*=\s*2000")

    def test_plant_map_reads_status_and_power_in_one_block(self):
        request = body(SKETCH, "bool startRequest(")
        self.assertIn("PLANT_GRID_START", request)
        self.assertIn("PLANT_GRID_COUNT", request)
        self.assertRegex(SKETCH, r"PLANT_GRID_START\s*=\s*30004")
        self.assertRegex(SKETCH, r"PLANT_GRID_COUNT\s*=\s*6")
        parse = body(SKETCH, "void parseGrid()")
        self.assertIn("const bool plant = requestMap == 0", parse)
        self.assertIn("plant ? &gridRegs[1] : &gridRegs[0]", parse)
        self.assertIn("sig.rawStatus = plant ? gridRegs[0] : 0", parse)
        self.assertIn("sig.rawOnGrid = plant ? gridRegs[5] : 0", parse)

    def test_ct_and_ongrid_checks_are_explicit(self):
        parse = body(SKETCH, "void parseGrid()")
        self.assertIn("cfg.requireCt || sig.ctOk", parse)
        self.assertIn("cfg.requireOnGrid || sig.onGrid", parse)
        self.assertIn("sig.ctOk = !plant || sig.rawStatus == 1", parse)
        self.assertIn("sig.onGrid = !plant || sig.rawOnGrid == 0", parse)
        sync = body(SKETCH, "void syncChannels()")
        self.assertIn("cfg.map == 2 && activeMap == 0", sync)

    def test_auto_map_probe_and_latch(self):
        self.assertRegex(SETTINGS, r"\bmap\s*=\s*2\b")
        self.assertIn('"sig_map"', SETTINGS)
        self.assertIn("cfg.map == 2", SETTINGS)  # autoValid accepts auto
        start = body(SKETCH, "bool startRequest(")
        self.assertIn("autoMapLatched ? activeMap : probeMap", start)
        self.assertIn("legacyReg = cfg.map == 1 ? uint16_t(cfg.reg) : 5004", start)
        self.assertIn("legacyInput = cfg.map == 1 ? cfg.input != 0 : false", start)
        parse = body(SKETCH, "void parseGrid()")
        self.assertIn("latchAutoMap(plausible)", parse)
        self.assertIn("gridProbeFailed()", parse)
        self.assertIn("void gridProbeFailed()", code(SKETCH))
        self.assertIn("probeMap = requestMap == 0 ? 1 : 0", body(SKETCH, "void gridProbeFailed()"))
        self.assertIn("autoMapLatched = true", body(SKETCH, "void latchAutoMap("))
        poll = body(SKETCH, "void pollModbus()")
        self.assertIn("cfg.map == 2 && autoMapLatched && activeMap == 0", poll)

    def test_grid_value_stays_on_screen_when_sample_invalidated(self):
        publish = body(CONTROL, "void publish(")
        # Publishing NAN made channel 5 blank on every MANUAL switch.
        self.assertIn("s.gridWatts = gridWatts;", publish)
        self.assertNotIn("s.gridFresh ? gridWatts : NAN", publish)
        self.assertIn("s.gridFresh = gridFresh(millis())", publish)

    def test_documented_sigenergy_defaults_and_v6_v7_map(self):
        self.assertRegex(SETTINGS, r"\bmap\s*=\s*2\b")
        self.assertRegex(SETTINGS, r"\breg\s*=\s*30005\b")
        self.assertRegex(SETTINGS, r"\binput\s*=\s*1\b")
        self.assertRegex(SETTINGS, r"\bunit\s*=\s*247\b")
        self.assertRegex(SETTINGS, r"\bport\s*=\s*502\b")
        self.assertRegex(SETTINGS, r"\bscale\s*=\s*1000\b")
        self.assertIn('"sig_map"', SETTINGS)
        self.assertIn('"sig_grid_addr"', SETTINGS)
        self.assertIn('"sig_grid_input"', SETTINGS)
        request = body(SKETCH, "bool startRequest(")
        self.assertIn("mb.readHreg(inverter", request)
        self.assertIn("mb.readIreg(inverter", request)
        self.assertIn("legacyInput", request)

    def test_signed_words_and_word_auto_correction(self):
        combine = body(SKETCH, "int32_t combineWords(")
        self.assertIn("(uint32_t(hi) << 16) | lo", combine)
        self.assertIn("(uint32_t(lo) << 16) | hi", combine)
        watts = body(SKETCH, "float wattsFromWords(")
        self.assertIn("cfg.wordAuto", watts)
        self.assertIn("swapped = true", watts)
        self.assertIn("plausibleWatts", watts)

    def test_safety_and_auto_validity_are_separate(self):
        self.assertIn("cfg.safetyValid = Heating::validPair(cfg.limit, cfg.reset)",
                      SETTINGS)
        self.assertNotIn("cfg.maximum > 0", body(SETTINGS, "void loadSettings()").split(
            "cfg.autoValid =", 1)[0])
        self.assertIn("cfg.autoValid = cfg.fixed > 0 && cfg.maximum > 0", SETTINGS)
        self.assertIn("cfg.valid = cfg.safetyValid && cfg.autoValid", SETTINGS)

    def test_engine_owns_gpio_and_rechecks_after_i2c(self):
        init = body(CONTROL, "inline void initializePins")
        self.assertLess(init.index("pinMode(pin, INPUT)"),
                        init.index("digitalWrite(pin, RELAY_OFF)"))
        self.assertLess(init.index("digitalWrite(pin, RELAY_OFF)"),
                        init.index("pinMode(pin, OUTPUT)"))
        apply = body(CONTROL, "void apply(")
        self.assertLess(apply.index("writeSsr(nextSsr)"),
                        apply.index("Mailbox::Guard guard"))
        for token in ("Heating::sameCommand", "allowed(c, now)", "Heating::timedOut"):
            self.assertGreater(apply.index(token), apply.index("writeSsr(nextSsr)"))
        gpio = body(CONTROL, "void gpio(")
        self.assertEqual(gpio.count("gpio_set_level("), 2)

    def test_fault_revokes_and_timers_expire_without_supla(self):
        step = body(CONTROL, "void step()")
        self.assertIn("if (fault && !wasFault) mailbox.revokeLocked()", step)
        self.assertIn("Heating::timedOut(now, d.started, d.duration)", step)
        self.assertIn("++d.revision", step)
        self.assertIn("c.modeExpired = true", step)
        revoke = body(CONTROL, "void revokeLocked()")
        for token in ("++data.command.generation", "d.on = false",
                      "d.duration = 0", "data.grid.valid = false"):
            self.assertIn(token, revoke)

    def test_stop_retries_zero_and_never_clears_fault_latch(self):
        stop = body(CONTROL, "void stop()")
        self.assertLess(stop.index("gpio(false, false)"), stop.index("writeSsr(0)"))
        self.assertIn("dacZeroPending", stop)
        self.assertIn("Heating::elapsed(millis(), lastDacWrite, 1000)", stop)
        write = body(CONTROL, "bool writeSsr")
        self.assertIn("dacZeroPending = !ok || watts > 0", write)
        self.assertNotIn("dacFault = false", write)
        self.assertNotIn("dacFault = false", stop)
        self.assertIn("dacZeroPending = dacFault", body(CONTROL, "void begin()"))

    def test_reset_reason_diagnostics(self):
        text = code(SKETCH)
        for token in ("esp_reset_reason()", "ESP_RST_BROWNOUT",
                      "ESP_RST_TASK_WDT", "ESP_RST_PANIC", "BROWNOUT"):
            self.assertIn(token, text)
        self.assertIn("printResetDiagnostics();", body(SKETCH, "void setup()"))

    def test_modbus_poll_does_not_use_removed_grid_helper(self):
        text = code(SKETCH)
        self.assertNotIn("pollGrid()", text)
        self.assertNotIn("requestAllowed", code(SKETCH).split("void demand", 1)[0])

    def test_gp8403_protocol_constants_and_encoding(self):
        for name, value in (("ADDRESS", "0x58"), ("RANGE_REG", "0x01"),
                            ("RANGE_10V", "0x11"), ("CHANNEL0_REG", "0x02"),
                            ("MAX_CODE", "0x0FFF"), ("DATA_SHIFT", "4")):
            self.assertRegex(code(DAC), rf"\b{name}\s*=\s*{value}\s*;")
        self.assertIn("Gp8403::wordForFraction(watts / cfg.maximum)",
                      body(CONTROL, "bool writeSsr"))
        self.assertIn("{Gp8403::low8(word), Gp8403::high8(word)}",
                      body(CONTROL, "bool writeSsr"))
        self.assertIn("(code > MAX_CODE ? MAX_CODE : code) << DATA_SHIFT",
                      body(DAC, "constexpr uint16_t encodeCode"))

    def test_static_assertions_present(self):
        self.assertGreaterEqual(code(LOGIC).count("static_assert("), 40)
        self.assertGreaterEqual(code(DAC).count("static_assert("), 10)

    def test_channels_preserved_and_diagnostics_added(self):
        setup = body(SKETCH, "void setup()")
        self.assertLess(setup.index("new TankChannel"), setup.index("channel2 = new"))
        self.assertIn("ctAlarm = new", setup)
        self.assertIn("pvPower = new", setup)
        self.assertIn("essPower = new", setup)
        self.assertIn("socValue = new", setup)
        # Channel numbers are creation order: 9=PV, 10=ESS, 11=SOC, 12=CT.
        self.assertLess(setup.index("pvPower = new"), setup.index("essPower = new"))
        self.assertLess(setup.index("essPower = new"), setup.index("socValue = new"))
        self.assertLess(setup.index("socValue = new"), setup.index("ctAlarm = new"))
        sync = body(SKETCH, "void syncChannels()")
        self.assertIn("ctAlarm", sync)
        self.assertIn("pvPower->setValue(sig.pvW)", sync)
        self.assertIn("essPower->setValue(sig.essW)", sync)
        self.assertIn("socValue->setValue(sig.soc)", sync)

    def test_settings_loaded_once_before_engine_start(self):
        setup = body(SKETCH, "void setup()")
        self.assertLess(setup.index("loadSettings()"), setup.index("Controller::start()"))
        self.assertNotIn("loadSettings()", body(SKETCH, "void loop()"))
        self.assertIn("cfg.valid = cfg.valid && ipOk", setup)


if __name__ == "__main__":
    unittest.main(verbosity=2)
