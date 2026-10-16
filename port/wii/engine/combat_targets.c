/*
COMBAT_TARGETS.C

(HWI-041) The targets' states and events (combat_targets.h). C types and the
C library only: the engine reaches it through combat_targets_scenario.c, the
host check (tools/wii/combat_targets_check.c) directly.
*/

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "combat_targets.h"

static const char *const event_names[NUMBER_OF_COMBAT_TARGET_EVENTS] =
{
	"spawned", "hit", "killed", "removed", "refused"
};

static const char *const cause_names[NUMBER_OF_COMBAT_TARGET_CAUSES] =
{
	"world", "harness"
};

static const char *const refusal_names[NUMBER_OF_COMBAT_TARGETS_REFUSALS] =
{
	"none", "no_player_unit", "not_damageable", "no_room", "no_impact_damage"
};

static const char *const state_names[NUMBER_OF_COMBAT_TARGET_STATES] =
{
	"empty", "alive", "dead", "removed"
};

/* ---------- the plan */

static int parse_long(const char **cursor, long *value)
{
	char *end;

	*value = strtol(*cursor, &end, 10);
	if (end == *cursor)
		return 0;
	*cursor = end;
	return 1;
}

int combat_targets_parse(const char *value, long ticks, struct combat_targets_plan *plan)
{
	const char *cursor = value;
	long spawn_tick, count, first_hit_tick = 0, hit_interval = 0;

	memset(plan, 0, sizeof(*plan));
	if (!value || !parse_long(&cursor, &spawn_tick) || *cursor++ != ':' || !parse_long(&cursor, &count))
		return 0;
	if (*cursor == ':')
	{
		cursor++;
		if (!parse_long(&cursor, &first_hit_tick) || *cursor++ != ':' || !parse_long(&cursor, &hit_interval))
			return 0;
		if (first_hit_tick <= spawn_tick || first_hit_tick >= ticks || hit_interval < 1 || hit_interval >= ticks)
			return 0;
	}
	if (*cursor || spawn_tick < 1 || spawn_tick >= ticks || count < COMBAT_TARGETS_MINIMUM ||
		count > COMBAT_TARGETS_MAXIMUM)
	{
		return 0;
	}
	plan->spawn_tick = spawn_tick;
	plan->count = (int)count;
	plan->first_hit_tick = first_hit_tick;
	plan->hit_interval = hit_interval;
	return 1;
}

int combat_targets_hit_due(const struct combat_targets_plan *plan, long tick)
{
	return plan->spawn_tick > 0 && plan->first_hit_tick > 0 && tick >= plan->first_hit_tick &&
		(tick - plan->first_hit_tick) % plan->hit_interval == 0;
}

/* ---------- the digest: CRC-32 (IEEE) of big-endian words */

static void digest_word(unsigned long *crc, unsigned long word)
{
	for (int shift = 24; shift >= 0; shift -= 8)
	{
		*crc ^= (word >> shift) & 0xffUL;
		for (int bit = 0; bit < 8; bit++)
			*crc = (*crc >> 1) ^ (0xedb88320UL & (0UL - (*crc & 1UL)));
	}
}

static unsigned long float_bits(float value)
{
	unsigned int bits;

	memcpy(&bits, &value, sizeof(bits));
	return (unsigned long)bits;
}

static void event_record(struct combat_targets_state *state, const struct combat_target_event *event)
{
	unsigned long crc = ~state->digest;

	digest_word(&crc, (unsigned long)event->kind);
	digest_word(&crc, (unsigned long)(long)event->target);
	digest_word(&crc, (unsigned long)event->tick);
	digest_word(&crc, (unsigned long)event->cause);
	digest_word(&crc, event->object_index);
	digest_word(&crc, float_bits(event->body_before));
	digest_word(&crc, float_bits(event->body_after));
	digest_word(&crc, float_bits(event->shield_before));
	digest_word(&crc, float_bits(event->shield_after));
	digest_word(&crc, (unsigned long)event->refusal);
	digest_word(&crc, (unsigned long)event->destroyed);
	state->digest = ~crc & 0xffffffffUL;
	state->events++;
}

static void event_new(struct combat_target_event *event, int kind, int target, long tick, int cause,
	const struct combat_target_track *track)
{
	memset(event, 0, sizeof(*event));
	event->kind = kind;
	event->target = target;
	event->tick = tick;
	event->cause = cause;
	if (track)
	{
		event->object_index = track->object_index;
		event->body_before = event->body_after = track->body_vitality;
		event->shield_before = event->shield_after = track->shield_vitality;
	}
}

/* ---------- the targets */

void combat_targets_reset(struct combat_targets_state *state)
{
	memset(state, 0, sizeof(*state));
	for (int target = 0; target < COMBAT_TARGETS_MAXIMUM; target++)
	{
		state->targets[target].killed_tick = -1;
		state->targets[target].removed_tick = -1;
		state->targets[target].spawn_tick = -1;
	}
}

int combat_targets_spawned(struct combat_targets_state *state, int target, long tick, unsigned long object_index,
	float body_vitality, float shield_vitality, struct combat_target_event *event)
{
	struct combat_target_track *track;
	struct combat_target_event spawned;

	if (target < 0 || target >= COMBAT_TARGETS_MAXIMUM || state->targets[target].state != COMBAT_TARGET_STATE_EMPTY)
		return 0;
	track = &state->targets[target];
	track->state = COMBAT_TARGET_STATE_ALIVE;
	track->object_index = object_index;
	track->spawn_tick = tick;
	track->body_vitality = body_vitality;
	track->shield_vitality = shield_vitality;
	if (target >= state->count)
		state->count = target + 1;
	state->spawned++;
	event_new(&spawned, COMBAT_TARGET_SPAWNED, target, tick, COMBAT_TARGET_CAUSE_HARNESS, track);
	event_record(state, &spawned);
	if (event)
		*event = spawned;
	return 1;
}

