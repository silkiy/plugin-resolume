# LyricsDisplay

A Windows x64 FFGL **source** that displays synchronized LRC lyrics on a transparent background. It reads a local timed lyric file, with optional LRCLIB lookup by artist and title. It does not transcribe audio or recognize the song being played.

## Install and try it

From the repository root, with Resolume closed:

```powershell
.\LyricsDisplay\build.ps1
```

Copy `LyricsDisplay\dist\LyricsDisplay.dll` into your Windows Documents folder under `Resolume Arena\Extra Effects` or `Resolume Avenue\Extra Effects`. Create the destination folder if needed. Use the Documents location shown by Windows, including OneDrive redirection. Close Resolume before replacing an installed DLL, then restart it. Keep third-party notices with distributed DLLs.

Create the configuration folder below if needed, then copy `LyricsDisplay/examples/lyrics.lrc` into it without replacing any lyrics you want to keep. Open Resolume, drag **LyricsDisplay** from **Sources** into an empty clip slot, and trigger it. Choose **Local LRC**, press **Reload**, then **Start**. Allow a moment for loading. The sample shows its first line immediately and changes every three seconds. It clears after twelve seconds; press Start to replay it. Audio is not required for this test.

## Configuration files

Open **Documents** in File Explorer. The configuration folder is:

```text
Resolume\LyricsDisplay
```

The plugin follows the actual Windows Documents location, including OneDrive redirection. Print the full path with:

```powershell
Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Resolume\LyricsDisplay'
```

| File | Purpose |
| --- | --- |
| `lyrics.lrc` | Local UTF-8 timed lyrics. |
| `track.txt` | Optional UTF-8 file with artist on line 1 and title on line 2 for online lookup. |

The current Documents file takes priority. If absent, the plugin also checks `%USERPROFILE%\Documents\Resolume\LyricsDisplay`. You can copy older configuration files into the current location manually.

For portable configuration, set `RESOLUME_PLUGIN_DATA_DIR` before launching Resolume. Files then live in `<data root>\LyricsDisplay`. Restart Resolume after changing that environment variable.

## Play your own lyrics

1. Replace `lyrics.lrc` with timed lyrics for your song and save as UTF-8.
2. Choose **Local LRC** or **Auto**, then press **Reload**. Loading runs in a background worker. Allow a moment for it to finish.
3. Press **Start** when the song begins. Start resets the lyric clock to zero each time.
4. Adjust **Offset ms**. Positive values show later lyrics sooner; negative values delay them.
5. Press **Stop** to stop the clock and clear the output. Audio will not override Stop. Press Start to play again.

On a newly loaded plugin instance, an audible peak from the **default Windows playback device** can start the clock once. For precise synchronization, use Start manually. The plugin does not follow seeking, pausing, or song changes in YouTube or another player. Restart manually after changing playback position. It does not capture external recording inputs directly.

## LRC format

```text
[00:00]First line
[00:03.5]Second line
[00:07.25]Third line
[00:11.125]Fourth line
[00:15.00]
```

Seconds-only timestamps and one, two, or three decimal places are supported. Multiple timestamps may share a line, such as `[00:02.00][00:12.00]Repeated line`. Blank timed lines clear output. Lines are sorted by timestamp; metadata tags are ignored. The `[offset:]` metadata tag is not applied; use **Offset ms** instead.

One lyric line is displayed at a time. It remains until the next cue, so add a blank final cue to clear it. Output is transparent before the first active cue, while stopped, or when no valid timed lyrics are loaded.

Text proportions are preserved when the output aspect ratio changes; unused canvas space remains transparent.

## Online lyrics

Create `track.txt` with the artist and title on separate lines. See `examples/track.example.txt`. Select **Online LRCLIB** and press Reload. Network access and a synchronized match are required. Plain lyrics without timestamps cannot be synchronized.

**Auto** uses valid local LRC first and only queries LRCLIB when local timed lyrics are unavailable. Reload respects the selected source. If online lookup fails or has no synchronized result, local lyrics are the fallback. To force online lookup, select Online LRCLIB. Background loading prevents older requests from overwriting a newer selection.

LRCLIB is an optional external service with its own availability and licensing. No API key is embedded. The plugin sends artist and title, and does not upload audio. Wait for loading before pressing Start. An active network request can briefly delay plugin removal while its timeout expires.

## Controls

| Control | Behavior |
| --- | --- |
| Source | Local LRC, Online LRCLIB, or Auto. |
| Start | Reset clock to zero and play. |
| Stop | Stop and clear; remains stopped despite audio. |
| Reload | Reload the selected source without blocking rendering. |
| Offset ms | Timing adjustment from −10000 to +10000 milliseconds. |
| Font Size | Size on the internal 2048 × 256 canvas, from 20 to 180 pixels. Reduce it if a long line is clipped. |
| Opacity | 0 is transparent; 1 is full text opacity. |

## Troubleshooting

- **Missing plugin:** look in Sources, check the scanned Extra Effects directory and x64 DLL, then restart Resolume.
- **Blank output:** trigger the clip, check clip/layer/composition opacity, select Local LRC, reload the sample, and press Start. Ensure plugin Opacity is above zero.
- **Wrong file:** print the actual configuration path above. OneDrive Documents can differ from `%USERPROFILE%\Documents`.
- **Incorrect timing:** restart from the song's beginning and adjust Offset ms. This plugin uses its own clock.
- **Online stays blank:** verify `track.txt`, connectivity, and availability of timed lyrics. Use Local LRC to test rendering independently.

## Build and verification

Requires Windows x64, Visual Studio 2022 with Desktop development with C++, the Windows SDK, CMake 3.21+, and OpenGL 4.1. The build script fetches and verifies the pinned Resolume FFGL SDK if needed.

```powershell
.\LyricsDisplay\build.ps1
```

To verify the installed plugin, load the supplied sample and press Start. Confirm that lines change every three seconds, Stop clears the output, and Start resets playback. Check positive and negative Offset ms values, Opacity, and Font Size. Edit the local LRC, press Reload, and verify that both Local LRC and Auto use the updated cues. Online LRCLIB lookup requires a separate network check with a valid artist and title.
