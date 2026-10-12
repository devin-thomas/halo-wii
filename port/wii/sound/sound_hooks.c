/*
SOUND_HOOKS.C

HWI-074: the engine side of the Wii's sound (wii_sound.h). Compiled as an
engine unit (the engine's headers and flags), in engine.dol and in the i686
host reference, which run it alike.

- The map run's sound (engine_map_run.h's "sound" plan line): the engine's
  own sound_initialize once, as shell_initialize starts it; then each run's
  frame renders the sound (sound_render, as main_loop's main_game_render
  does) on the boot hooks' frame clock, which the frame's time moves
  (boot_hooks.h). The engine's sound manager and its Xbox platform decide
  everything; the Wii's DirectSound (dsound_wii.c) mixes on that clock.
- The listener (listener=player, the default). main_loop moves the
  observer every frame (director_update, observer_update), and the sound
  reads its camera: the listener (refresh_listener) and each source's
  obstruction (compute_sound_obstruction); the game's own sound bookkeeping
  reads it too (the clusters a looping sound can be heard in, the sound
  environment: game_sound_update, game state). The map run
  never moves the observer, because the simulation reads it too (aim
  assist, the activation PVS: port/wii/render/render_engine.c). So while the
  sound manager's code runs (sound_render, sound_new_impulse,
  sound_refresh_looping, sound_idle), observer_get_camera answers with the engine's deterministic
  camera of the local player's unit (director_camera_deterministic, as the
  Wii renderer's camera), in the cluster it is in; everywhere else with the
  run's observer, as before. Neither writes anything.
- Each run's report (SOUND_RUN, SOUND_KIND, SOUND_CLASS lines) and its
  stream events (SOUND_EVENT: a stream starting, playing out or stopped,
  named by the sound and its class, with the tick), for the evidence.
- The sound's obstruction tests leave the objects' collision marks as they
  found them (below).
- The wrapped entry points pass through unless the sound runs.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "camera/director.h"
#include "camera/observer.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "memory/data.h"
#include "cache/cache_files.h"
#include "sound/sound_definitions.h"
#include "sound/sound_classes.h"
#include "sound/sound_manager.h"
#include "tag_files/tag_files.h"

#include "../engine/wii_platform.h"
#include "../engine/boot_hooks.h"
#include "wii_sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the engine's (sound_manager.c, game_sound.c) */
boolean sound_is_active(void);
extern struct data_array *sound_data;
extern struct data_array *looping_sound_data;

enum
{
	SOUND_HOOK_CLASSES = 64,
	SOUND_HOOK_EVENTS_PER_CLASS = 6,
	SOUND_HOOK_STREAMS = 128,
};

struct sound_hook_class
{
	unsigned long starts, ends, stops;
	long first_tick, last_tick;
	unsigned long logged;
};

static struct
{
	boolean active;
	boolean started;
	boolean wanted;
	struct wii_sound_map_options options;
	long depth;                  /* inside the sound's own code */
	boolean computing_camera;
	struct observer_result listener;
	/* the frame clock, as the run's frames' time */
	double run_seconds;
	unsigned long run_milliseconds;
	/* the run's report */
	boolean log_events;
	unsigned long events_logged;
	long sounds_peak, looping_sounds_peak;
	unsigned long obstruction_tests;
	unsigned long marks_not_restored;
	unsigned long notifications_withheld, mouth_updates_withheld, indices_withheld;
	unsigned long frames;
	struct sound_hook_class classes[SOUND_HOOK_CLASSES];
	/* each stream's sound class while it plays */
	short stream_class[SOUND_HOOK_STREAMS];
} sound_hooks;

/* ---------- the objects' collision marks, as the sound found them

Every collision test that can hit objects marks the objects it visits so it
visits each once (object_marker_begin: global_object_marker counts on; each
object visited keeps it, in its magic_number). The mark is the test's
scratch: nothing reads it but the marking itself, against the marker of the
test running. But it is in the objects' state, and the next test's marker
follows from how many tests ran. The sound tests too: each source's
obstruction from the listener (compute_sound_obstruction, game_sound.c), as
many as there are sounds to hear, and the listener's camera (a following
camera's own test, director_camera_deterministic). Those moved every later
mark of the simulation's own tests, and with them the objects' bytes. So
the sound's code leaves the marks as it found them: the marker, and each
object's mark, are put back when it returns (the outermost of the sound's
entry points below, and the listener's camera inside game_sound_update). */

