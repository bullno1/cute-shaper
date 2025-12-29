#include <cute.h>
#include <dcimgui.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
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

typedef struct {
	char* filename;

	uint64_t saved_version;
} document_t;

typedef struct {
	const char* message;
	ImGuiID id;
} error_popup_t;

typedef enum {
	COMMAND_NOOP,
	COMMAND_NEW,
	COMMAND_OPEN,
	COMMAND_SAVE,
	COMMAND_SAVE_AS,
} command_t;

typedef enum {
	SAVE_OK,
	SAVE_CANCELLED,
	SAVE_ERROR,
} save_result_t;

static bool
str_ends_with(const char *str, const char *suffix) {
	if (!str || !suffix) { return false; }
	size_t lenstr = strlen(str);
	size_t lensuffix = strlen(suffix);
	if (lensuffix > lenstr) { return false; }

	return strncmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}

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

static void
start_mouse_drag(CF_Coroutine* modal_coro, mouse_drag_info_t* drag_info) {
	if (modal_coro->id != 0) { return; }

	*modal_coro = cf_make_coroutine(mouse_drag_point, 0, drag_info);
	cf_coroutine_resume(*modal_coro);  // Let it copy the userdata before it goes out of scope
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

// Own loader because cf_fs is constrained by the VFS and remounting is troublesome
static void*
load_file_into_memory(const char* path, size_t* out_size) {
	FILE *f = fopen(path, "rb");
	if (f == NULL) { return NULL; }

	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}

	long size = ftell(f);
	if (size < 0) {
		fclose(f);
		return NULL;
	}
	rewind(f);

	void* data = cf_alloc((size_t)size);
	if (!data) {
		fclose(f);
		return NULL;
	}

	size_t read = fread(data, 1, (size_t)size, f);
	fclose(f);

	if (read != (size_t)size) {
		cf_free(data);
		return NULL;
	}

	if (out_size != NULL) {
		*out_size = (size_t)size;
	}

	return data;
}

static bool
save_memory_into_file(const char* path, const void* data, size_t size) {
	FILE* f = fopen(path, "wb");
	if (f == NULL) { return false; }

	size_t written = fwrite(data, 1, size, f);
	fclose(f);

	return written == size;
}

static CF_Sprite
load_sprite(const char* path, const void* content, size_t size) {
	if (str_ends_with(path, ".ase") || str_ends_with(path, ".asperite")) {
		return cf_make_sprite_from_memory(
			path,
			content, size
		);
	} else if (str_ends_with(path, ".png")) {
		CF_Sprite sprite = cf_sprite_defaults();
		CF_Image img;
		CF_Result result = cf_image_load_png_from_memory(content, size, &img);
		if (!cf_is_error(result)) {
			sprite = cf_make_easy_sprite_from_pixels(img.pix, img.w, img.h);
			cf_image_premultiply(&img);
		}
		cf_image_free(&img);
		return sprite;
	} else {
		return cf_sprite_defaults();
	}
}

static char* title_buf = NULL;
static void
set_title(const document_t* doc, uint64_t current_version) {
	const char* title = doc->filename;
	if (title == NULL) { title = "untitled"; }

	if (doc->saved_version != current_version) {
		int size = snprintf(NULL, 0, "%s *", title);
		title_buf = cf_realloc(title_buf, size + 1);
		snprintf(title_buf, size + 1, "%s *", title);
		cf_app_set_title(title_buf);
	} else {
		cf_app_set_title(title);
	}
}

static char*
strclone(const char* str) {
	size_t len = strlen(str);
	char* cpy = cf_alloc(len + 1);
	memcpy(cpy, str, len + 1);
	return cpy;
}

static char*
strprintf(const char* fmt, ...) {
	va_list args, args_copy;
	va_start(args, fmt);
	va_copy(args_copy, args);
	int size = vsnprintf(NULL, 0, fmt, args);
	va_end(args);

	char* result = cf_realloc(title_buf, size + 1);
	vsnprintf(result, size + 1, fmt, args_copy);
	va_end(args_copy);

	return result;
}

static void
show_error_popup(error_popup_t* popup, const char* message) {
	popup->message = message;
	ImGui_OpenPopupID(popup->id, ImGuiPopupFlags_None);
}

