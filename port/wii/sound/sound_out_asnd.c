/*
SOUND_OUT_ASND.C

HWI-074: the mix's output on the Wii (wii_sound.h): one ASND voice (libogc's
libasnd, BSD-3; its notice ships with releases, HWI-064), 16-bit stereo at
48 kHz, the DSP's own rate, so ASND neither resamples nor mixes anything
else. Built into engine.dol only (libogc).

The mixer (dsound_wii.c) writes on the game's thread, as much as the game's
clock has moved; the voice takes a buffer at a time from the DSP's interrupt
(ASND's voice callback). Between them, a ring of OUTPUT_BUFFERS buffers of
OUTPUT_BUFFER_FRAMES frames:
- a buffer is one of ASND's mixing ticks (ASND_GetSamplesPerTick, 1024
  frames): the voice asks for its next buffer once a tick, so a smaller
  buffer starves it (measured: 512 frames played at half the rate);
- the voice starts once OUTPUT_START_BUFFERS are full (the latency, about
  64 ms: a frame of two ticks, 67 ms, then drains no more than is waiting),
  and from then on is always given a buffer: the next full one, or,
  when the mixer has not filled one (an underrun: the game's frame came late),
  a silent one, counted, so the voice never stops and the game's sound
  resumes on its next buffer;
- a buffer the mixer would write with the ring full is dropped (the game's
  clock ran ahead of the DSP's, or the frame took longer than the ring), counted.
Nothing here waits on the other side; the game never blocks on the sound.
*/

#include <gccore.h>
#include <asndlib.h>
#include <string.h>

#include "wii_sound.h"

#define OUTPUT_VOICE 0
#define OUTPUT_BUFFER_FRAMES 1024
#define OUTPUT_BUFFERS 12
#define OUTPUT_START_BUFFERS 3
#define OUTPUT_BUFFER_BYTES (OUTPUT_BUFFER_FRAMES * 4)

static short output_ring[OUTPUT_BUFFERS][OUTPUT_BUFFER_FRAMES * 2] __attribute__((aligned(32)));
static short output_silence[OUTPUT_BUFFER_FRAMES * 2] __attribute__((aligned(32)));

static struct
{
	int open;
	int voice_started;
	/* the buffer the mixer fills, and how far; buffers full and waiting;
	the next the voice takes */
	unsigned int fill_index;
	unsigned long fill_frames;
	volatile unsigned int ready;
	volatile unsigned int play_index;
	volatile int underrun_run;
	struct wii_sound_output_report report;
} output;

/* ASND, from the DSP's interrupt: the voice can take its next buffer */
static void output_voice_callback(s32 voice)
{
	if (output.ready)
	{
		if (ASND_AddVoice(voice, output_ring[output.play_index], OUTPUT_BUFFER_BYTES) == SND_OK)
		{
			output.play_index = (output.play_index + 1) % OUTPUT_BUFFERS;
			output.ready--;
			output.report.frames_played += OUTPUT_BUFFER_FRAMES;
			output.underrun_run = 0;
		}
		return;
	}
	if (ASND_AddVoice(voice, output_silence, OUTPUT_BUFFER_BYTES) == SND_OK)
	{
		output.report.underruns++;
		if (!output.underrun_run)
			output.report.underrun_runs++;
		output.underrun_run++;
		if ((unsigned long)output.underrun_run > output.report.longest_underrun)
			output.report.longest_underrun = (unsigned long)output.underrun_run;
	}
}

static void output_start_voice(void)
{
	u32 level = IRQ_Disable();
	unsigned int first = output.play_index;

	output.play_index = (output.play_index + 1) % OUTPUT_BUFFERS;
	output.ready--;
	output.report.frames_played += OUTPUT_BUFFER_FRAMES;
	output.voice_started = 1;
	IRQ_Restore(level);
	ASND_SetVoice(OUTPUT_VOICE, VOICE_STEREO_16BIT, WII_SOUND_RATE, 0, output_ring[first], OUTPUT_BUFFER_BYTES, 255,
		255, output_voice_callback);
}

