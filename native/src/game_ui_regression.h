/* Opt-in regression invoked by ZSHARP_GAME_MODEL_UI_TEST. Included by the real
   model implementation so the same frame, style, and scene-setter paths run. */

#define CHECK(condition, code) do { if (!(condition)) { \
    if (error && error_size) snprintf(error, error_size, \
        "XR UI model check %d failed at line %d", (code), __LINE__); \
    free(rendered); free(model.active_scene); return 0; } } while (0)

static ZSharpGameObject ui_regression_button(char *scene, float x) {
    ZSharpGameObject object;
    memset(&object, 0, sizeof(object));
    object.shape = ZGAME_SHAPE_BUTTON;
    object.scene = scene;
    object.x = x;
    object.width = 0.6f;
    object.height = 0.2f;
    object.scale_x = object.scale_y = object.scale_z = 1;
    object.opacity = 1;
    object.visible = 1;
    object.color = 0x111111;
    object.hover_color_explicit = object.focus_color_explicit = 1;
    object.hover_color = 0x00ff00;
    object.focus_color = 0xff0000;
    object.hover_style_mask = 2;
    object.hover_scale_x = 1.5f;
    return object;
}

int zsharp_game_model_ui_regression(char *error, size_t error_size) {
    ZSharpGameModel model = {0};
    ZSharpGameScene scenes[2] = {{0}};
    ZSharpGameObject objects[4];
    ZSharpGameRenderFrame frame;
    ZSharpGameRenderObject *rendered = NULL;
    scenes[0].name = "Floor1";
    scenes[1].name = "Floor2";
    model.scenes = scenes;
    model.scene_count = 2;
    model.active_scene = zsharp_copy_text("Floor1", 6);
    CHECK(model.active_scene, 16);
    objects[0] = ui_regression_button("Floor2", 0);
    objects[1] = ui_regression_button("Floor1", -1);
    objects[2] = ui_regression_button("Floor1", 1);
    objects[2].hover_color = 0x0000ff;
    objects[3] = ui_regression_button("Floor2", 1);
    model.objects = objects;
    model.object_count = 4;
    model.xr_ui_active = 1;
    model.input.mouse_captured = 1;
    model.focused_button = 2;
    model.xr_hovered_buttons[0] = 2;
    model.xr_hovered_buttons[1] = 3;

    zsharp_game_model_frame(&model, &frame, &rendered);
    CHECK(rendered && frame.object_count == 2, 1);
    CHECK(rendered[0].render_id == 1 && rendered[0].model_index == 2, 2);
    CHECK(rendered[1].render_id == 2 && rendered[1].model_index == 3, 3);
    CHECK(rendered[0].color == 0x00ff00 && rendered[1].color == 0x0000ff, 4);
    CHECK(rendered[0].scale_x == 1.5f && rendered[0].ui_base_scale_x == 1, 5);
    CHECK(objects[1].scale_x == 1, 6); /* Hover cannot mutate collider/base bounds. */
    free(rendered); rendered = NULL;

    model.xr_hovered_buttons[0] = 0;
    zsharp_game_model_frame(&model, &frame, &rendered);
    CHECK(rendered[0].color == 0xff0000 && rendered[1].color == 0x0000ff, 7);
    free(rendered); rendered = NULL;
    objects[2].visible = 0;
    CHECK(!button_is_hovered(&model, 2, NULL), 8);
    objects[2].visible = 1;
    objects[2].opacity = 0;
    CHECK(!button_is_hovered(&model, 2, NULL), 9);
    objects[2].opacity = 1;
    model.xr_hovered_buttons[0] = 1; /* Stale inactive-scene index. */
    CHECK(!button_is_hovered(&model, 0, NULL), 10);

    CHECK(zsharp_game_model_set_property(&model, "Game.scene", ZWINDOW_VALUE_TEXT,
        "Floor2", error, error_size), 17);
    CHECK(model.scene_generation == 1 && !model.focused_button &&
          !model.xr_hovered_buttons[0] && !model.xr_hovered_buttons[1], 11);
    zsharp_game_model_frame(&model, &frame, &rendered);
    CHECK(rendered[0].model_index == 1 && rendered[1].model_index == 4, 12);
    CHECK(rendered[0].color == 0x111111 && rendered[1].color == 0x111111, 13);
    free(rendered); rendered = NULL;

    /* Desktop hover/focus semantics remain unchanged outside XR. */
    CHECK(zsharp_game_model_set_property(&model, "Game.scene", ZWINDOW_VALUE_TEXT,
        "Floor1", error, error_size), 18);
    model.xr_ui_active = 0;
    model.input.mouse_captured = 0;
    model.input.mouse_x = -1;
    model.focused_button = 3;
    zsharp_game_model_frame(&model, &frame, &rendered);
    CHECK(rendered[0].color == 0x00ff00 && rendered[1].color == 0xff0000, 14);
    free(rendered); rendered = NULL;
    model.input.mouse_captured = 1;
    zsharp_game_model_frame(&model, &frame, &rendered);
    CHECK(rendered[0].color == 0x111111 && rendered[1].color == 0x111111, 15);
    free(rendered); rendered = NULL;

    /* State transitions use both XR pointers too, even with captured desktop
       mouse input. Check real update/tween state rather than snapshot alone. */
    model.xr_ui_active = 1;
    model.focused_button = 0;
    model.xr_hovered_buttons[0] = 2;
    model.xr_hovered_buttons[1] = 3;
    objects[1].body = objects[2].body = ZGAME_BODY_STATIC;
    objects[1].style_transition_duration = objects[2].style_transition_duration = 0.1;
    zsharp_game_model_update(&model, 0.01);
    CHECK(objects[1].style_tween.initialized && objects[2].style_tween.initialized, 19);
    CHECK(objects[1].style_output[1] == 255 && objects[2].style_output[2] == 255, 20);
    CHECK(objects[1].style_output[4] == 1.5 && objects[1].scale_x == 1, 21);
    free(model.active_scene);
    puts("PASS: XR UI model snapshot, scene invalidation, dual hover, transitions and desktop regression");
    return 1;
}

#undef CHECK