static save_result_t
pick_save_target(error_popup_t* error_popup, document_t* doc) {
	nfdu8char_t* path = NULL;
	nfdu8filteritem_t filters[] = {
		{
			.name = "JSON",
			.spec = "json",
		}
	};
	nfdresult_t save_result = NFD_SaveDialogU8(
		&path,
		filters, sizeof(filters) / sizeof(filters[0]),
		NULL,
		NULL
	);

	if (save_result == NFD_OKAY) {
		cf_free(doc->filename);
		if (str_ends_with(path, ".json")) {
			doc->filename = strclone(path);
		} else {
			doc->filename = strprintf("%s.json", path);
		}

		NFD_FreePathU8(path);
		return SAVE_OK;
	} else if (save_result == NFD_ERROR) {
		show_error_popup(error_popup, NFD_GetError());
		return SAVE_ERROR;
	} else {
		return SAVE_CANCELLED;
	}
}

static save_result_t
do_save_doc(error_popup_t* error_popup, document_t* doc, const shape_t* shape) {
	CF_JDoc jdoc = cf_make_json(NULL, 0);
	CF_JVal root = cf_json_object(jdoc);
	cf_json_set_root(jdoc, root);

	cf_json_object_add_string(jdoc, root, "type", "polygon");

	CF_JVal verts = cf_json_array(jdoc);
	cf_json_object_add(jdoc, root, "vertices", verts);

	for (int i = 0; i < shape->num_vertices; ++i) {
		float data[] = { shape->verts[i].x, shape->verts[i].y };
		CF_JVal vert = cf_json_array_from_float(
			jdoc, data, sizeof(data) / sizeof(data[0])
		);
		cf_json_array_add(verts, vert);
	}

	save_result_t save_result = SAVE_OK;
	dyna char* content = cf_json_to_string(jdoc);
	if (!save_memory_into_file(doc->filename, content, slen(content))) {
		show_error_popup(error_popup, "Could not save file");
		save_result = SAVE_ERROR;
	}
	sfree(content);

	cf_destroy_json(jdoc);
	return save_result;
}

static save_result_t
save_doc_as(error_popup_t* error_popup, document_t* doc, const shape_t* shape) {
	save_result_t save_result;
	if ((save_result = pick_save_target(error_popup, doc)) == SAVE_OK) {
		return do_save_doc(error_popup, doc, shape);
	} else {
		return save_result;
	}
}