static int output_open(unsigned long rate)
{
	if (rate != WII_SOUND_RATE)
		return 0;
	memset(&output, 0, sizeof(output));
	memset(output_silence, 0, sizeof(output_silence));
	DCFlushRange(output_silence, sizeof(output_silence));
	output.report.buffer_frames = OUTPUT_BUFFER_FRAMES;
	output.report.buffers = OUTPUT_BUFFERS;
	output.report.latency_target_frames = OUTPUT_START_BUFFERS * OUTPUT_BUFFER_FRAMES;
	output.report.fill_minimum = (unsigned long)-1;
	ASND_Init();
	ASND_Pause(0);
	output.report.samples_per_tick = ASND_GetSamplesPerTick();
	output.open = 1;
	return output.report.samples_per_tick <= OUTPUT_BUFFER_FRAMES;
}

static void output_write(const short *frames, unsigned long count)
{
	if (!output.open)
		return;
	output.report.frames_written += count;
	while (count)
	{
		unsigned long room = OUTPUT_BUFFER_FRAMES - output.fill_frames;
		unsigned long take = count < room ? count : room;
		unsigned long waiting;

		/* the ring full (every other buffer waiting for the voice): the
		newest frames are dropped */
		if (output.ready >= OUTPUT_BUFFERS - 1)
		{
			output.report.frames_dropped += count;
			return;
		}
		memcpy(&output_ring[output.fill_index][output.fill_frames * 2], frames, take * 4);
		output.fill_frames += take;
		frames += take * 2;
		count -= take;
		if (output.fill_frames == OUTPUT_BUFFER_FRAMES)
		{
			u32 level;

			DCFlushRange(output_ring[output.fill_index], OUTPUT_BUFFER_BYTES);
			output.fill_index = (output.fill_index + 1) % OUTPUT_BUFFERS;
			output.fill_frames = 0;
			level = IRQ_Disable();
			output.ready++;
			IRQ_Restore(level);
			if (!output.voice_started && output.ready >= OUTPUT_START_BUFFERS)
				output_start_voice();
		}
		waiting = output.ready * OUTPUT_BUFFER_FRAMES + output.fill_frames;
		if (output.voice_started)
		{
			if (waiting < output.report.fill_minimum)
				output.report.fill_minimum = waiting;
			if (waiting > output.report.fill_maximum)
				output.report.fill_maximum = waiting;
		}
	}
}

static void output_close(void)
{
	if (!output.open)
		return;
	ASND_StopVoice(OUTPUT_VOICE);
	ASND_Pause(1);
	ASND_End();
	output.open = 0;
}

static void output_report(struct wii_sound_output_report *report)
{
	u32 level = IRQ_Disable();

	*report = output.report;
	IRQ_Restore(level);
	if (report->fill_minimum == (unsigned long)-1)
		report->fill_minimum = 0;
}

static void output_mark(void)
{
	u32 level = IRQ_Disable();
	unsigned long frames = output.report.buffer_frames, buffers = output.report.buffers;
	unsigned long latency = output.report.latency_target_frames, tick = output.report.samples_per_tick;

	memset(&output.report, 0, sizeof(output.report));
	output.report.buffer_frames = frames;
	output.report.buffers = buffers;
	output.report.latency_target_frames = latency;
	output.report.samples_per_tick = tick;
	output.report.fill_minimum = (unsigned long)-1;
	output.underrun_run = 0;
	IRQ_Restore(level);
}

static const struct wii_sound_output asnd_output =
{
	output_open,
	output_write,
	output_close,
	output_report,
	output_mark,
};

/* the driver's (wii_engine_main.c): the mix goes to ASND */
void wii_sound_use_asnd(void)
{
	wii_sound_set_output(&asnd_output);
}