extern long global_object_marker;

static struct
{
	long depth;
	long marker;
	long count;
	long *marks;
	long capacity;
} object_marks;

static void object_marks_save(
	void)
{
	long index;

	if (!sound_hooks.active || object_marks.depth++ > 0)
		return;
	object_marks.marker = global_object_marker;
	object_marks.count = object_header_data ? object_header_data->count : 0;
	if (object_marks.count > object_marks.capacity)
	{
		long *grown = (long *)realloc(object_marks.marks, (size_t)object_header_data->maximum_count * sizeof(long));

		if (!grown)
		{
			/* (not restorable: said by the report's count) */
			object_marks.count = -1;
			return;
		}
		object_marks.marks = grown;
		object_marks.capacity = object_header_data->maximum_count;
	}
	for (index = 0; index < object_marks.count; index++)
	{
		struct object_header_datum const *header = (struct object_header_datum const *)object_header_data->data + index;

		object_marks.marks[index] = header->identifier && header->datum ? header->datum->object.magic_number : 0;
	}
}

static void object_marks_restore(
	void)
{
	long index;

	if (!sound_hooks.active || object_marks.depth <= 0 || --object_marks.depth > 0)
		return;
	if (object_marks.count < 0)
	{
		sound_hooks.marks_not_restored++;
		return;
	}
	for (index = 0; index < object_marks.count; index++)
	{
		struct object_header_datum const *header = (struct object_header_datum const *)object_header_data->data + index;

		if (header->identifier && header->datum)
			header->datum->object.magic_number = object_marks.marks[index];
	}
	global_object_marker = object_marks.marker;
}

/* ---------- the listener */

struct observer_result const *__real_observer_get_camera(short local_player_index);

struct observer_result const *__wrap_observer_get_camera(
	short local_player_index)
{
	struct observer_result const *observer = __real_observer_get_camera(local_player_index);
	long player_index;
	struct player_datum *player;

	if (!sound_hooks.active || !sound_hooks.options.listener_follows_player || sound_hooks.depth <= 0 ||
		sound_hooks.computing_camera || local_player_index != 0)
	{
		return observer;
	}
	player_index = local_player_get_player_index(0);
	player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	if (!player || player->unit_index == NONE)
		return observer;
	sound_hooks.computing_camera = TRUE;
	object_marks_save();
	{
		real_point3d position;
		real_vector3d forward;
		real_vector3d left;
		real_vector3d up;

		sound_hooks.listener = *observer;
		director_camera_deterministic(player->unit_index, &position, &forward);
		normalize3d(&forward);
		up = *global_up3d;
		cross_product3d(&up, &forward, &left);
		if (normalize3d(&left) == 0.0f)
			left = *global_left3d;
		cross_product3d(&forward, &left, &up);
		normalize3d(&up);
		sound_hooks.listener.position = position;
		sound_hooks.listener.forward = forward;
		sound_hooks.listener.up = up;
		scenario_location_from_point(&sound_hooks.listener.location, &position);
	}
	object_marks_restore();
	sound_hooks.computing_camera = FALSE;

	return &sound_hooks.listener;
}

/* ---------- the sound's own code: the listener, and the frame clock's idle */

void __real_sound_render(void);
void __real_sound_idle(void);
void __real_game_sound_update(real dt);
long __real_sound_new_impulse(long definition_index, struct sound_source *source, long source_identifier,
	boolean (*track_proc)(long source_identifier, void const *track_data, struct sound_source *source),
	void const *track_data, short track_data_size);

void __wrap_sound_render(
	void)
{
	object_marks_save();
	sound_hooks.depth++;
	__real_sound_render();
	sound_hooks.depth--;
	object_marks_restore();
}

void __wrap_sound_idle(
	void)
{
	object_marks_save();
	sound_hooks.depth++;
	__real_sound_idle();
	sound_hooks.depth--;
	object_marks_restore();
}

