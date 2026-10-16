/*
FIRST_PERSON_WEAPONS.H

header included in hcex build.
*/

#ifndef __FIRST_PERSON_WEAPONS_H
#define __FIRST_PERSON_WEAPONS_H
#pragma once

/* ---------- headers */

#include "cseries/cseries.h"
#include "math/real_math.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

struct real_matrix4x3;
struct animation_state;
struct object_marker;
struct render_camera;

/* port (HWI-084): the pose first_person_weapon_build_node_matrices builds,
for a renderer that draws without first_person_weapon_render_update (the Wii
rasterizer): first_person_weapon_render_pose. */
enum
{
	FIRST_PERSON_WEAPON_RENDER_POSE_MAXIMUM_NODES = 64, /* MAXIMUM_NODES_PER_ANIMATION */
};

enum first_person_weapon_render_pose_result
{
	_first_person_weapon_render_pose_ok = 0,
	_first_person_weapon_render_pose_no_local_player,  /* no player at this local index */
	_first_person_weapon_render_pose_no_unit,          /* the player has no unit (dead), or the view model none yet */
	_first_person_weapon_render_pose_not_first_person, /* the director's perspective is not first person */
	_first_person_weapon_render_pose_zoomed,           /* the player is zoomed in */
	_first_person_weapon_render_pose_no_weapon,        /* no first-person weapon (switching, unarmed) */
	_first_person_weapon_render_pose_weapon_gone,      /* its weapon was deleted since the last tick */
	_first_person_weapon_render_pose_no_animations,    /* the weapon has no first-person animation graph */
	_first_person_weapon_render_pose_no_state_animation, /* no state animation (the engine's build asserts) */
	_first_person_weapon_render_pose_node_count,       /* the graph's nodes are not 1..64 */
	NUMBER_OF_FIRST_PERSON_WEAPON_RENDER_POSE_RESULTS
};

struct first_person_weapon_render_pose
{
	long unit_index;
	long weapon_index;
	long animation_graph_index;
	/* node_matrices: the animation graph's node domain (node_count nodes, in
	world space from the camera, as first_person_weapon->node_matrices) */
	short node_count;
	short pad;
	/* each model first_person_weapon_draw would draw (NONE: not drawn: no
	model, or its remapping table is not valid), its node count as the tag
	has it, and its remapping table: model node -> graph node, the first
	MIN(model nodes, 64) entries as model_build_remapping_table_for_animation_graph
	built them (unchecked here) */
	long weapon_model_index;
	short weapon_model_node_count;
	short weapon_node_remapping_table[FIRST_PERSON_WEAPON_RENDER_POSE_MAXIMUM_NODES];
	long hands_model_index;
	short hands_model_node_count;
	short hands_node_remapping_table[FIRST_PERSON_WEAPON_RENDER_POSE_MAXIMUM_NODES];
	struct real_matrix4x3 node_matrices[FIRST_PERSON_WEAPON_RENDER_POSE_MAXIMUM_NODES];
};

/* ---------- prototypes/FIRST_PERSON_WEAPONS.C */

short animation_update_render_only(
	long animation_graph_index,
	struct animation_state *state,
	long *sound_index);
void first_person_weapons_initialize(
	void);
void first_person_weapons_dispose(
	void);
void first_person_weapons_initialize_for_new_map(
	void);
void first_person_weapons_dispose_from_old_map(
	void);
void first_person_weapons_update(
	void);
long first_person_weapon_get_local_index(
	long weapon_index);
void first_person_weapon_draw(
	void);
short first_person_weapon_get_marker_by_name(
	long weapon_index,
	char const *name,
	struct object_marker *markers,
	short maximum_marker_count);
void first_person_weapon_render_update(
	void);
short first_person_weapon_get_marker_by_name_render(
	long weapon_index,
	char const *name,
	struct object_marker *markers,
	short maximum_marker_count);
void first_person_weapon_center_flashlight(
	long unit_index,
	real_point3d *position,
	real_vector3d *forward,
	real_vector3d *up);
boolean first_person_weapon_adjust_light(
	long weapon_index,
	char const *marker_name,
	real_point3d *position,
	real_vector3d *forward,
	real_vector3d *up);
void first_person_weapon_message_from_unit(
	long unit_index,
	short message_type);
void first_person_weapon_message_from_weapon(
	long weapon_index,
	short message_type);
struct real_matrix4x3 *first_person_weapon_get_node_matrix(
	short local_player_index,
	short node_index);
short first_person_weapon_render_pose(
	short local_player_index,
	struct render_camera const *camera,
	boolean store_orientations,
	struct first_person_weapon_render_pose *pose);

/* ---------- globals */

/* ---------- public code */

#endif // __FIRST_PERSON_WEAPONS_H
