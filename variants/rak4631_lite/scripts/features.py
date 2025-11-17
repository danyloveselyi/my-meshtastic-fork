# Helper script to toggle optional features for the RAK4631 Lite variant.
# Runs as a PlatformIO extra_script and converts simple boolean options (feature_*)
# into the corresponding compiler flags, source filters, and library selections.

from SCons.Script import Import, DefaultEnvironment  # type: ignore

env = Import("env")
if env is None or not hasattr(env, "GetProjectOption"):
    try:
        env = Import("projenv")
    except Exception:
        env = None

if env is None or not hasattr(env, "GetProjectOption"):
    env = DefaultEnvironment()

if env is None or not hasattr(env, "GetProjectOption"):
    # Nothing to do if we can't access the active construction environment (can happen in
    # inspection or non-build phases). Silently exit so we don't break other tasks.
    raise SystemExit


def _as_bool(option_name: str, default: bool = False) -> bool:
    candidates = []
    if not option_name.startswith("custom_"):
        candidates.append(f"custom_{option_name}")
    candidates.append(option_name)
    for name in candidates:
        value = env.GetProjectOption(name)
        if value is None:
            continue
        return str(value).strip().lower() in ("1", "true", "yes", "on")
    return default


def _flatten(value):
    if value is None:
        return []
    return env.Flatten(value)


def _normalize_entries(entries):
    normalized = []
    for entry in entries or []:
        current = entry
        if not isinstance(current, str):
            current = env.subst(current)
        current = current.strip()
        if current:
            normalized.append(current)
    return normalized


def _parse_define(entry):
    if isinstance(entry, tuple):
        name, val = entry
        return name, val
    if isinstance(entry, str):
        if "=" in entry:
            name, val = entry.split("=", 1)
            try:
                val = int(val)
            except ValueError:
                pass
            return name, val
        return entry, None
    return entry, None


def _set_define(name: str, value):
    if isinstance(value, bool):
        value = int(value)
    defines = env.get("CPPDEFINES", [])
    if not isinstance(defines, list):
        defines = list(defines)
    updated = False
    new_defines = []
    for entry in defines:
        key, _ = _parse_define(entry)
        if key == name:
            if value is None:
                updated = True
                continue
            new_defines.append((name, value))
            updated = True
        else:
            new_defines.append(entry)
    if not updated and value is not None:
        new_defines.append((name, value))
    env.Replace(CPPDEFINES=new_defines)


def _get_list(varname: str):
    value = env.get(varname)
    if value is None:
        option_name = varname.lower()
        try:
            value = env.GetProjectOption(option_name)
        except Exception:
            value = None
        if value is None:
            return []
    flattened = _flatten([value])
    result = []
    for item in flattened:
        if isinstance(item, str) and varname == "SRC_FILTER":
            result.extend(item.split())
        else:
            result.append(item)
    return _normalize_entries(result)


def _update_list(varname: str, adds=None, removes=None):
    adds = _normalize_entries(adds or [])
    removes = _normalize_entries(removes or [])
    if not adds and not removes:
        return
    current = _get_list(varname)
    changed = False
    for entry in removes:
        while entry in current:
            current.remove(entry)
            changed = True
    for entry in adds:
        if entry not in current:
            current.append(entry)
            changed = True
    if changed:
        env.Replace(**{varname: current})
        option_name = varname.lower()
        try:
            section = f"env:{env['PIOENV']}"
            config = env.GetProjectConfig()
            if config.has_option(section, option_name):
                config.set(section, option_name, current)
        except Exception:
            pass


