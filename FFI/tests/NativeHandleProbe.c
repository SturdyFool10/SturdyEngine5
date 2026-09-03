/// Manual end-to-end probe: brings up a real device through the FFI with native access enabled and
/// prints what the RHI, native-handle, window and time queries actually return.
///
/// Not a CI test — it creates a window and a graphics device. Run by hand on a machine with a GPU:
///   FfiNativeHandleProbe [vulkan|d3d12] [physical_device_id] [nonative]
///
/// `nonative` turns off `enable_native_access`, letting the same probe verify the RHI/window/time
/// surface without exercising the raw-handle extension.
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

    /* Mesh/task shader feature reporting. No shader compiler is available to this probe, so a
       real mesh pipeline/draw_mesh_tasks call cannot be exercised here — this only confirms the
       device correctly reports what it supports. */
    {
        uint32_t mesh_index = 0;
        uint32_t task_index = 0;
        SturdyBool mesh_enabled = STURDY_FALSE;
        SturdyBool task_enabled = STURDY_FALSE;
        SturdyResult mesh_index_result = sturdy_rhi_feature_index("mesh shaders", &mesh_index);
        SturdyResult task_index_result = sturdy_rhi_feature_index("task/amplification shaders", &task_index);
        SturdyResult mesh_enabled_result = STURDY_ERROR_NOT_AVAILABLE;
        SturdyResult task_enabled_result = STURDY_ERROR_NOT_AVAILABLE;
        if (mesh_index_result == STURDY_OK) {
            mesh_enabled_result = sturdy_rhi_feature_enabled(engine, mesh_index, &mesh_enabled);
        }
        if (task_index_result == STURDY_OK) {
            task_enabled_result = sturdy_rhi_feature_enabled(engine, task_index, &task_enabled);
        }
        printf("feature(mesh shaders)  -> index=%d(%d) enabled_query=%d(%d) enabled=%d\n",
               (int)mesh_index_result, mesh_index, (int)mesh_enabled_result, mesh_index, (int)mesh_enabled);
        printf("feature(task shaders)  -> index=%d(%d) enabled_query=%d(%d) enabled=%d\n",
               (int)task_index_result, task_index, (int)task_enabled_result, task_index, (int)task_enabled);
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

    {
        SturdyPresentationSettings settings;
        SturdyPresentationResolution resolution;
        SturdyHdrCapabilities hdr;
        SturdyHdrPresentationMode modes[8];
        uint32_t mode_count = 0;
        uint32_t i;

        printf("--- presentation/HDR ---\n");

        result = sturdy_presentation_settings_init(&settings);
        printf("presentation_settings_init -> %d (vsync=%d hdr=%d)\n", (int)result,
               (int)settings.vsync, (int)settings.hdr_enabled);

        memset(&settings, 0, sizeof(settings));
        result = sturdy_surface_presentation_settings(engine, surface, &settings);
        printf("surface_presentation_settings -> %d (vsync=%d variable_refresh=%d latency=%d "
               "hdr_enabled=%d hdr_color_space=%d transparent=%d image_count=%u)\n",
               (int)result, (int)settings.vsync, (int)settings.variable_refresh,
               (int)settings.latency, (int)settings.hdr_enabled, (int)settings.hdr_color_space,
               (int)settings.transparent_composition, settings.swapchain_image_count);

        result = sturdy_surface_set_presentation_settings(engine, surface, &settings);
        printf("surface_set_presentation_settings (round-trip, unchanged) -> %d\n", (int)result);

        memset(&resolution, 0, sizeof(resolution));
        result = sturdy_surface_presentation_resolution(engine, surface, &resolution);
        printf("surface_presentation_resolution -> %d (strategy=%d mode=%d degraded=%d "
               "composite_alpha=%d format=%d color_space=%d fullscreen_exclusive=%d)\n",
               (int)result, (int)resolution.strategy, (int)resolution.effective_mode,
               (int)resolution.degraded, (int)resolution.effective_composite_alpha,
               (int)resolution.effective_format, (int)resolution.effective_color_space,
               (int)resolution.full_screen_exclusive_active);

        memset(&hdr, 0, sizeof(hdr));
        memset(modes, 0, sizeof(modes));
        result = sturdy_surface_query_hdr_capabilities(engine, surface, &hdr, modes, 8, &mode_count);
        printf("surface_query_hdr_capabilities -> %d (status=%d supported=%d enabled_by_os=%d "
               "sdr_white=%f edr_headroom=%f mode_count=%u)\n",
               (int)result, (int)hdr.status, (int)hdr.hdr_supported, (int)hdr.hdr_enabled_by_os,
               (double)hdr.sdr_white_nits, (double)hdr.edr_headroom, mode_count);
        for (i = 0; i < mode_count && i < 8; ++i) {
            printf("  mode[%u] transfer=%d gamut=%d requires_os_hdr=%d\n", i,
                   (int)modes[i].transfer, (int)modes[i].gamut, (int)modes[i].requires_os_hdr_mode);
        }

        result = sturdy_surface_update_hdr_content_light_level(engine, surface, 1000.0f, 400.0f);
        printf("surface_update_hdr_content_light_level -> %d (expect NOT_AVAILABLE if HDR isn't "
               "active/supported on this backend/driver)\n",
               (int)result);
    }
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

/* Exercises the RHI resource surface end to end: clears a texture to a known color entirely on
   the GPU, copies it into a host-readback buffer, and checks the bytes that come back — proving
   texture/buffer creation, command encoding, barriers, submission and mapping all actually work
   rather than merely compiling. Also round-trips a bind-group-layout/bind-group/pipeline-layout
   creation, since exercising an actual pipeline would need real shader bytecode this probe has no
   compiler to produce. */
static void probe_rhi_resources(SturdyEngine engine) {
    SturdyResult result;
    SturdyTextureDesc texture_desc;
    SturdyTexture texture;
    SturdyBufferDesc buffer_desc;
    SturdyBuffer readback;
    SturdyBuffer uniform_buffer;
    SturdyCommandEncoderDesc encoder_desc;
    SturdyCommandEncoder encoder;
    SturdyCommandBuffer command_buffer;
    SturdyTextureSubresourceRange full_range;
    SturdyTextureBarrier tex_barrier;
    SturdyBufferTextureCopy copy_region;
    SturdyGlobalBarrier host_barrier;
    SturdyBindGroupLayoutEntry layout_entry;
    SturdyBindGroupLayoutDesc layout_desc;
    SturdyBindGroupLayout bind_group_layout;
    SturdyBindGroupEntry group_entry;
    SturdyBindGroupDesc group_desc;
    SturdyBindGroup bind_group;
    SturdyPipelineLayoutDesc pipeline_layout_desc;
    SturdyPipelineLayout pipeline_layout;
    SturdyQuerySetDesc query_set_desc;
    SturdyQuerySet query_set;
    int have_query_set = 0;
    float clear_color[4];
    void *mapped = NULL;
    size_t mapped_size = 0;
    unsigned char *bytes;
    /* Row pitch (width * 4 bytes/texel) must be a multiple of D3D12's 256-byte
       D3D12_TEXTURE_DATA_PITCH_ALIGNMENT for a buffer<->texture copy; Vulkan has no such
       requirement. 64 wide keeps this probe portable across both backends. */
    const unsigned int width = 64;
    const unsigned int height = 1;

    printf("--- rhi resources ---\n");

    (void)sturdy_rhi_texture_desc_init(&texture_desc);
    texture_desc.format = STURDY_FORMAT_RGBA8_UNORM;
    texture_desc.width = width;
    texture_desc.height = height;
    /* ColorAttachment is required for sturdy_rhi_command_encoder_clear_color_texture below: D3D12
       clears through a render-target view, so it needs an RTV-capable texture even though Vulkan
       has no such requirement for vkCmdClearColorImage. */
    texture_desc.usage = STURDY_TEXTURE_USAGE_TRANSFER_SRC | STURDY_TEXTURE_USAGE_TRANSFER_DST |
                         STURDY_TEXTURE_USAGE_COLOR_ATTACHMENT;
    texture_desc.label = "ffi probe texture";
    texture.id = 0;
    result = sturdy_rhi_create_texture(engine, &texture_desc, &texture);
    printf("create_texture          -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
        return;
    }

    /* An RGBA16Float sampled texture: the format HDR source textures (an EXR environment map, a
       PQ AVIF) are uploaded in. Checked on real hardware because it is the one texture format in
       this engine that nothing else creates through the sampled-texture path -- RGBA16Float is
       otherwise only ever a render-graph target -- so a driver or format-table gap in it would not
       show up anywhere else in this probe. */
    {
        SturdyTextureDesc hdr_desc;
        SturdyTexture hdr_texture;
        SturdyResult hdr_result;

        (void)sturdy_rhi_texture_desc_init(&hdr_desc);
        hdr_desc.format = STURDY_FORMAT_RGBA16_FLOAT;
        hdr_desc.width = width;
        hdr_desc.height = height;
        hdr_desc.usage = STURDY_TEXTURE_USAGE_SAMPLED | STURDY_TEXTURE_USAGE_TRANSFER_DST;
        hdr_desc.label = "ffi probe hdr texture";
        hdr_texture.id = 0;
        hdr_result = sturdy_rhi_create_texture(engine, &hdr_desc, &hdr_texture);
        printf("create_texture(rgba16f) -> %d\n", (int)hdr_result);
        if (hdr_result != STURDY_OK) {
            printf("  %s\n", sturdy_last_error_message());
            g_scene_ok = 0;
        } else {
            sturdy_rhi_destroy_texture(engine, hdr_texture);
        }
    }

    (void)sturdy_rhi_buffer_desc_init(&buffer_desc);
    buffer_desc.size = (uint64_t)(width * height * 4);
    buffer_desc.usage = STURDY_BUFFER_USAGE_TRANSFER_DST;
    buffer_desc.memory = STURDY_MEMORY_LOCATION_HOST_READBACK;
    buffer_desc.label = "ffi probe readback";
    readback.id = 0;
    result = sturdy_rhi_create_buffer(engine, &buffer_desc, &readback);
    printf("create_buffer(readback) -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
        sturdy_rhi_destroy_texture(engine, texture);
        return;
    }

    (void)sturdy_rhi_buffer_desc_init(&buffer_desc);
    buffer_desc.size = 256;
    buffer_desc.usage = STURDY_BUFFER_USAGE_UNIFORM;
    buffer_desc.memory = STURDY_MEMORY_LOCATION_DEVICE_LOCAL;
    buffer_desc.label = "ffi probe uniform";
    uniform_buffer.id = 0;
    result = sturdy_rhi_create_buffer(engine, &buffer_desc, &uniform_buffer);
    printf("create_buffer(uniform)  -> %d\n", (int)result);

    (void)sturdy_rhi_command_encoder_desc_init(&encoder_desc);
    encoder_desc.label = "ffi probe encoder";
    encoder.token = 0;
    result = sturdy_rhi_create_command_encoder(engine, &encoder_desc, &encoder);
    printf("create_command_encoder  -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
        sturdy_rhi_destroy_buffer(engine, uniform_buffer);
        sturdy_rhi_destroy_buffer(engine, readback);
        sturdy_rhi_destroy_texture(engine, texture);
        return;
    }

    full_range.base_mip_level = 0;
    full_range.mip_level_count = STURDY_ALL_REMAINING;
    full_range.base_array_layer = 0;
    full_range.array_layer_count = STURDY_ALL_REMAINING;

    /* GPU timestamp query set, bracketing the clear below. Exercises the query-set surface (added
       to the FFI well before this probe ever ran it against real hardware). */
    memset(&query_set_desc, 0, sizeof(query_set_desc));
    query_set_desc.struct_size = (uint32_t)sizeof(query_set_desc);
    query_set_desc.type = STURDY_QUERY_TYPE_TIMESTAMP;
    query_set_desc.count = 2;
    query_set_desc.label = "ffi probe timestamps";
    query_set.id = 0;
    result = sturdy_rhi_create_query_set(engine, &query_set_desc, &query_set);
    printf("create_query_set        -> %d\n", (int)result);
    have_query_set = (result == STURDY_OK);
    if (have_query_set) {
        result = sturdy_rhi_command_encoder_reset_query_set(encoder, query_set, 0, 2);
        printf("reset_query_set         -> %d\n", (int)result);
        result = sturdy_rhi_command_encoder_write_timestamp(encoder, STURDY_PIPELINE_STAGE_DRAW_INDIRECT,
                                                            query_set, 0);
        printf("write_timestamp[0]      -> %d\n", (int)result);
    }

    memset(&tex_barrier, 0, sizeof(tex_barrier));
    tex_barrier.texture = texture;
    tex_barrier.dst_stage = STURDY_PIPELINE_STAGE_TRANSFER;
    tex_barrier.dst_access = STURDY_ACCESS_TRANSFER_WRITE;
    tex_barrier.old_layout = STURDY_TEXTURE_LAYOUT_UNDEFINED;
    tex_barrier.new_layout = STURDY_TEXTURE_LAYOUT_TRANSFER_DST;
    tex_barrier.range = full_range;
    result = sturdy_rhi_command_encoder_barrier(encoder, 0, NULL, 0, NULL, 1, &tex_barrier);
    printf("barrier(undef->dst)     -> %d\n", (int)result);

    clear_color[0] = 1.0f;
    clear_color[1] = 0.0f;
    clear_color[2] = 0.0f;
    clear_color[3] = 1.0f;
    result = sturdy_rhi_command_encoder_clear_color_texture(encoder, texture, clear_color, &full_range);
    printf("clear_color_texture     -> %d\n", (int)result);

    if (have_query_set) {
        result = sturdy_rhi_command_encoder_write_timestamp(encoder, STURDY_PIPELINE_STAGE_TRANSFER,
                                                            query_set, 1);
        printf("write_timestamp[1]      -> %d\n", (int)result);
    }

    memset(&tex_barrier, 0, sizeof(tex_barrier));
    tex_barrier.texture = texture;
    tex_barrier.src_stage = STURDY_PIPELINE_STAGE_TRANSFER;
    tex_barrier.src_access = STURDY_ACCESS_TRANSFER_WRITE;
    tex_barrier.dst_stage = STURDY_PIPELINE_STAGE_TRANSFER;
    tex_barrier.dst_access = STURDY_ACCESS_TRANSFER_READ;
    tex_barrier.old_layout = STURDY_TEXTURE_LAYOUT_TRANSFER_DST;
    tex_barrier.new_layout = STURDY_TEXTURE_LAYOUT_TRANSFER_SRC;
    tex_barrier.range = full_range;
    result = sturdy_rhi_command_encoder_barrier(encoder, 0, NULL, 0, NULL, 1, &tex_barrier);
    printf("barrier(dst->src)       -> %d\n", (int)result);

    memset(&copy_region, 0, sizeof(copy_region));
    copy_region.array_layer_count = 1;
    copy_region.extent_width = width;
    copy_region.extent_height = height;
    copy_region.extent_depth_or_layers = 1;
    result = sturdy_rhi_command_encoder_copy_texture_to_buffer(encoder, texture, readback, &copy_region);
    printf("copy_texture_to_buffer  -> %d\n", (int)result);

    memset(&host_barrier, 0, sizeof(host_barrier));
    host_barrier.src_stage = STURDY_PIPELINE_STAGE_TRANSFER;
    host_barrier.src_access = STURDY_ACCESS_TRANSFER_WRITE;
    host_barrier.dst_stage = STURDY_PIPELINE_STAGE_HOST;
    host_barrier.dst_access = STURDY_ACCESS_HOST_READ;
    result = sturdy_rhi_command_encoder_barrier(encoder, 1, &host_barrier, 0, NULL, 0, NULL);
    printf("barrier(transfer->host) -> %d\n", (int)result);

    command_buffer.id = 0;
    result = sturdy_rhi_command_encoder_finish(encoder, &command_buffer);
    printf("command_encoder_finish  -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
        sturdy_rhi_destroy_buffer(engine, uniform_buffer);
        sturdy_rhi_destroy_buffer(engine, readback);
        sturdy_rhi_destroy_texture(engine, texture);
        return;
    }

    {
        /* One-shot GPU work waits on a fence scoped to its own submission, not
           sturdy_rhi_wait_idle: that call is a device-wide barrier, unsafe to interleave with any
           other GPU work the engine itself may have in flight (its own init-time uploads, for
           instance) — exactly the mismatch that produced a real device-lost failure here before
           this fence path was added. */
        SturdyFenceDesc fence_desc;
        SturdyFence fence;
        SturdyBool signaled = STURDY_FALSE;
        SturdySemaphoreDesc semaphore_desc;
        SturdySemaphore semaphore;
        SturdySemaphoreSignal signal;
        int have_semaphore;
        uint64_t semaphore_value = 0;

        memset(&fence_desc, 0, sizeof(fence_desc));
        fence_desc.struct_size = (uint32_t)sizeof(fence_desc);
        fence_desc.label = "ffi probe fence";
        fence.id = 0;
        result = sturdy_rhi_create_fence(engine, &fence_desc, &fence);
        printf("create_fence            -> %d\n", (int)result);

        /* Timeline semaphore, signaled by this submission — exercises sturdy_rhi_submit's signal
           array end to end (added to the FFI well before this probe ever ran it against real
           hardware). */
        memset(&semaphore_desc, 0, sizeof(semaphore_desc));
        semaphore_desc.struct_size = (uint32_t)sizeof(semaphore_desc);
        semaphore_desc.initial_value = 0;
        semaphore_desc.label = "ffi probe semaphore";
        semaphore.id = 0;
        result = sturdy_rhi_create_semaphore(engine, &semaphore_desc, &semaphore);
        printf("create_semaphore        -> %d\n", (int)result);
        have_semaphore = (result == STURDY_OK);

        memset(&signal, 0, sizeof(signal));
        signal.semaphore = semaphore;
        signal.value = 1;
        signal.stages = STURDY_PIPELINE_STAGE_TRANSFER;

        result = sturdy_rhi_submit(engine, STURDY_QUEUE_CLASS_GRAPHICS, 0, 1, &command_buffer, 0, NULL,
                                   have_semaphore ? 1u : 0u, have_semaphore ? &signal : NULL, fence,
                                   STURDY_TRUE);
        printf("submit                  -> %d\n", (int)result);

        result = sturdy_rhi_wait_fences(engine, 1, &fence, STURDY_TRUE, STURDY_WAIT_FOREVER, &signaled);
        printf("wait_fences             -> %d (signaled=%d)\n", (int)result, (int)signaled);

        if (have_semaphore) {
            result = sturdy_rhi_wait_semaphore(engine, semaphore, 1, STURDY_WAIT_FOREVER);
            printf("wait_semaphore          -> %d\n", (int)result);
            result = sturdy_rhi_semaphore_value(engine, semaphore, &semaphore_value);
            printf("semaphore_value         -> %d (%llu, expect 1)\n", (int)result,
                   (unsigned long long)semaphore_value);
            result = sturdy_rhi_destroy_semaphore(engine, semaphore);
            printf("destroy_semaphore       -> %d\n", (int)result);
        }

        if (have_query_set) {
            uint64_t timestamps[2];
            memset(timestamps, 0, sizeof(timestamps));
            result = sturdy_rhi_get_query_set_results(engine, query_set, 0, 2, timestamps, sizeof(timestamps),
                                                      sizeof(uint64_t), STURDY_QUERY_RESULT_FLAGS_RESULT_64_BIT |
                                                                            STURDY_QUERY_RESULT_FLAGS_WAIT);
            printf("get_query_set_results   -> %d (t0=%llu t1=%llu delta=%llu ticks)\n", (int)result,
                   (unsigned long long)timestamps[0], (unsigned long long)timestamps[1],
                   (unsigned long long)(timestamps[1] - timestamps[0]));
            result = sturdy_rhi_destroy_query_set(engine, query_set);
            printf("destroy_query_set       -> %d\n", (int)result);
        }

        result = sturdy_rhi_destroy_fence(engine, fence);
        printf("destroy_fence           -> %d\n", (int)result);
    }
    result = sturdy_rhi_destroy_command_buffer(engine, command_buffer);
    printf("destroy_command_buffer  -> %d\n", (int)result);

    result = sturdy_rhi_map_buffer(engine, readback, &mapped, &mapped_size);
    printf("map_buffer              -> %d (size=%zu)\n", (int)result, mapped_size);
    if (result == STURDY_OK && mapped != NULL) {
        bytes = (unsigned char *)mapped;
        printf("  pixel[0] = (%u,%u,%u,%u) (expect 255,0,0,255)\n",
               bytes[0], bytes[1], bytes[2], bytes[3]);
        sturdy_rhi_unmap_buffer(engine, readback);
    }

    /* Bind-group-layout / bind-group / pipeline-layout creation, without a pipeline that would
       need real shader bytecode this probe cannot compile. */
    memset(&layout_entry, 0, sizeof(layout_entry));
    layout_entry.binding = 0;
    layout_entry.shader_register = 0xffffffffu;
    layout_entry.type = STURDY_BINDING_TYPE_UNIFORM_BUFFER;
    layout_entry.visibility = STURDY_SHADER_STAGE_VERTEX | STURDY_SHADER_STAGE_FRAGMENT;
    layout_entry.count = 1;
    memset(&layout_desc, 0, sizeof(layout_desc));
    layout_desc.struct_size = (uint32_t)sizeof(layout_desc);
    layout_desc.entry_count = 1;
    layout_desc.entries = &layout_entry;
    layout_desc.label = "ffi probe bind group layout";
    bind_group_layout.id = 0;
    result = sturdy_rhi_create_bind_group_layout(engine, &layout_desc, &bind_group_layout);
    printf("create_bind_group_layout-> %d\n", (int)result);

    if (result == STURDY_OK) {
        memset(&group_entry, 0, sizeof(group_entry));
        group_entry.binding = 0;
        group_entry.buffer = uniform_buffer;
        group_entry.size = 256;
        memset(&group_desc, 0, sizeof(group_desc));
        group_desc.struct_size = (uint32_t)sizeof(group_desc);
        group_desc.layout = bind_group_layout;
        group_desc.entry_count = 1;
        group_desc.entries = &group_entry;
        group_desc.label = "ffi probe bind group";
        bind_group.id = 0;
        result = sturdy_rhi_create_bind_group(engine, &group_desc, &bind_group);
        printf("create_bind_group       -> %d\n", (int)result);
        if (result == STURDY_OK) {
            sturdy_rhi_destroy_bind_group(engine, bind_group);
        }

        memset(&pipeline_layout_desc, 0, sizeof(pipeline_layout_desc));
        pipeline_layout_desc.struct_size = (uint32_t)sizeof(pipeline_layout_desc);
        pipeline_layout_desc.bind_group_layout_count = 1;
        pipeline_layout_desc.bind_group_layouts = &bind_group_layout;
        pipeline_layout_desc.label = "ffi probe pipeline layout";
        pipeline_layout.id = 0;
        result = sturdy_rhi_create_pipeline_layout(engine, &pipeline_layout_desc, &pipeline_layout);
        printf("create_pipeline_layout  -> %d\n", (int)result);
        if (result == STURDY_OK) {
            sturdy_rhi_destroy_pipeline_layout(engine, pipeline_layout);
        }

        sturdy_rhi_destroy_bind_group_layout(engine, bind_group_layout);
    }

    /* Render bundles: record an (empty, pipeline-less) bundle, replay it into a real render pass
       against the probe's own texture, and round-trip the abandon-without-finishing path too. No
       real shader bytecode is available to this probe, so this proves lifecycle/wiring (create,
       finish, execute, release, destroy) rather than actual draw output. */
    {
        SturdyTextureViewDesc view_desc;
        SturdyTextureView view;
        SturdyRenderBundleDesc bundle_desc;
        SturdyRenderBundleEncoder bundle_encoder;
        SturdyRenderBundleEncoder abandoned_encoder;
        SturdyRenderBundle bundle;
        SturdyFormat color_format = STURDY_FORMAT_RGBA8_UNORM;
        SturdyCommandEncoderDesc rb_cmd_desc;
        SturdyCommandEncoder rb_cmd_encoder;
        SturdyTextureBarrier to_color;
        SturdyRenderPassDesc rp_desc;
        SturdyColorAttachment color_attachment;
        SturdyRenderPassEncoder render_pass;
        SturdyCommandBuffer rb_cmd_buffer;
        SturdyFenceDesc rb_fence_desc;
        SturdyFence rb_fence;
        SturdyBool rb_signaled = STURDY_FALSE;

        printf("--- render bundles ---\n");

        memset(&bundle_desc, 0, sizeof(bundle_desc));
        result = sturdy_rhi_render_bundle_desc_init(&bundle_desc);
        printf("render_bundle_desc_init -> %d (samples=%u)\n", (int)result, bundle_desc.samples);
        bundle_desc.color_format_count = 1;
        bundle_desc.color_formats = &color_format;
        bundle_desc.label = "ffi probe bundle";

        abandoned_encoder.token = 0;
        result = sturdy_rhi_create_render_bundle_encoder(engine, &bundle_desc, &abandoned_encoder);
        printf("create_render_bundle_encoder(abandoned) -> %d\n", (int)result);
        if (result == STURDY_OK) {
            result = sturdy_rhi_render_bundle_encoder_release(abandoned_encoder);
            printf("render_bundle_encoder_release -> %d\n", (int)result);
        }

        bundle_encoder.token = 0;
        result = sturdy_rhi_create_render_bundle_encoder(engine, &bundle_desc, &bundle_encoder);
        printf("create_render_bundle_encoder -> %d\n", (int)result);
        bundle.id = 0;
        result = sturdy_rhi_render_bundle_encoder_finish(bundle_encoder, &bundle);
        printf("render_bundle_encoder_finish -> %d (bundle.id=%llu)\n", (int)result,
               (unsigned long long)bundle.id);

        if (bundle.id != 0) {
            memset(&view_desc, 0, sizeof(view_desc));
            view_desc.struct_size = (uint32_t)sizeof(view_desc);
            view_desc.view_type = STURDY_TEXTURE_VIEW_TYPE_2D;
            view_desc.texture = texture;
            view_desc.format = STURDY_FORMAT_UNDEFINED;
            view_desc.mip_level_count = STURDY_ALL_REMAINING;
            view_desc.array_layer_count = STURDY_ALL_REMAINING;
            view.id = 0;
            result = sturdy_rhi_create_texture_view(engine, &view_desc, &view);
            printf("create_texture_view     -> %d\n", (int)result);

            (void)sturdy_rhi_command_encoder_desc_init(&rb_cmd_desc);
            rb_cmd_desc.label = "ffi probe render bundle encoder";
            rb_cmd_encoder.token = 0;
            result = sturdy_rhi_create_command_encoder(engine, &rb_cmd_desc, &rb_cmd_encoder);
            printf("create_command_encoder(rb) -> %d\n", (int)result);

            memset(&to_color, 0, sizeof(to_color));
            to_color.texture = texture;
            to_color.dst_stage = STURDY_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT;
            to_color.dst_access = STURDY_ACCESS_COLOR_ATTACHMENT_WRITE;
            to_color.old_layout = STURDY_TEXTURE_LAYOUT_TRANSFER_SRC;
            to_color.new_layout = STURDY_TEXTURE_LAYOUT_COLOR_ATTACHMENT;
            to_color.range = full_range;
            result = sturdy_rhi_command_encoder_barrier(rb_cmd_encoder, 0, NULL, 0, NULL, 1, &to_color);
            printf("barrier(src->color)     -> %d\n", (int)result);

            memset(&color_attachment, 0, sizeof(color_attachment));
            color_attachment.view = view;
            color_attachment.load_op = STURDY_LOAD_OP_LOAD;
            color_attachment.store_op = STURDY_STORE_OP_STORE;
            memset(&rp_desc, 0, sizeof(rp_desc));
            (void)sturdy_rhi_render_pass_desc_init(&rp_desc);
            rp_desc.color_attachment_count = 1;
            rp_desc.color_attachments = &color_attachment;
            rp_desc.render_area.width = width;
            rp_desc.render_area.height = height;
            rp_desc.allow_bundles = STURDY_TRUE;
            rp_desc.label = "ffi probe render bundle pass";
            render_pass.token = 0;
            result = sturdy_rhi_command_encoder_begin_render_pass(rb_cmd_encoder, &rp_desc, &render_pass);
            printf("begin_render_pass(bundles) -> %d\n", (int)result);

            result = sturdy_rhi_render_pass_execute_bundles(render_pass, 1, &bundle);
            printf("execute_bundles         -> %d\n", (int)result);

            result = sturdy_rhi_render_pass_end(render_pass);
            printf("render_pass_end         -> %d\n", (int)result);

            rb_cmd_buffer.id = 0;
            result = sturdy_rhi_command_encoder_finish(rb_cmd_encoder, &rb_cmd_buffer);
            printf("command_encoder_finish(rb) -> %d\n", (int)result);

            memset(&rb_fence_desc, 0, sizeof(rb_fence_desc));
            rb_fence_desc.struct_size = (uint32_t)sizeof(rb_fence_desc);
            rb_fence_desc.label = "ffi probe render bundle fence";
            rb_fence.id = 0;
            result = sturdy_rhi_create_fence(engine, &rb_fence_desc, &rb_fence);
            result = sturdy_rhi_submit(engine, STURDY_QUEUE_CLASS_GRAPHICS, 0, 1, &rb_cmd_buffer, 0, NULL, 0,
                                       NULL, rb_fence, STURDY_TRUE);
            printf("submit(rb)              -> %d\n", (int)result);
            result = sturdy_rhi_wait_fences(engine, 1, &rb_fence, STURDY_TRUE, STURDY_WAIT_FOREVER, &rb_signaled);
            printf("wait_fences(rb)         -> %d (signaled=%d)\n", (int)result, (int)rb_signaled);

            sturdy_rhi_destroy_fence(engine, rb_fence);
            sturdy_rhi_destroy_command_buffer(engine, rb_cmd_buffer);
            sturdy_rhi_destroy_texture_view(engine, view);
            result = sturdy_rhi_destroy_render_bundle(engine, bundle);
            printf("destroy_render_bundle   -> %d\n", (int)result);
        }
    }

    sturdy_rhi_destroy_buffer(engine, uniform_buffer);
    sturdy_rhi_destroy_buffer(engine, readback);
    sturdy_rhi_destroy_texture(engine, texture);
}

static SturdyBool on_init(SturdyEngine engine, void *user_data) {
    (void)user_data;
    report_startup(engine);
    probe_rhi_resources(engine);
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

/* Exercises the fine-grained render-graph settings setters once: init each struct to engine
   defaults, tweak a field, apply it, and report the result. */
static void apply_frame_settings(SturdyEngine engine, SturdyFrame frame) {
    SturdyResult result;
    SturdySceneSettings scene;
    SturdyShadowSettings shadows;
    SturdyAmbientOcclusionSettings ao;
    SturdyAntiAliasingSettings aa;
    SturdyBloomSettings bloom;
    SturdyToneMappingSettings tone_mapping;
    SturdyRestirGiSettings restir_gi;
    SturdyMotionBlurSettings motion_blur;
    SturdyFrame wrong_kind;

    printf("--- render graph settings ---\n");

    (void)sturdy_scene_settings_init(&scene);
    result = sturdy_frame_set_scene_settings(frame, &scene);
    printf("set_scene_settings       -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
    }

    (void)sturdy_shadow_settings_init(&shadows);
    shadows.cascade_count = 3;
    shadows.max_distance = 120.0f;
    result = sturdy_frame_set_shadow_settings(frame, &shadows);
    printf("set_shadow_settings      -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
    }

    (void)sturdy_ambient_occlusion_settings_init(&ao);
    ao.quality = STURDY_AMBIENT_OCCLUSION_QUALITY_MEDIUM;
    result = sturdy_frame_set_ambient_occlusion_settings(frame, &ao);
    printf("set_ao_settings          -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
    }

    /* An engine handle's token, presented where a frame handle is expected: the wrong HandleKind
       must be rejected rather than silently reinterpreted. */
    wrong_kind.token = engine.token;
    result = sturdy_frame_set_ambient_occlusion_settings(wrong_kind, &ao);
    printf("set_ao_settings(wrong handle kind) -> %d (expect 2 = invalid handle)\n", (int)result);

    (void)sturdy_anti_aliasing_settings_init(&aa);
    aa.msaa_samples = 4;
    result = sturdy_frame_set_anti_aliasing_settings(frame, &aa);
    printf("set_anti_aliasing_settings -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
    }

    (void)sturdy_bloom_settings_init(&bloom);
    bloom.intensity = 0.08f;
    result = sturdy_frame_set_bloom_settings(frame, &bloom);
    printf("set_bloom_settings       -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
    }

    (void)sturdy_tone_mapping_settings_init(&tone_mapping);
    tone_mapping.exposure = 1.1f;
    result = sturdy_frame_set_tone_mapping_settings(frame, &tone_mapping);
    printf("set_tone_mapping_settings -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
    }

    (void)sturdy_restir_gi_settings_init(&restir_gi);
    result = sturdy_frame_set_restir_gi_settings(frame, &restir_gi);
    printf("set_restir_gi_settings   -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
    }

    (void)sturdy_motion_blur_settings_init(&motion_blur);
    result = sturdy_frame_set_motion_blur_settings(frame, &motion_blur);
    printf("set_motion_blur_settings -> %d\n", (int)result);
    if (result != STURDY_OK) {
        printf("  %s\n", sturdy_last_error_message());
    }
}

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
            if (camera == STURDY_OK) {
                apply_frame_settings(engine, frame);
                fflush(stdout);
            }
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
    SturdyResult result;

    (void)user_data;
    printf("on_shutdown called\n");

    /* Exercised here rather than right after the scene stops using them: unloading a model still
       assigned to a live entity is not rejected (see sturdy_render_unload_asset's own doc comment),
       so this only proves the round trip once nothing references them any more. */
    if (g_scene_ok) {
        result = sturdy_render_unload_asset(engine, g_cube_model);
        printf("unload_asset(cube)     -> %d\n", (int)result);
        result = sturdy_render_unload_asset(engine, g_floor_model);
        printf("unload_asset(floor)    -> %d\n", (int)result);
        result = sturdy_render_unload_asset(engine, g_shader);
        printf("unload_asset(shader)   -> %d\n", (int)result);
        result = sturdy_render_unload_asset(engine, g_shader);
        printf("unload_asset(shader again) -> %d (expect 2 = invalid handle)\n", (int)result);
    }
    fflush(stdout);
}

/* Counts by level, to prove the log sink actually receives the engine's own messages (not just
   ones this probe prints itself) without spamming stdout with a full duplicate transcript. */
static uint32_t g_log_counts[6];

static void on_engine_log(SturdyLogLevel level, const char *message, size_t message_length, void *user_data) {
    (void)message;
    (void)user_data;
    if (message_length == 0) {
        return;
    }
    if ((int)level >= 0 && (int)level < 6) {
        g_log_counts[level]++;
    }
}

int main(int argc, char **argv) {
    SturdyRuntimeConfig config;
    SturdyGameLogic logic;
    int32_t exit_code = 0;
    SturdyResult result;
    SturdyLogSink log_sink;

    /* Unbuffered: this probe can hang inside a GPU call, and buffered output would be lost when it
       is killed, hiding exactly the line that would say where. */
    (void)setvbuf(stdout, NULL, _IONBF, 0);

    log_sink.id = 0;
    result = sturdy_log_add_sink(on_engine_log, NULL, &log_sink);
    printf("log_add_sink -> %d\n", (int)result);

    /* Before any engine exists: this is the GPU-picker path a real application would use to
       choose `physical_device_id`, not the already-selected adapter `sturdy_rhi_*` reports on a
       live device (see report_startup). */
    {
        SturdyGpuInfo gpus[8];
        uint32_t gpu_count = 0;
        uint32_t gpu_index;
        result = sturdy_gpu_enumerate(gpus, 8, &gpu_count);
        printf("gpu_enumerate           -> %d (count=%u)\n", (int)result, gpu_count);
        for (gpu_index = 0; gpu_index < gpu_count && gpu_index < 8; ++gpu_index) {
            printf("  [%u] name='%s' vendor='%s' id='%s' type=%d backends=0x%X\n", gpu_index,
                   gpus[gpu_index].name, gpus[gpu_index].vendor, gpus[gpu_index].physical_device_id,
                   (int)gpus[gpu_index].device_type, gpus[gpu_index].supported_backends);
        }
        result = sturdy_gpu_enumerate(NULL, 0, &gpu_count);
        printf("gpu_enumerate(count only) -> %d (count=%u)\n", (int)result, gpu_count);
    }

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

    printf("log sink message counts: trace=%u debug=%u info=%u warn=%u error=%u critical=%u\n", g_log_counts[0],
           g_log_counts[1], g_log_counts[2], g_log_counts[3], g_log_counts[4], g_log_counts[5]);
    result = sturdy_log_remove_sink(log_sink);
    printf("log_remove_sink -> %d\n", (int)result);
    /* Removing twice must not double-free or crash. */
    result = sturdy_log_remove_sink(log_sink);
    printf("log_remove_sink(again) -> %d\n", (int)result);

    /* `g_scene_ok` only becomes true after the full shader/pipeline/scene build in `build_scene`
       succeeds, so a failure to reach it (shader compile, bind-group-layout, or pipeline creation
       error on either backend) is a real regression, not an expected negative-path result. Turning
       that into a nonzero exit is what lets this run as an actual pass/fail CTest case instead of a
       log a human has to read. */
    if (!g_scene_ok) {
        printf("FAIL: scene build did not complete (see build_scene output above)\n");
        return 1;
    }
    return 0;
}
