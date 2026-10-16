/*
COMBAT_TARGETS.H

(HWI-041) Shootable targets in the real-map run: what a run's targets did,
tick by tick, read from the engine's own objects (combat_targets_scenario.c
spawns them with object_new and hits them with object_cause_damage; health,
shields and death are the engine's damage machinery's, never this file's).

This half decides nothing about the game. It keeps each target's state as
the engine reports it after a tick (present, body and shield vitality, the
object's dead bit) and turns the changes into events:

  spawned  the target's object was made (its vitality as made)
  hit      its body or shield vitality went down (cause: the harness's hit
           at this tick boundary, or the world: a projectile, an explosion)
  killed   its object's dead bit came on (or the object went while alive:
           destroyed outright)
  removed  its object is gone (garbage collected, deleted, destroyed)
  refused  a spawn or the harness's hits could not be made (why: below)

Every event goes into a CRC-32 of a fixed encoding (big-endian words, the
vitalities' IEEE bits), so the same run gives the same digest on the Wii
and the host. Only C types: compiled into the engine and into the host
check (tools/wii/combat_targets_check.c).
*/

#ifndef __HALO_WII_COMBAT_TARGETS_H
#define __HALO_WII_COMBAT_TARGETS_H

#define COMBAT_TARGETS_MAXIMUM 4
#define COMBAT_TARGETS_MINIMUM 1
/* an observation's events: a hit, killed and removed at most */
#define COMBAT_TARGETS_MAXIMUM_EVENTS_PER_OBSERVATION 3
/* where the targets stand: in front of the player, side by side, facing it */
#define COMBAT_TARGETS_DISTANCE 3.0f
#define COMBAT_TARGETS_SPACING 1.25f

enum
{
	COMBAT_TARGET_SPAWNED,
	COMBAT_TARGET_HIT,
	COMBAT_TARGET_KILLED,
	COMBAT_TARGET_REMOVED,
	COMBAT_TARGET_REFUSED,
	NUMBER_OF_COMBAT_TARGET_EVENTS
};

enum
{
	COMBAT_TARGET_CAUSE_WORLD,   /* what the tick did */
	COMBAT_TARGET_CAUSE_HARNESS, /* the harness's hit at this tick boundary */
	NUMBER_OF_COMBAT_TARGET_CAUSES
};

enum
{
	COMBAT_TARGETS_REFUSAL_NONE,
	COMBAT_TARGETS_REFUSAL_NO_PLAYER_UNIT,   /* no local player unit to stand them before */
	COMBAT_TARGETS_REFUSAL_NOT_DAMAGEABLE,   /* the unit made has no body vitality (no collision model) */
	COMBAT_TARGETS_REFUSAL_NO_ROOM,          /* object_new made nothing (the object pool is full) */
	COMBAT_TARGETS_REFUSAL_NO_IMPACT_DAMAGE, /* the player's weapon has no trigger, projectile or impact damage: no harness hits */
	NUMBER_OF_COMBAT_TARGETS_REFUSALS
};

enum
{
	COMBAT_TARGET_STATE_EMPTY,
	COMBAT_TARGET_STATE_ALIVE,
	COMBAT_TARGET_STATE_DEAD,
	COMBAT_TARGET_STATE_REMOVED,
	NUMBER_OF_COMBAT_TARGET_STATES
};

/* a plan line's targets=<spawn tick>:<count>[:<first hit tick>:<interval>] */
struct combat_targets_plan
{
	long spawn_tick;     /* 0: no targets */
	int count;
	long first_hit_tick; /* 0: no harness hits (live fire only) */
	long hit_interval;
};

/* a target's object as the engine has it after a tick */
struct combat_target_observation
{
	int present;                 /* the object's datum index still names a live object */
	float body_vitality;         /* object.body_vitality: 1 whole, 0 or below depleted */
	float shield_vitality;
	int dead;                    /* object.damage_flags' _object_dead_bit */
};

struct combat_target_event
{
	int kind;
	int target;                  /* -1: the whole set's (a refusal before any spawn) */
	long tick;
	int cause;
	unsigned long object_index;
	float body_before, body_after;
	float shield_before, shield_after;
	int refusal;
	int destroyed;               /* killed: the object went while alive */
};

struct combat_target_track
{
	int state;
	unsigned long object_index;
	long spawn_tick;
	long killed_tick;            /* -1: not yet */
	long removed_tick;
	float body_vitality;
	float shield_vitality;
	long hits;
	long harness_hits;
};

struct combat_targets_state
{
	struct combat_target_track targets[COMBAT_TARGETS_MAXIMUM];
	int count;                   /* targets spawned */
	unsigned long digest;        /* CRC-32 of every event, in order */
	long events;
	long spawned;
	long hits;
	long harness_hits;
	long world_hits;
	long kills;
	long removals;
	long refusals;
	int first_refusal;
	int hits_refused;            /* NO_IMPACT_DAMAGE: the harness deals none */
};

/* 1 if value is "<spawn tick>:<count>[:<first hit tick>:<interval>]" with
1 <= spawn tick < ticks, 1 <= count <= COMBAT_TARGETS_MAXIMUM, and if given
spawn tick < first hit tick < ticks, 1 <= interval < ticks; plan is all
zero otherwise */
int combat_targets_parse(const char *value, long ticks, struct combat_targets_plan *plan);
/* a harness hit falls after this tick */
int combat_targets_hit_due(const struct combat_targets_plan *plan, long tick);

void combat_targets_reset(struct combat_targets_state *state);
/* a target made; the event (if non-NULL) is filled. 0 if the slot is taken or out of range */
int combat_targets_spawned(struct combat_targets_state *state, int target, long tick, unsigned long object_index,
	float body_vitality, float shield_vitality, struct combat_target_event *event);
/* a refusal (target -1 for the set); the event is filled */
void combat_targets_refused(struct combat_targets_state *state, int target, long tick, int refusal,
	struct combat_target_event *event);
/* a target's object after a tick or a hit; returns the events (at most
COMBAT_TARGETS_MAXIMUM_EVENTS_PER_OBSERVATION) written to events */
int combat_targets_observe(struct combat_targets_state *state, int target, long tick, int cause,
	const struct combat_target_observation *observation, struct combat_target_event *events);
/* the first target alive (the harness's next to hit), or -1 */
int combat_targets_next_alive(const struct combat_targets_state *state);

/* the targets' places: count of them distance in front of origin along
forward's horizontal part (x, y; +x if it has none), spacing apart across
it, at origin's height; facing is the way back to origin */
void combat_targets_place(const float origin[3], const float forward[3], int count, float distance, float spacing,
	float positions[][3], float facing[3]);

const char *combat_target_event_name(int kind);
const char *combat_target_cause_name(int cause);
const char *combat_targets_refusal_name(int refusal);
const char *combat_target_state_name(int state);

#endif
