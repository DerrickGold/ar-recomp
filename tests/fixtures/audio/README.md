# Synthetic music fixture

`stream-probe.ogg` is a generated 80 ms, 440 Hz sine wave, not game audio.
It exercises compressed-cache ownership, playback, looping and reload with the
real Vorbis decoder. No encoder is needed when running the tests.

Regenerate with:

```sh
ffmpeg -f lavfi -i 'sine=frequency=440:sample_rate=44100:duration=0.08' \
  -ac 2 -c:a libvorbis -q:a 2 stream-probe.ogg
```
