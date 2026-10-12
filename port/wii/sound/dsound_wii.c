/*
DSOUND_WII.C

HWI-074: the Xbox's DirectSound, as the engine's sound platform
(source/sound/sound_dsound_xbox.c) calls it, on the Wii: a software mixer
that runs on the game's thread and writes 48 kHz 16-bit stereo to the
driver's output (wii_sound.h; ASND on the Wii, sound_out_asnd.c). The engine's
sound manager decides what plays, on which of its channels, and their gains,
pitches, positions and occlusion, exactly as on the Xbox; this file is the
Xbox's voice processor's part.

Why the game's thread, and the game's clock. Every frame the engine's sound
platform calls DirectSoundDoWork (dsound_begin_scene); it mixes there the
frames that the engine's clock (system_milliseconds, 48 frames a
millisecond) says have passed since the last call, and completes the packets
it has played (the engine's stream callback, which refills them). So what
the engine sees of the sound (a channel playing out, a packet done) follows
only the engine's own clock, as the game's frames do: under the map run's
frame clock the Wii and the i686 host reference mix the same frames and
call back the same packets at the same frames (wii_sound_report_get's CRCs),
and nothing the game decides waits on the output's timing. The output buffers
the mix for the voice (sound_out_asnd.c), and a voice that finds it empty
plays silence (an underrun, counted there).

What is mixed, as port/linux/src/dsound_sdl.c does (the same DirectSound
semantics), but in integers, so the two compilers' and machines' mixes are
the same bit for bit:
- the game's streams: Xbox ADPCM (mono or stereo, 22 or 44 kHz), decoded one
  64-frame block at a time as the mixer reaches it (xbox_adpcm.c), and
  16-bit PCM; a packet is read where the engine's sound cache holds it, and
  completed once it has played (the cache keeps it until then);
- pitch: each voice resampled to the output rate (SetFrequency) by linear
  interpolation, in 16.16 fixed point;
- gains (14 fractional bits), ramped over a chunk's 256 frames when they
  change: the stream volume (millibels); for 2D voices the front left and
  right mix bin volumes; for 3D voices DirectSound's inverse distance
  rolloff between the minimum and maximum distance (held beyond the
  maximum), an equal power pan from the source's direction in listener
  space, and the I3DL2 direct path with obstruction and occlusion, their
  high frequency level a one-pole low pass at the listener's HF reference;
- the sum clipped to 16 bits.
Not modelled (as the Linux port): Doppler, cones; nor the I3DL2 reverb the
Xbox's DSP ran (the Linux port's), which costs too much of the Wii's CPU for
this first sound: a reverb is HWI-040's to decide.

The gains are worked out in single-precision floats from the engine's floats
(powf, sqrtf and the pan's cosf/sinf, the port's musl maths on both
machines) and rounded to integers; everything after is integer.
*/

#include "platform.h"
#include "wii_platform.h"
#include "wii_sound.h"
#include "xbox_adpcm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the engine's clock (source/cseries/cseries_windows.c), which the boot
hooks' frame clock wraps (port/wii/engine/boot_hooks.c) */
unsigned long system_milliseconds(void);

/* ---------- constants */

#define MAXIMUM_STREAMS 128
/* the engine queues at most 4 (MAXIMUM_SOUND_PACKETS) */
#define STREAM_PACKETS 6
#define GAIN_BITS 14
#define GAIN_ONE (1L << GAIN_BITS)
#define RAMP_BITS 8
#define PHASE_ONE 0x10000UL
/* at most a second mixed in one call: a longer gap (a load without the
engine's sound idling) is skipped, not mixed */
#define MAXIMUM_FRAMES_PER_CALL WII_SOUND_RATE
#define PI 3.14159265f

/* ---------- streams */

struct wii_packet
{
	XMEDIAPACKET packet;
	unsigned long frames;
	BOOL finished;            /* played out by the mixer, not yet completed */
};

struct wii_stream
{
	/* must be first: in C an IDirectSoundStream is just { lpVtbl } */
	IDirectSoundStream object;
	int index;
	ULONG reference_count;
	LPFNXMEDIAOBJECTCALLBACK callback;
	LPVOID context;

	/* format */
	BOOL adpcm;
	unsigned int channels;
	DWORD sample_rate;
	DWORD frequency;
	unsigned long step;       /* source frames an output frame, 16.16 */
	int kind;                 /* enum wii_sound_voice_kind */

	LONG volume;              /* SetVolume, millibels */
	/* 2D: the front left and right mix bins' gains */
	float mix_left, mix_right;

	/* 3D */
	BOOL has_3d;
	DWORD mode;
	float position[3];
	float minimum_distance, maximum_distance;
	/* the I3DL2 direct path's level and its high frequency level, in
	millibels with obstruction and occlusion (SetI3DL2Source) */
	LONG direct, direct_hf;

	struct wii_packet packets[STREAM_PACKETS];
	unsigned int packet_head;
	unsigned int packet_count;
	/* the next frame of the first packet not finished */
	unsigned long cursor;
	/* the ADPCM block decoded last: its packet's buffer and block */
	const void *block_buffer;
	unsigned long block_index;
	short block[XBOX_ADPCM_BLOCK_FRAMES * 2];