void combat_targets_refused(struct combat_targets_state *state, int target, long tick, int refusal,
	struct combat_target_event *event)
{
	struct combat_target_event refused;

	event_new(&refused, COMBAT_TARGET_REFUSED, target, tick, COMBAT_TARGET_CAUSE_HARNESS, NULL);
	refused.refusal = refusal;
	if (!state->refusals)
		state->first_refusal = refusal;
	state->refusals++;
	if (refusal == COMBAT_TARGETS_REFUSAL_NO_IMPACT_DAMAGE)
		state->hits_refused = 1;
	event_record(state, &refused);
	if (event)
		*event = refused;
}

int combat_targets_observe(struct combat_targets_state *state, int target, long tick, int cause,
	const struct combat_target_observation *observation, struct combat_target_event *events)
{
	struct combat_target_track *track;
	int count = 0;

	if (target < 0 || target >= COMBAT_TARGETS_MAXIMUM)
		return 0;
	track = &state->targets[target];
	if (track->state == COMBAT_TARGET_STATE_ALIVE && observation->present)
	{
		/* a hit: either vitality went down (a shield's recharge, upward, is
		no event) */
		if (observation->body_vitality < track->body_vitality ||
			observation->shield_vitality < track->shield_vitality)
		{
			struct combat_target_event *event = &events[count++];

			event_new(event, COMBAT_TARGET_HIT, target, tick, cause, track);
			event->body_after = observation->body_vitality;
			event->shield_after = observation->shield_vitality;
			event_record(state, event);
			track->hits++;
			state->hits++;
			if (cause == COMBAT_TARGET_CAUSE_HARNESS)
			{
				track->harness_hits++;
				state->harness_hits++;
			}
			else
			{
				state->world_hits++;
			}
		}
		track->body_vitality = observation->body_vitality;
		track->shield_vitality = observation->shield_vitality;
		if (observation->dead)
		{
			struct combat_target_event *event = &events[count++];

			event_new(event, COMBAT_TARGET_KILLED, target, tick, cause, track);
			event_record(state, event);
			track->state = COMBAT_TARGET_STATE_DEAD;
			track->killed_tick = tick;
			state->kills++;
		}
	}
	else if (track->state == COMBAT_TARGET_STATE_ALIVE)
	{
		/* gone while alive: destroyed outright (object_destroy deletes the
		object as it kills it) */
		struct combat_target_event *event = &events[count++];

		event_new(event, COMBAT_TARGET_KILLED, target, tick, cause, track);
		event->destroyed = 1;
		event_record(state, event);
		track->state = COMBAT_TARGET_STATE_DEAD;
		track->killed_tick = tick;
		state->kills++;
	}
	else if (track->state == COMBAT_TARGET_STATE_DEAD && observation->present)
	{
		/* a body: its vitality is kept, its further damage is no hit */
		track->body_vitality = observation->body_vitality;
		track->shield_vitality = observation->shield_vitality;
	}
	if (track->state == COMBAT_TARGET_STATE_DEAD && !observation->present)
	{
		struct combat_target_event *event = &events[count++];

		event_new(event, COMBAT_TARGET_REMOVED, target, tick, cause, track);
		event_record(state, event);
		track->state = COMBAT_TARGET_STATE_REMOVED;
		track->removed_tick = tick;
		state->removals++;
	}
	return count;
}

int combat_targets_next_alive(const struct combat_targets_state *state)
{
	for (int target = 0; target < state->count; target++)
	{
		if (state->targets[target].state == COMBAT_TARGET_STATE_ALIVE)
			return target;
	}
	return -1;
}

/* ---------- placement */

void combat_targets_place(const float origin[3], const float forward[3], int count, float distance, float spacing,
	float positions[][3], float facing[3])
{
	float x = forward[0], y = forward[1];
	float length = sqrtf(x * x + y * y);

	if (length > 0.0f)
	{
		x /= length;
		y /= length;
	}
	else
	{
		x = 1.0f;
		y = 0.0f;
	}
	for (int target = 0; target < count; target++)
	{
		/* across: the forward turned a quarter left, offsets centred on 0 */
		float across = ((float)target - (float)(count - 1) * 0.5f) * spacing;

		positions[target][0] = origin[0] + x * distance - y * across;
		positions[target][1] = origin[1] + y * distance + x * across;
		positions[target][2] = origin[2];
	}
	facing[0] = -x;
	facing[1] = -y;
	facing[2] = 0.0f;
}

/* ---------- names */

const char *combat_target_event_name(int kind)
{
	return kind >= 0 && kind < NUMBER_OF_COMBAT_TARGET_EVENTS ? event_names[kind] : "?";
}

const char *combat_target_cause_name(int cause)
{
	return cause >= 0 && cause < NUMBER_OF_COMBAT_TARGET_CAUSES ? cause_names[cause] : "?";
}

const char *combat_targets_refusal_name(int refusal)
{
	return refusal >= 0 && refusal < NUMBER_OF_COMBAT_TARGETS_REFUSALS ? refusal_names[refusal] : "?";
}

const char *combat_target_state_name(int state)
{
	return state >= 0 && state < NUMBER_OF_COMBAT_TARGET_STATES ? state_names[state] : "?";
}