/* (a map's end: game_dispose_from_old_map) the sound manager fades every
sound out over 300 ms of the clock, idling the sound until the clock has
passed (sound_manager.c). Under the frame clock nothing else moves the clock
meanwhile: each read moves it a millisecond on, so the wait ends, the same
on every machine */
void __real_sound_dispose_from_old_map(void);

void __wrap_sound_dispose_from_old_map(
	void)
{
	boolean advancing = sound_hooks.started && wii_boot_frame_clock_on();

	if (advancing)
		wii_boot_frame_clock_advance_on_read(TRUE);
	object_marks_save();
	sound_hooks.depth++;
	__real_sound_dispose_from_old_map();
	sound_hooks.depth--;
	object_marks_restore();
	if (advancing)
		wii_boot_frame_clock_advance_on_read(FALSE);
}

/* (game_frame's) the game's own sound bookkeeping: which looping sounds can be
heard (the camera's clusters), the background sound and environment of the
camera's cluster. Its decisions are game state (object looping sounds,
scenario globals), so it keeps the run's observer, as without sound; only
what it asks of the sound manager (sound_refresh_looping, below) hears with
the listener */
void __wrap_game_sound_update(
	real dt)
{
	__real_game_sound_update(dt);
}

/* (game_sound.c, from game_sound_update and from the sound manager) */
void __real_compute_sound_obstruction(short local_player_index, struct sound_source *source, real distance);

void __wrap_compute_sound_obstruction(
	short local_player_index,
	struct sound_source *source,
	real distance)
{
	object_marks_save();
	sound_hooks.depth++;
	__real_compute_sound_obstruction(local_player_index, source, distance);
	sound_hooks.depth--;
	object_marks_restore();
	if (sound_hooks.active)
		sound_hooks.obstruction_tests++;
}

long __wrap_sound_new_impulse(
	long definition_index,
	struct sound_source *source,
	long source_identifier,
	boolean (*track_proc)(long source_identifier, void const *track_data, struct sound_source *source),
	void const *track_data,
	short track_data_size)
{
	long sound_index;

	object_marks_save();
	sound_hooks.depth++;
	sound_index = __real_sound_new_impulse(definition_index, source, source_identifier, track_proc, track_data,
		track_data_size);
	sound_hooks.depth--;
	object_marks_restore();

	return sound_index;
}

/* ---------- the sound's fields in the tags, left out of the tag slot's digest

The sound manager keeps state of its own in the sound tags, as the Xbox's
did: each definition's promotion counter and time and the impulse its script
started last (sound_manager.c, game_sound.c), each pitch range's played and
previous (and forced) permutations (sound_definitions.c), and each
permutation's place in the sound cache (xbox_sound_cache.c). Only the sound
reads them, and they follow its clock and its channels, so the map run's
tag-slot digest (real_map_scenario.c) reads them as zeros while the sound
runs: they are put back after. */

struct sound_tag_fields
{
	long *saved;
	long count;
};

static struct sound_tag_fields sound_tag_fields;

/* visit (index of the next saved value, and the field): save it and zero it,
or put it back */
static void sound_tag_field(long *field, int restore)
{
	if (restore)
		*field = sound_tag_fields.saved[sound_tag_fields.count++];
	else
	{
		if (sound_tag_fields.saved)
			sound_tag_fields.saved[sound_tag_fields.count] = *field;
		sound_tag_fields.count++;
		if (sound_tag_fields.saved)
			*field = 0;
	}
}

static void sound_tag_short(short *field, int restore)
{
	long value = *field;

	sound_tag_field(&value, restore);
	*field = (short)value;
}

