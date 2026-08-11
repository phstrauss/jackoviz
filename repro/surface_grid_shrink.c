/*
 * Minimal repro for Datoviz: shrink-recreate of dvz_geometry_surface_grid leaves a
 * stale GPU index buffer (draw validation / emit failures).
 *
 * Sequence:
 *   1. Build a lit surface mesh (ROWS × COLS_WIDE) with random heights.
 *   2. Draw for SHRINK_AT frames (optional height updates each frame).
 *   3. Tear down mesh → geometry → panel (no panel_remove_visual in the API).
 *   4. Recreate the same panel path with fewer columns (COLS_NARROW).
 *   5. Draw more frames.
 *
 * Expected (bug): after step 4, stderr spam like:
 *   _app_draw emit failed: scene draw resource validation failed:
 *     visual=primitive role=index
 *     draw_count ≈ (wide index count)  logical_count ≈ (narrow index count)
 *
 * Control: pass --grow to start narrow and recreate wider (should stay quiet).
 *
 * Build (jackoviz tree):
 *   cmake --build build --target surface_grid_shrink_repro
 * Run:
 *   ./build/surface_grid_shrink_repro
 *   ./build/surface_grid_shrink_repro --grow
 *   ./build/surface_grid_shrink_repro --frames 120
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <datoviz.h>

#define WIDTH      960u
#define HEIGHT     640u
#define ROWS       64u
#define COLS_WIDE  320u /* ~5× COLS_NARROW — matches jackoviz 20 kHz → 4 kHz ratio */
#define COLS_NARROW 64u
#define SHRINK_AT  45u  /* recreate on this frame index */
#define DEFAULT_FRAMES 120u

typedef struct Repro
{
    DvzScene* scene;
    DvzFigure* figure;
    DvzPanel* panel;
    DvzVisual* mesh;
    DvzGeometry* geometry;
    DvzApp* app;
    DvzView* view;
    DvzAnimation* timer;

    double* heights;
    DvzColor* colors;
    uint32_t rows;
    uint32_t cols;

    uint32_t frames;
    uint32_t frame_limit;
    uint32_t recreate_at;
    uint32_t cols_first;
    uint32_t cols_second;
    bool recreated;
    bool running;
} Repro;

static uint32_t g_rng = 0xC0FFEEu;

static uint32_t rng_u32(void)
{
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g_rng = x;
}

static double rng_unit(void) { return (double)rng_u32() / (double)UINT32_MAX; }

static void fill_heights(Repro* r, double phase)
{
    for (uint32_t row = 0; row < r->rows; row++)
    {
        const double v = -1.0 + 2.0 * (double)row / (double)(r->rows > 1u ? r->rows - 1u : 1u);
        for (uint32_t col = 0; col < r->cols; col++)
        {
            const double u =
                -1.0 + 2.0 * (double)col / (double)(r->cols > 1u ? r->cols - 1u : 1u);
            const double r2 = u * u + v * v;
            const double z = 0.35 * cos(10.0 * sqrt(r2) + phase) * exp(-1.1 * r2) +
                             0.12 * sin(8.0 * u + phase) * cos(6.0 * v) + 0.05 * rng_unit();
            const uint32_t i = row * r->cols + col;
            r->heights[i] = z;
            const double t = fmin(1.0, fmax(0.0, (z + 0.3) / 0.7));
            r->colors[i] = dvz_color_rgba(
                (uint8_t)(40.0 + 180.0 * t), (uint8_t)(80.0 + 120.0 * (1.0 - t)),
                (uint8_t)(160.0 + 60.0 * t), 255);
        }
    }
}

static bool alloc_buffers(Repro* r, uint32_t cols)
{
    free(r->heights);
    free(r->colors);
    r->heights = NULL;
    r->colors = NULL;
    r->cols = cols;
    const size_t n = (size_t)r->rows * (size_t)cols;
    r->heights = (double*)calloc(n, sizeof(double));
    r->colors = (DvzColor*)calloc(n, sizeof(DvzColor));
    return r->heights != NULL && r->colors != NULL;
}

static void destroy_surface(Repro* r)
{
    if (r->view != NULL)
        (void)dvz_view_connect_panel(r->view, NULL);
    if (r->panel != NULL)
        (void)dvz_panel_connect_input(r->panel, NULL);

    if (r->mesh != NULL)
    {
        (void)dvz_visual_set_visible(r->mesh, false);
        dvz_visual_destroy(r->mesh);
        r->mesh = NULL;
    }
    if (r->geometry != NULL)
    {
        dvz_geometry_destroy(r->geometry);
        r->geometry = NULL;
    }
    if (r->panel != NULL)
    {
        dvz_panel_destroy(r->panel);
        r->panel = NULL;
    }
}

