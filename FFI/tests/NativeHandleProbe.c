/// Manual end-to-end probe: brings up a real device through the FFI with native access enabled and
/// prints what the RHI, native-handle, window and time queries actually return.
///
/// Not a CI test — it creates a window and a graphics device. Run by hand on a machine with a GPU:
///   FfiNativeHandleProbe [vulkan|d3d12] [physical_device_id] [nonative]
///
/// `nonative` turns off `enable_native_access`, which is how the raw-handle extension was ruled out
/// as the cause of a D3D12 failure: the same abort reproduces with it disabled.
///
/// Known: the D3D12 backend aborts inside `sturdy_render_load_shader`, just after a
/// `ReflectionBinding` warning. Vulkan completes the whole scene. That is an engine-side D3D12
/// shader/pipeline problem, not an ABI one — every FFI call before it succeeds on both backends.
///
/// Startup queries run from `on_engine_initialized`; per-frame queries run from the first
/// `request_render_frame`, which then asks the window to close so the probe exits on its own
/// instead of sitting there as an interactive window.
#include <stdio.h>
#include <string.h>

#include <FFI/Sturdy.h>

static void report_startup(SturdyEngine engine) {
    SturdyBackend backend = STURDY_BACKEND_DEFAULT;
    SturdyAdapterInfo adapter;
    SturdyDeviceLimits limits;
    SturdyBool native = STURDY_FALSE;
    SturdyVulkanHandles vk;
    SturdyD3D12Handles dx;
    char text[256];
    size_t length = 0;
    uint32_t count = 0;
    uint32_t i = 0;
    void *queue = NULL;
    uint32_t family = 0;
    SturdyResult result;

    /* Each call is sequenced into `result` before the value is read: C leaves argument evaluation
       order unspecified, so printing the call and its out-param in one printf can show the
       pre-call value. */
    result = sturdy_rhi_backend(engine, &backend);
    printf("rhi_backend            -> %d (%d)\n", (int)result, (int)backend);
    printf("adapter_info           -> %d\n", (int)sturdy_rhi_adapter_info(engine, &adapter));
    printf("  vendor=0x%04X device=0x%04X type=%d discrete=%d\n",
           adapter.vendor_id, adapter.device_id, (int)adapter.device_type, (int)adapter.is_discrete);

    if (sturdy_rhi_adapter_string(engine, STURDY_ADAPTER_STRING_NAME, text, sizeof(text), &length) == STURDY_OK) {
        printf("  name='%s' (len=%zu)\n", text, length);
    }
    if (sturdy_rhi_adapter_string(engine, STURDY_ADAPTER_STRING_DRIVER_VERSION, text, sizeof(text), &length) == STURDY_OK) {
        printf("  driver='%s'\n", text);
    }
    if (sturdy_rhi_adapter_string(engine, STURDY_ADAPTER_STRING_PHYSICAL_DEVICE_ID, text, sizeof(text), &length) == STURDY_OK) {
        printf("  device_id_str='%s'\n", text);
    }

    printf("device_limits          -> %d\n", (int)sturdy_rhi_device_limits(engine, &limits));
    printf("  max_tex2d=%u max_color_attachments=%u timestamp_ns=%f\n",
           limits.max_texture_dimension_2d, limits.max_color_attachments, (double)limits.timestamp_period_ns);

    if (sturdy_rhi_queue_count(engine, &count) == STURDY_OK) {
        printf("queues=%u\n", count);
        for (i = 0; i < count; ++i) {
            SturdyQueueInfo q;
            if (sturdy_rhi_queue_info(engine, i, &q) == STURDY_OK) {
                printf("  [%u] class=%d caps=0x%02X lanes=%u dedicated=%d\n",
                       i, (int)q.queue_class, q.capabilities, q.lane_count, (int)q.dedicated);
            }
        }
    }

    if (sturdy_rhi_extension_count(engine, &count) == STURDY_OK) {
        printf("extensions=%u\n", count);
        for (i = 0; i < count; ++i) {
            uint32_t version = 0;
            if (sturdy_rhi_extension_name(engine, i, text, sizeof(text), &length, &version) == STURDY_OK) {
                printf("  [%u] %s v%u\n", i, text, version);
            }
        }
    }

    result = sturdy_native_available(engine, &native);
    printf("native_available       -> %d (%d)\n", (int)result, (int)native);

    /* Zeroed first so a failure path cannot be mistaken for real handles: the ABI leaves output
       structs untouched when it fails, so whatever was on the stack would otherwise be printed. */
    memset(&dx, 0, sizeof(dx));
    result = sturdy_native_d3d12(engine, &dx);
    printf("native_d3d12           -> %d\n", (int)result);
    printf("  factory=%p adapter=%p device=%p gfx_queue=%p\n",
           dx.factory, dx.adapter, dx.device, dx.graphics_queue);

    memset(&vk, 0, sizeof(vk));
    result = sturdy_native_vulkan(engine, &vk);
    printf("native_vulkan          -> %d\n", (int)result);
    printf("  instance=%p device=%p\n", vk.instance, vk.device);

    queue = NULL;
    result = sturdy_native_d3d12_queue(engine, STURDY_QUEUE_CLASS_COMPUTE, 0, &queue);
    printf("d3d12_queue(compute)   -> %d\n", (int)result);
    printf("  compute_queue=%p\n", queue);

    queue = NULL;
    family = 0xFFFFFFFFu;
    result = sturdy_native_vulkan_queue(engine, STURDY_QUEUE_CLASS_GRAPHICS, 0, &queue, &family);
    printf("vulkan_queue(graphics) -> %d (queue=%p family=%u)\n", (int)result, queue, family);
}

