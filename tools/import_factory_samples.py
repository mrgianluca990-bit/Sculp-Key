#!/usr/bin/env python3
"""Reproduce the Sculpt Key acoustic-piano bank from pinned Salamander sources.

Requires Python 3, NumPy, SciPy, ffmpeg and ffprobe. No extra sample pack is
needed at playback: the generated WAVs are the inputs bundled by CMake.
Run from any working directory, for example:
    python3 tools/import_factory_samples.py --cache ../key-research

Only sources with the exact Git blob checksums in factory_sample_sources.json
are accepted. Source files stay in --cache; the distributable bank goes to
Samples and its complete attribution goes to ThirdParty/Salamander.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import wave

import numpy as np
import scipy
from scipy.signal import resample_poly


PROJECT = Path(__file__).resolve().parents[1]
SOURCE_INDEX = Path(__file__).with_name("factory_sample_sources.json")
SOURCE_RATE = 48000
OUTPUT_RATE = 44100
SOURCE_LAYERS = (4, 8, 12)
FADE_OUT_SECONDS = 0.300
PREROLL_SECONDS = 0.002
ONSET_WINDOW_SECONDS = 0.001
MAX_TRIM_SECONDS = 0.015
NOTE_OFFSETS = {"C": 0, "D#": 3, "F#": 6, "A": 9}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def matches_source(data: bytes, item: dict) -> bool:
    git_blob = b"blob " + str(len(data)).encode("ascii") + b"\0" + data
    return (len(data) == item["size"]
            and hashlib.sha1(git_blob).hexdigest() == item["gitBlobSha1"])


def acquire(item: dict, destination: Path) -> Path:
    """Check cache, otherwise download atomically and verify before use."""
    if destination.is_file() and matches_source(destination.read_bytes(), item):
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    for attempt in range(3):
        try:
            request = urllib.request.Request(
                item["url"], headers={"User-Agent": "Sculpt-Key-sample-import/1"})
            with urllib.request.urlopen(request, timeout=90) as response:
                data = response.read()
            if not matches_source(data, item):
                raise RuntimeError("Source checksum mismatch: " + item["name"])
            temporary = destination.with_suffix(destination.suffix + ".partial")
            temporary.write_bytes(data)
            temporary.replace(destination)
            return destination
        except Exception:
            if attempt == 2:
                raise
            time.sleep(attempt + 1)
    raise RuntimeError("Unreachable download state")


def json_write(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8")


def midi_note(note: str) -> int:
    match = re.fullmatch(r"([A-G]#?)(\d)", note)
    if match is None:
        raise ValueError("Unexpected source note: " + note)
    return 12 * (int(match[2]) + 1) + NOTE_OFFSETS[match[1]]


def note_details(name: str) -> tuple[str, int, int]:
    match = re.fullmatch(r"([A-G]#?\d)v(\d+)\.flac", name)
    if match is None:
        raise ValueError("Unexpected source filename: " + name)
    return match[1], midi_note(match[1]), int(match[2])


def decode(path: Path) -> tuple[np.ndarray, dict]:
    info = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "a:0",
        "-show_entries", "stream=sample_rate,channels,bits_per_raw_sample",
        "-of", "json", str(path)]))["streams"][0]
    if (int(info["sample_rate"]) != SOURCE_RATE or int(info["channels"]) != 2
            or int(info["bits_per_raw_sample"]) != 24):
        raise RuntimeError("Expected stereo 48-kHz 24-bit source: " + str(path))
    raw = subprocess.check_output([
        "ffmpeg", "-nostdin", "-v", "error", "-i", str(path),
        "-map", "0:a:0", "-f", "f32le", "-acodec", "pcm_f32le", "pipe:1"])
    return np.frombuffer(raw, dtype="<f4").reshape(-1, 2), info


def conservative_trim(audio: np.ndarray) -> tuple[int, dict]:
    """Keep 2 ms before the first sustained, low-level hammer onset.

    One-ms RMS windows avoid confusing a zero crossing with silence. Two of
    three consecutive windows must cross the very low threshold, preventing
    an isolated noise spike from selecting a later or erratic starting point.
    Only the first 100 ms are searched and no more than 15 ms may be removed.
    The original SFZ offsets are intentionally not applied: they often skip
    a useful precursor to the hammer/strings attack.
    """
    peak = float(np.max(np.abs(audio)))
    threshold = max(3.0 / 32768.0, peak * 0.003)
    window = round(ONSET_WINDOW_SECONDS * SOURCE_RATE)
    count = min(len(audio) // window, round(0.100 * SOURCE_RATE) // window)
    blocks = audio[:count * window].astype(np.float64).reshape(count, window, 2)
    levels = np.sqrt(np.mean(blocks * blocks, axis=1)).max(axis=1)
    above = levels >= threshold
    first = None
    for index in range(max(0, count - 2)):
        if above[index] and np.count_nonzero(above[index:index + 3]) >= 2:
            first = index * window
            break
    trim = 0 if first is None else max(0, first - round(PREROLL_SECONDS * SOURCE_RATE))
    trim = min(trim, round(MAX_TRIM_SECONDS * SOURCE_RATE))
    return trim, {
        "method": "1ms stereo-channel RMS; 2 of 3 windows; 2ms retained preroll",
        "threshold": threshold,
        "firstSignificantSourceFrame": first,
        "trimStartSourceFrames": trim,
        "trimStartSeconds": trim / SOURCE_RATE,
        "maximumAllowedTrimSeconds": MAX_TRIM_SECONDS,
    }


def tail_limit(root: int) -> float:
    return 10.0 if root < 48 else (8.0 if root < 84 else 6.0)


def convert(item: dict, cache: Path, output: Path, source_offsets: dict) -> tuple[dict, dict]:
    source = acquire(item, cache / item["name"])
    note, root, source_layer = note_details(item["name"])
    layer = SOURCE_LAYERS.index(source_layer)
    audio, info = decode(source)
    trim, onset = conservative_trim(audio)
    maximum_source_frames = round(tail_limit(root) * SOURCE_RATE)
    clipped_source = audio[trim:trim + maximum_source_frames].astype(np.float64)
    # Exact rational 48,000 -> 44,100 conversion, no gain or pitch alteration.
    rendered = resample_poly(clipped_source, 147, 160, axis=0,
                             window=("kaiser", 8.6), padtype="constant")
    fade_frames = min(len(rendered), round(FADE_OUT_SECONDS * OUTPUT_RATE))
    phase = np.linspace(0.0, 1.0, fade_frames, dtype=np.float64)
    fade = 0.5 * (1.0 + np.cos(np.pi * phase))
    rendered[-fade_frames:] *= fade[:, None]
    peak = float(np.max(np.abs(rendered)))
    if peak > 1.0:
        # Do not silently normalize a single sample or collapse layer dynamics.
        raise RuntimeError(f"PCM conversion would clip {item['name']}: peak={peak}")
    pcm = np.clip(np.rint(rendered * 32768.0), -32768, 32767).astype("<i2")
    filename = f"piano_{note.replace('#', 'sharp')}_v{source_layer}.wav"
    destination = output / filename
    with wave.open(str(destination), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(OUTPUT_RATE)
        wav.writeframes(pcm.tobytes())
    output_bytes = destination.read_bytes()
    entry = {
        "file": filename, "rootNote": root, "velocityLayer": layer,
        "roundRobin": 0, "sampleRate": OUTPUT_RATE, "channels": 2,
        "sourceFile": item["name"], "sourceVelocityLayer": source_layer,
        "trimStartFrames": trim, "trimSourceSampleRate": SOURCE_RATE,
        "frames": len(pcm), "sha256": digest(output_bytes),
    }
    provenance = {
        **item, "sha256": digest(source.read_bytes()),
        "sourceFormat": {"sampleRate": SOURCE_RATE, "channels": 2,
                         "bitsPerSample": 24, "frames": len(audio)},
        "sourcePeak": float(np.max(np.abs(audio))),
        "onset": onset,
        "originalOptionalSfzOffsetFrames": source_offsets[(root, source_layer)],
        "originalOptionalSfzOffsetApplied": False,
        "maximumTailSeconds": tail_limit(root),
        "outputFile": filename, "outputPeak": peak,
        "outputFrames": len(pcm), "outputSha256": entry["sha256"],
    }
    return entry, provenance


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=PROJECT.parent / "key-research")
    parser.add_argument("--output", type=Path, default=PROJECT / "Samples")
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    if args.workers < 1:
        parser.error("--workers must be positive")
    for program in ("ffmpeg", "ffprobe"):
        if shutil.which(program) is None:
            parser.error(program + " is required")
    source_index = json.loads(SOURCE_INDEX.read_text(encoding="utf-8"))
    args.cache.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    third_party = PROJECT / "ThirdParty" / "Salamander"
    original = third_party / "upstream"
    references = source_index["referenceFiles"]
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        reference_paths = list(pool.map(
            lambda item: acquire(item, args.cache / "source" / item["name"]), references))
    for item, path in zip(references, reference_paths):
        target = original / item["name"]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    shutil.copyfile(original / "LICENSE", third_party / "LICENSE.txt")
    roots = sorted({note_details(item["name"])[1] for item in source_index["sampleFiles"]})
    source_offsets = {}
    for layer in SOURCE_LAYERS:
        text = (original / f"Data/vel_{layer:02}.txt").read_text(encoding="utf-8")
        for number, frames in re.findall(r"\$OFF(\d+)\s+(\d+)", text):
            source_offsets[(roots[int(number) - 1], layer)] = int(frames)
    source_files = sorted(source_index["sampleFiles"],
                          key=lambda item: note_details(item["name"])[1:])
    entries = []
    provenance = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        results = pool.map(lambda item: convert(item, args.cache, args.output, source_offsets),
                           source_files)
        for count, (entry, record) in enumerate(results, 1):
            entries.append(entry)
            provenance.append(record)
            if count % 15 == 0 or count == len(source_files):
                print(f"Converted {count}/{len(source_files)} stereo samples", flush=True)
    manifest = {
        "name": "Sculpt Key — Salamander acoustic piano subset",
        "license": "CC-BY-3.0", "author": "Alexander Holm",
        "sourceRepository": source_index["repository"],
        "sourceCommit": source_index["commit"],
        "recordedRoundRobinsPerNoteAndLayer": 1,
        "sourceVelocityLayers": list(SOURCE_LAYERS),
        "sampleFiles": entries,
    }
    json_write(args.output / "manifest.json", manifest)
    tsv = "# file\trootNote\tvelocityLayer\n" + "".join(
        f"{entry['file']}\t{entry['rootNote']}\t{entry['velocityLayer']}\n" for entry in entries)
    (args.output / "preview-bank.tsv").write_text(tsv, encoding="utf-8")
    converter_version = subprocess.check_output(["ffmpeg", "-version"], text=True).splitlines()[0]
    source_info = {
        "repository": source_index["repository"], "commit": source_index["commit"],
        "upstreamArchive": "https://archive.org/details/SalamanderGrandPianoV3",
        "instrument": "Yamaha C5", "recordingAuthor": "Alexander Holm",
        "sfzRemappingCredit": "kinwie", "license": "CC-BY-3.0",
        "licenseUrl": "https://creativecommons.org/licenses/by/3.0/",
        "originalRootNotes": roots, "includedSourceVelocityLayers": list(SOURCE_LAYERS),
        "sourceLayerVelocityRangesInUpstreamSfz": {"4": [37, 43], "8": [57, 64], "12": [89, 96]},
        "recordedRoundRobinsPerNoteAndLayer": 1,
        "roundRobinNote": "No distinct recorded RR takes are claimed. Any automatic variations are modeled in the engine.",
        "changes": [
            "Selected 30 original chromatic roots in minor-third steps, A0 through C8, at source velocity layers 4, 8, 12.",
            "Conservative deterministic leading trim: 1ms channel RMS, threshold=max(3/32768, sourcePeak*0.003), two of three windows, retaining 2ms preroll; at most 15ms removed.",
            "Kept the original stereo image, pitch, relative velocity-layer levels and attack. No sample or layer normalization, no time stretch, no random offsets.",
            "Resampled 48kHz to 44.1kHz with a rational 147/160 polyphase Kaiser-8.6 filter.",
            "Tail limits: 10s for MIDI roots below48, 8s for roots48–81, 6s for roots84–108; shorter source tails remain shorter.",
            "Applied a 300ms half-cosine tail fade, then round-to-nearest PCM16 conversion without random dither.",
            "The source SFZ, optional string resonance, release, hammer and pedal sample layers are not loaded by this subset."
        ],
        "conversionTools": {"python": sys.version.split()[0], "numpy": np.__version__,
                            "scipy": scipy.__version__, "ffmpeg": converter_version},
        "references": [{**item, "sha256": digest(path.read_bytes())}
                       for item, path in zip(references, reference_paths)],
        "samples": provenance,
    }
    json_write(args.output / "source-info.json", source_info)
    credit = f"""# Salamander Grand Piano — credits and changes

Sculpt Key contains a modified subset of **Salamander Grand Piano v3**,
recorded by **Alexander Holm** on a Yamaha C5. The upstream SFZ remapping
is by **kinwie**; the optional retuned mapping is credited upstream to
**Markus Fiedler**. Sculpt Key uses the original recordings without the
optional retuning and loads its own sampler mapping, not the upstream SFZ.

Source: {source_index['repository']}
Pinned source revision: {source_index['commit']}
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
"""
    (third_party / "CREDITS_AND_CHANGES.md").write_text(credit, encoding="utf-8")
    total_bytes = sum((args.output / entry["file"]).stat().st_size for entry in entries)
    print(f"Bank ready: {len(entries)} WAVs, {total_bytes / 1048576:.2f} MiB", flush=True)
    print(args.output / "preview-bank.tsv", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
