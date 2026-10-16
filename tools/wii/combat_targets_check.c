/*
 * HWI-041: host check of the real-map run's target states and events
 * (port/wii/engine/combat_targets.c, compiled whole). Run by
 * tools/wii/test_combat_targets.py, which reads the lines below:
 *
 *   CHECK <name> <ok|FAIL>
 *   EVENT <kind> <target> <tick> <cause> <object> <body before/after bits>
 *         <shield before/after bits> <refusal> <destroyed>   (hex words)
 *   DIGEST <scenario> <8 hex digits>
 *   CHECKS passed=<n> failed=<n>
 *
 * The test recomputes each DIGEST from the EVENT lines with zlib's CRC-32,
 * so the encoding (big-endian words, IEEE bits) is checked independently.
 */

#include <stdio.h>
#include <string.h>

#include "../../port/wii/engine/combat_targets.h"

static int passed, failed;

static void check(const char *name, int ok)
{
	printf("CHECK %s %s\n", name, ok ? "ok" : "FAIL");
	if (ok)
		passed++;
	else
		failed++;
}

static unsigned int bits(float value)
{
	unsigned int word;

	memcpy(&word, &value, sizeof(word));
	return word;
}

static void event_print(const struct combat_target_event *event)
{
	printf("EVENT %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned int)event->kind,
		(unsigned int)event->target, (unsigned int)event->tick, (unsigned int)event->cause,
		(unsigned int)event->object_index, bits(event->body_before), bits(event->body_after),
		bits(event->shield_before), bits(event->shield_after), (unsigned int)event->refusal,
		(unsigned int)event->destroyed);
}

static struct combat_target_observation seen(int present, float body, float shield, int dead)
{
	struct combat_target_observation observation;

	observation.present = present;
	observation.body_vitality = body;
	observation.shield_vitality = shield;
	observation.dead = dead;
	return observation;
}

/* observe and print the events; returns their count */
static int observe(struct combat_targets_state *state, int target, long tick, int cause,
	struct combat_target_observation observation, struct combat_target_event *events)
{
	int count = combat_targets_observe(state, target, tick, cause, &observation, events);

	for (int index = 0; index < count; index++)
		event_print(&events[index]);
	return count;
}

static void check_parse(void)
{
	struct combat_targets_plan plan;

	check("parse_spawn_only", combat_targets_parse("40:3", 300, &plan) && plan.spawn_tick == 40 &&
		plan.count == 3 && plan.first_hit_tick == 0 && plan.hit_interval == 0);
	check("parse_with_hits", combat_targets_parse("10:4:20:15", 300, &plan) && plan.spawn_tick == 10 &&
		plan.count == 4 && plan.first_hit_tick == 20 && plan.hit_interval == 15);
	check("parse_count_one", combat_targets_parse("1:1", 2, &plan) && plan.count == 1);
	check("refuse_count_zero", !combat_targets_parse("40:0", 300, &plan) && plan.spawn_tick == 0);
	check("refuse_count_five", !combat_targets_parse("40:5", 300, &plan) && plan.count == 0);
	check("refuse_spawn_zero", !combat_targets_parse("0:2", 300, &plan));
	check("refuse_spawn_negative", !combat_targets_parse("-4:2", 300, &plan));
	check("refuse_spawn_at_end", !combat_targets_parse("300:2", 300, &plan));
	check("refuse_hit_before_spawn", !combat_targets_parse("40:2:40:5", 300, &plan));
	check("refuse_hit_at_end", !combat_targets_parse("40:2:300:5", 300, &plan));
	check("refuse_interval_zero", !combat_targets_parse("40:2:50:0", 300, &plan));
	check("refuse_interval_run", !combat_targets_parse("40:2:50:300", 300, &plan));
	check("refuse_half_hits", !combat_targets_parse("40:2:50", 300, &plan));
	check("refuse_trailing", !combat_targets_parse("40:2x", 300, &plan));
	check("refuse_empty", !combat_targets_parse("", 300, &plan));
	check("refuse_null", !combat_targets_parse(NULL, 300, &plan));
	check("refuse_no_colon", !combat_targets_parse("40", 300, &plan));
}

