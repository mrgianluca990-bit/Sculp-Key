# Salamander Grand Piano — credits and changes

Sculpt Key contains a modified subset of **Salamander Grand Piano v3**,
recorded by **Alexander Holm** on a Yamaha C5. The upstream SFZ remapping
is by **kinwie**; the optional retuned mapping is credited upstream to
**Markus Fiedler**. Sculpt Key uses the original recordings without the
optional retuning and loads its own sampler mapping, not the upstream SFZ.

Source: https://github.com/sfzinstruments/SalamanderGrandPiano
Pinned source revision: 3382bf9496bba2486f5ab0de55a264d1dfc38404
Original archive: https://archive.org/details/SalamanderGrandPianoV3
License: **Creative Commons Attribution 3.0 Unported (CC BY 3.0)**
https://creativecommons.org/licenses/by/3.0/

The complete license is supplied as `LICENSE.txt`. Original README, SFZ
and mapping data are preserved under `upstream/` for provenance.

## Changes made for Sculpt Key

- Selected 90 stereo recordings: 30 roots from A0 to C8, at original
  velocity layers 4, 8 and 12. Source dynamic levels and stereo balance
  are preserved; individual samples/layers are not normalized.
- Removed only conservatively detected initial low-level preroll, keeping
  2 ms before the significant hammer onset. The exact trim for each sample
  is recorded in `Samples/manifest.json` and `Samples/source-info.json`.
  The more aggressive optional upstream SFZ attack offsets are not used.
- Converted 48 kHz/24-bit FLAC to 44.1 kHz/16-bit stereo WAV with rational
  polyphase resampling. No pitch correction or time stretching was applied.
- Limited long tails to 10 seconds in the bass, 8 in the middle, 6 in the
  treble, retaining naturally shorter recordings. A 300 ms half-cosine
  fade closes each tail.
- No separate release, hammer, pedal or sympathetic-resonance recordings
  are included in this 90-file subset.
- There is **one recorded take per root and velocity layer**. Automatic
  repetition variation in Sculpt Key is modeled and is not presented as
  additional independently recorded round-robin takes.

`tools/import_factory_samples.py` reproduces this conversion from the
pinned files, checks their Git blob hashes and records input/output SHA256.
The source FLAC cache is not part of the distributed Sculpt Key bank.

The source authors do not endorse Sculpt Key. The sample license applies
to these recordings and derivatives; it does not set the license of the
Sculpt Key application or JUCE code.
