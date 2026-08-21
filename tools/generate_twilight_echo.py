"""Generate an original seamless dark-fantasy ambient loop as WAV and MIDI."""

from __future__ import annotations

import math
import random
import struct
import wave
from pathlib import Path


SAMPLE_RATE = 44_100
BPM = 60
BEATS = 64
DURATION = BEATS * 60.0 / BPM
FRAMES = round(DURATION * SAMPLE_RATE)
TAU = 2.0 * math.pi
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "generated_music"


def hz(midi_note: float) -> float:
    return 440.0 * 2.0 ** ((midi_note - 69.0) / 12.0)


def circular_age(t: float, onset: float) -> float:
    return (t - onset) % DURATION


def softclip(value: float) -> float:
    return math.tanh(value * 1.15) / math.tanh(1.15)


def render_wav(path: Path) -> None:
    # Original pitch language: D, Eb, A, Bb, C, F and their upper extensions.
    drone = [(38, 0.16, -0.55), (45, 0.10, 0.40), (51, 0.055, -0.08), (56, 0.035, 0.65)]
    chimes = [
        (1.5, 74, -0.70), (5.0, 77, 0.52), (9.5, 75, -0.25),
        (13.0, 82, 0.72), (17.0, 73, -0.58), (21.5, 79, 0.30),
        (25.0, 76, 0.62), (29.5, 70, -0.35), (33.0, 81, 0.12),
        (37.5, 78, -0.68), (41.0, 85, 0.64), (45.5, 72, -0.12),
        (49.0, 80, 0.52), (53.5, 83, -0.46), (57.0, 75, 0.28),
        (61.5, 86, -0.18),
    ]
    bass_notes = [38, 39, 34, 36, 38, 31, 34, 37, 38, 43, 39, 34, 36, 31, 37, 35]
    pulses = [(b * 4 * 60.0 / BPM, n) for b, n in enumerate(bass_notes)]
    whispers = [(6.0, 59, -0.65), (14.0, 63, 0.55), (23.0, 58, -0.20),
                (31.0, 66, 0.70), (40.0, 61, -0.55), (48.0, 56, 0.35),
                (55.0, 64, -0.10), (63.0, 60, 0.50)]
    knocks = [(b + off, 43 + ((b * 5) % 7), (-0.55 if b % 2 else 0.55))
              for b in range(0, BEATS, 2) for off in (0.0, 0.75, 1.375)]
    rng = random.Random(0x54574C54)
    noise_table = [rng.uniform(-1.0, 1.0) for _ in range(4096)]

    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        chunk = bytearray()

        for i in range(FRAMES):
            t = i / SAMPLE_RATE
            loop_phase = t / DURATION
            left = right = 0.0

            # Slow, phase-locked spectral breathing.
            breath = 0.68 + 0.16 * math.sin(TAU * loop_phase) + 0.08 * math.sin(TAU * 3 * loop_phase + 0.8)
            section_motion = 0.78 + 0.22 * math.sin(TAU * 4 * loop_phase - 0.9) ** 2
            for note, amp, pan in drone:
                f = hz(note)
                # Quantize only the rendered oscillator to whole cycles per loop;
                # the MIDI retains the conventional equal-tempered pitch.
                cycles = round(f * DURATION)
                vibrato = 0.018 * math.sin(TAU * loop_phase)
                tone = (
                    math.sin(TAU * cycles * loop_phase + vibrato)
                    + 0.34 * math.sin(TAU * round(cycles * 2.003) * loop_phase + 1.1 + vibrato)
                    + 0.13 * math.sin(TAU * round(cycles * 0.499) * loop_phase)
                )
                value = amp * breath * section_motion * tone
                left += value * math.sqrt((1.0 - pan) * 0.5)
                right += value * math.sqrt((1.0 + pan) * 0.5)

            # Circular low swells; their releases cross the loop boundary cleanly.
            for onset, note in pulses:
                age = circular_age(t, onset)
                env = math.exp(-age / 2.8) * min(1.0, age / 0.22)
                cycles = round(hz(note) * DURATION)
                value = 0.13 * env * (
                    math.sin(TAU * cycles * loop_phase)
                    + 0.35 * math.sin(TAU * round(cycles * 1.5) * loop_phase + 0.4)
                )
                left += value * 0.72
                right += value * 0.69

            # Glass-like motif, deliberately unrelated to the reference melody.
            for beat, note, pan in chimes:
                onset = beat * 60.0 / BPM
                age = circular_age(t, onset)
                env = math.exp(-age / 1.75) * min(1.0, age / 0.012)
                f = hz(note)
                phase = TAU * f * age
                bell = math.sin(phase) + 0.54 * math.sin(phase * 2.71 + 0.3) + 0.25 * math.sin(phase * 4.13)
                value = 0.11 * env * bell
                left += value * math.sqrt((1.0 - pan) * 0.5)
                right += value * math.sqrt((1.0 + pan) * 0.5)

                # Two diffuse echoes make the motif inhabit a larger space.
                for delay, gain, echo_pan in ((0.43, 0.34, -pan * 0.7), (0.91, 0.18, pan * 0.4)):
                    echo_age = circular_age(t, onset + delay)
                    echo_env = math.exp(-echo_age / 1.5) * min(1.0, echo_age / 0.015)
                    echo = gain * 0.11 * echo_env * (
                        math.sin(TAU * f * echo_age + 0.2)
                        + 0.4 * math.sin(TAU * f * 2.71 * echo_age + 1.0)
                    )
                    left += echo * math.sqrt((1.0 - echo_pan) * 0.5)
                    right += echo * math.sqrt((1.0 + echo_pan) * 0.5)

            # Synthetic reversed voices: formant clusters bloom toward each onset.
            for beat, note, pan in whispers:
                onset = beat * 60.0 / BPM
                until = (onset - t) % DURATION
                age = circular_age(t, onset)
                reverse_env = math.exp(-until / 1.35) * min(1.0, until / 0.04)
                release_env = 0.42 * math.exp(-age / 1.8) * min(1.0, age / 0.025)
                env = reverse_env + release_env
                fundamental = hz(note - 24)
                fundamental_cycles = round(fundamental * DURATION)
                slow_phase = TAU * fundamental_cycles * loop_phase
                formant = (
                    0.62 * math.sin(slow_phase + 0.7 * math.sin(TAU * 12 * loop_phase))
                    + 0.24 * math.sin(TAU * round(fundamental_cycles * 3.07) * loop_phase + 1.4)
                    + 0.16 * math.sin(TAU * round(fundamental_cycles * 5.23) * loop_phase + 0.2)
                )
                tremolo = 0.72 + 0.28 * math.sin(TAU * 467 * loop_phase + note) ** 2
                value = 0.085 * env * tremolo * formant
                left += value * math.sqrt((1.0 - pan) * 0.5)
                right += value * math.sqrt((1.0 + pan) * 0.5)

            # Irregular mechanical ticks and hollow impacts establish a hidden pulse.
            for beat, note, pan in knocks:
                onset = beat * 60.0 / BPM
                age = circular_age(t, onset)
                env = math.exp(-age / 0.19) * min(1.0, age / 0.003)
                f = hz(note)
                metallic = math.sin(TAU * f * age) + 0.5 * math.sin(TAU * f * 3.83 * age + 0.4)
                value = 0.042 * env * metallic
                left += value * math.sqrt((1.0 - pan) * 0.5)
                right += value * math.sqrt((1.0 + pan) * 0.5)

            # High spectral threads enter during the second half and dissolve at the seam.
            spectral_gate = 0.5 - 0.5 * math.cos(TAU * loop_phase)
            shimmer = (
                math.sin(TAU * 173 * loop_phase + 2.0 * math.sin(TAU * 5 * loop_phase))
                + 0.45 * math.sin(TAU * 281 * loop_phase + 0.6)
                + 0.22 * math.sin(TAU * 419 * loop_phase + 1.8)
            )
            left += 0.025 * spectral_gate * shimmer
            right += 0.023 * spectral_gate * (
                math.sin(TAU * 179 * loop_phase + 1.1) + 0.4 * math.sin(TAU * 293 * loop_phase)
            )

            # Quiet, periodic granular air (table repeats exactly at the seam).
            noise_pos = loop_phase * len(noise_table) * 7
            ni = int(noise_pos) % len(noise_table)
            nf = noise_pos - math.floor(noise_pos)
            noise_l = noise_table[ni] * (1.0 - nf) + noise_table[(ni + 1) % len(noise_table)] * nf
            nri = (ni + 997) % len(noise_table)
            noise_r = noise_table[nri] * (1.0 - nf) + noise_table[(nri + 1) % len(noise_table)] * nf
            air = noise_l * (0.013 + 0.007 * math.sin(TAU * 5 * loop_phase) ** 2)
            left += air
            right += noise_r * 0.014

            l16 = int(max(-1.0, min(1.0, softclip(left))) * 32767)
            r16 = int(max(-1.0, min(1.0, softclip(right))) * 32767)
            chunk.extend(struct.pack("<hh", l16, r16))
            if len(chunk) >= 262_144:
                wav.writeframesraw(chunk)
                chunk.clear()
        wav.writeframes(chunk)