static void check_schedule(void)
{
	struct combat_targets_plan plan;
	int due[64];
	int count = 0;

	combat_targets_parse("10:2:20:15", 300, &plan);
	for (long tick = 1; tick <= 300 && count < 64; tick++)
	{
		if (combat_targets_hit_due(&plan, tick))
			due[count++] = (int)tick;
	}
	check("schedule_first", count > 0 && due[0] == 20);
	check("schedule_interval", count > 2 && due[1] == 35 && due[2] == 50);
	check("schedule_count", count == 19); /* 20, 35, ... 290 */
	combat_targets_parse("10:2", 300, &plan);
	count = 0;
	for (long tick = 1; tick <= 300; tick++)
		count += combat_targets_hit_due(&plan, tick);
	check("schedule_none_without_hits", count == 0);
	memset(&plan, 0, sizeof(plan));
	check("schedule_none_without_plan", !combat_targets_hit_due(&plan, 20));
}

/* the scenario the test recomputes: two targets, world and harness hits, a
death, a body hit again, a destruction outright and both removed */
static void check_lifecycle(void)
{
	struct combat_targets_state state;
	struct combat_target_event events[COMBAT_TARGETS_MAXIMUM_EVENTS_PER_OBSERVATION];
	struct combat_target_event event;
	int count;

	combat_targets_reset(&state);
	check("reset_empty", state.count == 0 && state.events == 0 && state.digest == 0 &&
		combat_targets_next_alive(&state) == -1 && state.targets[0].state == COMBAT_TARGET_STATE_EMPTY &&
		state.targets[0].killed_tick == -1);

	check("spawn_0", combat_targets_spawned(&state, 0, 40, 0xe1230005UL, 1.0f, 1.0f, &event) &&
		event.kind == COMBAT_TARGET_SPAWNED && event.target == 0 && event.tick == 40 &&
		state.targets[0].state == COMBAT_TARGET_STATE_ALIVE);
	event_print(&event);
	check("spawn_1", combat_targets_spawned(&state, 1, 40, 0xe1240006UL, 1.0f, 1.0f, &event));
	event_print(&event);
	check("spawn_slot_taken", !combat_targets_spawned(&state, 0, 41, 0x1UL, 1.0f, 1.0f, NULL));
	check("spawn_out_of_range", !combat_targets_spawned(&state, COMBAT_TARGETS_MAXIMUM, 41, 0x1UL, 1.0f, 1.0f, NULL) &&
		!combat_targets_spawned(&state, -1, 41, 0x1UL, 1.0f, 1.0f, NULL));
	check("spawned_counts", state.count == 2 && state.spawned == 2 && state.events == 2);
	check("next_alive_first", combat_targets_next_alive(&state) == 0);

	count = observe(&state, 0, 41, COMBAT_TARGET_CAUSE_WORLD, seen(1, 1.0f, 1.0f, 0), events);
	check("unchanged_no_event", count == 0 && state.events == 2);

	count = observe(&state, 0, 50, COMBAT_TARGET_CAUSE_HARNESS, seen(1, 1.0f, 0.25f, 0), events);
	check("shield_hit", count == 1 && events[0].kind == COMBAT_TARGET_HIT &&
		events[0].cause == COMBAT_TARGET_CAUSE_HARNESS && events[0].shield_before == 1.0f &&
		events[0].shield_after == 0.25f && events[0].body_after == 1.0f && state.harness_hits == 1);

	count = observe(&state, 0, 60, COMBAT_TARGET_CAUSE_WORLD, seen(1, 1.0f, 0.5f, 0), events);
	check("shield_recharge_no_event", count == 0 && state.targets[0].shield_vitality == 0.5f);

	count = observe(&state, 0, 65, COMBAT_TARGET_CAUSE_WORLD, seen(1, 0.75f, 0.0f, 0), events);
	check("world_hit", count == 1 && events[0].cause == COMBAT_TARGET_CAUSE_WORLD && events[0].body_before == 1.0f &&
		events[0].body_after == 0.75f && events[0].shield_before == 0.5f && state.world_hits == 1);

	count = observe(&state, 0, 80, COMBAT_TARGET_CAUSE_HARNESS, seen(1, -0.125f, 0.0f, 1), events);
	check("killing_hit", count == 2 && events[0].kind == COMBAT_TARGET_HIT && events[1].kind == COMBAT_TARGET_KILLED &&
		!events[1].destroyed && state.targets[0].state == COMBAT_TARGET_STATE_DEAD &&
		state.targets[0].killed_tick == 80 && state.kills == 1 && state.targets[0].harness_hits == 2);
	check("next_alive_after_death", combat_targets_next_alive(&state) == 1);

	count = observe(&state, 0, 95, COMBAT_TARGET_CAUSE_WORLD, seen(1, -0.5f, 0.0f, 1), events);
	check("body_hit_no_event", count == 0 && state.targets[0].hits == 3 && state.targets[0].body_vitality == -0.5f);

	count = observe(&state, 1, 95, COMBAT_TARGET_CAUSE_HARNESS, seen(0, 0.0f, 0.0f, 0), events);
	check("destroyed_outright", count == 2 && events[0].kind == COMBAT_TARGET_KILLED && events[0].destroyed &&
		events[1].kind == COMBAT_TARGET_REMOVED && state.targets[1].state == COMBAT_TARGET_STATE_REMOVED &&
		state.targets[1].killed_tick == 95 && state.targets[1].removed_tick == 95);
	check("next_alive_none", combat_targets_next_alive(&state) == -1);

	count = observe(&state, 0, 200, COMBAT_TARGET_CAUSE_WORLD, seen(0, 0.0f, 0.0f, 0), events);
	check("body_removed", count == 1 && events[0].kind == COMBAT_TARGET_REMOVED &&
		state.targets[0].removed_tick == 200 && state.removals == 2);

	count = observe(&state, 0, 201, COMBAT_TARGET_CAUSE_WORLD, seen(0, 0.0f, 0.0f, 0), events);
	check("removed_stays", count == 0);
	count = observe(&state, 2, 201, COMBAT_TARGET_CAUSE_WORLD, seen(0, 0.0f, 0.0f, 0), events);
	check("empty_slot_no_event", count == 0);
	count = observe(&state, COMBAT_TARGETS_MAXIMUM, 201, COMBAT_TARGET_CAUSE_WORLD, seen(1, 1.0f, 1.0f, 0), events);
	check("observe_out_of_range", count == 0);

	check("lifecycle_totals", state.spawned == 2 && state.hits == 3 && state.harness_hits == 2 &&
		state.world_hits == 1 && state.kills == 2 && state.removals == 2 && state.refusals == 0 && state.events == 9);
	printf("DIGEST lifecycle %08lx\n", state.digest);
	{
		struct combat_targets_state again;
		struct combat_target_event scratch[COMBAT_TARGETS_MAXIMUM_EVENTS_PER_OBSERVATION];
		struct combat_target_observation o;

		/* the same sequence again: the same digest; a tick apart: another */
		combat_targets_reset(&again);
		combat_targets_spawned(&again, 0, 40, 0xe1230005UL, 1.0f, 1.0f, NULL);
		combat_targets_spawned(&again, 1, 40, 0xe1240006UL, 1.0f, 1.0f, NULL);
		o = seen(1, 1.0f, 1.0f, 0);
		combat_targets_observe(&again, 0, 41, COMBAT_TARGET_CAUSE_WORLD, &o, scratch);
		o = seen(1, 1.0f, 0.25f, 0);
		combat_targets_observe(&again, 0, 50, COMBAT_TARGET_CAUSE_HARNESS, &o, scratch);
		o = seen(1, 1.0f, 0.5f, 0);
		combat_targets_observe(&again, 0, 60, COMBAT_TARGET_CAUSE_WORLD, &o, scratch);
		o = seen(1, 0.75f, 0.0f, 0);
		combat_targets_observe(&again, 0, 65, COMBAT_TARGET_CAUSE_WORLD, &o, scratch);
		o = seen(1, -0.125f, 0.0f, 1);
		combat_targets_observe(&again, 0, 80, COMBAT_TARGET_CAUSE_HARNESS, &o, scratch);
		o = seen(1, -0.5f, 0.0f, 1);
		combat_targets_observe(&again, 0, 95, COMBAT_TARGET_CAUSE_WORLD, &o, scratch);
		o = seen(0, 0.0f, 0.0f, 0);
		combat_targets_observe(&again, 1, 95, COMBAT_TARGET_CAUSE_HARNESS, &o, scratch);
		check("digest_repeatable_before_last", again.digest != state.digest);
		combat_targets_observe(&again, 0, 200, COMBAT_TARGET_CAUSE_WORLD, &o, scratch);
		check("digest_repeatable", again.digest == state.digest && again.events == state.events);
		combat_targets_reset(&again);
		combat_targets_spawned(&again, 0, 41, 0xe1230005UL, 1.0f, 1.0f, NULL);
		combat_targets_reset(&state);
		combat_targets_spawned(&state, 0, 40, 0xe1230005UL, 1.0f, 1.0f, NULL);
		check("digest_follows_tick", again.digest != state.digest);
	}
}

