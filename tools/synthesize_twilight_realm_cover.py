"""Analyze the supplied song, then synthesize a new extended cover from scratch."""

from __future__ import annotations

import math
import os
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf
from scipy import signal


ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(r"C:\Users\TheEtherNetBoyz\Downloads\The Twilight Realm - The Legend of Zelda_ Twilight Princess OST.mp3")
OUT = ROOT / "generated_music" / "the_twilight_realm_original_cover_extended.wav"
SR = 22_050
HOP_SECONDS = 4.0
REPETITIONS = 2
TAU = 2 * np.pi


def midi_hz(note: float) -> float:
    return 440.0 * 2 ** ((note - 69) / 12)


def pan_stereo(mono: np.ndarray, pan: float) -> np.ndarray:
    return np.column_stack((mono * math.sqrt((1 - pan) / 2), mono * math.sqrt((1 + pan) / 2)))


def add_voice(bus: np.ndarray, start: int, voice: np.ndarray, pan: float) -> None:
    if start < 0:
        voice = voice[-start:]
        start = 0
    if start >= len(bus):
        return
    end = min(len(bus), start + len(voice))
    bus[start:end] += pan_stereo(voice[: end - start], pan)


def oscillator(freq: float, seconds: float, phase: float = 0.0) -> tuple[np.ndarray, np.ndarray]:
    t = np.arange(round(seconds * SR), dtype=np.float64) / SR
    return t, TAU * freq * t + phase


def pad(freq: float, seconds: float, brightness: float) -> np.ndarray:
    t, p = oscillator(freq, seconds)
    attack = np.minimum(1, t / 1.8)
    release = np.minimum(1, (seconds - t) / 2.2)
    env = np.maximum(0, attack * release)
    detune = np.sin(p * 0.997) + np.sin(p * 1.003 + 0.7)
    tone = 0.42 * detune + brightness * 0.18 * np.sin(2 * p + 1.1) + 0.09 * np.sin(0.5 * p)
    return (env * tone).astype(np.float32)


def bell(freq: float, seconds: float, variant: int) -> np.ndarray:
    t, p = oscillator(freq, seconds, variant * 0.37)
    env = (1 - np.exp(-t / 0.006)) * np.exp(-t / (1.35 + 0.2 * (variant % 3)))
    tone = np.sin(p) + 0.52 * np.sin(2.71 * p + 0.2) + 0.24 * np.sin(4.19 * p + 1.0)
    return (0.24 * env * tone).astype(np.float32)


def choir(freq: float, seconds: float, seed: int) -> np.ndarray:
    t, p = oscillator(freq, seconds)
    swell = np.sin(np.pi * np.clip(t / seconds, 0, 1)) ** 1.7
    formants = 0.55 * np.sin(p) + 0.24 * np.sin(3.03 * p + 1.2) + 0.13 * np.sin(5.11 * p)
    flutter = 0.76 + 0.24 * np.sin(TAU * (5.1 + seed % 5 * 0.23) * t + seed) ** 2
    return (0.16 * swell * flutter * formants).astype(np.float32)


def analyze_harmony() -> tuple[np.ndarray, float, float]:
    audio, source_sr = librosa.load(SOURCE, sr=SR, mono=True)
    tempo, _ = librosa.beat.beat_track(y=audio, sr=source_sr)
    chroma = librosa.feature.chroma_cqt(y=audio, sr=source_sr, hop_length=2048)
    block = max(1, round(HOP_SECONDS * source_sr / 2048))
    chords = []
    for frame in range(0, chroma.shape[1], block):
        energy = chroma[:, frame:frame + block].mean(axis=1)
        chords.append(np.argsort(energy)[-3:][::-1])
    return np.asarray(chords), float(np.asarray(tempo).reshape(-1)[0]), len(audio) / source_sr


