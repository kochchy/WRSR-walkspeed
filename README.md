# Walkspeed Plugin

[![Steam Workshop](https://img.shields.io/badge/Steam-Workshop-blue?logo=steam)](https://steamcommunity.com/sharedfiles/filedetails/?id=3795596452)

**Download from Steam Workshop:** [Walkspeed (ID: 3795596452)](https://steamcommunity.com/sharedfiles/filedetails/?id=3795596452)

A plugin for TesmioLoader (Workers & Resources: Soviet Republic) that allows you to configure the walking speed and travel times of citizens.

## Features
* **Walk Speed Scaling**: Globally scale citizen walking speeds (e.g., make them walk 50% faster or slower) while preserving the natural vanilla variance (0.9x to 1.1x).
* **Travel Timer Dampening**: Optionally dampen the internal state-timers to extend the maximum travel/waiting times, allowing citizens to undertake longer journeys without timing out.

## Installation
1. Ensure you have the TesmioLoader framework installed.
2. Download or compile the plugin, and copy `walkspeed.dll` and `walkspeed.ini` to your game directory:
   `Steam\steamapps\common\SovietRepublic\tesmioloader\build\plugins\`
3. Activate the plugin via the `tesmiolauncher.exe` interface.

## Configuration
Edit `walkspeed.ini` in your plugins folder to adjust the settings. The file contains detailed comments explaining how each multiplier works.
