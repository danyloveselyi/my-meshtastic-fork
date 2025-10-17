<div align="center" markdown="1">
<div align="center" style="background:#ffe;border:1px solid #ccc;padding:10px;margin-bottom:20px">
<b>This is a personal fork of Meshtastic firmware.</b><br>
<b>Changes:</b> Added memory monitoring module (<code>MemoryMonitorModule</code>) for tracking RAM/Flash usage, logging extreme values, and warning about low memory.<br>
<b>Purpose:</b> This module helps developers and users monitor device stability, detect memory leaks, and optimize firmware performance.
</div>

<img src=".github/meshtastic_logo.png" alt="Meshtastic Logo" width="80"/>
<h1>Meshtastic Firmware</h1>

![GitHub release downloads](https://img.shields.io/github/downloads/meshtastic/firmware/total)
[![CI](https://img.shields.io/github/actions/workflow/status/meshtastic/firmware/main_matrix.yml?branch=master&label=actions&logo=github&color=yellow)](https://github.com/meshtastic/firmware/actions/workflows/ci.yml)
[![CLA assistant](https://cla-assistant.io/readme/badge/meshtastic/firmware)](https://cla-assistant.io/meshtastic/firmware)
[![Fiscal Contributors](https://opencollective.com/meshtastic/tiers/badge.svg?label=Fiscal%20Contributors&color=deeppink)](https://opencollective.com/meshtastic/)
[![Vercel](https://img.shields.io/static/v1?label=Powered%20by&message=Vercel&style=flat&logo=vercel&color=000000)](https://vercel.com?utm_source=meshtastic&utm_campaign=oss)

<a href="https://trendshift.io/repositories/5524" target="_blank"><img src="https://trendshift.io/api/badge/repositories/5524" alt="meshtastic%2Ffirmware | Trendshift" style="width: 250px; height: 55px;" width="250" height="55"/></a>

</div>

</div>

<div align="center">
	<a href="https://meshtastic.org">Website</a>
	-
	<a href="https://meshtastic.org/docs/">Documentation</a>
</div>

## Overview

This repository contains the official device firmware for Meshtastic, an open-source LoRa mesh networking project designed for long-range, low-power communication without relying on internet or cellular infrastructure. The firmware supports various hardware platforms, including ESP32, nRF52, RP2040/RP2350, and Linux-based devices.

Meshtastic enables text messaging, location sharing, and telemetry over a decentralized mesh network, making it ideal for outdoor adventures, emergency preparedness, and remote operations.

### Get Started


Join our community and help improve Meshtastic! 🚀

## Stats

![Alt](https://repobeats.axiom.co/api/embed/8025e56c482ec63541593cc5bd322c19d5c0bdcf.svg "Repobeats analytics image")

## How to Build the Firmware

**Development Environment:**
- Uses [PlatformIO](https://platformio.org/) (cross-platform build system for embedded development).
- Main configuration is in the `platformio.ini` file.

**Build Steps:**
1. Install PlatformIO (via VSCode extension or with `pip install platformio`).
2. In the project root, run:
	```bash
	pio run -e <board_name>
	```
	For example, for RAK4631:
	```bash
	pio run -e rak4631_eth_gw
	```

3. To flash the device, use:
	```bash
	pio run -e rak4631_eth_gw -t upload
	```

**Note:**
- The build is only supported for the following environments:
  - `rak4631_eth_gw`
  - `rak4631`
Other boards or environments are not guaranteed to work with this fork.

**Additional Info:**
- The project uses extra configs for different platforms and device variants (`arch/*/*.ini`, `variants/*/platformio.ini`).
- Scripts for code analysis and testing are available in the `bin/` folder.

---