static void sound_tags_walk(int restore)
{
	struct tag_iterator iterator;
	long definition_index;

	sound_tag_fields.count = 0;
	tag_iterator_new(&iterator, SOUND_DEFINITION_TAG);
	for (definition_index = tag_iterator_next(&iterator); definition_index != NONE;
		definition_index = tag_iterator_next(&iterator))
	{
		struct sound_definition *definition = sound_definition_get(definition_index);
		short range;

		sound_tag_field(&definition->promotion_counter, restore);
		sound_tag_field(&definition->promotion_time, restore);
		sound_tag_field(&definition->scripting_sound_index, restore);
		for (range = 0; range < definition->pitch_ranges.count; range++)
		{
			struct sound_pitch_range *pitch_range =
				TAG_BLOCK_GET_ELEMENT(&definition->pitch_ranges, range, struct sound_pitch_range);
			short permutation;

			sound_tag_field((long *)&pitch_range->played_permutation_mask, restore);
			sound_tag_short(&pitch_range->previous_permutation_index, restore);
			sound_tag_short(&pitch_range->forced_permutation_index, restore);
			for (permutation = 0; permutation < pitch_range->permutations.count; permutation++)
			{
				struct sound_permutation *entry =
					TAG_BLOCK_GET_ELEMENT(&pitch_range->permutations, permutation, struct sound_permutation);

				sound_tag_field(&entry->unknown0, restore);
				sound_tag_field((long *)&entry->unknown1, restore);
				sound_tag_field((long *)&entry->unknown2, restore);
				sound_tag_field((long *)&entry->unknown3, restore);
			}
		}
	}
}

int wii_sound_tags_mask_begin(
	void)
{
	if (!sound_hooks.active || sound_tag_fields.saved)
		return FALSE;
	/* (counted first, then saved) */
	sound_tags_walk(FALSE);
	sound_tag_fields.saved = (long *)malloc((size_t)(sound_tag_fields.count ? sound_tag_fields.count : 1) * sizeof(long));
	if (!sound_tag_fields.saved)
		return FALSE;
	sound_tags_walk(FALSE);
	return TRUE;
}

void wii_sound_tags_mask_end(
	void)
{
	if (!sound_tag_fields.saved)
		return;
	sound_tags_walk(TRUE);
	free(sound_tag_fields.saved);
	sound_tag_fields.saved = NULL;
}

/* ---------- the game's reactions to its sounds (couplings=presentation)

Five things the Xbox's game did because a sound played, which are game
state, and which the game without sound never does (the Wii's until now,
and the map run's reference digests):
- a scripted line a unit speaks makes the unit speak (scripted_sound_new:
  object_type_notify_impulse_sound, only if the sound manager started the
  sound: unit_speak, the unit's speech state, which its dialogue and the AI
  then follow);
- the unit's mouth follows the line's mouth data, once a frame
  (sound_manager.c update_channels: game_sound_set_mouth_aperture, the unit's
  mouth aperture, an object function its animation and attachments read);
  frames are not ticks on the Wii, so this follows the render cadence;
- scripted sound class fades move the classes' gains, once a frame, in the
  game state (sound_classes_update from sound_render);
- a looping sound is deleted when its stop has played out (below);
- a sound's index is kept where it was started (below).
With couplings=presentation (the default) the game stays as without sound:
the unit is not notified, its mouth is not moved, and the fades move a copy
of the gains outside the game state, which the sound manager reads
(sound_class_get_gain). couplings=original runs the Xbox's own. Which the
game should do is HWI-040's to decide (the lines' mouths do not move with
presentation; with original, a10 is not cadence-invariant: the mouth). */

void __real_object_type_notify_impulse_sound(long object_index, long sound_index, long source_object_index);
void __real_game_sound_set_mouth_aperture(long object_index, real mouth_aperture);
void __real_sound_classes_update(long ticks);
real __real_sound_class_get_gain(short class_index);

static boolean sound_hooks_presentation(
	void)
{
	return sound_hooks.active && !sound_hooks.options.game_couplings;
}

void __wrap_object_type_notify_impulse_sound(
	long object_index,
	long sound_index,
	long source_object_index)
{
	if (sound_hooks_presentation())
	{
		sound_hooks.notifications_withheld++;
		return;
	}
	__real_object_type_notify_impulse_sound(object_index, sound_index, source_object_index);
}

void __wrap_game_sound_set_mouth_aperture(
	long object_index,
	real mouth_aperture)
{
	if (sound_hooks_presentation())
	{
		sound_hooks.mouth_updates_withheld++;
		return;
	}
	__real_game_sound_set_mouth_aperture(object_index, mouth_aperture);
}

