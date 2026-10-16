/*
COMBAT_TARGETS_SCENARIO.H

(HWI-041) Shootable targets in the real-map run, through the engine's own
objects and damage. A plan line's targets=<spawn tick>:<count>[:<first hit
tick>:<interval>] (engine_map_run.h) has, in every run but the warm-up:

- after the spawn tick, count (1-4) units of the local player's own unit
  definition (the map's biped, original content: its collision model gives
  its body and shield vitality) made by object_new, as player spawning makes
  the player's, COMBAT_TARGETS_DISTANCE in front of the player, side by
  side, facing it, on the covenant team (enemies of the player). No AI actor
  drives them: they stand where the engine's physics puts them.
- after the first hit tick and every interval after it, one hit on the first
  target alive, dealt by object_cause_damage with the impact damage of the
  projectile the player's weapon's first trigger fires, filled as a
  projectile's impact fills it (projectiles.c): from the weapon, owned by
  the player and its unit. The damage effect's range, the resistances, the
  shields, the dead bit and an outright destruction are the engine's.
- after every tick, each target's object read back (combat_targets.h): the
  tick's own hits (the player's live fire, an explosion) are the world's.

Nothing runs unless a run is armed with a plan that asks for targets: the
boot mode and ordinary play never make them. Only C types here:
engine_map_run.c includes it beside the platform layer.
*/

#ifndef __HALO_WII_COMBAT_TARGETS_SCENARIO_H
#define __HALO_WII_COMBAT_TARGETS_SCENARIO_H

#include "combat_targets.h"

/* the next runs' targets (NULL or a zero spawn tick: none); clears the last run's */
void combat_targets_scenario_set_plan(const struct combat_targets_plan *plan);
/* each event as it happens (NULL: none) */
void combat_targets_scenario_set_observer(void (*observer)(const struct combat_target_event *event));
/* after each tick of an armed run (engine_map_run.c's tick observer) */
void combat_targets_scenario_after_tick(long tick);
/* the last run's targets */
void combat_targets_scenario_state(struct combat_targets_state *state);
/* the damage effect the harness's hits use (its tag index, -1 for none) */
long combat_targets_scenario_damage_definition(void);

#endif