def synthesize(chords: np.ndarray, tempo: float, source_duration: float) -> np.ndarray:
    section_duration = len(chords) * HOP_SECONDS
    duration = section_duration * REPETITIONS
    bus = np.zeros((round(duration * SR), 2), dtype=np.float32)
    rng = np.random.default_rng(0x54574C54)

    for repeat in range(REPETITIONS):
        offset = repeat * section_duration
        for index, pcs in enumerate(chords):
            start_sec = offset + index * HOP_SECONDS
            intensity = 0.72 if repeat == 0 else 1.0
            root_octave = 35 if repeat == 0 else 47

            # Preserve the analyzed harmonic contour while rebuilding every timbre.
            for rank, pc in enumerate(pcs):
                midi = root_octave + ((int(pc) - root_octave) % 12) + (12 if rank == 2 else 0)
                voice = pad(midi_hz(midi), HOP_SECONDS + 2.4, 0.7 + 0.2 * repeat)
                add_voice(bus, round((start_sec - 1.0) * SR), voice * intensity * (0.15 - rank * 0.025), (-0.55, 0.4, 0.05)[rank])

            # The lead uses the strongest extracted pitch class, with a new contour.
            lead_pc = int(pcs[0])
            contour = (0, 7, 3, 10, 5, 1, 8, 6)[index % 8]
            lead_midi = 71 + ((lead_pc + contour - 11) % 12) + (12 if repeat and index % 4 == 3 else 0)
            for sub, delay in enumerate((0.0, 1.45, 2.65)):
                note = lead_midi + (0, -5, 2)[sub]
                hit = bell(midi_hz(note), 3.2, index + sub + repeat * 11)
                add_voice(bus, round((start_sec + delay) * SR), hit * (0.50 if repeat else 0.38), (-0.65, 0.5, -0.1)[sub])

            if index % 3 == 1:
                vocal_note = 47 + ((int(pcs[1]) - 11) % 12)
                vocal = choir(midi_hz(vocal_note), 6.0, index + repeat * 19)
                # Reverse the first pass swells; answer them forward in variation two.
                if repeat == 0:
                    vocal = vocal[::-1].copy()
                add_voice(bus, round((start_sec - 2.0) * SR), vocal * 0.72, 0.55 if index % 2 else -0.55)

            # Low mechanical heartbeat follows the analyzed tempo.
            beat = 60.0 / max(1.0, tempo)
            for pulse_index in range(max(1, round(HOP_SECONDS / beat))):
                pulse_start = start_sec + pulse_index * beat
                length = round(0.42 * SR)
                t = np.arange(length) / SR
                f = midi_hz(30 + ((lead_pc - 6) % 12))
                thump = np.sin(TAU * f * t) * np.exp(-t / 0.11)
                click = rng.normal(0, 1, length) * np.exp(-t / 0.018)
                mono = (0.09 * thump + 0.012 * click).astype(np.float32) * intensity
                add_voice(bus, round(pulse_start * SR), mono, -0.22 if pulse_index % 2 else 0.22)

    # Entirely synthetic granular air and long stereo ambience.
    noise = rng.normal(0, 1, len(bus)).astype(np.float32)
    sos = signal.butter(3, [1800, 7200], btype="bandpass", fs=SR, output="sos")
    air = signal.sosfilt(sos, noise).astype(np.float32)
    slow = 0.5 + 0.5 * np.sin(TAU * np.arange(len(bus)) / len(bus) * 7) ** 2
    bus[:, 0] += air * slow * 0.012
    bus[:, 1] += np.roll(air, 1703) * slow * 0.011

    # Multi-tap feedback-like space without retaining any source recording.
    dry = bus.copy()
    for delay_sec, gain, swap in ((0.31, 0.20, True), (0.73, 0.13, False), (1.37, 0.075, True)):
        delay = round(delay_sec * SR)
        delayed = np.roll(dry, delay, axis=0)
        if swap:
            delayed = delayed[:, ::-1]
        bus += delayed * gain

    # Equal-power circular crossfade, then rotate to place the seam in stable material.
    cross = round(8.0 * SR)
    theta = np.linspace(0, np.pi / 2, cross, dtype=np.float32)[:, None]
    bridge = bus[-cross:] * np.cos(theta) + bus[:cross] * np.sin(theta)
    cut = len(bus) // 3
    bus = np.concatenate((bus[cut:-cross], bridge, bus[cross:cut]), axis=0)

    peak = np.max(np.abs(bus))
    if peak > 0:
        bus *= 0.82 / peak
    return np.tanh(bus * 1.08).astype(np.float32)


def main() -> None:
    if not SOURCE.exists():
        raise FileNotFoundError(SOURCE)
    OUT.parent.mkdir(exist_ok=True)
    chords, tempo, duration = analyze_harmony()
    result = synthesize(chords, tempo, duration)
    sf.write(OUT, result, SR, subtype="PCM_16")
    print(f"Analyzed tempo: {tempo:.3f} BPM")
    print(f"Harmonic blocks: {len(chords)}")
    print(f"Cover duration: {len(result) / SR:.3f}s")
    print(OUT)


if __name__ == "__main__":
    main()