/* (the fourth) game_sound_update keeps an object's looping sound, or deletes
an unattached one, by what the sound manager answers (game_sound.c
update_potentially_audible_looping_sound): with no sound, a sound stopping
has stopped at once; with sound, only once its stop has played out. Which
frame a looping sound's datum is freed then decides the index the next one
takes, and objects keep those indices (their attachments). With
couplings=presentation the sound plays its stop, and the game hears the
answer it hears without sound. */
boolean __real_sound_refresh_looping(long definition_index, long looping_sound_index, struct sound_source *source,
	short refresh_state, boolean alternate, real fade_time);

boolean __wrap_sound_refresh_looping(
	long definition_index,
	long looping_sound_index,
	struct sound_source *source,
	short refresh_state,
	boolean alternate,
	real fade_time)
{
	boolean finished;

	object_marks_save();
	sound_hooks.depth++;
	finished = __real_sound_refresh_looping(definition_index, looping_sound_index, source, refresh_state, alternate,
		fade_time);
	sound_hooks.depth--;
	object_marks_restore();

	if (!sound_hooks_presentation())
		return finished;
	return refresh_state == _looping_sound_refresh_stop;
}

/* (the fifth) two callers keep the index of the sound they started, in the
game state: the first-person weapon (to stop it when the view changes state)
and a unit's dialogue (unit_dialogue.c: never read again). Without sound the
index is NONE. With couplings=presentation they keep NONE: the sound plays
on, and a first-person sound is not cut short when the view's state changes */
long __real_object_impulse_sound_new(long object_index, long definition_index, short node_index,
	real_point3d const *position, real_vector3d const *forward, real scale);

long __wrap_object_impulse_sound_new(
	long object_index,
	long definition_index,
	short node_index,
	real_point3d const *position,
	real_vector3d const *forward,
	real scale)
{
	long sound_index = __real_object_impulse_sound_new(object_index, definition_index, node_index, position, forward,
		scale);

	if (sound_hooks_presentation() && sound_index != NONE)
	{
		sound_hooks.indices_withheld++;
		return NONE;
	}
	return sound_index;
}

/* the sound classes' gains (sound_classes.c's struct sound_class_datum: the
game state's, which scripts set the fade of) */
struct sound_class_gain
{
	real desired_gain;
	real gain;
	short ticks;
};

enum
{
	SOUND_HOOK_SOUND_CLASSES = 51,
};

extern struct sound_class_gain *sound_class_data;

static struct
{
	boolean valid;
	struct sound_class_gain seen;   /* the game state's, as last adopted */
	struct sound_class_gain gain;   /* the copy the sound fades */
} sound_class_copies[SOUND_HOOK_SOUND_CLASSES];

/* the copy adopts what a script set in the game state since */
static struct sound_class_gain *sound_class_copy(
	short class_index)
{
	struct sound_class_gain const *state = &sound_class_data[class_index];

	if (!sound_class_copies[class_index].valid ||
		state->desired_gain != sound_class_copies[class_index].seen.desired_gain ||
		state->ticks != sound_class_copies[class_index].seen.ticks ||
		state->gain != sound_class_copies[class_index].seen.gain)
	{
		sound_class_copies[class_index].seen = *state;
		sound_class_copies[class_index].gain.desired_gain = state->desired_gain;
		sound_class_copies[class_index].gain.ticks = state->ticks;
		if (!sound_class_copies[class_index].valid || !state->ticks)
			sound_class_copies[class_index].gain.gain = state->ticks ? state->gain : state->desired_gain;
		sound_class_copies[class_index].valid = TRUE;
	}

	return &sound_class_copies[class_index].gain;
}

void __wrap_sound_classes_update(
	long ticks)
{
	short class_index;

	if (!sound_hooks_presentation() || !sound_class_data)
	{
		__real_sound_classes_update(ticks);
		return;
	}
	/* sound_classes_update's interpolation, on the copies */
	for (class_index = 0; ticks > 0 && class_index < SOUND_HOOK_SOUND_CLASSES; class_index++)
	{
		struct sound_class_gain *copy = sound_class_copy(class_index);

		if (copy->ticks > ticks)
		{
			copy->gain = (real)ticks / copy->ticks * (copy->desired_gain - copy->gain) + copy->gain;
			copy->ticks -= (short)ticks;
		}
		else
		{
			copy->gain = copy->desired_gain;
			copy->ticks = 0;
		}
	}
}