def vlq(value: int) -> bytes:
    out = bytearray([value & 0x7F])
    value >>= 7
    while value:
        out.insert(0, 0x80 | (value & 0x7F))
        value >>= 7
    return bytes(out)


def render_midi(path: Path) -> None:
    ppq = 480
    events: list[tuple[int, int, bytes]] = []
    tempo = round(60_000_000 / BPM)
    events.append((0, 0, b"\xff\x51\x03" + tempo.to_bytes(3, "big")))
    events.append((0, 0, b"\xff\x58\x04\x04\x02\x18\x08"))
    events.append((0, 0, b"\xff\x03\x15Twilight Echo Rebuilt"))

    # Sustained drone track on channel 1.
    for note, velocity in [(38, 45), (45, 31), (51, 22)]:
        events.append((0, 2, bytes([0x90, note, velocity])))
        events.append((BEATS * ppq, 0, bytes([0x80, note, 0])))

    # Low harmonic swells on channel 2.
    bass_notes = [38, 39, 34, 36, 38, 31, 34, 37, 38, 43, 39, 34, 36, 31, 37, 35]
    for bar, note in enumerate(bass_notes):
        start = bar * 4 * ppq
        events.append((start, 2, bytes([0x91, note, 34])))
        events.append((start + 3 * ppq, 0, bytes([0x81, note, 0])))

    # Original chime motif on channel 3.
    motif = [(1.5, 74), (5.0, 77), (9.5, 75), (13.0, 82), (17.0, 73),
             (21.5, 79), (25.0, 76), (29.5, 70), (33.0, 81), (37.5, 78),
             (41.0, 85), (45.5, 72), (49.0, 80), (53.5, 83), (57.0, 75),
             (61.5, 86)]
    for beat, note in motif:
        start = round(beat * ppq)
        events.append((start, 2, bytes([0x92, note, 52])))
        events.append((min(BEATS * ppq, start + ppq), 0, bytes([0x82, note, 0])))

    # Reverse-voice guide notes on channel 4 for sound-design replacement in a DAW.
    for beat, note in [(6, 59), (14, 63), (23, 58), (31, 66), (40, 61),
                       (48, 56), (55, 64), (63, 60)]:
        start = round(beat * ppq)
        events.append((max(0, start - 2 * ppq), 2, bytes([0x93, note, 38])))
        events.append((min(BEATS * ppq, start + ppq), 0, bytes([0x83, note, 0])))

    events.sort(key=lambda event: (event[0], event[1]))
    track = bytearray()
    previous = 0
    for tick, _, data in events:
        track.extend(vlq(tick - previous))
        track.extend(data)
        previous = tick
    track.extend(vlq(BEATS * ppq - previous))
    track.extend(b"\xff\x2f\x00")

    header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, ppq)
    path.write_bytes(header + b"MTrk" + struct.pack(">I", len(track)) + track)


def main() -> None:
    OUT.mkdir(exist_ok=True)
    wav_path = OUT / "twilight_echo_seamless.wav"
    midi_path = OUT / "twilight_echo_seamless.mid"
    render_wav(wav_path)
    render_midi(midi_path)
    print(f"Wrote {wav_path} ({DURATION:.3f}s, {FRAMES} frames)")
    print(f"Wrote {midi_path} ({BEATS} beats at {BPM} BPM)")


if __name__ == "__main__":
    main()
