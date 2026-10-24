import importlib.util
from pathlib import Path
import unittest
import sys

sys.path.insert(0, str(Path(__file__).parents[1]))

spec = importlib.util.spec_from_file_location("provision", Path(__file__).parents[1] / "provision.py")
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)


class ProvisionTests(unittest.TestCase):
    def config(self):
        return {"ssid": "Home WiFi", "wifi_password": "test-wifi-password", "mqtt_uri": "mqtt://192.168.1.10:1883",
                "mqtt_user": "gas_reader", "mqtt_password": "test-mqtt-password", "ota_password": "test-ota-password"}

    def test_valid(self):
        config = self.config()
        self.assertEqual(provision.validate(config), config)

    def test_placeholders_not_provisioned(self):
        config = self.config(); config["ssid"] = "CHANGE_ME_WIFI"
        with self.assertRaises(ValueError): provision.validate(config)

    def test_encoded_byte_limits(self):
        config = self.config(); config["ssid"] = "ő" * 17
        with self.assertRaises(ValueError): provision.validate(config)

    def test_invalid_uri(self):
        for uri in ("https://broker", "mqtt://user:password@broker", "mqtt://broker/topic", "mqtt://broker:99999"):
            config = self.config(); config["mqtt_uri"] = uri
            with self.subTest(uri=uri), self.assertRaises(ValueError): provision.validate(config)

    def test_short_password_and_control_character(self):
        for key, value in (("ota_password", "short"), ("wifi_password", "short"), ("mqtt_password", "secret\nline")):
            config = self.config(); config[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): provision.validate(config)


    def test_fallback_optional_or_empty(self):
        config = self.config() | {"fallback_ssid": "", "fallback_wifi_password": ""}
        self.assertEqual(provision.validate(config), config)

    def test_fallback_valid_with_short_nvs_keys(self):
        config = self.config() | {"fallback_ssid": "Backup WiFi", "fallback_wifi_password": "backup-test-password"}
        self.assertEqual(provision.validate(config), config)
        self.assertTrue(all(len(provision.NVS_KEYS.get(key, key)) <= 15 for key in config))

    def test_fallback_pair_required(self):
        for fields in ({"fallback_ssid": "Backup"}, {"fallback_wifi_password": "backup-test-password"},
                       {"fallback_ssid": "Backup", "fallback_wifi_password": ""}):
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                provision.validate(self.config() | fields)

    def test_fallback_limits_and_password(self):
        valid = self.config() | {"fallback_ssid": "Backup", "fallback_wifi_password": "backup-test-password"}
        for key, value in (("fallback_ssid", "ő" * 17), ("fallback_ssid", "CHANGE_ME_BACKUP"),
                           ("fallback_wifi_password", "short"), ("fallback_wifi_password", "g" * 64),
                           ("fallback_wifi_password", "password\n"), ("fallback_wifi_password", None)):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                provision.validate(valid | {key: value})
        self.assertEqual(provision.validate(valid | {"fallback_wifi_password": "a" * 64})["fallback_wifi_password"], "a" * 64)

    def test_unknown_key_rejected(self):
        with self.assertRaises(ValueError):
            provision.validate(self.config() | {"fallback_password": "typo-password"})


if __name__ == "__main__": unittest.main()