real __wrap_sound_class_get_gain(
	short class_index)
{
	if (!sound_hooks_presentation() || !sound_class_data || class_index < 0 ||
		class_index >= SOUND_HOOK_SOUND_CLASSES)
	{
		return __real_sound_class_get_gain(class_index);
	}
	return sound_class_copy(class_index)->gain;
}

/* ---------- stream events */

/* the sound definition (and its pitch range and permutation) whose
permutation context is; NONE if none */
static long sound_hooks_permutation_owner(
	void const *context,
	short *range_index,
	short *permutation_index)
{
	struct tag_iterator iterator;
	long definition_index;

	tag_iterator_new(&iterator, SOUND_DEFINITION_TAG);
	for (definition_index = tag_iterator_next(&iterator); definition_index != NONE;
		definition_index = tag_iterator_next(&iterator))
	{
		struct sound_definition *definition = sound_definition_get(definition_index);
		short range;

		for (range = 0; range < definition->pitch_ranges.count; range++)
		{
			struct sound_pitch_range *pitch_range =
				TAG_BLOCK_GET_ELEMENT(&definition->pitch_ranges, range, struct sound_pitch_range);
			byte const *first;

			if (!pitch_range->permutations.count)
				continue;
			first = (byte const *)TAG_BLOCK_GET_ELEMENT(&pitch_range->permutations, 0, struct sound_permutation);
			if ((byte const *)context >= first &&
				(byte const *)context < first + pitch_range->permutations.count * sizeof(struct sound_permutation))
			{
				*range_index = range;
				*permutation_index = (short)(((byte const *)context - first) / sizeof(struct sound_permutation));
				return definition_index;
			}
		}
	}

	return NONE;
}

static char const *const event_names[] = { "start", "end", "stop" };
static char const *const kind_names[NUMBER_OF_WII_SOUND_VOICE_KINDS] =
{
	"2d_mono", "3d_mono", "stereo_22k", "stereo_44k", "other"
};

static void sound_hooks_event(
	int event,
	int stream,
	int kind,
	void const *context)
{
	short range = NONE, permutation = NONE;
	long definition_index = NONE;
	short sound_class = NONE;
	long tick = game_in_progress() ? game_time_get() : NONE;

	if (!sound_hooks.active)
		return;
	if (event == _wii_sound_event_start || stream < 0 || stream >= SOUND_HOOK_STREAMS)
	{
		definition_index = context ? sound_hooks_permutation_owner(context, &range, &permutation) : NONE;
		if (definition_index != NONE)
			sound_class = sound_definition_get(definition_index)->sound_class;
		if (stream >= 0 && stream < SOUND_HOOK_STREAMS)
			sound_hooks.stream_class[stream] = sound_class;
	}
	else
	{
		/* (the stream's sound as it started: a looping sound queues the next
		permutation on the same stream) */
		sound_class = sound_hooks.stream_class[stream];
	}
	if (sound_class >= 0 && sound_class < SOUND_HOOK_CLASSES)
	{
		struct sound_hook_class *record = &sound_hooks.classes[sound_class];

		switch (event)
		{
		case _wii_sound_event_start: record->starts++; break;
		case _wii_sound_event_end: record->ends++; break;
		default: record->stops++; break;
		}
		if (record->first_tick == NONE)
			record->first_tick = tick;
		record->last_tick = tick;
		if (sound_hooks.log_events && sound_hooks.options.events && record->logged < SOUND_HOOK_EVENTS_PER_CLASS * 3)
		{
			record->logged++;
			wii_log("SOUND_EVENT tick=%ld clock_ms=%lu event=%s class=%d tag=%08lx range=%d permutation=%d stream=%d "
				"kind=%s\n", tick, wii_sound_clock_milliseconds(), event_names[event], sound_class,
				(unsigned long)definition_index, range, permutation, stream, kind_names[kind]);
		}
	}
}

