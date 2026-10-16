"""HWI-074: the Wii's sound (port/wii/sound): the sound pack (tools/wii/sound_stage.py
and sound_pack.c), the ADPCM decoder against the host reference
(media_formats.decode_xbox_adpcm), the plan's sound line, the build's units."""

import contextlib
import io
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))

import content_convert as cc  # noqa: E402
import content_fixtures as fx  # noqa: E402
import engine_build  # noqa: E402
import engine_unsupported  # noqa: E402
import halo_cache as hc  # noqa: E402
import media_formats as mf  # noqa: E402
import run_dolphin  # noqa: E402
import sound_stage as ss  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SOUND = ROOT / "port" / "wii" / "sound"


def compiler():
    for name in ("gcc", "cc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


def reference_crc(payload, channels):
    samples = mf.decode_xbox_adpcm(payload, channels)
    return zlib.crc32(struct.pack("<%dh" % len(samples), *samples)) & 0xFFFFFFFF


def random_adpcm(blocks, channels, seed, wild_index=False):
    """blocks of random nibbles and headers; wild_index: step indices past 88
    (the decoder clamps them, as the Xbox's does)"""
    generator = random.Random(seed)
    data = bytearray(generator.getrandbits(8) for _ in range(36 * channels * blocks))
    for block in range(blocks):
        for channel in range(channels):
            at = 36 * channels * block + 4 * channel
            data[at + 2] = generator.randrange(256) if wild_index else generator.randrange(89)
            data[at + 3] = 0
    return bytes(data)


@unittest.skipUnless(compiler(), "no host C compiler")
class SoundHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.exe = cls.root / ("sound_check.exe" if sys.platform == "win32" else "sound_check")
        build = subprocess.run([compiler(), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
                                "-D__USE_MINGW_ANSI_STDIO=1", f"-I{ROOT / 'port/wii/engine'}", "-o", str(cls.exe),
                                *(str(SOUND / name) for name in ("sound_check.c", "sound_pack.c", "xbox_adpcm.c"))],
                               capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError("host build failed:\n" + build.stderr[-4000:])
        # fixture maps: one sound's permutations in Xbox ADPCM (alpha), and in
        # PCM (beta: a sound's permutations share its compression)
        sounds = [(0, 0, 1, fx.adpcm_payload(3, 1, 5)), (0, 0, 1, fx.adpcm_payload(2, 1, 6)),
                  (0, 0, 1, fx.adpcm_payload(70, 1, 9))]
        pcm = [(1, 1, 0, bytes(range(256)) * 4), (1, 1, 0, bytes(range(255, -1, -1)) * 2)]
        staging = fx.write_staging(cls.root / "staging", {"alpha": fx.build_map("alpha", sounds=sounds),
                                                          "beta": fx.build_map("beta", sounds=pcm)})
        cls.out = cls.root / "content"
        cc.run(staging, None, ["sounds"], "gx-baseline-v1", cls.out)
        cls.generation = cls.out / "generations" / (cls.out / "CURRENT").read_text(encoding="ascii").split()[0]
        cls.maps = staging / "maps"
        cls.map = cls.maps / "alpha.map"

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_check(self, *arguments):
        result = subprocess.run([str(self.exe), *map(str, arguments)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.splitlines()

    def test_decoder_equals_the_host_reference_bit_for_bit(self):
        cases = [(1, 1, 1, False), (1, 40, 2, False), (2, 1, 3, False), (2, 33, 4, False), (1, 25, 5, True),
                 (2, 25, 6, True), (1, 3, 7, False)]
        for channels, blocks, seed, wild in cases:
            payload = random_adpcm(blocks, channels, seed, wild)
            # the extremes: the largest step from the first nibble, both signs
            if seed == 7:
                payload = bytearray(payload)
                payload[0:4] = struct.pack("<hBB", 32767, 88, 0)
                payload[4:36] = bytes([0x77] * 32)
                payload[36:40] = struct.pack("<hBB", -32768, 88, 0)
                payload[40:72] = bytes([0xFF] * 32)
                payload = bytes(payload)
            path = self.root / f"adpcm-{channels}-{seed}.bin"
            path.write_bytes(payload)
            line = self.run_check("decode", channels, path)[0]
            self.assertEqual(line, f"DECODE frames={blocks * 64} crc={reference_crc(payload, channels):08x}",
                             (channels, blocks, seed))

    def staged_pack(self, name="alpha.wsnd", map_name="alpha"):
        cache = hc.open_map(self.maps / f"{map_name}.map")
        entries = ss.plan(cache, map_name, self.generation)
        path = self.root / name
        path.write_bytes(ss.pack_bytes(map_name, self.generation.name, entries))
        return path, entries

    def test_every_permutation_is_served_as_the_map_holds_it(self):
        for map_name, codec in (("alpha", 1), ("beta", 2)):
            with self.subTest(map_name):
                self.check_pack(map_name, codec)

    def check_pack(self, map_name, expected_codec):
        path, entries = self.staged_pack(f"{map_name}.wsnd", map_name)
        cache = hc.open_map(self.maps / f"{map_name}.map")
        lines = self.run_check("pack", path, map_name)
        reads = [line for line in lines if line.startswith("READ ") and "crc=" in line]
        self.assertEqual(len(reads), len(entries))
        for line, (offset, size, crc, container, codec) in zip(reads, entries):
            original = bytes(cache.file_span(offset, size))
            self.assertEqual(line, f"READ offset={offset:08x} size={size} result=1 crc={zlib.crc32(original):08x}")
        # the decoded PCM logged equals the host reference's
        decodes = [line for line in lines if line.startswith("SOUND_DECODE ")]
        self.assertEqual(len(decodes), len(entries))
        for line, (offset, size, crc, container, codec) in zip(decodes, entries):
            frames, expected = ss.decoded_crc(container)
            self.assertIn(f"offset={offset:08x} codec={codec}", line)
            self.assertTrue(line.endswith(f"frames={frames} crc={expected:08x}"), line)
        self.assertIn(f"PACK entries={len(entries)} reads={len(entries)}", lines[-1])
        self.assertTrue(lines[-1].endswith("read_failures=0"), lines[-1])
        # a read of another size is refused; an offset with no entry is not the pack's
        refusals = [line for line in lines if line.startswith("READ ") and "crc=" not in line]
        self.assertTrue(refusals[0].endswith("result=-1"), refusals[0])
        self.assertTrue(refusals[1].endswith("result=0"), refusals[1])
        self.assertEqual({codec for *_rest, codec in entries}, {expected_codec})

    def test_damage_and_another_maps_pack_are_refused(self):
        path, entries = self.staged_pack("damaged.wsnd")
        data = bytearray(path.read_bytes())
        info, read = ss.read_pack(path)
        self.assertEqual((info["map"], info["entries"]), ("alpha", len(entries)))
        # a sample byte flipped: that read fails its CRC; the others still pass
        offset, size, crc, container, codec = entries[0]
        at = data.find(container)
        data[at + 40] ^= 0xFF
        path.write_bytes(bytes(data))
        lines = self.run_check("pack", path, "alpha")
        self.assertIn(f"READ offset={offset:08x} size={size} result=-1 crc=00000000", lines)
        self.assertTrue(any(line.startswith("SOUND_ERROR pack=alpha") for line in lines))
        self.assertTrue(lines[-1].endswith("read_failures=1"), lines[-1])
        # another map's name, or a damaged header: not opened
        self.assertEqual(self.run_check("pack", path, "beta"), ["SOUND_PACK map=beta open=0 reason=\"damaged or "
                                                                 "another map's: stage it again\"", "OPEN result=0"])
        data[20] ^= 1
        path.write_bytes(bytes(data))
        self.assertIn("OPEN result=0", self.run_check("pack", path, "alpha"))

    def test_staging_refuses_a_container_that_is_not_the_maps(self):
        cache = hc.open_map(self.map)
        victim = sorted((self.generation / "sounds" / "alpha").glob("*.hws"))[0]
        original = victim.read_bytes()
        try:
            changed = bytearray(original)
            changed[-1] ^= 1
            victim.write_bytes(bytes(changed))
            with self.assertRaises(ss.StageError):
                ss.plan(cache, "alpha", self.generation)
        finally:
            victim.write_bytes(original)
        self.assertTrue(ss.plan(cache, "alpha", self.generation))

    def test_publish_is_atomic_and_recorded(self):
        output = self.root / "published"
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            code = ss.main(["--map", str(self.map), "--generation", str(self.generation), "--output", str(output)])
        self.assertIn('"entries": 3', printed.getvalue())
        self.assertEqual(code, 0)
        self.assertTrue((output / "alpha.wsnd").is_file())
        self.assertTrue((output / "alpha.wsnd.json").is_file())
        self.assertFalse(any(name.name.endswith(".tmp") for name in output.iterdir()))


class SoundPackFormatTests(unittest.TestCase):
    def test_layout_matches_the_reader(self):
        container = ss_container(1, 22050, 1, fx.adpcm_payload(2, 1, 3))
        data = ss.pack_bytes("m", "0123456789abcdef", [(0x800, len(container) - 32, 7, container, 1)])
        self.assertEqual(data[:8], b"HWSPACK1")
        self.assertEqual(struct.unpack_from(">I", data, 60)[0], zlib.crc32(data[:60]) & 0xFFFFFFFF)
        offset, size, at, length, crc, codec, zero = struct.unpack_from(">IIIIIHH", data, 64 + 4)
        self.assertEqual((offset, size, length, crc, codec, zero), (0x800, len(container) - 32, len(container), 7, 1, 0))
        self.assertEqual(at % 32, 0)
        self.assertEqual(data[at:at + length], container)

    def test_names_that_do_not_fit_are_refused(self):
        with self.assertRaises(ss.StageError):
            ss.pack_bytes("a-name-of-sixteen", "0123456789abcdef", [])


def ss_container(codec, rate, channels, payload):
    frames = len(payload) // (36 * channels) * 64 if codec == 1 else len(payload) // (2 * channels)
    header = struct.pack(">4sHHIHHII", b"HWS1", 1, codec, rate, channels, 0, frames, len(payload))
    return header + bytes(32 - len(header)) + payload


class CompareSoundRunsTests(unittest.TestCase):
    RUN = ("SOUND_RUN map=prisoner cycle=0 cadence=30hz frames=300 clock_ms=10000 work_calls=300 frames_mixed=480096 "
           "mix_end_frame=960000 skipped_frames=0 voices_max=14 voice_frames=2046672 voices_by_calls=1,2 sounds_peak=14 "
           "looping_sounds_peak=0 streams=81 packets_queued=465 packets_completed=207 packets_flushed=220 "
           "blocks_decoded=14388 starts=134 ends=53 stops=68 obstruction_tests=1763 marks_unrestored=0 "
           "notifications_withheld=0 mouth_withheld=0 indices_withheld=42 clipped=0 mix_us={us} mix_peak_us=900 "
           "mix_crc={crc} events_crc=11111111 pack_reads=46 pack_bytes=907416 pack_pcm=0 pack_failures=0 pack_us=5 "
           "decode_checks=46")

    def logs(self, wii_crc, wii_us=100, host_us=7):
        import compare_sound_runs as csr
        return csr, csr.parse_text(self.RUN.format(us=wii_us, crc=wii_crc)), \
            csr.parse_text(self.RUN.format(us=host_us, crc="aaaaaaaa"))

    def test_timings_are_reported_and_decisions_compared(self):
        csr, wii, host = self.logs("aaaaaaaa")
        rows, mismatches = csr.compare_runs(wii, host)
        self.assertEqual(mismatches, 0)
        self.assertEqual(rows[0]["wii_timings"]["mix_us"], "100")
        csr, wii, host = self.logs("bbbbbbbb")
        rows, mismatches = csr.compare_runs(wii, host)
        self.assertEqual(mismatches, 1)
        self.assertEqual(rows[0]["different"], ["mix_crc"])

    def test_split_at_the_sound_start(self):
        import compare_sound_runs as csr
        text = ("BEGIN target=engine\nFIXED_STEP pass=1\nMAP_LINE map=prisoner\nRUN map=prisoner cycle=0 cadence=30hz\n"
                "SOUND_START rate=48000\nSOUND_MAP start=1\nMAP_LINE map=prisoner\nRUN map=prisoner cycle=0 cadence=30hz\n"
                "END target=engine\n")
        without, with_sound = csr.split(text)
        self.assertTrue(without.endswith("RUN map=prisoner cycle=0 cadence=30hz\n"))
        self.assertTrue(with_sound.startswith("BEGIN target=engine\nFIXED_STEP pass=1\nSOUND_START rate=48000\n"))
        self.assertEqual(with_sound.count("RUN map="), 1)
        self.assertEqual(csr.split("no sound\n"), ("no sound\n", ""))


class SoundBuildTests(unittest.TestCase):
    def test_audio_dump_option(self):
        files = run_dolphin.profile_files("D3D", dump_audio=True)
        self.assertIn("[DSP]\nDumpAudio = True\nDumpAudioSilent = True\n", files["Dolphin.ini"])
        self.assertNotIn("DumpAudio", run_dolphin.profile_files("D3D")["Dolphin.ini"])
        run_dolphin.check_stock_limits(files)

    def test_units_and_wraps(self):
        names = {path.name for path in engine_build.WII_PLATFORM}
        self.assertTrue({"dsound_wii.c", "sound_pack.c", "xbox_adpcm.c"} <= names)
        self.assertIn(Path("port/wii/sound/sound_hooks.c"), engine_build.ENGINE_AUTHORED)
        self.assertEqual(engine_build.SOUND_OUTPUT, (Path("port/wii/sound/sound_out_asnd.c"),))
        self.assertNotIn(Path("port/wii/sound/sound_out_asnd.c"), engine_build.WII_PLATFORM)
        for name in ("observer_get_camera", "sound_render", "sound_idle", "game_sound_update", "sound_new_impulse",
                     "sound_dispose_from_old_map", "compute_sound_obstruction", "object_type_notify_impulse_sound",
                     "game_sound_set_mouth_aperture", "sound_refresh_looping", "object_impulse_sound_new"):
            self.assertIn(f"-Wl,--wrap={name}", engine_build.LINK_FLAGS)
            self.assertIn(f"__wrap_{name}(", (SOUND / "sound_hooks.c").read_text(encoding="utf-8"), name)
        # (HWI-078) the class fades are the game state's on every port: no copy of them
        for name in ("sound_classes_update", "sound_class_get_gain"):
            self.assertNotIn(f"-Wl,--wrap={name}", engine_build.LINK_FLAGS)
            self.assertNotIn(f"__wrap_{name}(", (SOUND / "sound_hooks.c").read_text(encoding="utf-8"), name)
        for path in (*engine_build.SOUND_OUTPUT, *(SOUND / name for name in ("wii_sound.h", "xbox_adpcm.h"))):
            self.assertTrue((ROOT / path).is_file(), path)

    def test_directsound_is_implemented_not_stubbed(self):
        self.assertNotIn("dsound", engine_unsupported.SUBSYSTEMS)
        stubs = (ROOT / "port/wii/engine/unsupported_sdk.c").read_text(encoding="utf-8")
        port = (ROOT / "port/wii/engine/unsupported_port.c").read_text(encoding="utf-8")
        mixer = (SOUND / "dsound_wii.c").read_text(encoding="utf-8")
        for name in ("DirectSoundCreate", "DirectSoundDoWork", "IDirectSoundStream_SetVolume",
                     "IDirectSound_CreateSoundStream", "DirectSoundStopStream", "DirectSoundGetStreamVoiceStatus"):
            self.assertNotIn(f" {name}(", stubs + port, name)
            self.assertIn(f" {name}(", mixer, name)

    def test_the_sound_random_stream_is_the_wii_builds_only(self):
        for prefix in ("port/wii/engine/include/halo_wii_prefix.h", "port/wii/engine/include/halo_wii_host_prefix.h"):
            self.assertIn("#define HALO_SOUND_RANDOM_STREAM 1", (ROOT / prefix).read_text(encoding="utf-8"))
        for other in ("port/linux/include/halo_linux_prefix.h",):
            self.assertNotIn("HALO_SOUND_RANDOM_STREAM", (ROOT / other).read_text(encoding="utf-8"))
        for unit in ("source/sound/sound_manager.c", "source/sound/sound_definitions.c"):
            text = (ROOT / unit).read_text(encoding="latin-1")
            self.assertNotIn("get_global_local_random_seed_address()", text)
            self.assertNotIn("real_local_random_range(", text)

    def test_the_plan_takes_a_sound_line(self):
        text = (ROOT / "port/wii/engine/engine_map_run.c").read_text(encoding="utf-8")
        self.assertIn('!strncmp(line, "sound", 5)', text)
        for option in ('"listener"', '"verify"', '"events"', '"couplings"'):
            self.assertIn(option, text)


@unittest.skipUnless(compiler(), "no host C compiler")
class FadeTickTests(unittest.TestCase):
    """HWI-078: the scripted sound class fades on the game's ticks (port/wii/sound/fade_check.c, with the
    engine's own sound_classes.c)"""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        include = root / "include"
        include.mkdir()
        # the engine headers sound_classes.c includes: fade_check.c defines what it uses of them
        for name in ("cseries.h", "errors.h", "game_state.h", "real_math.h", "objects.h"):
            (include / name).write_text("", encoding="ascii")
        exe = root / ("fade_check.exe" if sys.platform == "win32" else "fade_check")
        build = subprocess.run([compiler(), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
                                "-D__USE_MINGW_ANSI_STDIO=1", f"-I{include}", "-o", str(exe),
                                str(SOUND / "fade_check.c"), "-lm"], capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError("host build failed:\n" + build.stderr[-4000:])
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        cls.exit = run.returncode
        cls.output = run.stdout
        cls.lines = {}
        for line in run.stdout.splitlines():
            kind, _, rest = line.partition(" ")
            values = dict(item.split("=", 1) for item in rest.split())
            cls.lines.setdefault(kind, []).append(values)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_the_ticks_fades_equal_the_xboxs_at_30hz_on_every_tick(self):
        self.assertEqual(self.exit, 0, self.output)
        xbox = self.lines["FADE_XBOX"][0]
        thirty = next(run for run in self.lines["FADE_TICK"] if run["cadence"] == "30hz")
        self.assertEqual((thirty["ticks"], thirty["frames"]), ("240", "240"))
        self.assertEqual(thirty["first_divergent"], "-1")
        self.assertEqual(thirty["chain"], xbox["chain"])

    def test_the_fades_are_measurably_present(self):
        for run in self.lines["FADE_TICK"]:
            self.assertGreater(int(run["fade_ticks"]), 0, run)
            self.assertGreater(int(run["fade_class_steps"]), int(run["fade_ticks"]), run)

    def test_every_cadence_and_a_revert_follow_the_ticks_alone(self):
        runs = self.lines["FADE_TICK"]
        self.assertEqual([run["cadence"] for run in runs], ["30hz", "60hz", "20hz", "144hz", "irregular", "vsync"])
        self.assertEqual({run["chain"] for run in runs}, {self.lines["FADE_XBOX"][0]["chain"]})
        self.assertEqual({run["first_divergent"] for run in runs}, {"-1"})
        self.assertEqual(len({(run["fade_ticks"], run["fade_class_steps"]) for run in runs}), 1)
        # (frames differ: the cadences are really different)
        self.assertEqual(len({run["frames"] for run in runs}), 5)
        self.assertEqual(self.lines["FADE_REVERT"][0]["first_divergent"], "-1")
        self.assertEqual(self.lines["FADE_RESULT"][0], {"failures": "0", "frame_coupled_differing": "2",
                                                        "assertions_failed": "0"})

    def test_the_frame_coupling_it_replaces_follows_the_frame_rate(self):
        """the check can see a frame-rate dependence: HWI-074's per-frame fades diverge at 20 Hz and irregular"""
        frame = {run["cadence"]: run for run in self.lines["FADE_FRAME"]}
        self.assertEqual(frame["30hz"]["first_divergent"], "-1")
        for cadence in ("20hz", "irregular"):
            self.assertNotEqual(frame[cadence]["first_divergent"], "-1", cadence)
            self.assertNotEqual(frame[cadence]["chain"], self.lines["FADE_XBOX"][0]["chain"], cadence)


class FadeTickPathTests(unittest.TestCase):
    """HWI-078: where the engine moves the fades (every port's game_time.c and sound_manager.c)"""

    def test_each_tick_moves_the_fades_and_a_frame_does_not(self):
        game_time = (ROOT / "source/game/game_time.c").read_text(encoding="latin-1")
        tick = game_time.index("\t\t\t\t\t\tgame_tick();\n")
        loop = game_time[tick:game_time.index("game_time_globals->server_time++;", tick)]
        self.assertIn("local_random_tick_end();", loop)
        self.assertLess(loop.index("local_random_tick_end();"), loop.index("sound_tick_update();"))
        self.assertEqual(game_time.count("sound_tick_update();"), 1)
        manager = (ROOT / "source/sound/sound_manager.c").read_text(encoding="latin-1")
        render = manager[manager.index("\nvoid sound_render(\n"):]
        render = render[:render.index("\n}\n")]
        self.assertNotIn("sound_classes_update(", render)
        self.assertNotIn("leftover_ticks", render)
        step = manager[manager.index("\nvoid sound_tick_update(\n"):]
        step = step[:step.index("\n}\n")]
        self.assertIn("sound_classes_update(1);", step)
        for gate in ("sound_manager_globals.initialized", "sound_manager_globals.enabled",
                     "!sound_manager_globals.paused"):
            self.assertIn(gate, step)
        self.assertEqual(manager.count("sound_classes_update("), 1)

    def test_the_run_reports_the_fades(self):
        hooks = (SOUND / "sound_hooks.c").read_text(encoding="utf-8")
        self.assertIn("fade_ticks=%lu fade_class_steps=%lu", hooks)
        self.assertIn("sound_tick_report_reset();", hooks)
        import compare_sound_runs as csr
        self.assertIn("fade_ticks", csr.DETERMINISTIC)
        self.assertIn("fade_class_steps", csr.DETERMINISTIC)


if __name__ == "__main__":
    unittest.main()
