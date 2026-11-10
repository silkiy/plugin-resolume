# LyricsDisplay for Resolume

LyricsDisplay is a Windows x64 FFGL source for live use that renders one synchronized
lyric line over Resolume. It supports local `.LRC` files and an optional online fallback
through LRCLIB, has bounded network timeouts, survives unavailable audio devices, and
keeps the render thread free of network and file I/O. Windows Output is used to detect
the start of playback; **Start** remains available for deterministic show control.

## Configuration

Create this directory:

```text
%USERPROFILE%\Documents\Resolume\LyricsDisplay
```

Add:

* `lyrics.lrc` - local synchronized lyrics.
* `track.txt` - optional two-line file: artist on line 1, title on line 2. It is used
  for the online LRCLIB lookup.

Example `track.txt`:

```text
Daft Punk
Get Lucky
```

Use **Source** to select **Local LRC**, **Online LRCLIB**, or **Auto**. Auto uses the
local file when it exists and otherwise queries LRCLIB. If the online request fails,
the local file remains the fallback. **Reload** performs the lookup again without
blocking rendering. Use **Start** to reset the lyric clock manually; the first audible
Windows Output peak also starts it automatically. **Stop** and **Offset ms** are
available for live synchronization.

The online service is optional and requires network access. Lyrics are returned by
LRCLIB according to its availability and licensing. No API key is embedded. LRCLIB
lookup uses the artist and title in `track.txt`; it does not guess the song from
audio, so provide that file when using an online mode.

## Build and installation

Install Visual Studio 2022 with Desktop development with C++, the Windows SDK, and
CMake 3.21 or later. Run:

```powershell
.\LyricsDisplay\build.ps1
```

Copy `LyricsDisplay\dist\LyricsDisplay.dll` to `Documents\Resolume Arena\Extra Effects`
or `Documents\Resolume Avenue\Extra Effects`, then restart Resolume. It appears under
**Sources**.

## LRC format

```text
[00:12.50]First lyric line
[00:17.20]Second lyric line
```

LRC timestamps are parsed to centisecond precision. The renderer displays one line and
keeps the source transparent when no line is active.