	/* the resampler: the two frames the output is between, and how far */
	BOOL primed;
	long frame0[2], frame1[2];
	unsigned long phase;
	/* gains (GAIN_BITS) and the direct path's low pass (15 bits) the mixer
	ramps from, and the low pass's state */
	BOOL gains_valid;
	long gain_left, gain_right;
	long lowpass_coefficient;
	long lowpass[2];
};

static struct wii_stream *streams[MAXIMUM_STREAMS];
static int stream_count;

static IDirectSoundStreamVtbl stream_vtable;

/* the listener, in DirectSound's left-handed +y up space */
static struct
{
	float position[3];
	float front[3];
	float top[3];
	float rolloff_factor;
	float distance_factor;
	float hf_reference;
} listener = { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 1.0f, 1.0f, 5000.0f };

/* ---------- the mixer's state */

static const struct wii_sound_output *output;
static int output_open;
static FILE *capture;
static wii_sound_event_proc event_hook;

static int mixer_started;
static BOOL clock_started;
static unsigned long clock_last;

static long mix_buffer[WII_SOUND_CHUNK_FRAMES * 2];
static short mix_output[WII_SOUND_CHUNK_FRAMES * 2];

static struct wii_sound_report report;
static unsigned long crc_table[256];

/* ---------- CRC-32 (the zlib polynomial), over bytes as given */

static void crc_initialize(void)
{
	unsigned long index;

	for (index = 0; index < 256; index++)
	{
		unsigned long value = index;
		int bit;

		for (bit = 0; bit < 8; bit++)
			value = value & 1 ? 0xEDB88320UL ^ (value >> 1) : value >> 1;
		crc_table[index] = value;
	}
}

static unsigned long crc_add(unsigned long crc, const unsigned char *bytes, unsigned long count)
{
	crc = ~crc & 0xFFFFFFFFUL;
	while (count--)
		crc = crc_table[(crc ^ *bytes++) & 0xFF] ^ (crc >> 8);
	return ~crc & 0xFFFFFFFFUL;
}

static unsigned long crc_add_word(unsigned long crc, unsigned long word)
{
	unsigned char bytes[4];

	bytes[0] = (unsigned char)word;
	bytes[1] = (unsigned char)(word >> 8);
	bytes[2] = (unsigned char)(word >> 16);
	bytes[3] = (unsigned char)(word >> 24);
	return crc_add(crc, bytes, 4);
}

/* ---------- gains */

static float gain_from_millibels(LONG millibels)
{
	if (millibels <= DSBVOLUME_MIN)
		return 0.0f;
	if (millibels >= 0)
		return 1.0f;
	return powf(10.0f, (float)millibels / 2000.0f);
}

static long gain_fixed(float gain)
{
	long value;

	if (!(gain > 0.0f))
		return 0;
	if (gain >= 2.0f)
		return 2 * GAIN_ONE;
	value = (long)(gain * (float)GAIN_ONE + 0.5f);
	return value;
}

