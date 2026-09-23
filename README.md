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
