"""Read-only audio and movie format helpers for the content pipeline.

Xbox ADPCM (Halo's sound compression 1) is decoded exactly as the Linux
port's mixer does (port/linux/src/dsound_sdl.c): per block a 4-byte header
per channel (first sample, step index), then 4-byte groups of eight nibbles,
low nibble first, alternating between channels; 64 samples per channel, the
header sample first and the 64th nibble ignored. Output is signed 16-bit
big-endian interleaved PCM, written with explicit byte order.

Bink and other container headers are parsed field by field for inventory
only; no movie is decoded here.
"""
import struct

ADPCM_BLOCK_BYTES = 36
ADPCM_BLOCK_SAMPLES = 64
SAMPLE_RATES = {0: 22050, 1: 44100}
ENCODINGS = {0: 1, 1: 2}  # encoding -> channels
COMPRESSIONS = {0: "pcm16le", 1: "xbox_adpcm", 2: "ima_adpcm", 3: "ogg_vorbis"}

_INDEX = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
_STEP = (7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
         88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
         658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
         3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
         16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767)


class MediaError(ValueError):
    """A bounded media validation failure; no input bytes in the message."""


def adpcm_frames(size, channels):
    """Frames an Xbox ADPCM payload of `size` bytes decodes to; rejects partial blocks."""
    if channels not in (1, 2):
        raise MediaError("unsupported channel count")
    block = ADPCM_BLOCK_BYTES * channels
    if size % block:
        raise MediaError("ADPCM payload is not a whole number of blocks")
    return size // block * ADPCM_BLOCK_SAMPLES


def decode_xbox_adpcm(data, channels):
    """Xbox ADPCM -> list of interleaved signed 16-bit samples."""
    frames = adpcm_frames(len(data), channels)
    out = [0] * (frames * channels)
    block_bytes = ADPCM_BLOCK_BYTES * channels
    for block in range(len(data) // block_bytes):
        base = block * block_bytes
        first = block * ADPCM_BLOCK_SAMPLES
        for channel in range(channels):
            predictor = data[base + 4 * channel] | data[base + 4 * channel + 1] << 8
            if predictor & 0x8000:
                predictor -= 0x10000
            index = min(data[base + 4 * channel + 2], 88)
            out[first * channels + channel] = predictor
            for group in range(8):
                nibbles = base + 4 * channels + (group * channels + channel) * 4
                for k in range(4):
                    value = data[nibbles + k]
                    sample = group * 8 + k * 2 + 1
                    for nibble, position in ((value & 15, sample), (value >> 4, sample + 1)):
                        if position >= ADPCM_BLOCK_SAMPLES:
                            continue
                        step = _STEP[index]
                        diff = step >> 3
                        if nibble & 1:
                            diff += step >> 2
                        if nibble & 2:
                            diff += step >> 1
                        if nibble & 4:
                            diff += step
                        predictor += -diff if nibble & 8 else diff
                        predictor = max(-32768, min(32767, predictor))
                        index = max(0, min(88, index + _INDEX[nibble]))
                        out[(first + position) * channels + channel] = predictor
    return out


def pcm16_be(samples):
    """Signed 16-bit samples as explicit big-endian bytes."""
    return b"".join(struct.pack(">h", s) for s in samples)


def pcm16le_to_be(data, channels):
    if len(data) % (2 * channels):
        raise MediaError("PCM payload is not a whole number of frames")
    out = bytearray(len(data))
    out[0::2], out[1::2] = data[1::2], data[0::2]
    return bytes(out)


# ---- container headers --------------------------------------------------------
BINK_AUDIO_16BIT, BINK_AUDIO_STEREO, BINK_AUDIO_DCT = 0x4000, 0x2000, 0x1000


def parse_bink(header, file_size):
    """Decode a Bink 1 file header (FFmpeg libavformat/bink.c field order)."""
    if len(header) < 44 or header[:3] != b"BIK":
        raise MediaError("not a Bink 1 header")
    revision = chr(header[3]) if 0x61 <= header[3] <= 0x7A else None
    if revision is None:
        raise MediaError("Bink revision byte outside a-z")
    size_minus_8, frames, largest, _, width, height, fps_num, fps_den, video_flags, tracks = \
        struct.unpack_from("<10I", header, 4)
    if size_minus_8 + 8 != file_size:
        raise MediaError("Bink size field disagrees with the file size")
    if not 0 < frames <= 1_000_000 or not 0 < width <= 4096 or not 0 < height <= 4096 or not fps_den:
        raise MediaError("Bink header fields outside bounds")
    if tracks > 256 or len(header) < 44 + 12 * tracks:
        raise MediaError("Bink audio track table outside header")
    audio = []
    for i in range(tracks):
        rate, flags = struct.unpack_from("<HH", header, 44 + 4 * tracks + 4 * i)
        audio.append({"sample_rate": rate, "channels": 2 if flags & BINK_AUDIO_STEREO else 1,
                      "transform": "dct" if flags & BINK_AUDIO_DCT else "rdft",
                      "bits": 16 if flags & BINK_AUDIO_16BIT else 8})
    return {"container": "bink", "codec": "bink1-video", "revision": revision, "frames": frames,
            "largest_frame_bytes": largest, "width": width, "height": height,
            "fps": [fps_num, fps_den], "duration_seconds": round(frames * fps_den / fps_num, 3),
            "video_flags": video_flags, "audio_tracks": audio,
            "average_bitrate_kbps": round(file_size * 8 / (frames * fps_den / fps_num) / 1000, 1)}


def sniff(name, header, file_size):
    """Identify a non-map file's container from its leading bytes."""
    if header[:3] == b"BIK":
        return parse_bink(header, file_size)
    if header[:4] == b"RIFF" and header[8:12] == b"WAVE" and len(header) >= 36:
        fmt, channels, rate = struct.unpack_from("<HHI", header, 20)
        bits = struct.unpack_from("<H", header, 34)[0]
        codecs = {1: "pcm", 0x69: "xbox_adpcm", 0x11: "ima_adpcm"}
        return {"container": "riff-wave", "codec": codecs.get(fmt, "format-0x%04x" % fmt), "channels": channels,
                "sample_rate": rate, "bits": bits}
    if header[:16] == bytes.fromhex("3026b2758e66cf11a6d900aa0062ce6c"):
        return {"container": "asf", "codec": "wma (by extension; stream header not parsed)"}
    if header[:8] == b"\x89PNG\r\n\x1a\n":
        width, height = struct.unpack_from(">II", header, 16)
        return {"container": "png", "width": width, "height": height}
    if header[:4] == b"XPR0":
        return {"container": "xpr0", "codec": "xbox packed resource"}
    if header[:4] == b"XBEH":
        return {"container": "xbe", "codec": "xbox executable"}
    return {"container": "unknown"}
