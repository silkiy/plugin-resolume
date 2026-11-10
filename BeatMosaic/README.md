# BeatMosaic for Resolume

BeatMosaic is a Windows x64 FFGL effect for Resolume Arena and Avenue. It applies short visual accents to the clip, layer, or composition you are already playing. Choose audio from the laptop, a sound card, Resolume's FFT controls, the Resolume tempo, or a manual trigger.

![BeatMosaic showing a synthetic clip before and after a 4 × 4 Shuffle Hit](../docs/images/beat-mosaic.png)

## Requirements and installation

- A Windows 10 or later x64 desktop, an OpenGL 4.1-capable graphics driver, and Resolume Arena or Avenue 7.3.1 or later. [FFGL SDK compatibility](https://github.com/resolume/ffgl)
- A video clip or generated source to process. To work with music alone, first put any visual source on the layer you want to animate.

1. [Build the DLL](#build-from-source), then close Resolume.
2. Copy `dist\BeatMosaic.dll` to `Documents\Resolume Arena\Extra Effects` or `Documents\Resolume Avenue\Extra Effects`. Use the **Documents** location shown by File Explorer if Windows redirects it to OneDrive.
3. Reopen Resolume. Search for **BeatMosaic** in **Effects** and drag it onto the active visual clip, its layer, or the composition.
4. In the effect controls, select the audio input and pattern. Set **Input** to **Manual** and press **Hit** for a quick check of the visual effect. [FFGL directories](https://resolume.com/support/en/preferences) · [Resolume effects](https://resolume.com/support/en/effects)

For a custom plugin location, add the folder containing the DLL under **Preferences → Video → FFGL Directories**, then restart Resolume.

## Use YouTube or other laptop audio

1. Choose the intended speakers, headphones, or USB playback device in **Windows Settings → System → Sound → Output**.
2. Play the music in your browser or another application. In Windows Volume Mixer, route that application to the same default output.
3. Set BeatMosaic's **Input** to **Windows Output**. Start with **Pattern** set to **Slice** and **Rate** set to **Every Beat**.
4. Adjust **Sensitivity** until kick or bass accents are picked up consistently. Adjust **Amount** for visual strength and **Decay** for the length of the accent.

Windows Output analyzes the shared audio mix from the default Windows playback device via WASAPI loopback. This includes other audible applications on the same device. [Microsoft WASAPI loopback](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)

## Use a sound card, line-in, or mixer feed

1. Connect the feed and select that recording device in **Windows Settings → System → Sound → Input**.
2. Confirm that the Windows input meter moves when music plays.
3. Set BeatMosaic's **Input** to **Windows Input**. It reads the default Windows recording device, so the selected device needs to expose a Windows recording endpoint. [Microsoft audio capture](https://learn.microsoft.com/en-us/windows/win32/coreaudio/capturing-a-stream)

### Use the audio input configured in Resolume

1. Open **Preferences → Audio**. Select the input device and the channels for **External Audio FFT**.
2. Set BeatMosaic's **Input** to **Resolume FFT**.
3. Open the parameter menu next to **Audio Level** and select **External FFT**. In the analysis controls, choose **L** for low frequencies. Adjust **Gain** until **Audio Level** visibly follows the music, then set BeatMosaic's **Sensitivity**. For audio playing in Resolume itself, **Composition FFT** can drive the same parameter. [Resolume audio preferences](https://resolume.com/support/en/preferences) · [Resolume audio analysis](https://resolume.com/support/en/parameter-animation#audio-analysis)

The Windows input modes detect transients in the bass range. **Resolume FFT** reacts to changes in the band selected for **Audio Level**. In these modes, **Rate** counts detected accents, so soft attacks and sparse bass can produce a different rhythm from the track's beat grid. Use **Resolume BPM** to follow the host's beat grid and align it with **Tap** and **Resync**. [Resolume BPM](https://resolume.com/support/en/bpm)

## Effect controls

| Control | Default | Behavior |
| --- | --- | --- |
| **Input** | **Windows Output** | Selects **Windows Output**, **Windows Input**, **Resolume FFT**, **Resolume BPM**, or **Manual**. |
| **Pattern** | **Slice** | Chooses the visual response described below. |
| **Grid** | **4 x 4** | Uses four or eight slices per frame in **Slice**, or a square grid in **Wave** and **Shuffle**. |
| **Rate** | **Every Beat** | Responds to every detected accent or every second or fourth accent. In **Resolume BPM** mode, the interval uses host beats. |
| **Amount** | `65%` | Sets the base effect strength. At `0%`, the original image passes through. |
| **Brightness** | `100%` | Multiplies the effect strength for every pattern. Use values above `100%` for a stronger Slice, Wave, Shuffle, or Strobe accent. Range `0–400%`. |
| **Sensitivity** | `65%` | Increases or decreases responsiveness to audio accents. |
| **Decay** | `180` ms | Sets how long an accent takes to fade back toward the original frame. Range `80–600` ms. |
| **Audio Level** | `0%` | Receives the FFT level in **Resolume FFT** mode. |
| **Hit** | Button | Triggers one accent. Works in any input mode, including **Manual**. |

| Pattern | Output |
| --- | --- |
| **Slice** | Alternating horizontal strips shift in opposite directions and slide back as the accent decays. |
| **Wave** | Tiles move with a staggered wave across the grid. |
| **Shuffle** | The tiles are rearranged on a trigger and blend back toward the original image. |
| **Strobe** | A brief lift in the brightness of the existing pixels, with the original transparency preserved. Brightness controls the white flash intensity; at high values it can reach full white. Pulses last at most 140 ms and start at least 350 ms apart. |

To accent a drop manually, select **Manual**, choose **Slice** or **Shuffle**, and press **Hit** on cue. The effect returns to the original visual after the accent. BeatMosaic analyzes audio locally while it renders and keeps only level measurements for the visual response.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| BeatMosaic is missing in Resolume | Look in **Effects**. Check the x64 DLL's installation folder and restart Resolume. |
| **Hit** works but YouTube does not move the effect | In Windows Volume Mixer, check that the browser uses the default output selected in Windows Sound. Set **Input** to **Windows Output**. |
| The external sound card does not trigger the effect | Check the Windows input meter and the default recording device for **Windows Input**. For an input selected inside Resolume, use **Resolume FFT** and verify **Audio Level** moves. |
| The effect triggers too often or misses kicks | Use **Every Beat** while tuning. Lower **Sensitivity** for extra triggers or raise it for missed accents. For **Resolume FFT**, start with the low-frequency band and check **Gain** and **Fall**. |
| The visual looks unchanged | Trigger the visual clip, enable the effect, and raise **Amount** and the effect's opacity. **Manual → Hit** gives a test pulse without a music feed. |
| The playback device was switched | Set the desired Windows default again. BeatMosaic checks for endpoint changes and reconnects. |

## Build from source

Install Visual Studio 2022 with **Desktop development with C++**, the Windows SDK, and CMake 3.21 or later. From the repository root, run the following in PowerShell.

```powershell
.\BeatMosaic\build.ps1
```

From the `BeatMosaic` directory, run `.\build.ps1`. The script downloads the pinned FFGL SDK when its cache is empty, verifies the archive checksum, and writes `dist\BeatMosaic.dll`. Keep the notice alongside the DLL when distributing it. [Build script](build.ps1) · [Third-party notices](THIRD-PARTY-NOTICES.txt)

## Verification

Standalone FFGL/OpenGL tests verified the four patterns, exact passthrough on padded textures, alpha handling, manual triggers and decay, Strobe limits, and simulated FFT and host beat input. A separate WASAPI test measured real audio from the default Windows playback device. The recording endpoint was detected by Windows. Live validation of the sound card, External FFT mapping, and beat timing in Resolume is pending.