static bool
save_doc(error_popup_t* error_popup, document_t* doc, const shape_t* shape) {
	if (doc->filename == NULL) {
		return save_doc_as(error_popup, doc, shape);
	} else {
		return do_save_doc(error_popup, doc, shape);
	}
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
	CF_Sprite demo_sprite = cf_make_demo_sprite();
	CF_Sprite sprite = demo_sprite;

	int animation_index = 3;
	(void)animation_index;
	cf_sprite_play(&sprite, "hold_down");
	float draw_scale = 1.f;
	CF_V2 draw_offset = { 0.f, 0.f };

	CF_Coroutine modal_coro = { 0 };

	shape_history_t history = { 0 };
	shape_t* shape = &history.entries[history.current_index].shape;

	document_t doc = { 0 };
	uint64_t last_shape_version = 0;
	uint64_t last_doc_version = 0;
	set_title(&doc, 0);

	command_t command = COMMAND_NOOP;
	error_popup_t error_popup = { 0 };

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

		error_popup.id = ImGui_GetID("Error");
		ImGuiID help_popup = ImGui_GetID("Help");
		if (ImGui_BeginMainMenuBar()) {
			if (ImGui_BeginMenu("File")) {
				if (ImGui_MenuItemEx("New...", "Ctrl+N", false, true)) {
					command = COMMAND_NEW;
				}

				if (ImGui_MenuItemEx("Open...", "Ctrl+O", false, true)) {
					command = COMMAND_OPEN;
				}

				if (ImGui_MenuItemEx("Save", "Ctrl+S", false, true)) {
					command = COMMAND_SAVE;
				}

				if (ImGui_MenuItemEx("Save As...", "Ctrl+Shift+S", false, true)) {
					command = COMMAND_SAVE_AS;
				}

				ImGui_EndMenu();
			}

			if (ImGui_BeginMenu("Sprite")) {
				if (ImGui_MenuItem("Load")) {
					nfdu8char_t* path = NULL;
					nfdu8filteritem_t filters[] = {
						{
							.name = "All supported sprites",
							.spec = "ase,aseprite,png",
						},
						{
							.name = "aseprite",
							.spec = "ase,aseprite",
						},
						{
							.name = "png",
							.spec = "png",
						}
					};
					nfdresult_t open_result = NFD_OpenDialogU8(
						&path,
						filters, sizeof(filters) / sizeof(filters[0]),
						NULL
					);
					if (open_result == NFD_OKAY) {
						size_t size = 0;
						void* content = load_file_into_memory(path, &size);
						if (content != NULL) {
							CF_Sprite new_sprite = load_sprite(path, content, size);
							if (new_sprite.name) {
								if (strcmp(sprite.name, "easy_sprite") == 0) {
									cf_easy_sprite_unload(&sprite);
								} else if (sprite.name != demo_sprite.name) {
									cf_sprite_unload(sprite.name);
								}
								sprite = new_sprite;
							} else {
								show_error_popup(&error_popup, "Could not load sprite");
							}

							cf_free(content);
						} else {
							show_error_popup(&error_popup, "Could not read file");
						}

						NFD_FreePathU8(path);
					} else if (open_result == NFD_ERROR) {
						show_error_popup(&error_popup, NFD_GetError());
					}
				}

				int num_anims = hsize(sprite.animations);
				if (ImGui_BeginMenuEx("Animation", num_anims > 0)) {
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
					ImGui_OpenPopupID(help_popup, ImGuiPopupFlags_AnyPopup);
				}
				ImGui_EndMenu();
			}
			ImGui_EndMainMenuBar();
		}

		if (ImGui_BeginPopup("Help", ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui_Text(
				"Left click: Add vertex\n"
				"Right click: Remove vertex\n"
				"Middle mouse drag: Pan\n"
				"Scroll: Zoom"
			);
			ImGui_EndPopup();
		}

		if (ImGui_BeginPopupModal("Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui_Text("%s", error_popup.message);

			if (ImGui_Button("Ok")) {
				ImGui_CloseCurrentPopup();
			}
			ImGui_EndPopup();
		}

		if (modal_coro.id != 0) {
			cf_coroutine_resume(modal_coro);
			if (cf_coroutine_state(modal_coro) == CF_COROUTINE_STATE_DEAD) {
				cf_destroy_coroutine(modal_coro);
				modal_coro.id = 0;
			}
		}

		if (modal_coro.id == 0 && !ImGui_GetIO()->WantCaptureMouse) {
			if (cf_mouse_just_pressed(CF_MOUSE_BUTTON_MIDDLE)) {
				start_mouse_drag(&modal_coro, &(mouse_drag_info_t){
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
					start_mouse_drag(&modal_coro, &(mouse_drag_info_t){
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

		if (modal_coro.id == 0 && !ImGui_GetIO()->WantCaptureKeyboard) {
			if (cf_key_down(CF_KEY_LCTRL) || cf_key_down(CF_KEY_LCTRL)) {
				if (cf_key_just_pressed(CF_KEY_N)) {
					command = COMMAND_NEW;
				} else if (cf_key_just_pressed(CF_KEY_O)) {
					command = COMMAND_OPEN;
				} else if (cf_key_just_pressed(CF_KEY_S)) {
					if (cf_key_down(CF_KEY_LSHIFT) || cf_key_down(CF_KEY_RSHIFT)) {
						command = COMMAND_SAVE_AS;
					} else {
						command = COMMAND_SAVE;
					}
				}
			}
		}

		const uint64_t shape_version = history.entries[history.current_index].version;

		switch (command) {
			case COMMAND_NEW: {
				/*new_doc(&modal_coro, &error_popup, &doc, &history);*/
			} break;
			case COMMAND_OPEN: {
				/*open_doc(&modal_coro, &error_popup, &doc, &history);*/
			} break;
			case COMMAND_SAVE: {
				if (save_doc(&error_popup, &doc, shape) == SAVE_OK) {
					doc.saved_version = shape_version;
				}
			} break;
			case COMMAND_SAVE_AS: {
				if (save_doc_as(&error_popup, &doc, shape) == SAVE_OK) {
					doc.saved_version = shape_version;
				}
			} break;
			case COMMAND_NOOP: break;
		}
		command = COMMAND_NOOP;

		if (
			last_shape_version != shape_version
			||
			last_doc_version != doc.saved_version
		) {
			set_title(&doc, shape_version);

			last_shape_version = shape_version;
			last_doc_version = doc.saved_version;
		}

		cf_app_draw_onto_screen(true);
	}

	if (modal_coro.id != 0) {
		cf_destroy_coroutine(modal_coro);
	}

	cf_destroy_app();
	NFD_Quit();
	cf_free(title_buf);
	cf_free(doc.filename);

	return 0;
}