static bool create_surface(Repro* r, uint32_t cols)
{
    if (!alloc_buffers(r, cols))
        return false;
    fill_heights(r, 0.0);

    DvzPanelDesc pdesc = dvz_panel_desc();
    pdesc.x = 0.0f;
    pdesc.y = 0.0f;
    pdesc.width = 1.0f;
    pdesc.height = 1.0f;
    r->panel = dvz_panel(r->figure, &pdesc);
    if (r->panel == NULL)
        return false;
    if (dvz_panel_set_background_color(r->panel, dvz_color_rgba(12, 14, 20, 255)) != DVZ_OK)
        return false;

    DvzCameraDesc camera = dvz_camera_desc();
    camera.view.eye[0] = -0.4f;
    camera.view.eye[1] = 2.4f;
    camera.view.eye[2] = 4.6f;
    camera.view.target[0] = 0.0f;
    camera.view.target[1] = 0.35f;
    camera.view.target[2] = 0.0f;
    camera.view.up[0] = 0.0f;
    camera.view.up[1] = 1.0f;
    camera.view.up[2] = 0.0f;
    camera.projection.fov_y = 0.66f;
    camera.projection.near_clip = 0.05f;
    camera.projection.far_clip = 100.0f;
    if (dvz_panel_set_camera_desc(r->panel, &camera) != DVZ_OK)
        return false;

    DvzGeometrySurfaceGridDesc desc = dvz_geometry_surface_grid_desc();
    desc.rows = r->rows;
    desc.cols = r->cols;
    desc.heights = r->heights;
    desc.colors = r->colors;
    desc.origin[0] = -2.6;
    desc.origin[1] = 0.0;
    desc.origin[2] = +1.8;
    desc.col_basis[0] = 5.2 / (double)(r->cols > 1u ? r->cols - 1u : 1u);
    desc.col_basis[1] = 0.0;
    desc.col_basis[2] = 0.0;
    desc.row_basis[0] = 0.0;
    desc.row_basis[1] = 0.0;
    desc.row_basis[2] = -3.6 / (double)(r->rows > 1u ? r->rows - 1u : 1u);
    desc.height_axis[0] = 0.0;
    desc.height_axis[1] = 1.0;
    desc.height_axis[2] = 0.0;
    desc.height_scale = 1.35;

    r->geometry = dvz_geometry_surface_grid(&desc);
    if (r->geometry == NULL)
        return false;

    r->mesh = dvz_mesh(r->scene, 0);
    if (r->mesh == NULL)
        return false;

    DvzMaterialDesc material = dvz_phong_material_desc();
    material.light_direction[0] = -1.0f;
    material.light_direction[1] = 1.0f;
    material.light_direction[2] = 1.0f;
    material.phong.ambient = 0.28f;
    material.phong.diffuse = 0.72f;
    material.phong.specular = 0.42f;
    material.phong.shininess = 48.0f;
    if (dvz_visual_set_material(r->mesh, &material) != DVZ_OK)
        return false;
    if (dvz_mesh_set_geometry(r->mesh, r->geometry) != DVZ_OK)
        return false;
    if (dvz_panel_add_visual(r->panel, r->mesh, NULL) != DVZ_OK)
        return false;

    DvzController* arcball = dvz_arcball(r->scene, NULL);
    if (arcball == NULL ||
        dvz_panel_bind_controller(r->panel, arcball, DVZ_DIM_MASK_XYZ) != DVZ_OK)
        return false;

    if (r->view != NULL)
        (void)dvz_view_connect_panel(r->view, r->panel);

    return true;
}

static bool upload(Repro* r)
{
    if (r->geometry == NULL || r->mesh == NULL || r->heights == NULL)
        return false;
    fill_heights(r, 0.05 * (double)r->frames);
    const uint32_t vertex_count = r->rows * r->cols;
    if (dvz_geometry_surface_grid_update_heights(r->geometry, r->heights, vertex_count) != DVZ_OK)
        return false;
    if (r->geometry->colors != NULL)
        memcpy(r->geometry->colors, r->colors, (size_t)vertex_count * sizeof(DvzColor));
    return dvz_mesh_set_geometry(r->mesh, r->geometry) == DVZ_OK;
}

static void on_timer(DvzAnimation* animation, double t, double dt, uint64_t tick, void* user_data)
{
    (void)animation;
    (void)t;
    (void)dt;
    (void)tick;

    Repro* r = (Repro*)user_data;
    if (!r->running)
        return;

    if (!r->recreated && r->frames == r->recreate_at)
    {
        fprintf(
            stderr,
            "repro: recreate surface %u×%u → %u×%u (destroy mesh/geometry/panel, then create)\n",
            r->rows, r->cols, r->rows, r->cols_second);
        destroy_surface(r);
        if (!create_surface(r, r->cols_second))
        {
            fprintf(stderr, "repro: recreate failed\n");
            r->running = false;
            dvz_app_stop(r->app);
            return;
        }
        r->recreated = true;
        fprintf(stderr, "repro: recreate done — watch for index draw_count validation errors\n");
    }

    (void)upload(r);

    r->frames++;
    if (r->frame_limit != 0u && r->frames >= r->frame_limit)
    {
        r->running = false;
        dvz_app_stop(r->app);
    }
}

static void usage(const char* prog)
{
    fprintf(
        stderr,
        "Usage: %s [--grow] [--frames N]\n"
        "  default: start %ux%u, recreate as %ux%u at frame %u (triggers bug)\n"
        "  --grow:  start %ux%u, recreate as %ux%u (control; should stay quiet)\n",
        prog, ROWS, COLS_WIDE, ROWS, COLS_NARROW, SHRINK_AT, ROWS, COLS_NARROW, ROWS, COLS_WIDE);
}

int main(int argc, char** argv)
{
    bool grow = false;
    uint32_t frame_limit = DEFAULT_FRAMES;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--grow") == 0)
            grow = true;
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frame_limit = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            usage(argv[0]);
            return EXIT_SUCCESS;
        }
        else
        {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    Repro r;
    memset(&r, 0, sizeof(r));
    r.rows = ROWS;
    r.frame_limit = frame_limit;
    r.recreate_at = SHRINK_AT;
    r.cols_first = grow ? COLS_NARROW : COLS_WIDE;
    r.cols_second = grow ? COLS_WIDE : COLS_NARROW;
    r.running = true;

    fprintf(
        stderr,
        "repro: surface_grid %s  %u×%u → %u×%u at frame %u, run %u frames\n",
        grow ? "GROW (control)" : "SHRINK (bug)", r.rows, r.cols_first, r.rows, r.cols_second,
        r.recreate_at, r.frame_limit);

    r.scene = dvz_scene();
    if (r.scene == NULL)
        return EXIT_FAILURE;
    r.figure = dvz_figure(r.scene, WIDTH, HEIGHT, 0);
    if (r.figure == NULL)
        return EXIT_FAILURE;

    if (!create_surface(&r, r.cols_first))
    {
        fprintf(stderr, "repro: initial create failed\n");
        return EXIT_FAILURE;
    }

    DvzAnimTimerDesc timer_desc = dvz_anim_timer_desc();
    timer_desc.mode = DVZ_TIMER_EVERY_FRAME;
    timer_desc.callback = on_timer;
    timer_desc.user_data = &r;
    r.timer = dvz_anim_timer(r.scene, &timer_desc);
    if (r.timer == NULL || dvz_anim_start(r.timer, 0.0) != DVZ_OK)
        return EXIT_FAILURE;

    DvzAppConfig cfg = dvz_app_config();
    cfg.schedule_mode = DVZ_APP_SCHEDULE_CONTINUOUS;
    cfg.fps_cap = 60.0;
    r.app = dvz_app_with_config(r.scene, &cfg);
    if (r.app == NULL)
        return EXIT_FAILURE;

    r.view = dvz_view_window(r.app, r.figure, WIDTH, HEIGHT, "surface_grid_shrink_repro");
    if (r.view == NULL)
        return EXIT_FAILURE;
    (void)dvz_view_connect_panel(r.view, r.panel);

    dvz_app_run(r.app, frame_limit == 0u ? 0u : frame_limit);

    fprintf(stderr, "repro: finished after %u frames (recreated=%s)\n", r.frames,
            r.recreated ? "yes" : "no");

    if (r.app != NULL)
        dvz_app_destroy(r.app);
    if (r.geometry != NULL)
        dvz_geometry_destroy(r.geometry);
    if (r.scene != NULL)
        dvz_scene_destroy(r.scene);
    free(r.heights);
    free(r.colors);
    return EXIT_SUCCESS;
}