FEATURES = {
    "admin_module": {
        "description": "Remote admin commands (PIN-protected config changes)",
        "default": True,
        "defines": {
            "on": {"MESHTASTIC_EXCLUDE_ADMIN": 0},
            "off": {"MESHTASTIC_EXCLUDE_ADMIN": 1},
        },
    },
    "node_info": {
        "description": "Node info announcements for phone apps",
        "default": True,
        "defines": {
            "on": {"MESHTASTIC_EXCLUDE_NODEINFO": 0},
            "off": {"MESHTASTIC_EXCLUDE_NODEINFO": 1},
        },
    },
    "neighbor_info": {
        "description": "Advertise neighbor tables for better routing",
        "default": True,
        "defines": {
            "on": {"MESHTASTIC_EXCLUDE_NEIGHBORINFO": 0},
            "off": {"MESHTASTIC_EXCLUDE_NEIGHBORINFO": 1},
        },
        "src_filter": {
            "off": ["-<modules/NeighborInfoModule.cpp>"],
        },
    },
    "range_test": {
        "description": "LoRa range test helper; also needed to expose monitoring commands",
        "default": False,
        "defines": {
            "on": {"MESHTASTIC_EXCLUDE_RANGETEST": 0},
            "off": {"MESHTASTIC_EXCLUDE_RANGETEST": 1},
        },
        "src_filter": {
            "on": ["+<modules/RangeTestModule.cpp>"],
        },
    },
    "monitoring": {
        "description": "DeviceStatsModule with dynamic max nodes",
        "default": False,
        "requires": ["range_test"],
        "defines": {
            "on": {
                "MESHTASTIC_EXCLUDE_DEVICESTATS": 0,
                "FEATURE_MONITORING": 1,
            },
            "off": {
                "MESHTASTIC_EXCLUDE_DEVICESTATS": 1,
                "FEATURE_MONITORING": 0,
            },
        },
        "src_filter": {
            "on": ["+<modules/DeviceStatsModule.cpp>"],
        },
    },
    "device_telemetry": {
        "description": "Periodic device telemetry (temp, uptime, voltage)",
        "default": True,
        "defines": {
            "on": {"MESHTASTIC_EXCLUDE_DEVICE_TELEMETRY": 0},
            "off": {"MESHTASTIC_EXCLUDE_DEVICE_TELEMETRY": 1},
        },
        "src_filter": {
            "off": ["-<modules/Telemetry/DeviceTelemetry.cpp>"],
        },
    },
    "power_telemetry": {
        "description": "INA/INA32xx/MAX17048 power telemetry reporting",
        "default": True,
        "defines": {
            "on": {"MESHTASTIC_EXCLUDE_POWER_TELEMETRY": 0},
            "off": {"MESHTASTIC_EXCLUDE_POWER_TELEMETRY": 1},
        },
        "src_filter": {
            "off": ["-<modules/Telemetry/PowerTelemetry.cpp>"],
        },
    },
    "store_forward": {
        "description": "Store-and-forward mesh relay",
        "default": False,
        "defines": {
            "on": {"MESHTASTIC_EXCLUDE_STOREFORWARD": 0},
            "off": {"MESHTASTIC_EXCLUDE_STOREFORWARD": 1},
        },
        "src_filter": {
            "on": ["+<modules/StoreForwardModule.cpp>"],
        },
    },
    "remote_hardware": {
        "description": "Remote GPIO control module",
        "default": False,
        "defines": {
            "on": {"MESHTASTIC_EXCLUDE_REMOTEHARDWARE": 0},
            "off": {"MESHTASTIC_EXCLUDE_REMOTEHARDWARE": 1},
        },
        "src_filter": {
            "on": ["+<modules/RemoteHardwareModule.cpp>"],
        },
    },
    "canned_messages": {
        "description": "Predefined canned messages UI",
        "default": False,
        "defines": {
            "on": {"MESHTASTIC_EXCLUDE_CANNEDMESSAGES": 0},
            "off": {"MESHTASTIC_EXCLUDE_CANNEDMESSAGES": 1},
        },
        "src_filter": {
            "on": ["+<modules/CannedMessageModule.cpp>"],
        },
    },
    "reply_module": {
        "description": "Legacy auto-reply module",
        "default": False,
        "defines": {
            "on": {"MESHTASTIC_EXCLUDE_REPLY": 0},
            "off": {"MESHTASTIC_EXCLUDE_REPLY": 1},
        },
        "src_filter": {
            "on": ["+<modules/ReplyModule.cpp>"],
        },
    },
    "radiolib_ax25": {
        "description": "Enable AX.25 framing support in RadioLib",
        "default": False,
        "defines": {
            "on": {"RADIOLIB_EXCLUDE_AX25": 0},
            "off": {"RADIOLIB_EXCLUDE_AX25": 1},
        },
    },
    "radiolib_afsk": {
        "description": "Enable AFSK modem support in RadioLib",
        "default": False,
        "defines": {
            "on": {"RADIOLIB_EXCLUDE_AFSK": 0},
            "off": {"RADIOLIB_EXCLUDE_AFSK": 1},
        },
    },
    "radiolib_aprs": {
        "description": "Enable APRS helpers (requires AX.25 + AFSK)",
        "default": False,
        "requires": ["radiolib_ax25", "radiolib_afsk"],
        "defines": {
            "on": {"RADIOLIB_EXCLUDE_APRS": 0},
            "off": {"RADIOLIB_EXCLUDE_APRS": 1},
        },
    },
    "radiolib_bell": {
        "description": "Enable Bell modem emulation",
        "default": False,
        "defines": {
            "on": {"RADIOLIB_EXCLUDE_BELL": 0},
            "off": {"RADIOLIB_EXCLUDE_BELL": 1},
        },
    },
    "radiolib_morse": {
        "description": "Enable Morse encoder/decoder",
        "default": False,
        "defines": {
            "on": {"RADIOLIB_EXCLUDE_MORSE": 0},
            "off": {"RADIOLIB_EXCLUDE_MORSE": 1},
        },
    },
    "radiolib_rtty": {
        "description": "Enable RTTY modem",
        "default": False,
        "defines": {
            "on": {"RADIOLIB_EXCLUDE_RTTY": 0},
            "off": {"RADIOLIB_EXCLUDE_RTTY": 1},
        },
    },
    "radiolib_sstv": {
        "description": "Enable SSTV encoder",
        "default": False,
        "defines": {
            "on": {"RADIOLIB_EXCLUDE_SSTV": 0},
            "off": {"RADIOLIB_EXCLUDE_SSTV": 1},
        },
    },
    "rgb_led": {
        "description": "Enable onboard RGB status LED (NCP5623)",
        "default": True,
        "defines": {
            "on": {"FEATURE_RGB_LED_DISABLED": 0},
            "off": {"FEATURE_RGB_LED_DISABLED": 1},
        },
        "lib_deps": {
            "on": ["rakwireless/RAKwireless NCP5623 RGB LED library@^1.0.2"],
        },
        "lib_ignore": {
            "remove_on": [
                "RAKwireless NCP5623 RGB LED library",
                "rakwireless/RAKwireless NCP5623 RGB LED library@^1.0.2",
            ],
        },
    },
}


