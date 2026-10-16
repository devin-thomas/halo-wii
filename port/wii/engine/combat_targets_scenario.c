/*
COMBAT_TARGETS_SCENARIO.C

(HWI-041) The real-map run's shootable targets, engine side
(combat_targets_scenario.h). It makes the targets and deals the harness's
hits through the engine's own object_new and object_cause_damage, and reads
their vitality and dead bit back from their objects; it never writes an
object's vitality or damage flags itself.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "tag_files/tag_groups.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "objects/damage.h"
#include "units/units.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "items/projectile_definitions.h"

#include "real_map_scenario.h"
#include "combat_targets_scenario.h"

static struct
{
	struct combat_targets_plan plan;
	struct combat_targets_state state;
	long object_indices[COMBAT_TARGETS_MAXIMUM];
	long damage_definition_index;
	void (*observer)(const struct combat_target_event *event);
} combat_targets_globals;

static void event_report(
	const struct combat_target_event *event)
{
	if (combat_targets_globals.observer)
		combat_targets_globals.observer(event);

	return;
}

static long local_player_index(
	void)
{
	return local_player_get_player_index(0);
}

static long local_player_unit(
	void)
{
	long player_index = local_player_index();
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	return player ? player->unit_index : NONE;
}

/* the impact damage of the projectile the unit's weapon's first trigger
fires, as a projectile's impact deals it (projectiles.c); NONE if it has
none */
static long weapon_impact_damage(
	long unit_index)
{
	struct unit_datum *unit = unit_try_and_get(unit_index);
	struct weapon_datum *weapon;
	struct weapon_definition *weapon_definition;
	struct weapon_trigger_definition *trigger;
	struct projectile_definition *projectile_definition;

	if (!unit || !VALID_INDEX(unit->unit.current_weapon_index, MAXIMUM_WEAPONS_PER_UNIT))
		return NONE;
	weapon = weapon_try_and_get(unit->unit.weapon_object_indices[unit->unit.current_weapon_index]);
	if (!weapon)
		return NONE;
	weapon_definition = weapon_definition_get(weapon->definition_index);
	if (weapon_definition->weapon.triggers.count < 1)
		return NONE;
	trigger = TAG_BLOCK_GET_ELEMENT(&weapon_definition->weapon.triggers, 0, struct weapon_trigger_definition);
	if (trigger->projectile.index == NONE)
		return NONE;
	projectile_definition = projectile_definition_get(trigger->projectile.index);

	return projectile_definition->projectile.impact_damage.index;
}

static void observation_get(
	long object_index,
	struct combat_target_observation *observation)
{
	struct object_datum *object = object_index != NONE ? object_try_and_get(object_index) : NULL;

	csmemset(observation, 0, sizeof(*observation));
	if (object)
	{
		observation->present = TRUE;
		observation->body_vitality = object->object.body_vitality;
		observation->shield_vitality = object->object.shield_vitality;
		observation->dead = TEST_FLAG(object->object.damage_flags, _object_dead_bit) ? TRUE : FALSE;
	}

	return;
}

static void target_observe(
	int target,
	long tick,
	int cause)
{
	struct combat_target_observation observation;
	struct combat_target_event events[COMBAT_TARGETS_MAXIMUM_EVENTS_PER_OBSERVATION];
	int count;
	int index;

	observation_get(combat_targets_globals.object_indices[target], &observation);
	count = combat_targets_observe(&combat_targets_globals.state, target, tick, cause, &observation, events);
	for (index = 0; index < count; index++)
		event_report(&events[index]);

	return;
}

static void refuse(
	int target,
	long tick,
	int refusal)
{
	struct combat_target_event event;

	combat_targets_refused(&combat_targets_globals.state, target, tick, refusal, &event);
	event_report(&event);

	return;
}

