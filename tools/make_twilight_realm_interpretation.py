"""Build an extended, layered remix from the user-supplied original recording."""

from __future__ import annotations

import math
import subprocess
import wave
from array import array
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "generated_music"
FFMPEG = ROOT / ".tools/python/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v7.1.exe"
SOURCE = Path(r"C:\Users\TheEtherNetBoyz\Downloads\The Twilight Realm - The Legend of Zelda_ Twilight Princess OST.mp3")
RATE = 44_100
CHANNELS = 2
SECTION_FADE = 9.0
CIRCLE_FADE = 10.0
WAV_OUT = OUT / "the_twilight_realm_extended_interpretation.wav"
MP3_OUT = OUT / "the_twilight_realm_extended_interpretation.mp3"


FILTERS = {
    # Pass one stays close to the recording and opens its stereo atmosphere.
    "a": (
        "[0:a]asplit=4[base][air][echo][body];"
        "[base]volume=0.82[b];"
        "[air]highpass=f=1800,chorus=0.45:0.55:36|51:0.22|0.16:0.31|0.24:1.9|1.3,volume=0.16[a];"
        "[echo]aecho=0.7:0.35:430|910:0.20|0.11,highpass=f=350,volume=0.24[e];"
        "[body]lowpass=f=240,bass=g=5:f=90:w=0.6,volume=0.18[d];"
        "[b][a][e][d]amix=inputs=4:normalize=0,alimiter=limit=0.90[out]"
    ),
    # Pass two is the developed section: darker, wider, and more unstable.
    "b": (
        "[0:a]asplit=5[base][low][glass][ghost][motion];"
        "[base]volume=0.72[b];"
        "[low]lowpass=f=170,bass=g=8:f=72:w=0.5,volume=0.22[l];"
        "[glass]highpass=f=2300,chorus=0.5:0.7:29|47:0.26|0.18:0.36|0.25:2.2|1.6,volume=0.17[g];"
        "[ghost]aecho=0.75:0.45:320|690|1210:0.22|0.14|0.08,aphaser=in_gain=0.5:out_gain=0.65:delay=2.5:decay=0.35:speed=0.25,volume=0.24[h];"
        "[motion]highpass=f=600,tremolo=f=5.7:d=0.32,flanger=delay=3:depth=2.2:regen=15:width=55:speed=0.18,volume=0.10[m];"
        "[b][l][g][h][m]amix=inputs=5:normalize=0,alimiter=limit=0.90[out]"
    ),
    # Pass three behaves like a suspended breakdown before rebuilding.
    "c": (
        "[0:a]asplit=5[base][mist][sub][reversecolor][spark];"
        "[base]lowpass=f=1450,volume=0.54[b];"
        "[mist]aecho=0.8:0.62:560|1180|1760:0.28|0.15|0.08,lowpass=f=4200,volume=0.28[m];"
        "[sub]lowpass=f=115,bass=g=9:f=68:w=0.45,volume=0.25[s];"
        "[reversecolor]areverse,highpass=f=900,chorus=0.4:0.6:41|63:0.18|0.12:0.25|0.19:1.3|0.9,areverse,volume=0.10[r];"
        "[spark]highpass=f=3100,tremolo=f=7.3:d=0.55,aecho=0.6:0.3:240|480:0.18|0.09,volume=0.10[p];"
        "[b][m][s][r][p]amix=inputs=5:normalize=0,alimiter=limit=0.88[out]"
    ),
}


def ffmpeg(*args: str) -> None:
    subprocess.run([str(FFMPEG), "-hide_banner", "-loglevel", "error", "-y", *args], check=True)


def decode_variant(name: str, graph: str) -> Path:
    path = OUT / f"twilight_remix_pass_{name}.wav"
    ffmpeg(
        "-i", str(SOURCE), "-filter_complex", graph, "-map", "[out]",
        "-ar", str(RATE), "-ac", str(CHANNELS), "-c:a", "pcm_s16le", str(path),
    )
    return path


def read_pcm(path: Path) -> tuple[array, int]:
    with wave.open(str(path), "rb") as wav:
        if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) != (RATE, CHANNELS, 2):
            raise ValueError(f"Unexpected audio format: {path}")
        frames = wav.getnframes()
        samples = array("h")
        samples.frombytes(wav.readframes(frames))
        return samples, frames


def equal_power_join(left: array, right: array, fade_frames: int) -> array:
    if fade_frames <= 0:
        return left + right
    result = array("h", left[: len(left) - fade_frames * CHANNELS])
    left_start = len(left) - fade_frames * CHANNELS
    for frame in range(fade_frames):
        theta = frame / (fade_frames - 1) * math.pi * 0.5
        a, b = math.cos(theta), math.sin(theta)
        for channel in range(CHANNELS):
            value = left[left_start + frame * CHANNELS + channel] * a + right[frame * CHANNELS + channel] * b
            result.append(round(max(-32768, min(32767, value))))
    result.extend(right[fade_frames * CHANNELS:])
    return result


def circularize(samples: array, frames: int, fade_frames: int, cut: int) -> array:
    # Start and end at the same untouched point; hide the former endpoint inside.
    first = array("h", samples[cut * CHANNELS:(frames - fade_frames) * CHANNELS])
    tail = array("h", samples[(frames - fade_frames) * CHANNELS:])
    head = array("h", samples[:fade_frames * CHANNELS])
    bridge = equal_power_join(tail, head, fade_frames)
    second = array("h", samples[fade_frames * CHANNELS:cut * CHANNELS])
    first.extend(bridge)
    first.extend(second)
    return first


def main() -> None:
    if not SOURCE.exists() or not FFMPEG.exists():
        raise FileNotFoundError("Source audio or workspace FFmpeg helper is missing")
    OUT.mkdir(exist_ok=True)
    temp_paths = [decode_variant(name, graph) for name, graph in FILTERS.items()]
    variants = [read_pcm(path) for path in temp_paths]
    source_frames = min(frames for _, frames in variants)
    trimmed = [array("h", samples[:source_frames * CHANNELS]) for samples, _ in variants]

    section_fade = round(SECTION_FADE * RATE)
    timeline = equal_power_join(trimmed[0], trimmed[1], section_fade)
    timeline = equal_power_join(timeline, trimmed[2], section_fade)
    timeline_frames = len(timeline) // CHANNELS

    # Choose an exposed boundary well inside pass one, away from all transitions.
    cut = source_frames // 2
    circle_fade = round(CIRCLE_FADE * RATE)
    final = circularize(timeline, timeline_frames, circle_fade, cut)

    with wave.open(str(WAV_OUT), "wb") as wav:
        wav.setnchannels(CHANNELS)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(final.tobytes())

    ffmpeg(
        "-i", str(WAV_OUT), "-map_metadata", "-1", "-c:a", "libmp3lame", "-b:a", "224k",
        "-metadata", "title=The Twilight Realm - Extended Interpretation",
        "-metadata", "comment=Remix made from user-supplied source audio with new layered sections",
        str(MP3_OUT),
    )
    for path in temp_paths:
        path.unlink(missing_ok=True)
    print(f"Source duration: {source_frames / RATE:.3f}s")
    print(f"Remix duration: {len(final) / CHANNELS / RATE:.3f}s")
    print(WAV_OUT)
    print(MP3_OUT)


if __name__ == "__main__":
    main()