static void check_refusals(void)
{
	struct combat_targets_state state;
	struct combat_target_event event;

	combat_targets_reset(&state);
	combat_targets_refused(&state, -1, 40, COMBAT_TARGETS_REFUSAL_NO_PLAYER_UNIT, &event);
	event_print(&event);
	check("refusal_event", event.kind == COMBAT_TARGET_REFUSED && event.target == -1 &&
		event.refusal == COMBAT_TARGETS_REFUSAL_NO_PLAYER_UNIT && state.refusals == 1 &&
		state.first_refusal == COMBAT_TARGETS_REFUSAL_NO_PLAYER_UNIT && !state.hits_refused && state.count == 0);
	combat_targets_refused(&state, 2, 40, COMBAT_TARGETS_REFUSAL_NO_ROOM, &event);
	event_print(&event);
	check("refusal_first_kept", state.refusals == 2 && state.first_refusal == COMBAT_TARGETS_REFUSAL_NO_PLAYER_UNIT);
	combat_targets_refused(&state, -1, 40, COMBAT_TARGETS_REFUSAL_NO_IMPACT_DAMAGE, &event);
	event_print(&event);
	check("refusal_no_impact_damage_stops_hits", state.hits_refused && state.events == 3);
	printf("DIGEST refusals %08lx\n", state.digest);
	check("refusal_names", !strcmp(combat_targets_refusal_name(COMBAT_TARGETS_REFUSAL_NOT_DAMAGEABLE), "not_damageable") &&
		!strcmp(combat_targets_refusal_name(NUMBER_OF_COMBAT_TARGETS_REFUSALS), "?") &&
		!strcmp(combat_target_event_name(COMBAT_TARGET_KILLED), "killed") &&
		!strcmp(combat_target_cause_name(COMBAT_TARGET_CAUSE_HARNESS), "harness") &&
		!strcmp(combat_target_state_name(COMBAT_TARGET_STATE_DEAD), "dead"));
}