static void targets_spawn(
	long tick)
{
	long unit_index = local_player_unit();
	struct object_datum *unit;
	long definition_index;
	float origin[3], forward[3], facing[3];
	float positions[COMBAT_TARGETS_MAXIMUM][3];
	int target;

	if (unit_index == NONE)
	{
		refuse(-1, tick, COMBAT_TARGETS_REFUSAL_NO_PLAYER_UNIT);
		return;
	}
	combat_targets_globals.damage_definition_index = weapon_impact_damage(unit_index);
	unit = object_get(unit_index);
	definition_index = unit->definition_index;
	origin[0] = unit->object.position.x;
	origin[1] = unit->object.position.y;
	origin[2] = unit->object.position.z;
	forward[0] = unit->object.forward.i;
	forward[1] = unit->object.forward.j;
	forward[2] = unit->object.forward.k;
	combat_targets_place(origin, forward, combat_targets_globals.plan.count, COMBAT_TARGETS_DISTANCE,
		COMBAT_TARGETS_SPACING, positions, facing);
	for (target = 0; target < combat_targets_globals.plan.count; target++)
	{
		struct object_placement_data placement;
		struct object_datum *object;
		long object_index;

		/* as player spawning makes the player's unit (players.c), of the
		player's own definition, on the player's enemies' team */
		object_placement_data_new(&placement, definition_index, NONE);
		placement.position.x = positions[target][0];
		placement.position.y = positions[target][1];
		placement.position.z = positions[target][2];
		placement.forward.i = facing[0];
		placement.forward.j = facing[1];
		placement.forward.k = facing[2];
		placement.up = *global_up3d;
		placement.owner_team_index = _game_team_covenant;
		object_index = object_new(&placement);
		if (object_index == NONE)
		{
			refuse(target, tick, COMBAT_TARGETS_REFUSAL_NO_ROOM);
			break;
		}
		object = object_get(object_index);
		if (object->object.maximum_body_vitality <= 0.f)
		{
			object_delete(object_index);
			refuse(target, tick, COMBAT_TARGETS_REFUSAL_NOT_DAMAGEABLE);
			break;
		}
		combat_targets_globals.object_indices[target] = object_index;
		{
			struct combat_target_event event;

			combat_targets_spawned(&combat_targets_globals.state, target, tick, (unsigned long)object_index,
				object->object.body_vitality, object->object.shield_vitality, &event);
			event_report(&event);
		}
	}
	if (combat_targets_globals.plan.first_hit_tick && combat_targets_globals.damage_definition_index == NONE)
		refuse(-1, tick, COMBAT_TARGETS_REFUSAL_NO_IMPACT_DAMAGE);

	return;
}

/* one hit on the target, filled as a projectile's impact fills it */
static void target_hit(
	int target,
	long tick)
{
	long object_index = combat_targets_globals.object_indices[target];
	long unit_index = local_player_unit();
	struct object_datum *object = object_get(object_index);
	struct damage_data damage;
	real_vector3d direction;

	damage_data_new(&damage, combat_targets_globals.damage_definition_index);
	SET_FLAG(damage.flags, _damage_from_weapon_bit, TRUE);
	damage.epicenter = object->object.bounding_sphere_center;
	damage.origin = object->object.bounding_sphere_center;
	direction = *global_forward3d;
	if (unit_index != NONE)
	{
		struct object_datum *unit = object_get(unit_index);

		damage.owner_player_index = local_player_index();
		damage.owner_object_index = unit_index;
		damage.owner_team_index = unit->object.owner_team_index;
		vector_from_points3d(&unit->object.position, &object->object.bounding_sphere_center, &direction);
		if (normalize3d(&direction) == 0.f)
			direction = *global_forward3d;
	}
	damage.direction = direction;
	object_cause_damage(&damage, object_index, NONE, NONE, NONE, NULL);
	target_observe(target, tick, COMBAT_TARGET_CAUSE_HARNESS);

	return;
}

void combat_targets_scenario_set_plan(
	const struct combat_targets_plan *plan)
{
	int target;

	csmemset(&combat_targets_globals.plan, 0, sizeof(combat_targets_globals.plan));
	if (plan && plan->spawn_tick > 0)
		combat_targets_globals.plan = *plan;
	combat_targets_reset(&combat_targets_globals.state);
	for (target = 0; target < COMBAT_TARGETS_MAXIMUM; target++)
		combat_targets_globals.object_indices[target] = NONE;
	combat_targets_globals.damage_definition_index = NONE;

	return;
}

void combat_targets_scenario_set_observer(
	void (*observer)(const struct combat_target_event *event))
{
	combat_targets_globals.observer = observer;

	return;
}

void combat_targets_scenario_after_tick(
	long tick)
{
	int target;

	if (!combat_targets_globals.plan.spawn_tick || !real_map_armed())
		return;
	/* what the tick did to them */
	for (target = 0; target < combat_targets_globals.state.count; target++)
		target_observe(target, tick, COMBAT_TARGET_CAUSE_WORLD);
	if (tick == combat_targets_globals.plan.spawn_tick)
		targets_spawn(tick);
	if (combat_targets_hit_due(&combat_targets_globals.plan, tick) &&
		combat_targets_globals.damage_definition_index != NONE)
	{
		target = combat_targets_next_alive(&combat_targets_globals.state);
		if (target != -1)
			target_hit(target, tick);
	}

	return;
}

void combat_targets_scenario_state(
	struct combat_targets_state *state)
{
	*state = combat_targets_globals.state;

	return;
}

long combat_targets_scenario_damage_definition(
	void)
{
	return combat_targets_globals.damage_definition_index;
}