static float dot3(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* DirectSound's inverse distance law, held beyond the maximum distance; a
maximum below the minimum (the game sets the maximum first) is the minimum */
static float distance_attenuation(const struct wii_stream *stream, float distance)
{
	float minimum = stream->minimum_distance;
	float maximum = stream->maximum_distance > minimum ? stream->maximum_distance : minimum;

	if (minimum <= 0.0f || distance <= minimum)
		return 1.0f;
	if (distance > maximum)
		distance = maximum;
	return minimum / (minimum + listener.rolloff_factor * (distance - minimum));
}

/* a 3D voice's gains to the left and right: distance, an equal power pan
(close sources, and those straight ahead or behind, centred; neither ear
below a quarter of the way), and the direct path's level */
static void spatialize(const struct wii_stream *stream, float *left, float *right)
{
	float offset[3], right_axis[3], distance, attenuation, side, ahead, horizontal, pan, angle, direct;
	int axis;

	if (stream->mode == DS3DMODE_HEADRELATIVE)
	{
		for (axis = 0; axis < 3; axis++)
			offset[axis] = stream->position[axis];
		side = offset[0];
		ahead = offset[2];
	}
	else
	{
		for (axis = 0; axis < 3; axis++)
			offset[axis] = stream->position[axis] - listener.position[axis];
		/* left-handed: right = top x front */
		right_axis[0] = listener.top[1] * listener.front[2] - listener.top[2] * listener.front[1];
		right_axis[1] = listener.top[2] * listener.front[0] - listener.top[0] * listener.front[2];
		right_axis[2] = listener.top[0] * listener.front[1] - listener.top[1] * listener.front[0];
		side = dot3(offset, right_axis);
		ahead = dot3(offset, listener.front);
	}
	/* in the game's units, as the minimum and maximum distances are */
	distance = sqrtf(dot3(offset, offset));
	attenuation = distance_attenuation(stream, distance);
	horizontal = sqrtf(side * side + ahead * ahead);
	pan = horizontal > 1.0e-4f ? side / horizontal : 0.0f;
	if (distance < stream->minimum_distance && stream->minimum_distance > 0.0f)
		pan *= distance / stream->minimum_distance;
	pan *= 0.75f;
	angle = (pan + 1.0f) * 0.25f * PI;
	direct = gain_from_millibels(stream->direct);
	*left = cosf(angle) * attenuation * direct;
	*right = sinf(angle) * attenuation * direct;
}

/* the coefficient a (15 bits) of the one-pole low pass y = x + a (y' - x)
whose gain is gain at the listener's HF reference */
static long lowpass_fixed(float gain)
{
	float frequency = listener.hf_reference, cosine, a, b, coefficient;

	if (gain >= 1.0f)
		return 0;
	if (gain < 0.001f)
		gain = 0.001f;
	if (frequency < 20.0f)
		frequency = 20.0f;
	if (frequency > 0.45f * WII_SOUND_RATE)
		frequency = 0.45f * WII_SOUND_RATE;
	cosine = cosf(2.0f * PI * frequency / WII_SOUND_RATE);
	a = 1.0f - gain * gain;
	b = 1.0f - gain * gain * cosine;
	coefficient = (b - sqrtf(b * b - a * a)) / a;
	if (!(coefficient > 0.0f))
		return 0;
	if (coefficient >= 0.999f)
		coefficient = 0.999f;
	return (long)(coefficient * 32768.0f + 0.5f);
}

static void stream_targets(const struct wii_stream *stream, long *left, long *right, long *lowpass)
{
	float gain_left, gain_right, volume = gain_from_millibels(stream->volume);

	*lowpass = 0;
	if (stream->has_3d && stream->mode != DS3DMODE_DISABLE)
	{
		spatialize(stream, &gain_left, &gain_right);
		if (stream->direct_hf < stream->direct)
			*lowpass = lowpass_fixed(gain_from_millibels(stream->direct_hf - stream->direct));
	}
	else
	{
		gain_left = stream->mix_left;
		gain_right = stream->mix_right;
	}
	*left = gain_fixed(gain_left * volume);
	*right = gain_fixed(gain_right * volume);
}

/* ---------- reading a stream's frames */

static unsigned long packet_frames(const struct wii_stream *stream, unsigned long bytes)
{
	if (stream->adpcm)
		return bytes / (XBOX_ADPCM_BLOCK_BYTES * stream->channels) * XBOX_ADPCM_BLOCK_FRAMES;
	return bytes / (2 * stream->channels);
}

/* the stream's next frame into frame (left, right: a mono frame twice),
finishing the packets it passes; 0 once they run out */
static int stream_take_frame(struct wii_stream *stream, long *frame)
{
	for (;;)
	{
		struct wii_packet *packet = NULL;
		unsigned int position;

		for (position = 0; position < stream->packet_count; position++)
		{
			struct wii_packet *candidate = &stream->packets[(stream->packet_head + position) % STREAM_PACKETS];

			if (!candidate->finished)
			{
				packet = candidate;
				break;
			}
		}
		if (!packet)
			return 0;
		if (stream->cursor < packet->frames)
		{
			const unsigned char *bytes = (const unsigned char *)packet->packet.pvBuffer;
			unsigned int last = stream->channels - 1;

			if (stream->adpcm)
			{
				unsigned long block = stream->cursor / XBOX_ADPCM_BLOCK_FRAMES;
				unsigned long within = stream->cursor % XBOX_ADPCM_BLOCK_FRAMES;

				if (stream->block_buffer != packet->packet.pvBuffer || stream->block_index != block)
				{
					xbox_adpcm_decode_block(bytes + block * XBOX_ADPCM_BLOCK_BYTES * stream->channels,
						stream->channels, stream->block);
					stream->block_buffer = packet->packet.pvBuffer;
					stream->block_index = block;
					report.blocks_decoded++;
				}
				frame[0] = stream->block[within * stream->channels];
				frame[1] = stream->block[within * stream->channels + last];
			}
			else
			{
				/* little-endian 16-bit, as the Xbox's */
				const unsigned char *sample = bytes + stream->cursor * 2 * stream->channels;

				frame[0] = (short)(sample[0] | (sample[1] << 8));
				frame[1] = (short)(sample[2 * last] | (sample[2 * last + 1] << 8));
			}
			stream->cursor++;
			return 1;
		}
		stream->cursor = 0;
		packet->finished = TRUE;
	}
}

static void stream_restart(struct wii_stream *stream)
{
	stream->cursor = 0;
	stream->block_buffer = NULL;
	stream->primed = FALSE;
	stream->phase = 0;
	stream->gains_valid = FALSE;
	stream->lowpass[0] = stream->lowpass[1] = 0;
}

/* ---------- mixing */

/* mixes a voice into mix_buffer's frames; ramp: its gains move from where
they were to the target over these frames */
static void mix_voice(struct wii_stream *stream, unsigned long frames, int ramp)
{
	long target_left, target_right, target_lowpass;
	long left, right, step_left = 0, step_right = 0;
	unsigned long frame;

	stream_targets(stream, &target_left, &target_right, &target_lowpass);
	if (!stream->gains_valid)
	{
		stream->gain_left = target_left;
		stream->gain_right = target_right;
		stream->gains_valid = TRUE;
	}
	if (!ramp)
	{
		stream->gain_left = target_left;
		stream->gain_right = target_right;
	}
	stream->lowpass_coefficient = target_lowpass;
	left = stream->gain_left << RAMP_BITS;
	right = stream->gain_right << RAMP_BITS;
	if (ramp && frames)
	{
		step_left = ((target_left - stream->gain_left) << RAMP_BITS) / (long)frames;
		step_right = ((target_right - stream->gain_right) << RAMP_BITS) / (long)frames;
	}
	if (!stream->primed)
	{
		if (!stream_take_frame(stream, stream->frame0))
			return;
		if (!stream_take_frame(stream, stream->frame1))
			stream->frame1[0] = stream->frame1[1] = 0;
		stream->phase = 0;
		stream->primed = TRUE;
	}
	for (frame = 0; frame < frames; frame++)
	{
		long fraction = (long)(stream->phase >> 1);
		long sample[2];
		long gain_left = left >> RAMP_BITS, gain_right = right >> RAMP_BITS;
		int channel;

		for (channel = 0; channel < 2; channel++)
		{
			long from = stream->frame0[channel];

			sample[channel] = from + (((stream->frame1[channel] - from) * fraction) >> 15);
			if (stream->lowpass_coefficient)
			{
				stream->lowpass[channel] = sample[channel] +
					(((stream->lowpass[channel] - sample[channel]) * stream->lowpass_coefficient) >> 15);
				sample[channel] = stream->lowpass[channel];
			}
			else
			{
				stream->lowpass[channel] = sample[channel];
			}
		}
		mix_buffer[frame * 2] += (sample[0] * gain_left) >> GAIN_BITS;
		mix_buffer[frame * 2 + 1] += (sample[1] * gain_right) >> GAIN_BITS;
		left += step_left;
		right += step_right;
		stream->phase += stream->step;
		while (stream->phase >= PHASE_ONE)
		{
			stream->phase -= PHASE_ONE;
			stream->frame0[0] = stream->frame1[0];
			stream->frame0[1] = stream->frame1[1];
			if (!stream_take_frame(stream, stream->frame1))
				stream->frame1[0] = stream->frame1[1] = 0;
		}
	}
	stream->gain_left = target_left;
	stream->gain_right = target_right;
	report.voice_frames += frames;
}

static int stream_playing(const struct wii_stream *stream)
{
	return stream->packet_count != 0 && stream->sample_rate != 0;
}

/* frames of the mix: chunk by chunk, every playing voice, then the chunk
clipped, hashed and written */
static void mix(unsigned long frames)
{
	int first = 1;

	while (frames)
	{
		unsigned long count = frames < WII_SOUND_CHUNK_FRAMES ? frames : WII_SOUND_CHUNK_FRAMES;
		unsigned char bytes[WII_SOUND_CHUNK_FRAMES * 4];
		unsigned long sample;
		int index;

		memset(mix_buffer, 0, count * 2 * sizeof(mix_buffer[0]));
		for (index = 0; index < stream_count; index++)
		{
			if (streams[index] && stream_playing(streams[index]))
				mix_voice(streams[index], count, first);
		}
		for (sample = 0; sample < count * 2; sample++)
		{
			long value = mix_buffer[sample];

			if (value > 32767)
			{
				value = 32767;
				report.clipped_samples++;
			}
			else if (value < -32768)
			{
				value = -32768;
				report.clipped_samples++;
			}
			mix_output[sample] = (short)value;
			bytes[sample * 2] = (unsigned char)value;
			bytes[sample * 2 + 1] = (unsigned char)((unsigned long)value >> 8);
		}
		report.mix_crc = crc_add(report.mix_crc, bytes, count * 4);
		if (capture)
			fwrite(bytes, 1, count * 4, capture);
		if (output_open && output->write)
			output->write(mix_output, count);
		report.frames_mixed += count;
		frames -= count;
		first = 0;
	}
}

static void stream_event(struct wii_stream *stream, int event, const void *context)
{
	switch (event)
	{
	case _wii_sound_event_start: report.starts++; break;
	case _wii_sound_event_end: report.ends++; break;
	default: report.stops++; break;
	}
	report.events_crc = crc_add_word(report.events_crc, (unsigned long)event);
	report.events_crc = crc_add_word(report.events_crc, (unsigned long)stream->index);
	report.events_crc = crc_add_word(report.events_crc, report.frames_mixed);
	report.events_crc = crc_add_word(report.events_crc, (unsigned long)(size_t)context);
	if (event_hook)
		event_hook(event, stream->index, stream->kind, context);
}

/* completes the head packet; the game's callback may queue more */
static void stream_complete_head(struct wii_stream *stream, DWORD status, DWORD completed_size)
{
	struct wii_packet *entry = &stream->packets[stream->packet_head];
	XMEDIAPACKET packet = entry->packet;

	entry->finished = FALSE;
	stream->packet_head = (stream->packet_head + 1) % STREAM_PACKETS;
	stream->packet_count--;
	if (status == XMEDIAPACKET_STATUS_SUCCESS)
		report.packets_completed++;
	else
		report.packets_flushed++;
	if (!stream->packet_count)
	{
		if (status == XMEDIAPACKET_STATUS_SUCCESS)
			stream_event(stream, _wii_sound_event_end, packet.pContext);
		stream_restart(stream);
	}
	if (packet.pdwCompletedSize)
		*packet.pdwCompletedSize = completed_size;
	if (packet.pdwStatus)
		*packet.pdwStatus = status;
	if (stream->callback)
		stream->callback(stream->context, packet.pContext, status);
	else if (packet.hCompletionEvent)
		SetEvent(packet.hCompletionEvent);
}

static void streams_complete_finished(void)
{
	int index;

	for (index = 0; index < stream_count; index++)
	{
		struct wii_stream *stream = streams[index];

		while (stream && stream->packet_count && stream->packets[stream->packet_head].finished)
			stream_complete_head(stream, XMEDIAPACKET_STATUS_SUCCESS, stream->packets[stream->packet_head].packet.dwMaxSize);
	}
}

static void mixer_start(void)
{
	if (mixer_started)
		return;
	mixer_started = 1;
	crc_initialize();
	memset(&report, 0, sizeof(report));
	report.started = 1;
	output_open = output && output->open && output->open(WII_SOUND_RATE);
	wii_log("SOUND_START rate=%d chunk=%d output=%s stream_packets=%d\n", WII_SOUND_RATE, WII_SOUND_CHUNK_FRAMES,
		output_open ? "open" : output ? "failed" : "none", STREAM_PACKETS);
}

/* ---------- the driver's and the engine side's interface (wii_sound.h) */

void wii_sound_set_output(const struct wii_sound_output *driver_output)
{
	output = driver_output;
}

void wii_sound_set_capture(const char *path)
{
	if (capture)
		fclose(capture);
	capture = path ? fopen(path, "wb") : NULL;
}

void wii_sound_set_event_hook(wii_sound_event_proc proc)
{
	event_hook = proc;
}

unsigned long wii_sound_clock_milliseconds(void)
{
	return clock_last;
}

void wii_sound_report_get(struct wii_sound_report *result)
{
	*result = report;
	result->streams = (unsigned long)stream_count;
	result->started = mixer_started;
}

void wii_sound_mark(void)
{
	int started = report.started;

	memset(&report, 0, sizeof(report));
	report.started = started;
	if (output_open && output->mark)
		output->mark();
}

int wii_sound_output_report_get(struct wii_sound_output_report *result)
{
	memset(result, 0, sizeof(*result));
	if (!output_open || !output->report)
		return 0;
	output->report(result);
	return 1;
}

/* ---------- the stream interface */

static struct wii_stream *stream_from_interface(void *stream)
{
	return (struct wii_stream *)stream;
}

static ULONG STDMETHODCALLTYPE stream_add_reference(IDirectSoundStream *object)
{
	return ++stream_from_interface(object)->reference_count;
}

static HRESULT STDMETHODCALLTYPE stream_flush(IDirectSoundStream *object);

static ULONG STDMETHODCALLTYPE stream_release(IDirectSoundStream *object)
{
	struct wii_stream *stream = stream_from_interface(object);
	ULONG count = --stream->reference_count;

	if (count)
		return count;
	stream_flush(object);
	streams[stream->index] = NULL;
	free(stream);
	return 0;
}

static HRESULT STDMETHODCALLTYPE stream_get_info(IDirectSoundStream *object, LPXMEDIAINFO information)
{
	struct wii_stream *stream = stream_from_interface(object);

	memset(information, 0, sizeof(*information));
	information->dwFlags = XMO_STREAMF_FIXED_SAMPLE_SIZE | XMO_STREAMF_INPUT_ASYNC;
	information->dwInputSize = stream->adpcm ? XBOX_ADPCM_BLOCK_BYTES * stream->channels : 2 * stream->channels;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_get_status(IDirectSoundStream *object, LPDWORD status)
{
	*status = stream_from_interface(object)->packet_count < 4 ? XMO_STATUSF_ACCEPT_INPUT_DATA : 0;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_process(IDirectSoundStream *object, LPCXMEDIAPACKET input,
	LPCXMEDIAPACKET result)
{
	struct wii_stream *stream = stream_from_interface(object);
	struct wii_packet *entry;

	(void)result;
	if (!input)
		return E_INVALIDARG;
	if (stream->packet_count == STREAM_PACKETS)
		return E_OUTOFMEMORY;
	entry = &stream->packets[(stream->packet_head + stream->packet_count) % STREAM_PACKETS];
	entry->packet = *input;
	entry->frames = input->pvBuffer ? packet_frames(stream, input->dwMaxSize) : 0;
	entry->finished = FALSE;
	if (input->pdwStatus)
		*input->pdwStatus = XMEDIAPACKET_STATUS_PENDING;
	if (input->pdwCompletedSize)
		*input->pdwCompletedSize = 0;
	report.packets_queued++;
	if (!stream->packet_count)
	{
		/* a stream that ran dry starts over */
		stream_restart(stream);
		stream->packet_count++;
		stream_event(stream, _wii_sound_event_start, input->pContext);
		return S_OK;
	}
	stream->packet_count++;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_discontinuity(IDirectSoundStream *object)
{
	(void)object;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_flush(IDirectSoundStream *object)
{
	struct wii_stream *stream = stream_from_interface(object);
	BOOL stopped = FALSE;

	while (stream->packet_count)
	{
		struct wii_packet *head = &stream->packets[stream->packet_head];

		if (!stopped && !head->finished)
		{
			stopped = TRUE;
			stream_event(stream, _wii_sound_event_stop, head->packet.pContext);
		}
		stream_complete_head(stream, head->finished ? XMEDIAPACKET_STATUS_SUCCESS : XMEDIAPACKET_STATUS_FLUSHED,
			head->finished ? head->packet.dwMaxSize : 0);
	}
	stream_restart(stream);
	return S_OK;
}

static IDirectSoundStreamVtbl stream_vtable =
{
	stream_add_reference,
	stream_release,
	stream_get_info,
	stream_get_status,
	stream_process,
	stream_discontinuity,
	stream_flush,
};

static void stream_set_step(struct wii_stream *stream)
{
	DWORD frequency = stream->frequency ? stream->frequency : stream->sample_rate;

	stream->step = (unsigned long)(((unsigned long long)frequency << 16) / WII_SOUND_RATE);
}

/* ---------- the DirectSound object */

struct wii_direct_sound
{
	ULONG reference_count;
};

static struct wii_direct_sound direct_sound = { 0 };

HRESULT WINAPI DirectSoundCreate(LPGUID device_id, LPDIRECTSOUND *result, LPUNKNOWN outer)
{
	(void)device_id;
	(void)outer;
	mixer_start();
	direct_sound.reference_count++;
	*result = (LPDIRECTSOUND)&direct_sound;
	return DS_OK;
}

ULONG WINAPI IDirectSound_Release(LPDIRECTSOUND sound)
{
	(void)sound;
	return direct_sound.reference_count ? --direct_sound.reference_count : 0;
}

/* every frame (dsound_begin_scene), and in the engine's idle loops
(sound_idle): the frames its clock has moved on, then the packets done */
VOID WINAPI DirectSoundDoWork(void)
{
	unsigned long now = system_milliseconds();
	unsigned long frames;
	unsigned long long started, spent;
	unsigned long playing = 0;
	unsigned long playing_by_kind[NUMBER_OF_WII_SOUND_VOICE_KINDS];
	int index;

	if (!mixer_started)
		return;
	if (!clock_started)
	{
		clock_started = TRUE;
		clock_last = now;
	}
	frames = (now - clock_last) * (WII_SOUND_RATE / 1000);
	clock_last = now;
	if (frames > MAXIMUM_FRAMES_PER_CALL)
	{
		report.clock_skipped_frames += frames - MAXIMUM_FRAMES_PER_CALL;
		frames = MAXIMUM_FRAMES_PER_CALL;
	}
	report.work_calls++;
	memset(playing_by_kind, 0, sizeof(playing_by_kind));
	for (index = 0; index < stream_count; index++)
	{
		if (streams[index] && stream_playing(streams[index]))
		{
			playing++;
			playing_by_kind[streams[index]->kind]++;
		}
	}
	if (playing > report.voices_maximum)
		report.voices_maximum = playing;
	for (index = 0; index < NUMBER_OF_WII_SOUND_VOICE_KINDS; index++)
	{
		if (playing_by_kind[index] > report.voices_maximum_by_kind[index])
			report.voices_maximum_by_kind[index] = playing_by_kind[index];
	}
	report.voice_calls[playing < WII_SOUND_VOICE_HISTOGRAM - 1 ? playing : WII_SOUND_VOICE_HISTOGRAM - 1]++;
	started = wii_time_microseconds();
	mix(frames);
	streams_complete_finished();
	spent = wii_time_microseconds() - started;
	report.mix_microseconds += spent;
	if (spent > report.mix_microseconds_peak)
		report.mix_microseconds_peak = spent;
}

VOID WINAPI DirectSoundUseFullHRTF(void)
{
}

HRESULT WINAPI IDirectSound_GetCaps(LPDIRECTSOUND sound, LPDSCAPS caps)
{
	(void)sound;
	memset(caps, 0, sizeof(*caps));
	caps->dwFree2DBuffers = 64;
	caps->dwFree3DBuffers = 64;
	caps->dwFreeBufferSGEs = 2047;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_GetSpeakerConfig(LPDIRECTSOUND sound, LPDWORD speaker_config)
{
	(void)sound;
	*speaker_config = DSSPEAKER_STEREO;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_DownloadEffectsImage(LPDIRECTSOUND sound, LPCVOID image, DWORD image_size,
	LPCDSEFFECTIMAGELOC image_location, LPDSEFFECTIMAGEDESC *image_description)
{
	(void)sound;
	(void)image;
	(void)image_size;
	(void)image_location;
	if (image_description)
		*image_description = NULL;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_CommitDeferredSettings(LPDIRECTSOUND sound)
{
	(void)sound;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetMixBinHeadroom(LPDIRECTSOUND sound, DWORD mix_bin_mask, DWORD headroom)
{
	(void)sound;
	(void)mix_bin_mask;
	(void)headroom;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetI3DL2Listener(LPDIRECTSOUND sound, LPCDSI3DL2LISTENER properties, DWORD apply)
{
	(void)sound;
	(void)apply;
	/* (only the HF reference: the reverb is not modelled) */
	listener.hf_reference = properties->flHFReference;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetDistanceFactor(LPDIRECTSOUND sound, FLOAT factor, DWORD apply)
{
	(void)sound;
	(void)apply;
	listener.distance_factor = factor > 0.0f ? factor : 1.0f;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetRolloffFactor(LPDIRECTSOUND sound, FLOAT factor, DWORD apply)
{
	(void)sound;
	(void)apply;
	listener.rolloff_factor = factor >= 0.0f ? factor : 1.0f;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetPosition(LPDIRECTSOUND sound, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	(void)sound;
	(void)apply;
	listener.position[0] = x;
	listener.position[1] = y;
	listener.position[2] = z;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetVelocity(LPDIRECTSOUND sound, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	(void)sound;
	(void)x;
	(void)y;
	(void)z;
	(void)apply;
	return DS_OK;
}

static void normalize3(float *vector)
{
	float length = sqrtf(dot3(vector, vector));

	if (length > 1.0e-6f)
	{
		vector[0] /= length;
		vector[1] /= length;
		vector[2] /= length;
	}
}

HRESULT WINAPI IDirectSound_SetOrientation(LPDIRECTSOUND sound, FLOAT x_front, FLOAT y_front, FLOAT z_front,
	FLOAT x_top, FLOAT y_top, FLOAT z_top, DWORD apply)
{
	(void)sound;
	(void)apply;
	listener.front[0] = x_front;
	listener.front[1] = y_front;
	listener.front[2] = z_front;
	listener.top[0] = x_top;
	listener.top[1] = y_top;
	listener.top[2] = z_top;
	normalize3(listener.front);
	normalize3(listener.top);
	return DS_OK;
}

static int voice_kind(const WAVEFORMATEX *format, BOOL has_3d)
{
	if (!format)
		return _wii_sound_voice_other;
	if (format->nChannels == 2)
		return format->nSamplesPerSec > 22050 ? _wii_sound_voice_stereo_44k : _wii_sound_voice_stereo_22k;
	return has_3d ? _wii_sound_voice_3d_mono : _wii_sound_voice_2d_mono;
}

HRESULT WINAPI IDirectSound_CreateSoundStream(LPDIRECTSOUND sound, LPCDSSTREAMDESC description,
	LPDIRECTSOUNDSTREAM *result, LPUNKNOWN outer)
{
	const WAVEFORMATEX *format = description->lpwfxFormat;
	struct wii_stream *stream;
	int index;

	(void)sound;
	(void)outer;
	for (index = 0; index < stream_count && streams[index]; index++)
		;
	if (index >= MAXIMUM_STREAMS)
		return E_OUTOFMEMORY;
	stream = calloc(1, sizeof(*stream));
	if (!stream)
		return E_OUTOFMEMORY;
	stream->object.lpVtbl = &stream_vtable;
	stream->index = index;
	stream->reference_count = 1;
	stream->callback = description->lpfnCallback;
	stream->context = description->lpvContext;
	stream->adpcm = format && format->wFormatTag == WAVE_FORMAT_XBOX_ADPCM;
	stream->channels = format && format->nChannels == 2 ? 2 : 1;
	stream->sample_rate = format ? format->nSamplesPerSec : 0;
	stream->frequency = stream->sample_rate;
	stream->has_3d = (description->dwFlags & DSSTREAMCAPS_CTRL3D) != 0;
	stream->kind = voice_kind(format, stream->has_3d);
	stream->volume = 0;
	/* DirectSound's default mix bins: a mono voice to both fronts, a stereo
	voice's channels to the front left and right */
	stream->mix_left = 1.0f;
	stream->mix_right = 1.0f;
	stream->mode = DS3DMODE_NORMAL;
	stream->minimum_distance = DS3D_DEFAULTMINDISTANCE;
	stream->maximum_distance = DS3D_DEFAULTMAXDISTANCE;
	stream_set_step(stream);
	stream_restart(stream);
	streams[index] = stream;
	if (index == stream_count)
		stream_count++;
	*result = &stream->object;
	return DS_OK;
}

/* January-era DirectSound exports the game declares itself
(sound_dsound_xbox.c) */

void __stdcall DirectSoundStopStream(LPDIRECTSOUNDSTREAM stream)
{
	stream_flush(stream);
}

/* 0: the stream's voice has finished, which ends the channel's stopping */
unsigned long __stdcall DirectSoundGetStreamVoiceStatus(LPDIRECTSOUNDSTREAM stream)
{
	return stream_from_interface(stream)->packet_count != 0;
}

HRESULT WINAPI IDirectSoundStream_SetFrequency(LPDIRECTSOUNDSTREAM stream, DWORD frequency)
{
	struct wii_stream *record = stream_from_interface(stream);

	record->frequency = frequency ? frequency : record->sample_rate;
	stream_set_step(record);
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetVolume(LPDIRECTSOUNDSTREAM stream, LONG volume)
{
	stream_from_interface(stream)->volume = volume;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetMixBins(LPDIRECTSOUNDSTREAM stream, DWORD mix_bin_mask)
{
	struct wii_stream *record = stream_from_interface(stream);

	record->mix_left = (mix_bin_mask & DSMIXBIN_FRONT_LEFT) ? 1.0f : 0.0f;
	record->mix_right = (mix_bin_mask & DSMIXBIN_FRONT_RIGHT) ? 1.0f : 0.0f;
	return DS_OK;
}

/* volumes come in the order of the set bits of the mask */
HRESULT WINAPI IDirectSoundStream_SetMixBinVolumes(LPDIRECTSOUNDSTREAM stream, DWORD mix_bin_mask, const LONG *volumes)
{
	struct wii_stream *record = stream_from_interface(stream);
	unsigned long bit, index = 0;

	for (bit = 0; bit < 32; bit++)
	{
		if (!(mix_bin_mask & (1UL << bit)))
			continue;
		if ((1UL << bit) == DSMIXBIN_FRONT_LEFT)
			record->mix_left = gain_from_millibels(volumes[index]);
		else if ((1UL << bit) == DSMIXBIN_FRONT_RIGHT)
			record->mix_right = gain_from_millibels(volumes[index]);
		index++;
	}
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetMode(LPDIRECTSOUNDSTREAM stream, DWORD mode, DWORD apply)
{
	(void)apply;
	stream_from_interface(stream)->mode = mode;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetPosition(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	struct wii_stream *record = stream_from_interface(stream);

	(void)apply;
	record->position[0] = x;
	record->position[1] = y;
	record->position[2] = z;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetMinDistance(LPDIRECTSOUNDSTREAM stream, FLOAT distance, DWORD apply)
{
	(void)apply;
	stream_from_interface(stream)->minimum_distance = distance;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetMaxDistance(LPDIRECTSOUNDSTREAM stream, FLOAT distance, DWORD apply)
{
	(void)apply;
	stream_from_interface(stream)->maximum_distance = distance;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetI3DL2Source(LPDIRECTSOUNDSTREAM stream, LPCDSI3DL2BUFFER source, DWORD apply)
{
	struct wii_stream *record = stream_from_interface(stream);
	LONG direct;

	(void)apply;
	/* the direct path: at low frequencies obstruction and occlusion take
	their LF ratio of their high frequency levels, which (with the source's
	own) muffle it above the HF reference */
	direct = source->lDirect + (LONG)(source->Obstruction.lHFLevel * source->Obstruction.flLFRatio) +
		(LONG)(source->Occlusion.lHFLevel * source->Occlusion.flLFRatio);
	if (direct > 0)
		direct = 0;
	record->direct = direct;
	record->direct_hf = source->lDirect + source->lDirectHF + source->Obstruction.lHFLevel + source->Occlusion.lHFLevel;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetVelocity(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	(void)stream;
	(void)x;
	(void)y;
	(void)z;
	(void)apply;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetConeAngles(LPDIRECTSOUNDSTREAM stream, DWORD inside, DWORD outside, DWORD apply)
{
	(void)stream;
	(void)inside;
	(void)outside;
	(void)apply;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetConeOrientation(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	(void)stream;
	(void)x;
	(void)y;
	(void)z;
	(void)apply;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetConeOutsideVolume(LPDIRECTSOUNDSTREAM stream, LONG volume, DWORD apply)
{
	(void)stream;
	(void)volume;
	(void)apply;
	return DS_OK;
}

/* ---------- buffers

The game's only buffer is a silent looping one that kept the Xbox's voice
processor busy (dsound_fix_rear_speakers); it needs no mixing. */

struct null_buffer
{
	ULONG reference_count;
	LPVOID data;
	DWORD size;
	BOOL playing;
};

HRESULT WINAPI DirectSoundCreateBuffer(LPCDSBUFFERDESC description, LPDIRECTSOUNDBUFFER *result)
{
	struct null_buffer *buffer = calloc(1, sizeof(*buffer));

	(void)description;
	if (!buffer)
		return E_OUTOFMEMORY;
	buffer->reference_count = 1;
	*result = (LPDIRECTSOUNDBUFFER)buffer;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_CreateSoundBuffer(LPDIRECTSOUND sound, LPCDSBUFFERDESC description,
	LPDIRECTSOUNDBUFFER *result, LPUNKNOWN outer)
{
	(void)sound;
	(void)outer;
	return DirectSoundCreateBuffer(description, result);
}

ULONG WINAPI IDirectSoundBuffer_Release(LPDIRECTSOUNDBUFFER buffer)
{
	struct null_buffer *record = (struct null_buffer *)buffer;
	ULONG count = --record->reference_count;

	if (!count)
		free(record);
	return count;
}

HRESULT WINAPI IDirectSoundBuffer_SetBufferData(LPDIRECTSOUNDBUFFER buffer, LPVOID data, DWORD size)
{
	struct null_buffer *record = (struct null_buffer *)buffer;

	record->data = data;
	record->size = size;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Play(LPDIRECTSOUNDBUFFER buffer, DWORD reserved1, DWORD reserved2, DWORD flags)
{
	(void)reserved1;
	(void)reserved2;
	(void)flags;
	((struct null_buffer *)buffer)->playing = TRUE;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Stop(LPDIRECTSOUNDBUFFER buffer)
{
	((struct null_buffer *)buffer)->playing = FALSE;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetCurrentPosition(LPDIRECTSOUNDBUFFER buffer, DWORD play_cursor)
{
	(void)buffer;
	(void)play_cursor;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetLoopRegion(LPDIRECTSOUNDBUFFER buffer, DWORD loop_start, DWORD loop_length)
{
	(void)buffer;
	(void)loop_start;
	(void)loop_length;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetPitch(LPDIRECTSOUNDBUFFER buffer, LONG pitch)
{
	(void)buffer;
	(void)pitch;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetVolume(LPDIRECTSOUNDBUFFER buffer, LONG volume)
{
	(void)buffer;
	(void)volume;
	return DS_OK;
}