/* ---------- the map run (wii_sound.h) */

void wii_sound_set_wanted(
	int wanted)
{
	sound_hooks.wanted = wanted ? TRUE : FALSE;
}

int wii_sound_wanted(
	void)
{
	return sound_hooks.wanted;
}

int wii_sound_started(
	void)
{
	return sound_hooks.started;
}

int wii_sound_map_active(
	void)
{
	return sound_hooks.active;
}

int wii_sound_map_start(
	const struct wii_sound_map_options *options)
{
	short index;

	if (sound_hooks.started)
		return sound_hooks.active;
	sound_hooks.options = *options;
	sound_hooks.wanted = TRUE;
	for (index = 0; index < SOUND_HOOK_CLASSES; index++)
		sound_hooks.classes[index].first_tick = NONE;
	/* the frame clock first, so the sound's clock starts with it */
	wii_boot_set_frame_clock(TRUE);
	wii_sound_pack_set_verify(options->verify_decode);
	wii_sound_set_event_hook(sound_hooks_event);
	sound_hooks.started = TRUE;
	sound_initialize();
	sound_hooks.active = sound_is_active() ? TRUE : FALSE;
	if (!sound_hooks.active)
		sound_hooks.started = FALSE;
	wii_log("SOUND_MAP start=%d listener=%s verify_decode=%d events=%d couplings=%s sounds_capacity=%ld "
		"looping_capacity=%ld\n", sound_hooks.active, options->listener_follows_player ? "player" : "observer",
		options->verify_decode, options->events, options->game_couplings ? "original" : "presentation", sound_data ? (long)sound_data->maximum_count : 0L,
		looping_sound_data ? (long)looping_sound_data->maximum_count : 0L);

	return sound_hooks.active;
}

void wii_sound_map_frame(
	float dt)
{
	unsigned long milliseconds;

	if (!sound_hooks.active)
		return;
	/* the frame clock moves as the run's frames' time (whole milliseconds of
	the run's time so far), then main_loop's main_game_render's sound */
	sound_hooks.run_seconds += (double)dt;
	milliseconds = (unsigned long)(sound_hooks.run_seconds * 1000.0);
	if (milliseconds > sound_hooks.run_milliseconds)
	{
		wii_boot_advance_frame_clock(milliseconds - sound_hooks.run_milliseconds);
		sound_hooks.run_milliseconds = milliseconds;
	}
	sound_render();
	sound_hooks.frames++;
	if (sound_data && sound_data->actual_count > sound_hooks.sounds_peak)
		sound_hooks.sounds_peak = sound_data->actual_count;
	if (looping_sound_data && looping_sound_data->actual_count > sound_hooks.looping_sounds_peak)
		sound_hooks.looping_sounds_peak = looping_sound_data->actual_count;
}

void wii_sound_map_run_begin(
	void)
{
	short index;

	if (!sound_hooks.active)
		return;
	sound_hooks.run_seconds = 0.0;
	sound_hooks.run_milliseconds = 0;
	sound_hooks.frames = 0;
	sound_hooks.sounds_peak = 0;
	sound_hooks.looping_sounds_peak = 0;
	sound_hooks.obstruction_tests = 0;
	sound_hooks.marks_not_restored = 0;
	sound_hooks.notifications_withheld = 0;
	sound_hooks.mouth_updates_withheld = 0;
	sound_hooks.indices_withheld = 0;
	csmemset(sound_class_copies, 0, sizeof(sound_class_copies));
	sound_hooks.events_logged = 0;
	sound_hooks.log_events = TRUE;
	csmemset(sound_hooks.classes, 0, sizeof(sound_hooks.classes));
	for (index = 0; index < SOUND_HOOK_CLASSES; index++)
		sound_hooks.classes[index].first_tick = NONE;
	wii_sound_mark();
}