static int near(float a, float b)
{
	float d = a - b;

	return d < 1e-5f && d > -1e-5f;
}

static void check_placement(void)
{
	float origin[3] = {10.0f, -4.0f, 2.5f};
	float positions[COMBAT_TARGETS_MAXIMUM][3];
	float facing[3];
	float east[3] = {1.0f, 0.0f, 0.3f};
	float north[3] = {0.0f, 2.0f, 0.0f};
	float up[3] = {0.0f, 0.0f, 1.0f};

	combat_targets_place(origin, east, 3, COMBAT_TARGETS_DISTANCE, COMBAT_TARGETS_SPACING, positions, facing);
	check("place_east", near(positions[0][0], 13.0f) && near(positions[0][1], -5.25f) &&
		near(positions[1][0], 13.0f) && near(positions[1][1], -4.0f) &&
		near(positions[2][0], 13.0f) && near(positions[2][1], -2.75f) &&
		near(positions[0][2], 2.5f) && near(positions[2][2], 2.5f));
	check("place_east_facing", near(facing[0], -1.0f) && near(facing[1], 0.0f) && near(facing[2], 0.0f));

	combat_targets_place(origin, north, 2, COMBAT_TARGETS_DISTANCE, COMBAT_TARGETS_SPACING, positions, facing);
	check("place_north_normalised", near(positions[0][0], 10.625f) && near(positions[0][1], -1.0f) &&
		near(positions[1][0], 9.375f) && near(positions[1][1], -1.0f) && near(facing[1], -1.0f));

	combat_targets_place(origin, up, 1, COMBAT_TARGETS_DISTANCE, COMBAT_TARGETS_SPACING, positions, facing);
	check("place_vertical_falls_back", near(positions[0][0], 13.0f) && near(positions[0][1], -4.0f) &&
		near(facing[0], -1.0f));

	combat_targets_place(origin, east, COMBAT_TARGETS_MAXIMUM, COMBAT_TARGETS_DISTANCE, COMBAT_TARGETS_SPACING,
		positions, facing);
	check("place_four_apart", near(positions[0][1], -5.875f) && near(positions[3][1], -2.125f) &&
		near(positions[1][1] - positions[0][1], COMBAT_TARGETS_SPACING));
}

int main(void)
{
	check_parse();
	check_schedule();
	check_lifecycle();
	check_refusals();
	check_placement();
	printf("CHECKS passed=%d failed=%d\n", passed, failed);
	return failed ? 1 : 0;
}