static void report_frame(SturdyEngine engine, SturdySurface surface, const SturdyFrameInput *input) {
    SturdyWindowSnapshot snapshot;
    double delta = -1.0;
    double unscaled = -1.0;
    double scale = -1.0;
    uint64_t tick = 0;
    uint32_t count = 0;
    SturdyResult result;

    printf("--- first frame ---\n");
    printf("frame_input: index=%llu %ux%u delta=%f live_resize=%d\n",
           (unsigned long long)input->frame_index, input->framebuffer_width,
           input->framebuffer_height, input->delta_seconds, (int)input->live_resize);

    result = sturdy_window_count(engine, &count);
    printf("window_count           -> %d (%u)\n", (int)result, count);

    memset(&snapshot, 0, sizeof(snapshot));
    result = sturdy_window_find(engine, surface, &snapshot);
    printf("window_find            -> %d\n", (int)result);
    printf("  surface_id=%llu size=%ux%u fb=%ux%u pos=%d,%d opacity=%f focused=%d locked=%d\n",
           (unsigned long long)snapshot.surface_id, snapshot.width, snapshot.height,
           snapshot.framebuffer_width, snapshot.framebuffer_height,
           snapshot.position_x, snapshot.position_y, (double)snapshot.opacity,
           (int)snapshot.focused, (int)snapshot.mouse_locked);

    memset(&snapshot, 0, sizeof(snapshot));
    result = sturdy_window_primary(engine, &snapshot);
    printf("window_primary         -> %d (surface_id=%llu)\n",
           (int)result, (unsigned long long)snapshot.surface_id);

    result = sturdy_time_delta_seconds(engine, &delta);
    printf("time_delta             -> %d (%f)\n", (int)result, delta);
    result = sturdy_time_unscaled_delta_seconds(engine, &unscaled);
    printf("time_unscaled_delta    -> %d (%f)\n", (int)result, unscaled);
    result = sturdy_time_tick_index(engine, &tick);
    printf("time_tick_index        -> %d (%llu)\n", (int)result, (unsigned long long)tick);
    result = sturdy_time_scale(engine, &scale);
    printf("time_scale             -> %d (%f)\n", (int)result, scale);

    /* Round-trip the scale, then restore it. */
    result = sturdy_time_set_scale(engine, 0.25);
    printf("time_set_scale(0.25)   -> %d\n", (int)result);
    scale = -1.0;
    result = sturdy_time_scale(engine, &scale);
    printf("time_scale (after)     -> %d (%f)\n", (int)result, scale);
    result = sturdy_time_set_scale(engine, -1.0);
    printf("time_set_scale(-1)     -> %d (expect 1 = invalid argument)\n", (int)result);
    (void)sturdy_time_set_scale(engine, 1.0);

    result = sturdy_window_set_cursor_icon(engine, surface, STURDY_CURSOR_ICON_POINTER);
    printf("set_cursor_icon        -> %d\n", (int)result);
    result = sturdy_window_set_cursor_icon(engine, surface, (SturdyCursorIcon)9999);
    printf("set_cursor_icon(bogus) -> %d (expect 1 = invalid argument)\n", (int)result);
}

