# KineticTypography

A Windows x64 FFGL **source** that animates UTF-8 text in Resolume Arena or Avenue. Four manually selectable modes support accents from the default Windows playback device.

## Install and try it

From the repository root, with Resolume closed:

```powershell
.\KineticTypography\build.ps1
```

Copy `KineticTypography\dist\KineticTypography.dll` into your Windows Documents folder under `Resolume Arena\Extra Effects` or `Resolume Avenue\Extra Effects`. Create the destination folder if needed. Use the Documents location shown by Windows, including OneDrive redirection. Keep third-party notices with distributed DLLs.

Open Resolume, find **KineticTypography** in **Sources**, drag it to an empty clip slot, and trigger the clip. For a quick check, select **Wave**. No music is required to see the text or animation.

Resolume may show the source name as **KineticTypograph** because FFGL limits the source name length. Search for `Kinetic`. Text proportions are preserved in landscape and portrait output.

Close Resolume before replacing an installed DLL, then restart it to load the new build.

## Change the text

Open **Documents** in File Explorer. Edit this file as UTF-8:

```text
Resolume\KineticTypography\text.txt
```

The plugin follows the actual Windows Documents location, including OneDrive redirection. Print the full path with:

```powershell
Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Resolume\KineticTypography\text.txt'
```

Create the configuration folder if needed, then create `text.txt` or copy `KineticTypography/examples/text.txt` into it. Preserve existing text when updating the plugin. After saving changes, press **Reload** in the clip panel. Newlines create multiple centered lines. Short text works best; reduce **Font Size** for longer text. Missing or empty files show `Kinetic Typography`.

For compatibility, the plugin uses `%USERPROFILE%\Documents\Resolume\KineticTypography\text.txt` if the file in the actual Documents folder is absent. You can copy that older file into the current location manually.

## Controls

| Control | Behavior |
| --- | --- |
| Mode | Typewriter, Wave, Bounce, or Scatter; always manually selectable. |
| Trigger | Restart the current animation. |
| Speed | Animation cycles per second, from 0.05 to 2.0. |
| Amount | Distortion strength for Wave, Bounce, and Scatter. |
| Font Size | Text size on the internal 2048 × 512 canvas, from 24 to 220 pixels. |
| Hue | Text color. |
| Reload | Read `text.txt` again and restart the animation. |

Typewriter reveals once and holds the completed text. An audio accent can restart it after completion and a one-second hold, so repeated peaks cannot keep hiding the text. Other modes animate continuously, with a minimum 200 ms between audio restarts.

Audio uses WASAPI loopback on the **default Windows playback device**. Play music through that device to add accents. The source does not capture an external recording input directly. Trigger remains available without audio.

## Troubleshooting

- **Missing plugin:** check Sources, the DLL installation path, and Preferences → Video → FFGL Directories. Use the x64 DLL and restart Resolume.
- **Blank output:** trigger the clip, set clip/layer/composition opacity above zero, select Wave, and press Reload.
- **Old text:** print the path above, save UTF-8, and press Reload. The current Documents file takes priority over the legacy file.
- **No audio accents:** confirm music is playing through the Windows default playback device. Animation and Trigger do not depend on audio.

For portable configuration, set `RESOLUME_PLUGIN_DATA_DIR` before launching Resolume. The file is then `<data root>\KineticTypography\text.txt`. Restart Resolume after changing that environment variable.

## Build and verification

Requires Windows x64, Visual Studio 2022 with Desktop development with C++, the Windows SDK, CMake 3.21+, and OpenGL 4.1. The build script fetches and verifies the pinned Resolume FFGL SDK when not cached.

```powershell
.\KineticTypography\build.ps1
```

To verify the installed plugin, trigger its source clip in Resolume, check all four modes, and press Trigger. In Typewriter mode, wait for the reveal to finish and confirm the text stays visible. Edit `text.txt`, press Reload, and confirm the new text appears. Check Font Size, Hue, and both landscape and portrait compositions for readable, upright text.
