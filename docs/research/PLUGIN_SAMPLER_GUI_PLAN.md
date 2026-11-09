# AU/VST instruments in the Convert Sound tab: integration plan (2026-11-09)

Status: **IMPLEMENTED 2026-11-10** as the **Sample Plugin** tab (`apps/macos/Source/PluginSamplerComponent.*`,
`src/ui/sound/plugin_sampler_model.h`). The gaps listed below are closed:
- **Output guard.** `outputFolderRefusal` in `package_writer.h` is shared by WAV conversion, the CLI and the GUI.
- **Progress.** The sampler prints `progress<TAB>i<TAB>N` before each note.
- **Licence.** A note on the tab says the user must hold a licence that permits sampling.

Two things go beyond the plan:
- **Staging.** The render lands in a private staging folder and is previewed with the WAV engine before
  Generate copies it.
- **Memory.** The memory estimate and cap were measured (`estimateRender`).

## Decision: run the existing sampler as a child process

The Convert Sound tab will run `aiarranger-plugin-sampler` (`apps/macos/Tools/plugin_sampler.cpp`)
with `juce::ChildProcess`. It will not host plugins inside the app.

- **One engine.** Rendering, looping, velocity layers and package writing stay in the tool that is
  already tested. The GUI only builds the argument list and reads the result.
- **Isolation.** A third-party plugin that crashes, hangs or leaks takes down only the child
  process. The app reports the exit code and stderr, and can cancel with `ChildProcess::kill`.
- **No audio or MIDI devices.** The sampler renders offline through `processBlock`. It opens no
  CoreMIDI port and no audio device, so the "no MIDI transmission" rule holds.

## Workflow in the tab

1. **List instruments.** Run `aiarranger-plugin-sampler list` on a worker thread and show the
   installed instruments in a combo box.
2. **Set the render parameters.** Each field maps 1:1 to an existing flag:
   - program, `--low`/`--high`/`--step`
   - velocities (`--vel`) and splits (`--splits`)
   - hold and release
   - loop and stereo
3. **Choose the destination.** It must be a new local folder. Apply the same `/Volumes/` and
   existing-folder refusal as Generate, before launching the child process.
4. **Render.** Read progress from stdout. A Cancel button kills the child process.
5. **On success,** open the package in Resource Graph, as Generate already does. The package's
   `wav/` folder follows the `<layer>_<Org>_<Low>_<High>.wav` naming, so it can also be loaded
   back into Convert Sound to preview the map and checks.

## Gaps to close first

- **Output guard.** The sampler refuses an existing folder (`package_writer.h`) but has no
  `/Volumes/` guard of its own. Move the guard from `SoundConversionModel::generate` into a shared
  helper that both use.
- **Progress.** The sampler prints only at the end. Add one line per rendered note so the GUI can
  show progress.
- **Licence.** The user must hold a licence that permits sampled use of the plugin. The GUI shows
  this before rendering; it does not check licences, and it never bypasses one.

## Tests to add with the implementation

- The argument list built from the GUI fields: a pure function with a unit test.
- Refusals for `/Volumes` and an existing destination, before any process starts.
- A fake child process (a script that exits non-zero, or one that sleeps and is then killed) to
  check error reporting and cancel without a real plugin.