/* Scene built from C, to prove the rendering surface actually puts geometry on screen. */
static SturdyAsset g_shader;
static SturdyAsset g_floor_model;
static SturdyAsset g_cube_model;
static SturdyEntity g_floor;
static SturdyEntity g_cube;
static SturdyEntity g_sun;
static int g_scene_ok = 0;
/* Keeps the window up so it can be screenshotted, instead of closing after a few frames. */
static int g_hold = 0;

static void identity(float *m) {
    int i;
    for (i = 0; i < 16; ++i) {
        m[i] = 0.0f;
    }
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void build_scene(SturdyEngine engine) {
    SturdyShapeParams params;
    SturdyResult result;
    float matrix[16];
    float radiance[3];
    uint32_t primitives = 0;
    uint32_t vertices = 0;
    uint32_t triangles = 0;
    SturdyComponentId transform_component = 0;

    printf("--- scene ---\n");

    result = sturdy_ui_register_font(engine, "Fonts/MapleMono-NF-Regular.ttf", 1);
    printf("ui_register_font       -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
    }

    result = sturdy_render_load_shader(engine, "Shaders/gbuffer_geometry.slang", "depthOnlyMain",
                                       &g_shader);
    printf("load_shader            -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
        return;
    }

    /* A floor plane and a cube above it. */
    result = sturdy_render_shape_params_init(&params);
    printf("shape_params_init      -> %d\n", (int)result);
    params.width = 12.0f;
    params.depth = 12.0f;
    result = sturdy_render_create_shape_model(engine, STURDY_SHAPE_PLANE, &params, g_shader,
                                              "ffi probe floor", &g_floor_model);
    printf("create_shape_model     -> %d (plane)\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
        return;
    }

    (void)sturdy_render_shape_params_init(&params);
    params.size = 1.5f;
    result = sturdy_render_create_shape_model(engine, STURDY_SHAPE_CUBE, &params, g_shader,
                                              "ffi probe cube", &g_cube_model);
    printf("create_shape_model     -> %d (cube)\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
        return;
    }

    result = sturdy_render_model_info(engine, g_cube_model, &primitives, &vertices, &triangles);
    printf("model_info             -> %d (prims=%u verts=%u tris=%u)\n",
           (int)result, primitives, vertices, triangles);

    result = sturdy_render_set_model_vec4(engine, g_cube_model, 0, "base_color_factor",
                                          0.9f, 0.35f, 0.2f, 1.0f);
    printf("set_model_vec4         -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
    }
    result = sturdy_render_set_model_float(engine, g_cube_model, 0, "roughness_factor", 0.4f);
    printf("set_model_float        -> %d\n", (int)result);
    result = sturdy_render_set_model_float(engine, g_cube_model, 0, "not_a_real_parameter", 1.0f);
    printf("set_model_float(bogus) -> %d (expect nonzero)\n", (int)result);
    result = sturdy_render_set_model_vec4(engine, g_floor_model, 0, "base_color_factor",
                                          0.55f, 0.55f, 0.58f, 1.0f);
    printf("floor base color       -> %d\n", (int)result);
    (void)sturdy_render_set_model_float(engine, g_floor_model, 0, "roughness_factor", 0.9f);

    /* sturdy_render_spawn creates the entity and registers the transform component, which is
       otherwise unreachable by name until something has used it. */
    result = sturdy_render_spawn(engine, &g_floor);
    printf("render_spawn floor     -> %d\n", (int)result);
    result = sturdy_render_spawn(engine, &g_cube);
    printf("render_spawn cube      -> %d\n", (int)result);
    result = sturdy_render_spawn(engine, &g_sun);
    printf("render_spawn sun       -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
        return;
    }

    /* Now that a transform exists, the engine component is reachable by name too. */
    result = sturdy_ecs_find_component(engine, "sturdy.engine.world_transform",
                                       &transform_component);
    printf("find world_transform   -> %d (after spawn)\n", (int)result);

    result = sturdy_render_set_model(engine, g_floor, g_floor_model, STURDY_TRUE);
    printf("set_model floor        -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
    }

    identity(matrix);
    matrix[13] = 1.2f; /* lift the cube above the floor */
    result = sturdy_render_set_transform(engine, g_cube, matrix);
    printf("set_transform cube     -> %d\n", (int)result);
    result = sturdy_render_set_model(engine, g_cube, g_cube_model, STURDY_TRUE);
    printf("set_model cube         -> %d\n", (int)result);

    /* Read the transform back to confirm it round-trips through the ECS. */
    {
        float read_back[16];
        int i;
        int matches = 1;
        result = sturdy_render_get_transform(engine, g_cube, read_back);
        for (i = 0; i < 16; ++i) {
            if (read_back[i] != matrix[i]) {
                matches = 0;
            }
        }
        printf("get_transform cube     -> %d (round-trips: %s)\n", (int)result, matches ? "yes" : "NO");
    }

    /* A sun pointing down and to the side. */
    result = sturdy_render_set_light_direction(engine, g_sun, -0.4f, -0.8f, -0.45f);
    printf("set_light_direction    -> %d\n", (int)result);
    result = sturdy_render_set_light_direction(engine, g_sun, 0.0f, 0.0f, 0.0f);
    printf("light_direction(zero)  -> %d (expect 1)\n", (int)result);
    result = sturdy_render_set_light_direction(engine, g_sun, -0.4f, -0.8f, -0.45f);

    radiance[0] = 3.0f;
    radiance[1] = 2.8f;
    radiance[2] = 2.5f;
    result = sturdy_render_set_directional_light(engine, g_sun, radiance, 0.27f, STURDY_TRUE);
    printf("set_directional_light  -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
        return;
    }

    /* Argument validation must reject nonsense rather than reaching the renderer. */
    result = sturdy_render_set_directional_light(engine, g_sun, radiance, 0.0f, STURDY_TRUE);
    printf("light(bad angle)       -> %d (expect 1)\n", (int)result);
    result = sturdy_render_set_spot_light(engine, g_sun, radiance, 10.0f, 40.0f, 20.0f, 0.05f, STURDY_TRUE);
    printf("spot(inner>outer)      -> %d (expect 1)\n", (int)result);

    g_scene_ok = 1;
}

/* glTF import, exercised against real files that ship with the engine's dependencies. */
static void import_gltf_scene(SturdyEngine engine, const char *path, const char *what) {
    SturdyGltfScene scene;
    SturdyResult result;
    uint32_t models = 0;
    uint32_t instances = 0;
    uint32_t lights = 0;
    uint32_t spawned = 0;
    uint32_t i;
    char name[128];
    size_t length = 0;

    scene.token = 0;
    result = sturdy_gltf_import(engine, path, g_shader, &scene);
    printf("gltf_import(%s) -> %d\n", what, (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
        return;
    }

    (void)sturdy_gltf_model_count(scene, &models);
    (void)sturdy_gltf_instance_count(scene, &instances);
    (void)sturdy_gltf_light_count(scene, &lights);
    printf("  models=%u instances=%u lights=%u\n", models, instances, lights);

    for (i = 0; i < instances && i < 3; ++i) {
        SturdyGltfInstance instance;
        memset(&instance, 0, sizeof(instance));
        result = sturdy_gltf_instance_at(scene, i, &instance);
        name[0] = 0;
        (void)sturdy_gltf_instance_name(scene, i, name, sizeof(name), &length);
        printf("  instance[%u] -> %d name='%s' pos=(%.2f,%.2f,%.2f)\n",
               i, (int)result, name,
               (double)instance.world_transform[12], (double)instance.world_transform[13],
               (double)instance.world_transform[14]);
    }
    for (i = 0; i < lights && i < 3; ++i) {
        SturdyGltfLight light;
        memset(&light, 0, sizeof(light));
        result = sturdy_gltf_light_at(scene, i, &light);
        printf("  light[%u] -> %d kind=%d radiance=(%.2f,%.2f,%.2f) range=%.2f\n",
               i, (int)result, (int)light.kind, (double)light.radiance[0],
               (double)light.radiance[1], (double)light.radiance[2], (double)light.range);
    }

    /* Out-of-range access must be reported, not read past the end. */
    {
        SturdyGltfInstance past;
        result = sturdy_gltf_instance_at(scene, instances + 10, &past);
        printf("  instance_at(past end) -> %d (expect 12)\n", (int)result);
    }

    result = sturdy_gltf_spawn_all(engine, scene, &spawned);
    printf("  spawn_all -> %d (%u entities)\n", (int)result, spawned);

    result = sturdy_gltf_release(scene);
    printf("  release -> %d\n", (int)result);
    result = sturdy_gltf_release(scene);
    printf("  release again -> %d (expect 3 = handle expired)\n", (int)result);
    result = sturdy_gltf_model_count(scene, &models);
    printf("  use after release -> %d (expect 3)\n", (int)result);
}

static SturdyBool on_init(SturdyEngine engine, void *user_data) {
    (void)user_data;
    report_startup(engine);
    build_scene(engine);
    if (g_scene_ok) {
        import_gltf_scene(engine, ".cache/deps/cgltf-src/fuzz/data/Box.glb", "Box.glb");
        import_gltf_scene(engine,
                          ".cache/deps/gdeflate-src/Samples/BulkLoadDemo/BulkLoadDemo/SampleModel/Avocado.gltf",
                          "Avocado.gltf");
        import_gltf_scene(engine, "does/not/exist.gltf", "missing file");
    }
    fflush(stdout);
    return STURDY_TRUE;
}

static int g_ui_reported = 0;

/* Builds a small panel each frame, proving the C caller can drive the immediate-mode UI. */
static void draw_ui(SturdyEngine engine, const SturdyFrameInput *input, SturdyFrame frame) {
    SturdyUiElement root;
    SturdyUiElement panel;
    SturdyUiTextStyle style;
    SturdyResult result;
    SturdyBool hovered = STURDY_FALSE;
    float px = -1.0f;
    float py = -1.0f;

    result = sturdy_ui_begin(engine, input);
    if (result != STURDY_OK) {
        if (!g_ui_reported) {
            printf("ui_begin -> %d\n", (int)result);
            printf("  %s\n", sturdy_last_error_message());
            g_ui_reported = 1;
        }
        return;
    }

    (void)sturdy_ui_element_init(&root);
    root.width_kind = STURDY_UI_SIZING_FIXED;
    root.width_value = (float)input->framebuffer_width;
    root.height_kind = STURDY_UI_SIZING_FIXED;
    root.height_value = (float)input->framebuffer_height;
    root.padding_left = 16.0f;
    root.padding_top = 16.0f;
    root.id = "ffi-ui-root";

    (void)sturdy_ui_element_init(&panel);
    panel.width_kind = STURDY_UI_SIZING_FIXED;
    panel.width_value = 260.0f;
    panel.height_kind = STURDY_UI_SIZING_FIT;
    panel.direction = STURDY_UI_DIRECTION_TOP_TO_BOTTOM;
    panel.padding_left = 12.0f;
    panel.padding_right = 12.0f;
    panel.padding_top = 10.0f;
    panel.padding_bottom = 10.0f;
    panel.child_gap = 6.0f;
    panel.background[0] = 0.08f;
    panel.background[1] = 0.09f;
    panel.background[2] = 0.12f;
    panel.background[3] = 0.92f;
    panel.corner_radius[0] = 8.0f;
    panel.corner_radius[1] = 8.0f;
    panel.corner_radius[2] = 8.0f;
    panel.corner_radius[3] = 8.0f;
    panel.id = "ffi-ui-panel";

    (void)sturdy_ui_text_style_init(&style);
    style.font_id = 1;
    style.font_size = 18;

    result = sturdy_ui_begin_element(engine, &root);
    if (result == STURDY_OK) {
        result = sturdy_ui_begin_element(engine, &panel);
        if (result == STURDY_OK) {
            (void)sturdy_ui_text(engine, "Sturdy FFI", &style);
            style.font_size = 14;
            style.color[0] = 0.7f;
            style.color[1] = 0.75f;
            style.color[2] = 0.85f;
            (void)sturdy_ui_text(engine, "UI built entirely from C", &style);
            (void)sturdy_ui_end_element(engine);
        }
        (void)sturdy_ui_end_element(engine);
    }

    (void)sturdy_ui_hovered(engine, "ffi-ui-panel", &hovered);
    (void)sturdy_ui_pointer_position(engine, &px, &py);

    if (!g_ui_reported) {
        SturdyResult unbalanced;
        printf("--- ui ---\n");
        printf("ui_begin/elements/text -> %d\n", (int)result);
        printf("ui_hovered(panel) -> ok (%d) pointer=(%.0f,%.0f)\n", (int)hovered,
               (double)px, (double)py);
        unbalanced = sturdy_ui_end_element(engine);
        printf("ui_end_element(unbalanced) -> %d (expect 1)\n", (int)unbalanced);
        g_ui_reported = 1;
    }

    result = sturdy_ui_end(engine, frame);
    if (g_ui_reported == 1) {
        printf("ui_end -> %d\n", (int)result);
        if (result != STURDY_OK) {
            printf("  %s\n", sturdy_last_error_message());
        }
        g_ui_reported = 2;
    }
}

static SturdyBool on_frame(SturdyEngine engine,
                           SturdySurface surface,
                           const SturdyFrameInput *input,
                           SturdyFrame frame,
                           void *user_data) {
    static int reported = 0;
    static int frames = 0;
    static int rendered = 0;
    static int closing = 0;
    uint64_t request_id = 0;
    SturdyResult result;

    (void)frame;
    (void)user_data;

    if (reported == 0) {
        reported = 1;
        report_frame(engine, surface, input);
        fflush(stdout);
    }

    /* Draw the C-built scene for a few frames, then close. More than one frame is deliberate:
       the first uploads geometry and compiles pipelines, so a failure that only appears in
       steady state would be missed by a single-frame test. */
    ++frames;
    if (g_scene_ok && (g_hold || frames <= 3)) {
        SturdyResult camera;

        camera = sturdy_frame_set_camera_position(frame, 7.0f, 5.0f, 9.0f);
        if (camera == STURDY_OK) {
            camera = sturdy_frame_camera_look_at(frame, 0.0f, 1.0f, 0.0f);
        }
        if (camera == STURDY_OK) {
            camera = sturdy_frame_set_camera_perspective(frame, 55.0f, 0.05f, 500.0f);
        }
        if (camera == STURDY_OK && input->framebuffer_width > 0 && input->framebuffer_height > 0) {
            camera = sturdy_frame_set_camera_viewport(frame, input->framebuffer_width,
                                                      input->framebuffer_height);
        }
        if (camera == STURDY_OK) {
            camera = sturdy_frame_set_ambient_light(frame, 0.03f, 0.035f, 0.045f, 1.0f);
        }
        if (frames == 1) {
            printf("frame camera setup     -> %d\n", (int)camera);
            if (camera != STURDY_OK) {
                printf("  %s\n", sturdy_last_error_message());
            }
            fflush(stdout);
        }
        if (camera == STURDY_OK) {
            draw_ui(engine, input, frame);
            rendered = frames;
            return STURDY_TRUE;
        }
    }

    if (!closing && !g_hold) {
        closing = 1;
        printf("rendered %d frame(s) of the C-built scene\n", rendered);
        result = sturdy_window_request_close(engine, surface, &request_id);
        printf("window_request_close   -> %d (id=%llu)\n", (int)result, (unsigned long long)request_id);
        fflush(stdout);
    }
    return STURDY_FALSE;
}

static void on_shutdown(SturdyEngine engine, void *user_data) {
    (void)engine;
    (void)user_data;
    printf("on_shutdown called\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    SturdyRuntimeConfig config;
    SturdyGameLogic logic;
    int32_t exit_code = 0;
    SturdyResult result;

    /* Unbuffered: this probe can hang inside a GPU call, and buffered output would be lost when it
       is killed, hiding exactly the line that would say where. */
    (void)setvbuf(stdout, NULL, _IONBF, 0);

    if (sturdy_runtime_config_init(&config) != STURDY_OK) {
        return 1;
    }
    config.app_name = "Sturdy FFI native handle probe";
    config.window_title = "Sturdy FFI native handle probe";
    config.enable_native_access = STURDY_TRUE;
    {
        /* Scanned across all arguments so it can be passed without also supplying a device id. */
        int arg;
        for (arg = 1; arg < argc; ++arg) {
            if (strcmp(argv[arg], "nonative") == 0) {
                config.enable_native_access = STURDY_FALSE;
            }
            if (strcmp(argv[arg], "hold") == 0) {
                g_hold = 1;
            }
        }
    }
    if (argc > 1 && strcmp(argv[1], "vulkan") == 0) {
        config.graphics_backend = STURDY_BACKEND_VULKAN;
    } else if (argc > 1 && strcmp(argv[1], "d3d12") == 0) {
        config.graphics_backend = STURDY_BACKEND_D3D12;
    }
    if (argc > 2 && strcmp(argv[2], "nonative") != 0 && strcmp(argv[2], "hold") != 0) {
        config.physical_device_id = argv[2];
    }
    printf("requested backend: %d device_id='%s'\n",
           (int)config.graphics_backend,
           config.physical_device_id != NULL ? config.physical_device_id : "");

    logic.struct_size = (uint32_t)sizeof(SturdyGameLogic);
    logic.reserved = 0;
    logic.user_data = NULL;
    logic.on_engine_initialized = on_init;
    logic.request_render_frame = on_frame;
    logic.on_shutdown = on_shutdown;
    logic.destroy = NULL;

    result = sturdy_runtime_run(&config, &logic, 0, NULL, &exit_code);
    printf("runtime_run -> %d (exit=%d) msg='%s'\n", (int)result, (int)exit_code, sturdy_last_error_message());
    return 0;
}
