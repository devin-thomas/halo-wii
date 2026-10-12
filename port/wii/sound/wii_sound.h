/*
WII_SOUND.H

HWI-074: the Wii's sound (port/wii/sound/README.md). The engine's own sound
manager (source/sound/sound_manager.c) and its Xbox platform
(source/sound/sound_dsound_xbox.c) run unchanged; what they call of the Xbox's
DirectSound is implemented here (dsound_wii.c) as a software mixer that runs
on the game's thread, on the game's clock, and writes 48 kHz 16-bit stereo to
an output: ASND on the Wii (sound_out_asnd.c), none on the i686 host
reference (the mix is only hashed).

Only C types here: the driver includes it beside libogc, the engine-side
units beside the engine's headers, and the platform units beside the SDK's.
*/

#ifndef __HALO_WII_SOUND_H
#define __HALO_WII_SOUND_H

#ifdef __cplusplus
extern "C" {
#endif

#define WII_SOUND_RATE 48000
/* frames mixed at a time: the gains ramp over the first chunk of a mix */
#define WII_SOUND_CHUNK_FRAMES 256

/* ---------- the output (the driver's: ASND on the Wii) */

struct wii_sound_output_report
{
	unsigned long buffer_frames;      /* one output buffer */
	unsigned long buffers;            /* in the ring */
	unsigned long latency_target_frames;
	unsigned long samples_per_tick;   /* the voice's mixing tick (ASND) */
	unsigned long frames_written;     /* by the mixer */
	unsigned long frames_dropped;     /* the ring was full: the newest dropped */
	unsigned long frames_played;      /* handed to the voice */
	unsigned long underruns;          /* buffers the voice asked for that were not filled */
	unsigned long underrun_runs;      /* separate runs of underruns */
	unsigned long longest_underrun;   /* buffers in the longest run */
	unsigned long fill_minimum, fill_maximum; /* frames waiting, as the mixer wrote */
	unsigned long silent_start_buffers; /* underruns before the first frame was written */
};

struct wii_sound_output
{
	/* 1 if the output started; frames at the rate, interleaved stereo */
	int (*open)(unsigned long rate);
	void (*write)(const short *frames, unsigned long count);
	void (*close)(void);
	void (*report)(struct wii_sound_output_report *report);
	/* the counters start again (wii_sound_mark) */
	void (*mark)(void);
};

/* before DirectSoundCreate: the output the mix goes to (NULL: none) */
void wii_sound_set_output(const struct wii_sound_output *output);
/* (sound_out_asnd.c, engine.dol) the ASND voice as the output */
void wii_sound_use_asnd(void);

/* ---------- the mixer's report (dsound_wii.c) */

enum wii_sound_voice_kind
{
	_wii_sound_voice_2d_mono,
	_wii_sound_voice_3d_mono,
	_wii_sound_voice_stereo_22k,
	_wii_sound_voice_stereo_44k,
	_wii_sound_voice_other,
	NUMBER_OF_WII_SOUND_VOICE_KINDS
};

#define WII_SOUND_VOICE_HISTOGRAM 17      /* 0..15 voices, and 16 or more */

struct wii_sound_report
{
	int started;
	unsigned long streams;             /* created (the engine's actual channels) */
	unsigned long work_calls;          /* DirectSoundDoWork */
	unsigned long frames_mixed;
	unsigned long frames_total;        /* since the mixer started (not reset by a mark) */
	unsigned long clock_skipped_frames;/* a gap over the most one call mixes */
	unsigned long voices_maximum;      /* voices playing in one call */
	unsigned long voices_maximum_by_kind[NUMBER_OF_WII_SOUND_VOICE_KINDS];
	unsigned long voice_calls[WII_SOUND_VOICE_HISTOGRAM]; /* calls by voices playing */
	unsigned long voice_frames;        /* voices x frames mixed */
	unsigned long packets_queued, packets_completed, packets_flushed;
	unsigned long blocks_decoded;      /* Xbox ADPCM blocks (64 frames) */
	unsigned long starts, ends, stops; /* a stream starting, playing out, stopped early */
	unsigned long clipped_samples;
	unsigned long long mix_microseconds; /* mixing and decoding, the guest's clock */
	unsigned long long mix_microseconds_peak; /* the longest one call took */
	unsigned long mix_crc;             /* CRC-32 of every frame mixed (LE 16-bit) */
	unsigned long events_crc;          /* CRC-32 of the stream events in order */
};

/* the report since the mixer started, or since the last mark */
void wii_sound_report_get(struct wii_sound_report *report);
void wii_sound_mark(void);
/* the driver's output's own report; 0 if there is none */
int wii_sound_output_report_get(struct wii_sound_output_report *report);

/* (the host reference) the mix written to a file as it is made: raw 16-bit
little-endian stereo at WII_SOUND_RATE; NULL stops */
void wii_sound_set_capture(const char *path);

/* stream events: a stream starting (its first packet), playing out (its last
packet finished), stopped (flushed with packets left); context is the
packet's (the engine's sound permutation). The engine side names them. */
enum wii_sound_event
{
	_wii_sound_event_start,
	_wii_sound_event_end,
	_wii_sound_event_stop,
};

typedef void (*wii_sound_event_proc)(int event, int stream, int kind, const void *context);
void wii_sound_set_event_hook(wii_sound_event_proc proc);

/* the clock the mixer follows: the engine's system_milliseconds; with none,
the mixer mixes what its caller asks */
unsigned long wii_sound_clock_milliseconds(void);

/* ---------- the engine side (sound_hooks.c)

The map run's sound (engine_map_run.h's "sound" plan line): the engine's
sound starts once, before the first map (sound_initialize, as
shell_initialize starts it); each run's frame then renders it
(sound_render, as main_loop does) on a frame clock that the frames' time
advances, so the Wii and the i686 host mix and decide alike. The listener
follows the local player's unit (the engine's deterministic camera), for the
sound's readers only: the run's observer, which the simulation reads, is not
moved. */

struct wii_sound_map_options
{
	int listener_follows_player;     /* 1: as above; 0: the run's observer */
	int verify_decode;               /* log each permutation's decoded PCM CRC as it loads */
	int events;                      /* log each stream event (the first ones of each class) */
	/* the game's reactions to its sounds playing (sound_hooks.c): 0 (the
	default) keeps them as with no sound, 1 is the Xbox's */
	int game_couplings;
};

/* 1 if the engine's sound started */
int wii_sound_map_start(const struct wii_sound_map_options *options);
int wii_sound_map_active(void);
/* (real_map_scenario.c) a frame of dt seconds ran: the frame clock moves and
the sound renders */
void wii_sound_map_frame(float dt);
/* a run's start and its report lines (engine_map_run.c) */
void wii_sound_map_run_begin(void);
void wii_sound_map_run_end(const char *map, int cycle, const char *cadence);
/* (real_map_scenario.c's tag-slot digest) the sound's own fields in the sound
tags read as zeros between these, while the sound runs; begin returns 1 if
it masked them */
int wii_sound_tags_mask_begin(void);
void wii_sound_tags_mask_end(void);
/* the boot mode asks for sound (boot.txt sound=1) */
void wii_sound_set_wanted(int wanted);
int wii_sound_wanted(void);
/* the engine's sound started (sound_initialize ran with an output) */
int wii_sound_started(void);

/* ---------- the sound pack (sound_pack.c): a map's samples on the card */

/* a staged map's sounds: d:\maps\<name>.wsnd beside <name>.wmap
(tools/wii/sound_stage.py), opened with the map */
int wii_sound_pack_open(const char *path, const char *map_name);
void wii_sound_pack_close(void);
/* 1 if the pack serves the map file's bytes at offset (the sound samples
the cache asks for), read into buffer and checked; 0 if it has none there,
-1 if it has but they could not be read or failed their check */
int wii_sound_pack_read(long offset, long size, void *buffer);

struct wii_sound_pack_report
{
	int open;
	unsigned long entries;
	unsigned long reads, bytes, failures;
	unsigned long pcm_reads;          /* PCM permutations (restored to little-endian) */
	unsigned long long read_microseconds;
	unsigned long decode_checks;      /* permutations whose decoded PCM was logged */
};

void wii_sound_pack_report_get(struct wii_sound_pack_report *report);
/* log each read's decoded PCM CRC (SOUND_DECODE lines) */
void wii_sound_pack_set_verify(int verify);

#ifdef __cplusplus
}
#endif

#endif
