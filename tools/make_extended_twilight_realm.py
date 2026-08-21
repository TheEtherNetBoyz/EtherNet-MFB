"""Create a seamless extended edit from a user-supplied Twilight Realm recording."""

from __future__ import annotations

import math
import subprocess
import sys
import wave
from array import array
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "generated_music"
FFMPEG = ROOT / ".tools/python/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v7.1.exe"
SOURCE = Path(r"C:\Users\TheEtherNetBoyz\Downloads\The Twilight Realm - The Legend of Zelda_ Twilight Princess OST.mp3")
DECODED = OUT / "twilight_realm_source_pcm.wav"
WAV_OUT = OUT / "the_twilight_realm_extended_seamless.wav"
MP3_OUT = OUT / "the_twilight_realm_extended_seamless.mp3"
RATE = 44_100
CHANNELS = 2
CROSSFADE_SECONDS = 8.0
REPETITIONS = 2


def run(*args: str) -> None:
    subprocess.run([str(FFMPEG), "-hide_banner", "-loglevel", "error", "-y", *args], check=True)


def write_frames(wav: wave.Wave_write, samples: array, start: int, end: int) -> None:
    chunk_frames = 65_536
    for frame in range(start, end, chunk_frames):
        stop = min(end, frame + chunk_frames)
        wav.writeframesraw(samples[frame * CHANNELS:stop * CHANNELS].tobytes())


def main() -> None:
    if not SOURCE.exists():
        raise FileNotFoundError(SOURCE)
    if not FFMPEG.exists():
        raise FileNotFoundError(FFMPEG)
    OUT.mkdir(exist_ok=True)

    run("-i", str(SOURCE), "-vn", "-ar", str(RATE), "-ac", str(CHANNELS), "-c:a", "pcm_s16le", str(DECODED))
    with wave.open(str(DECODED), "rb") as src:
        if src.getsampwidth() != 2 or src.getnchannels() != CHANNELS or src.getframerate() != RATE:
            raise ValueError("Decoded source format is not 44.1 kHz 16-bit stereo")
        frames = src.getnframes()
        samples = array("h")
        samples.frombytes(src.readframes(frames))

    crossfade = round(CROSSFADE_SECONDS * RATE)
    cut = frames // 2
    if crossfade >= cut or cut >= frames - crossfade:
        raise ValueError("Source is too short for the selected crossfade")

    # One loop is: middle -> near-end, end/start equal-power crossfade,
    # then near-start -> middle. Its outer boundary is untouched adjacent audio.
    with wave.open(str(WAV_OUT), "wb") as dst:
        dst.setnchannels(CHANNELS)
        dst.setsampwidth(2)
        dst.setframerate(RATE)
        for _ in range(REPETITIONS):
            write_frames(dst, samples, cut, frames - crossfade)
            mixed = array("h")
            for i in range(crossfade):
                theta = (i / max(1, crossfade - 1)) * math.pi * 0.5
                fade_out = math.cos(theta)
                fade_in = math.sin(theta)
                tail_base = (frames - crossfade + i) * CHANNELS
                head_base = i * CHANNELS
                for channel in range(CHANNELS):
                    value = samples[tail_base + channel] * fade_out + samples[head_base + channel] * fade_in
                    mixed.append(round(max(-32768, min(32767, value))))
            dst.writeframesraw(mixed.tobytes())
            write_frames(dst, samples, crossfade, cut)

    run(
        "-i", str(WAV_OUT), "-map_metadata", "-1", "-c:a", "libmp3lame", "-b:a", "192k",
        "-metadata", "title=The Twilight Realm - Extended Seamless Edit",
        "-metadata", "comment=Extended edit created from user-supplied source audio",
        str(MP3_OUT),
    )
    DECODED.unlink(missing_ok=True)

    loop_frames = (frames - crossfade) * REPETITIONS
    print(f"Source: {frames / RATE:.3f} seconds")
    print(f"Extended loop: {loop_frames / RATE:.3f} seconds ({loop_frames} frames)")
    print(WAV_OUT)
    print(MP3_OUT)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise
