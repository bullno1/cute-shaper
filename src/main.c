#include <cute.h>
#include <dcimgui.h>
#include <string.h>
#include <nfd.h>

#define MAX_NUM_VERTICES 128
#define MAX_HISTORY_ENTRIES 128

static const float VERT_SIZE = 5.f;

typedef struct {
	CF_V2 verts[MAX_NUM_VERTICES];
	int num_vertices;
} shape_t;

typedef struct {
	shape_t shape;
	uint64_t version;
} shape_history_entry_t;

typedef struct {
	shape_history_entry_t entries[MAX_HISTORY_ENTRIES];
	int current_index;
	uint64_t current_version;
} shape_history_t;

typedef struct {
	CF_V2* point;
	float scale;
	CF_MouseButton button;
} mouse_drag_info_t;

static float
point_to_segment_distance_squared(CF_V2 p, CF_V2 a, CF_V2 b) {
    CF_V2 ab = cf_sub(b, a);
    CF_V2 ap = cf_sub(p, a);

    float ab2 = cf_dot(ab, ab);
    float ap_ab = cf_dot(ap, ab);

    float t = ab2 > 0.0f ? ap_ab / ab2 : 0.0f;  // handle zero-length segment
    t = fmaxf(0.0f, fminf(1.0f, t));

    CF_V2 closest = cf_add(a, cf_mul(ab, t));

	CF_V2 d = cf_sub(p, closest);

    return cf_dot(d, d);
}

static void
mouse_drag_point(CF_Coroutine coro) {
	mouse_drag_info_t drag_info = *(mouse_drag_info_t*)cf_coroutine_get_udata(coro);
	CF_V2 original_value = *drag_info.point;
	CF_V2 original_mouse_pos = { cf_mouse_x(), cf_mouse_y() };

	while (cf_mouse_down(drag_info.button)) {
		CF_V2 mouse_pos = { cf_mouse_x(), cf_mouse_y() };
		CF_V2 mouse_delta = cf_sub(mouse_pos, original_mouse_pos);
		mouse_delta.y = -mouse_delta.y;
		CF_V2 point_delta = cf_div(mouse_delta, drag_info.scale);
		*drag_info.point = cf_add(original_value, point_delta);

		cf_coroutine_yield(coro);
	}
}

static CF_Coroutine
start_mouse_drag(mouse_drag_info_t* drag_info) {
	CF_Coroutine coro = cf_make_coroutine(mouse_drag_point, 0, drag_info);
	cf_coroutine_resume(coro);  // Let it copy the userdata before it goes out of scope
	return coro;
}

static shape_t*
commit_shape(shape_history_t* history) {
	shape_history_entry_t* current_entry = &history->entries[history->current_index];
	int next_index = (history->current_index + 1) % MAX_HISTORY_ENTRIES;
	history->entries[next_index] = *current_entry;
	history->entries[next_index].version = ++history->current_version;
	history->current_index = next_index;
	return &history->entries[next_index].shape;
}

int
main(int argc, const char* argv[]) {
	NFD_Init();

	int options = CF_APP_OPTIONS_WINDOW_POS_CENTERED_BIT;
	cf_make_app("cute shaper", 0, 0, 0, 640, 480, options, argv[0]);
	cf_fs_mount(cf_fs_get_working_directory(), "/", true);
	cf_app_set_vsync(true);
	cf_clear_color(0.5f, 0.5f, 0.5f, 0.f);
	cf_app_init_imgui();

	int sprite_index = 0;
	(void)sprite_index;
	CF_Sprite sprite = cf_make_demo_sprite();

	int animation_index = 3;
	(void)animation_index;
	cf_sprite_play(&sprite, "hold_down");
	float draw_scale = 1.f;
	CF_V2 draw_offset = { 0.f, 0.f };

	bool show_help = false;
	CF_Coroutine mouse_coro = { 0 };

	shape_history_t history = { 0 };
	shape_t* shape = &history.entries[history.current_index].shape;

	while (cf_app_is_running()) {
		cf_app_update(NULL);
		cf_sprite_update(&sprite);

		cf_draw_push();
			cf_draw_translate_v2(draw_offset);
			cf_draw_scale(draw_scale, draw_scale);
			CF_M3x2 draw_transform = cf_draw_peek();

			cf_draw_sprite(&sprite);

			cf_draw_polyline(shape->verts, shape->num_vertices, 0.2f, true);
		cf_draw_pop();

		CF_V2 mouse_world = cf_screen_to_world(cf_v2(cf_mouse_x(), cf_mouse_y()));

		// Draw outside of transform for a consistent shape size
		int hovered_vert = -1;
		for (int i = 0; i < shape->num_vertices; ++i) {
			CF_V2 vert = cf_mul(draw_transform, shape->verts[i]);

			bool vert_hovered = cf_len(cf_sub(vert, mouse_world)) <= VERT_SIZE;
			CF_Color vert_color = vert_hovered ? cf_color_green() : cf_color_white();
			vert_color.a = 0.5f;
			if (vert_hovered) {
				hovered_vert = i;
			}

			cf_draw_push_color(vert_color);
			cf_draw_circle_fill2(vert, VERT_SIZE);
			cf_draw_pop_color();
		}

		if (ImGui_BeginMainMenuBar()) {
			if (ImGui_BeginMenu("File")) {
				if (ImGui_MenuItem("New")) {
				}

				if (ImGui_MenuItem("Open")) {
				}

				if (ImGui_MenuItem("Save")) {
				}
				ImGui_EndMenu();
			}

			if (ImGui_BeginMenu("Sprite")) {
				if (ImGui_MenuItem("Load")) {
				}

				if (ImGui_BeginMenu("Animation")) {
					for (int i = 0; i < hsize(sprite.animations); ++i) {
						if (ImGui_MenuItem(sprite.animations[i]->name)) {
							cf_sprite_play(&sprite, sprite.animations[i]->name);
						}
					}
					ImGui_EndMenu();
				}
				ImGui_EndMenu();
			}

			if (ImGui_BeginMenu("Help")) {
				if (ImGui_MenuItem("How to use")) {
					show_help = true;
				}
				ImGui_EndMenu();
			}
			ImGui_EndMainMenuBar();
		}

		if (show_help) {
			if (ImGui_Begin("Help", &show_help, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui_Text(
					"Left click: Add vertex\n"
					"Right click: Remove vertex\n"
					"Middle mouse drag: Pan\n"
					"Scroll: Zoom"
				);
			}
			ImGui_End();
		}

		if (!ImGui_GetIO()->WantCaptureMouse) {
			if (mouse_coro.id != 0) {
				cf_coroutine_resume(mouse_coro);
				if (cf_coroutine_state(mouse_coro) == CF_COROUTINE_STATE_DEAD) {
					cf_destroy_coroutine(mouse_coro);
					mouse_coro.id = 0;
				}
			} else {
				if (cf_mouse_just_pressed(CF_MOUSE_BUTTON_MIDDLE)) {
					mouse_coro = start_mouse_drag(&(mouse_drag_info_t){
						.point = &draw_offset,
						.button = CF_MOUSE_BUTTON_MIDDLE,
						.scale = 1.f,
					});
				} else if (cf_mouse_just_pressed(CF_MOUSE_BUTTON_LEFT)) {
					shape = commit_shape(&history);

					CF_V2* dragged_vert = NULL;
					if (hovered_vert >= 0) {
						dragged_vert = &shape->verts[hovered_vert];
					} else if (shape->num_vertices < MAX_NUM_VERTICES) {
						CF_V2 new_vert = cf_mul(cf_invert(draw_transform), mouse_world);

						if (shape->num_vertices < 3) {
							// Insert at the end
							shape->verts[shape->num_vertices++] = new_vert;
							dragged_vert = &shape->verts[shape->num_vertices - 1];
						} else {
							// Insert between the closest edge
							float closest_distant_sq = 1.0f / 0.0f;
							int insert_index = shape->num_vertices;
							for (int i = 0; i < shape->num_vertices; ++i) {
								CF_V2 a = shape->verts[i];
								CF_V2 b = shape->verts[(i + 1) % shape->num_vertices];

								float distance_sq = point_to_segment_distance_squared(new_vert, a, b);
								if (distance_sq < closest_distant_sq) {
									closest_distant_sq = distance_sq;
									insert_index = i;
								}
							}

							memmove(
								&shape->verts[insert_index + 2],
								&shape->verts[insert_index + 1],
								(shape->num_vertices - insert_index - 1) * sizeof(shape->verts[0])
							);
							shape->verts[insert_index + 1] = new_vert;
							++shape->num_vertices;
							dragged_vert = &shape->verts[insert_index + 1];
						}
					}

					if (dragged_vert != NULL) {
						mouse_coro = start_mouse_drag(&(mouse_drag_info_t){
							.point = dragged_vert,
							.button = CF_MOUSE_BUTTON_LEFT,
							.scale = draw_scale,
						});
					}
				} else if (cf_mouse_just_pressed(CF_MOUSE_BUTTON_RIGHT) && hovered_vert >= 0) {
					shape = commit_shape(&history);
					memmove(
						&shape->verts[hovered_vert],
						&shape->verts[hovered_vert + 1],
						(shape->num_vertices - hovered_vert) * sizeof(shape->verts[0])
					);
					--shape->num_vertices;
				} else if (cf_mouse_just_pressed(CF_MOUSE_BUTTON_X1)) {
					int prev_index = history.current_index - 1;
					if (prev_index < 0) { prev_index += MAX_HISTORY_ENTRIES; }
					shape_history_entry_t* prev_entry = &history.entries[prev_index];
					if (prev_entry->version < history.entries[history.current_index].version) {
						history.current_index = prev_index;
						shape = &prev_entry->shape;
					}
				} else if (cf_mouse_just_pressed(CF_MOUSE_BUTTON_X2)) {
					int next_index = (history.current_index + 1) % MAX_HISTORY_ENTRIES;
					shape_history_entry_t* next_entry = &history.entries[next_index];
					if (next_entry->version > history.entries[history.current_index].version) {
						history.current_index = next_index;
						shape = &next_entry->shape;
					}
				} else if (cf_mouse_wheel_motion() != 0.f) {
					draw_scale += cf_mouse_wheel_motion();
				}
			}
		}

		cf_app_draw_onto_screen(true);
	}

	if (mouse_coro.id != 0) {
		cf_destroy_coroutine(mouse_coro);
	}

	cf_destroy_app();
	NFD_Quit();
	return 0;
}