def _resolve_states():
    states = {}
    for feature, info in FEATURES.items():
        states[feature] = _as_bool(f"feature_{feature}", info.get("default", False))

    changed = True
    while changed:
        changed = False
        for feature, info in FEATURES.items():
            if not states[feature]:
                continue
            for dependency in info.get("requires", []):
                if not states.get(dependency, False):
                    states[dependency] = True
                    print(f"[features] Enabling '{dependency}' because '{feature}' requires it")
                    changed = True

    for feature, info in FEATURES.items():
        conflicts = info.get("conflicts", [])
        if not conflicts:
            continue
        for conflict in conflicts:
            if states.get(feature) and states.get(conflict):
                raise ValueError(f"Feature '{feature}' conflicts with '{conflict}'. Update platformio.ini to resolve")

    return states


def _apply_feature(feature: str, enabled: bool):
    info = FEATURES[feature]

    defines = info.get("defines", {})
    for name, value in defines.get("on" if enabled else "off", {}).items():
        _set_define(name, value)

    src_cfg = info.get("src_filter", {})
    if src_cfg:
        if enabled:
            _update_list("SRC_FILTER", adds=src_cfg.get("on"), removes=src_cfg.get("off"))
        else:
            _update_list("SRC_FILTER", adds=src_cfg.get("off"), removes=src_cfg.get("on"))

    lib_deps = info.get("lib_deps", {})
    if lib_deps:
        if enabled:
            _update_list("LIB_DEPS", adds=lib_deps.get("on"), removes=lib_deps.get("off"))
        else:
            _update_list("LIB_DEPS", adds=lib_deps.get("off"), removes=lib_deps.get("on"))

    lib_ignore = info.get("lib_ignore", {})
    if lib_ignore:
        if enabled:
            _update_list("LIB_IGNORE", removes=lib_ignore.get("remove_on"), adds=lib_ignore.get("add_on"))
        else:
            _update_list("LIB_IGNORE", adds=lib_ignore.get("remove_on"), removes=lib_ignore.get("add_on"))


feature_states = _resolve_states()
for feature_name, active in feature_states.items():
    _apply_feature(feature_name, active)

print("[features] RAK4631 lite feature summary:")
for feature_name, data in FEATURES.items():
    status = "ON " if feature_states.get(feature_name) else "off"
    print(f"  - {feature_name:<16} : {status} :: {data['description']}")

print(f"[features] LIB_DEPS after processing: {env.get('LIB_DEPS')}")
print(f"[features] LIB_IGNORE after processing: {env.get('LIB_IGNORE')}")
try:
    print(f"[features] Project lib_deps option: {env.GetProjectOption('lib_deps')}")
except Exception:
    print("[features] Project lib_deps option unavailable")
