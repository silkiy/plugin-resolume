# KineticTypography for Resolume

KineticTypography is a custom Windows x64 FFGL source for live VJ typography. It
renders text from a configuration file and animates it in real time with four
purpose-built modes:

* **Typewriter** reveals the text from left to right.
* **Wave** bends the baseline into a moving wave.
* **Bounce** adds a beat-friendly vertical bounce and scale pulse.
* **Scatter** applies a kinetic displacement field.

The plugin is self-contained and does not require a web service, media server, or
external control application. Windows Output loopback is used only for beat accents;
the **Trigger** event is available for deterministic MIDI/OSC/Resolume control.

## Configure text

Create:

```text
%USERPROFILE%\Documents\Resolume\KineticTypography\text.txt
```

Save UTF-8 text in the file. Multiple lines are supported and are rendered as a
centered text block. Example:

```text
MOVE
WITH THE MUSIC
```

Press **Reload** after changing the file. If the file is missing, a safe default
message is used. The plugin never reads files from the render thread.

## Controls

| Control | Behavior |
| --- | --- |
| **Mode** | Typewriter, Wave, Bounce, or Scatter |
| **Trigger** | Restarts the animation phase immediately |
| **Reload** | Reloads `text.txt` |
| **Speed** | Animation speed, `0.05–2.0` |
| **Amount** | Displacement strength, `0–100%` |
| **Font Size** | 24–220 px |
| **Hue** | Continuous HSV color selection |

The audio detector uses the default Windows playback device through WASAPI loopback.
It supports common 16-bit PCM and 32-bit float formats, reconnects when the device
is unavailable, and detects rising peaks rather than triggering on every sample.
Audio is never uploaded or persisted.

## Build and install

Install Visual Studio 2022 with **Desktop development with C++**, the Windows SDK,
and CMake 3.21 or later. From the repository root:

```powershell
.\KineticTypography\build.ps1
```

Copy `KineticTypography\dist\KineticTypography.dll` to:

```text
Documents\Resolume Arena\Extra Effects
```

or the equivalent Avenue folder, then restart Resolume. The plugin appears under
**Sources**. Keep `THIRD-PARTY-NOTICES.txt` next to the DLL when distributing it.
