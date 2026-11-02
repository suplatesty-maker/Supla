"""Kontrakty zrodel i model podzialu energii. Bez kompilatora i sprzetu.

Te testy lapia regresje strukturalne. NIE dowodza poprawnosci kompilacji,
timingu FreeRTOS, dzialania GPIO, PZEM, Modbusa ani bezpieczenstwa 230 V.

Uruchomienie: python3 verify_source.py
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent
SKETCH = (ROOT / "pompa-ciepla-sigenergy.ino").read_text(encoding="utf-8")
CONTROL = (ROOT / "pump_control.h").read_text(encoding="utf-8")
MIX = (ROOT / "source_mix.h").read_text(encoding="utf-8")
SETTINGS = (ROOT / "settings.h").read_text(encoding="utf-8")
METER = (ROOT / "pump_meter.h").read_text(encoding="utf-8")
CONFIG = (ROOT / "config.h").read_text(encoding="utf-8")


def code(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def body(text, signature):
    text = code(text)
    start = text.index("{", text.index(signature))
    depth = 1
    for end in range(start + 1, len(text)):
        depth += (text[end] == "{") - (text[end] == "}")
        if depth == 0:
            return text[start + 1:end]
    raise AssertionError(f"Unclosed body: {signature}")


def split(pump_w, grid_w):
    """Python twin of SourceMix::split."""
    auto = min(pump_w, max(-grid_w, 0.0))
    return pump_w - auto, auto


def sale_wanted(pv_w, grid_total_w, pv_max_w, on_w):
    export = max(-grid_total_w, 0.0)
    return pv_w <= pv_max_w and export >= on_w


class AllocationModel(unittest.TestCase):
    def test_user_example(self):
        # Pompa 1000 W, eksport 400 W -> 400 W auto, 600 W z sieci.
        self.assertEqual(split(1000, -400), (600, 400))

    def test_known_points(self):
        self.assertEqual(split(1000, 0), (1000, 0))
        self.assertEqual(split(1000, 800), (1000, 0))
        self.assertEqual(split(1000, -500), (500, 500))
        self.assertEqual(split(1000, -2500), (0, 1000))
        self.assertEqual(split(0, -500), (0, 0))

    def test_conservation(self):
        for pump in (0, 1, 10, 1000, 3500):
            for grid in (-5000, -1000, -1, 0, 1, 1000, 5000):
                g, a = split(pump, grid)
                self.assertAlmostEqual(g + a, pump)
                self.assertGreaterEqual(g, 0)
                self.assertGreaterEqual(a, 0)

    def test_cpp_asserts_match_model(self):
        points = [(1000, -400), (1500, -500), (1000, 0), (1000, 800),
                  (1000, -2500), (0, -500)]
        for pump, grid in points:
            g, a = split(pump, grid)
            self.assertIn("static_assert", MIX)
            self.assertIn(f"split({pump}.0f, {grid}.0f)", MIX)
            self.assertAlmostEqual(g + a, pump)

    def test_sale_needs_no_pv_and_export(self):
        self.assertTrue(sale_wanted(0, -1000, 100, 1000))
        self.assertFalse(sale_wanted(2000, -2000, 100, 1000))
        self.assertFalse(sale_wanted(0, -500, 100, 1000))
        self.assertFalse(sale_wanted(0, 1000, 100, 1000))


class SourceContracts(unittest.TestCase):
    def test_sketch_does_not_write_relay_directly(self):
        sketch = code(SKETCH)
        self.assertNotIn("digitalWrite", sketch)
        self.assertNotIn("pinMode", sketch)
        # Only the relay task touches the relay GPIO.
        self.assertIn("digitalWrite(Config::PUMP_RELAY_PIN", code(CONTROL))

    def test_relay_task_has_no_network_or_supla(self):
        control = code(CONTROL)
        for forbidden in ("Supla", "WiFi", "Modbus", "Serial", "mb."):
            self.assertNotIn(forbidden, control)

    def test_block_and_release_are_both_immediate(self):
        # Pompa ma wlasna ochrone sprezarki, wiec przekaznik idzie za zadaniem
        # bez opoznienia w obie strony.
        task = body(CONTROL, "inline void taskEntry")
        self.assertIn("desired != allowed", task)
        self.assertIn("digitalWrite(Config::PUMP_RELAY_PIN", task)
        # Zadnego czekania na ponowny start w firmware.
        for gone in ("minSwitch", "firstDecision", "lastChange"):
            self.assertNotIn(gone, code(CONTROL))

    def test_relay_fail_safe_and_nc_option(self):
        init = body(CONTROL, "inline void initializePin")
        self.assertLess(init.index("pinMode(Config::PUMP_RELAY_PIN, INPUT)"),
                        init.index("digitalWrite"))
        self.assertLess(init.index("digitalWrite"),
                        init.index("pinMode(Config::PUMP_RELAY_PIN, OUTPUT)"))
        self.assertIn("PUMP_RELAY_NC", CONFIG)
        self.assertIn("failSafeAllowed", CONTROL)

    def test_phase_is_the_default_source(self):
        # Domyslnie czytamy faze CT z pompa, nie sume calej instalacji.
        self.assertRegex(SETTINGS, r"gridPhase\s*=\s*1")
        self.assertRegex(SETTINGS, r"blockCount\s*=\s*54")
        self.assertIn('"sig_grid_phase"', SETTINGS)
        # Wariant "CT widzi pompe" zostal usuniety - jest jedna logika.
        self.assertNotIn("ctIncludesPump", code(SKETCH) + code(SETTINGS))

    def test_fixed_sigenergy_block_start(self):
        self.assertIn("SIG_BLOCK_START = 30004", SETTINGS)
        self.assertNotIn("sig_block_start", SETTINGS)
        self.assertNotIn("cfg.blockStart", code(SKETCH))
        self.assertIn("SIG_BLOCK_START", code(SKETCH))

    def test_sigenergy_register_offsets(self):
        sketch = code(SKETCH)
        self.assertIn("modbusRegs[0]", sketch)   # 30004 status CT
        self.assertIn("modbusRegs[1]", sketch)   # 30005 moc sieci (suma)
        self.assertIn("modbusRegs[5]", sketch)   # 30009 on/off-grid
        self.assertIn("modbusRegs[10]", sketch)  # 30014 SOC
        self.assertIn("modbusRegs[31]", sketch)  # 30035 PV
        self.assertIn("modbusRegs[33]", sketch)  # 30037 ESS
        self.assertIn("readIreg", sketch)
        self.assertIn("cfg.unit", sketch)

    def test_per_phase_registers(self):
        # 30052/30054/30056 -> offsety 48/50/52 od 30004.
        self.assertIn("48 + 2 * (phase - 1)", code(SKETCH))
        parse = body(SKETCH, "void parseSigenergy")
        self.assertIn("phaseRegisterOffset", parse)
        self.assertIn("cfg.gridPhase", parse)
        self.assertIn("cfg.blockCount >= 54", parse)
        self.assertIn("cfg.gridPhase == 0 || cfg.blockCount >= 54", SETTINGS)

    def test_energy_split_uses_pzem_delta_and_freshness(self):
        process = body(SKETCH, "void processPzem")
        for token in ("lastEnergyKwh", "delta", "pzemReset", "allocatable",
                      "autoRatio", "addKwh(autoEnergy", "addKwh(gridEnergy",
                      "addKwh(unknownEnergy", "SIG_STALE_MS", "MAX_ALLOC_AGE_MS",
                      "sequence()"):
            self.assertIn(token, process)

    def test_pzem_requires_real_ac_supply(self):
        read = body(METER, "void readValuesFromDevice")
        self.assertIn("voltage < 80.0f", read)
        self.assertIn("frequency < 40.0f", read)
        self.assertIn("resetReadParameters()", read)

    def test_no_volatile_as_synchronization(self):
        self.assertNotRegex(code(CONTROL + SKETCH), r"\bvolatile\b")
        self.assertIn("portENTER_CRITICAL", CONTROL)
        self.assertIn("portEXIT_CRITICAL", CONTROL)

    def test_no_restart_delay_left_in_firmware(self):
        # Ochrona sprezarki jest w sterowniku pompy, nie w ESP32.
        for text in (CONFIG, SETTINGS, code(SKETCH)):
            self.assertNotIn("min_switch", text)
            self.assertNotIn("minSwitch", text)
            self.assertNotIn("DEFAULT_MIN_SWITCH", text)

    def test_channel_order_contract(self):
        setup = body(SKETCH, "void setup()")
        order = [
            "gridEnergy = new",
            "autoEnergy = new",
            "unknownEnergy = new",
            "pzemMeter = new",
            "blockSlider = new",
            "saleSwitch = new",
            "saleInfo = new",
            "pumpStatus = new",
            "sigGrid = new",
            "sigPv = new",
            "sigEss = new",
            "sigSoc = new",
            "pzemAlarm = new",
            "sigAlarm = new",
            "configAlarm = new",
            "pumpAlarm = new",
        ]
        positions = [setup.index(token) for token in order]
        self.assertEqual(positions, sorted(positions))

    def test_sale_block_feeds_relay_task(self):
        update = body(SKETCH, "void updateControl")
        self.assertIn("saleSwitch->isOn()", update)
        self.assertIn("autoSale", update)
        self.assertIn("control.saleBlock = saleBlock", update)
        self.assertIn("control.manualBlock = manualBlock", update)
        self.assertIn("setBinary(saleInfo, saleBlock)", update)

    def test_auto_sale_uses_pure_logic_and_plant_total(self):
        update = body(SKETCH, "void updateControl")
        self.assertIn("SourceMix::saleWanted", update)
        self.assertIn("SourceMix::saleKeep", update)
        selfIn = "sig.gridTotalW"
        self.assertIn(selfIn, update)
        # Sprzedaz nie moze byc liczona z mocy jednej fazy.
        self.assertNotIn("sig.gridW < 0.0f", update)

    def test_sale_thresholds_are_configurable(self):
        self.assertIn('"sale_on_w"', SETTINGS)
        self.assertIn('"sale_pv_max_w"', SETTINGS)
        self.assertIn('"sale_off_w"', SETTINGS)
        self.assertRegex(SETTINGS, r"saleOnW\s*=\s*1000")
        self.assertIn("cfg.salePvMaxW <= cfg.saleOnW", SETTINGS)

    def test_pump_min_w_and_persistence(self):
        self.assertIn("pump_min_w", SETTINGS)
        self.assertIn("setKeepStateInStorage(true)", SKETCH)
        self.assertIn("MAX_ALLOC_STEP_KWH", code(SKETCH))

    def test_modbus_rejects_late_transactions(self):
        response = body(SKETCH, "bool modbusResponse")
        self.assertIn("transactionId != modbusTransaction", response)
        self.assertIn("Config::SIG_TIMEOUT_MS", response)
        parse = body(SKETCH, "void parseSigenergy")
        self.assertIn("sample.at = modbusRequestAt", parse)

    def test_split_reset_is_group_wide(self):
        group = body(SKETCH, "int handleCalcfgFromServer")
        self.assertIn("SUPLA_CALCFG_CMD_RESET_COUNTERS", group)
        self.assertIn("meter->setCounter(0)", group)
        self.assertIn("SUPERUSER", group.upper())
        setup = body(SKETCH, "void setup()")
        self.assertEqual(setup.count("setGroup("), 3)
        self.assertIn("setDefaultStateRestore()", setup)


if __name__ == "__main__":
    unittest.main(verbosity=2)
