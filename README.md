# Audio To MIDI

A JUCE audio plug-in (AU / VST3 / Standalone) that converts incoming audio to MIDI notes, using [Cycfi Q](https://github.com/cycfi/q) for pitch detection.

## Building

```bash
git clone --recurse-submodules <this-repo-url>
cd midi
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

If you cloned without `--recurse-submodules`, run `git submodule update --init --recursive` first.

## Tests

```bash
ctest --test-dir build --output-on-failure
```

`PitchDetectorTests` checks the pitch detector against synthetic guitar tones; `ProcessorTests` renders audio through the plug-in and checks its MIDI output. Both run on every push and pull request via GitHub Actions.