void wii_sound_map_run_end(
	char const *map,
	int cycle,
	char const *cadence)
{
	struct wii_sound_report report;
	struct wii_sound_output_report output;
	struct wii_sound_pack_report pack;
	short index;
	char histogram[WII_SOUND_VOICE_HISTOGRAM * 11 + 1];
	long used = 0;

	if (!sound_hooks.active)
		return;
	sound_hooks.log_events = FALSE;
	wii_sound_report_get(&report);
	wii_sound_pack_report_get(&pack);
	wii_sound_output_report_get(&output);
	histogram[0] = 0;
	for (index = 0; index < WII_SOUND_VOICE_HISTOGRAM; index++)
		used += sprintf(histogram + used, "%s%lu", index ? "," : "", report.voice_calls[index]);
	wii_log("SOUND_RUN map=%s cycle=%d cadence=%s frames=%lu clock_ms=%lu work_calls=%lu frames_mixed=%lu "
		"skipped_frames=%lu voices_max=%lu voice_frames=%lu voices_by_calls=%s sounds_peak=%ld looping_sounds_peak=%ld "
		"streams=%lu packets_queued=%lu packets_completed=%lu packets_flushed=%lu blocks_decoded=%lu starts=%lu ends=%lu "
		"stops=%lu obstruction_tests=%lu marks_unrestored=%lu notifications_withheld=%lu mouth_withheld=%lu indices_withheld=%lu clipped=%lu mix_us=%lu mix_peak_us=%lu mix_crc=%08lx events_crc=%08lx pack_reads=%lu pack_bytes=%lu "
		"pack_pcm=%lu pack_failures=%lu pack_us=%lu decode_checks=%lu\n",
		map, cycle, cadence, sound_hooks.frames, sound_hooks.run_milliseconds, report.work_calls, report.frames_mixed,
		report.clock_skipped_frames, report.voices_maximum, report.voice_frames, histogram, sound_hooks.sounds_peak,
		sound_hooks.looping_sounds_peak, report.streams, report.packets_queued, report.packets_completed,
		report.packets_flushed, report.blocks_decoded, report.starts, report.ends, report.stops, sound_hooks.obstruction_tests,
		sound_hooks.marks_not_restored, sound_hooks.notifications_withheld, sound_hooks.mouth_updates_withheld,
		sound_hooks.indices_withheld,
		report.clipped_samples,
		(unsigned long)report.mix_microseconds, (unsigned long)report.mix_microseconds_peak, report.mix_crc,
		report.events_crc, pack.reads, pack.bytes, pack.pcm_reads, pack.failures, (unsigned long)pack.read_microseconds,
		pack.decode_checks);
	wii_log("SOUND_KIND map=%s cycle=%d cadence=%s 2d_mono=%lu 3d_mono=%lu stereo_22k=%lu stereo_44k=%lu other=%lu\n",
		map, cycle, cadence, report.voices_maximum_by_kind[_wii_sound_voice_2d_mono],
		report.voices_maximum_by_kind[_wii_sound_voice_3d_mono], report.voices_maximum_by_kind[_wii_sound_voice_stereo_22k],
		report.voices_maximum_by_kind[_wii_sound_voice_stereo_44k], report.voices_maximum_by_kind[_wii_sound_voice_other]);
	for (index = 0; index < SOUND_HOOK_CLASSES; index++)
	{
		struct sound_hook_class const *record = &sound_hooks.classes[index];

		if (!record->starts && !record->ends && !record->stops)
			continue;
		wii_log("SOUND_CLASS map=%s cycle=%d cadence=%s class=%d starts=%lu ends=%lu stops=%lu first_tick=%ld "
			"last_tick=%ld\n", map, cycle, cadence, index, record->starts, record->ends, record->stops,
			record->first_tick, record->last_tick);
	}
	if (output.buffers)
	{
		wii_log("SOUND_OUTPUT map=%s cycle=%d cadence=%s buffer_frames=%lu buffers=%lu latency_target=%lu written=%lu "
			"dropped=%lu played=%lu underruns=%lu underrun_runs=%lu longest_underrun=%lu silent_start=%lu fill_min=%lu "
			"fill_max=%lu\n", map, cycle, cadence, output.buffer_frames, output.buffers, output.latency_target_frames,
			output.frames_written, output.frames_dropped, output.frames_played, output.underruns, output.underrun_runs,
			output.longest_underrun, output.silent_start_buffers, output.fill_minimum, output.fill_maximum);
	}
}
