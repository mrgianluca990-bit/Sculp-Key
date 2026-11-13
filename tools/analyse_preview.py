#!/usr/bin/env python3
"""Measure the fixed 44-second KeyEngine preview; never modifies its audio."""
from pathlib import Path
import json
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

ROOT = Path(__file__).resolve().parents[1]
rate, pcm = wavfile.read(ROOT / "Preview" / "SculptKey-demo.wav")
if pcm.dtype != np.int16 or pcm.ndim != 2 or pcm.shape[1] != 2:
    raise SystemExit("Expected stereo PCM16 preview")
audio = pcm.astype(np.float64) / 32768.0

def db(value):
    return float(20.0 * np.log10(max(1e-10, float(value))))

result = {
    "sampleRate": int(rate),
    "seconds": len(audio) / rate,
    "peakDbfs": db(np.max(np.abs(audio))),
    "finite": bool(np.isfinite(audio).all()),
    "sections": [],
    "measurementNote": "Each anchor uses its first six seconds. RMS is not perceptual loudness.",
}
for index, name in enumerate(("Piano", "Rhodes", "Hammond", "Synth")):
    section = audio[index * 8 * rate:(index * 8 + 6) * rate]
    spectrum = np.fft.rfft(section[:, 0])
    frequency = np.fft.rfftfreq(len(section), 1.0 / rate)
    energy = np.abs(spectrum) ** 2
    result["sections"].append({
        "name": name,
        "rmsDbfs": db(np.sqrt(np.mean(section * section))),
        "peakDbfs": db(np.max(np.abs(section))),
        "highAbove2kPercent": float(100.0 * energy[frequency > 2000].sum() / energy.sum()),
        "stereoCorrelation": float(np.corrcoef(section.T)[0, 1]),
        "dc": float(np.mean(section)),
    })
result["estimated4xTruePeakDbfs"] = db(np.max(np.abs(resample_poly(audio, 4, 1, axis=0))))
result["last200msPeakDbfs"] = db(np.max(np.abs(audio[-int(rate * 0.2):])))
levels = [section["rmsDbfs"] for section in result["sections"]]
result["anchorRmsRangeDb"] = max(levels) - min(levels)
output = ROOT / "docs" / "preview-metrics.json"
output.parent.mkdir(exist_ok=True)
output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
