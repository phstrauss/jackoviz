/*
 * jackoviz.c — realtime mono JACK capture → Kaiser-windowed FFTW3 → Datoviz spectrogram
 *
 * POSIX (macOS / Linux). Build with the accompanying Makefile.
 *
 * Usage:
 *   ./jackoviz [-n 1024|2048|4096|8192] [-f hz] [-b beta] [-c client] [-s source]
 *              [--frames N] [--fast]
 *
 * Connect a mono source to "jackoviz:input", or pass -s system:capture_1.
 * Keys: 0 = oscilloscope, 1 = spectrum XY, 2 = 2D STFT image, 3 = 3D surface,
 *       f = cycle dB floor, c = cycle dB ceiling,
 *       m = cycle max plot Hz (8/12/16/20/4 kHz; disabled if CLI -f was given),
 *       w = toggle line width (1↔2 px),
 *       p = pause/resume visual processing (JACK ringbuffer keeps writing).
 * --fast keeps only scope + 1D spectrum (keys 2/3 disabled).
 */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fftw3.h>
#include <jack/jack.h>
#include "jvz_jack_ringbuffer.h"
#include "jackoviz.h"

#include <datoviz.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

/*************************************************************************************************/
/*  Tunables                                                                                     */
/*************************************************************************************************/

#define DEFAULT_FFT_SIZE     4096u
#define DEFAULT_KAISER_BETA  4.5
#define DEFAULT_HISTORY      256u /* spectrogram time columns (power of two) */
#define AUDIO_RB_BYTES       64*sizeof(float)*1024 /* 64 KiB of float samples */
#define WINDOW_WIDTH         1280u
#define WINDOW_HEIGHT        720u
#define DB_CEIL_DEFAULT           (-20.0)
#define DB_FLOOR_DEFAULT          (-100.0)
#define DB_STORAGE_FLOOR          (-140.0) /* deepest clamp for stored spectra */
#define DB_STORAGE_CEIL           (0.0)   /* highest clamp for stored spectra */
#define DEFAULT_PLOT_FREQ_MAX_HZ  8000.0
#define DB_FLOOR_OPTION_COUNT     5u
#define DB_CEIL_OPTION_COUNT      3u
#define DB_TICK_MAX               16u
#define VIRIDIS_LUT_SIZE          256u
#define PLOT_FREQ_OPTION_COUNT    5u

static const double DB_FLOOR_OPTIONS[DB_FLOOR_OPTION_COUNT] = {
    -100.0, -110.0, -120.0, -130.0, -140.0};
static const double DB_CEIL_OPTIONS[DB_CEIL_OPTION_COUNT] = {0.0, -10.0, -20.0};
/* Key m cycles these when -f was not given on the CLI (8000 → … → 4000 → 8000). */
static const double PLOT_FREQ_OPTIONS[PLOT_FREQ_OPTION_COUNT] = {
    8000.0, 12000.0, 16000.0, 20000.0, 4000.0};

/* CPU viridis LUT for 3D mesh vertex colors (mesh has no scalar colormap path yet). */
static DvzColor g_viridis_lut[VIRIDIS_LUT_SIZE];
static bool g_viridis_lut_ready = false;

static void viridis_lut_init(void)
{
    if (g_viridis_lut_ready)
        return;
    for (uint32_t i = 0; i < VIRIDIS_LUT_SIZE; i++)
    {
        const double t = (double)i / (double)(VIRIDIS_LUT_SIZE - 1u);
        (void)dvz_colormap_builtin_sample(DVZ_BUILTIN_COLORMAP_VIRIDIS, t, &g_viridis_lut[i]);
        g_viridis_lut[i].a = 255;
    }
    g_viridis_lut_ready = true;
}

/* Map normalized t ∈ [0, 1] to a memoized viridis RGBA. */
static DvzColor viridis_lut_sample(double t)
{
    if (t <= 0.0)
        return g_viridis_lut[0];
    if (t >= 1.0)
        return g_viridis_lut[VIRIDIS_LUT_SIZE - 1u];
    const uint32_t i = (uint32_t)(t * (double)(VIRIDIS_LUT_SIZE - 1u) + 0.5);
    return g_viridis_lut[i < VIRIDIS_LUT_SIZE ? i : VIRIDIS_LUT_SIZE - 1u];
}

/*************************************************************************************************/
/*  Doubly-mapped 2-D ringbuffer (time × frequency)                                              */
/*************************************************************************************************/

/*
 * Physical pages of size `byte_size` are mapped twice in a contiguous virtual range of
 * `2 * byte_size`. Column writes wrap in the first mapping; reads of the last H columns can use a
 * single contiguous pointer even when the history crosses the seam.
 */

typedef struct SpecRing
{
    double* map;       /* base of the 2× mirrored mapping */
    size_t byte_size;  /* one logical buffer (= capacity * stride * sizeof(double), page-aligned) */
    uint32_t capacity; /* columns; power of two */
    uint32_t n_bins;   /* active bins per column */
    uint32_t stride;   /* doubles per column (>= n_bins), pads for page alignment */
    uint32_t write_col; /* monotonically increasing */
    int shm_fd;
} SpecRing;

static bool is_pow2_u32(uint32_t x) { return x != 0u && (x & (x - 1u)) == 0u; }

static size_t page_size(void)
{
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t)p : 4096u;
}

static int spec_ring_create(SpecRing* rb, uint32_t capacity_cols, uint32_t n_bins)
{
    memset(rb, 0, sizeof(*rb));
    if (!is_pow2_u32(capacity_cols) || n_bins == 0u)
        return -1;

    rb->capacity = capacity_cols;
    rb->n_bins = n_bins;
    rb->stride = n_bins;
    rb->shm_fd = -1;

    /* Mirrored mmap length must equal the ring period and be a multiple of the page size. */
    const size_t page = page_size();
    while (((size_t)rb->capacity * (size_t)rb->stride * sizeof(double)) % page != 0u)
    {
        rb->stride++;
        if (rb->stride > n_bins + (uint32_t)(page / sizeof(double)) + 8u)
            return -1;
    }
    rb->byte_size = (size_t)rb->capacity * (size_t)rb->stride * sizeof(double);

    char name[64];
    snprintf(name, sizeof name, "/jackoviz-spec-%d", (int)getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return -1;
    (void)shm_unlink(name);
    if (ftruncate(fd, (off_t)rb->byte_size) != 0)
    {
        close(fd);
        return -1;
    }

    void* placeholder =
        mmap(NULL, rb->byte_size * 2u, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (placeholder == MAP_FAILED)
    {
        close(fd);
        return -1;
    }

    void* first = mmap(
        placeholder, rb->byte_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (first == MAP_FAILED)
    {
        munmap(placeholder, rb->byte_size * 2u);
        close(fd);
        return -1;
    }

    void* second = mmap(
        (char*)placeholder + rb->byte_size, rb->byte_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_FIXED, fd, 0);
    if (second == MAP_FAILED)
    {
        munmap(placeholder, rb->byte_size * 2u);
        close(fd);
        return -1;
    }

    rb->map = (double*)placeholder;
    rb->shm_fd = fd;
    memset(rb->map, 0, rb->byte_size);
    return 0;
}

static void spec_ring_destroy(SpecRing* rb)
{
    if (rb == NULL)
        return;
    if (rb->map != NULL)
        munmap(rb->map, rb->byte_size * 2u);
    if (rb->shm_fd >= 0)
        close(rb->shm_fd);
    memset(rb, 0, sizeof(*rb));
    rb->shm_fd = -1;
}

static void spec_ring_push(SpecRing* rb, const double* column)
{
    const uint32_t slot = rb->write_col & (rb->capacity - 1u);
    double* dst = rb->map + (size_t)slot * (size_t)rb->stride;
    memcpy(dst, column, (size_t)rb->n_bins * sizeof(double));
    if (rb->stride > rb->n_bins)
        memset(dst + rb->n_bins, 0, (size_t)(rb->stride - rb->n_bins) * sizeof(double));
    rb->write_col++;
}

/* Contiguous view of the last `count` columns (count <= capacity). Oldest first.
 * Columns are stride-apart; callers that need packed bins should copy. */
static const double* spec_ring_history(const SpecRing* rb, uint32_t count)
{
    if (count == 0u || count > rb->capacity)
        return NULL;
    if (rb->write_col < count)
        return rb->map;
    const uint32_t start = (rb->write_col - count) & (rb->capacity - 1u);
    return rb->map + (size_t)start * (size_t)rb->stride;
}

/* Most recently pushed spectrum column, or NULL if empty. */
static const double* spec_ring_latest(const SpecRing* rb)
{
    if (rb == NULL || rb->write_col == 0u)
        return NULL;
    const uint32_t slot = (rb->write_col - 1u) & (rb->capacity - 1u);
    return rb->map + (size_t)slot * (size_t)rb->stride;
}

/*************************************************************************************************/
/*  Kaiser window                                                                                */
/*************************************************************************************************/

static double bessel_i0(double x)
{
    const double y = x * x * 0.25;
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; k++)
    {
        term *= y / ((double)k * (double)k);
        sum += term;
        if (term < 1e-14 * sum)
            break;
    }
    return sum;
}

static void kaiser_window(double* w, uint32_t n, double beta)
{
    const double denom = bessel_i0(beta);
    const double mid = 0.5 * (double)(n - 1u);
    for (uint32_t i = 0; i < n; i++)
    {
        const double r = ((double)i - mid) / mid;
        const double arg = beta * sqrt(fmax(0.0, 1.0 - r * r));
        w[i] = bessel_i0(arg) / denom;
    }
}

/*************************************************************************************************/
/*  Application state                                                                            */
/*************************************************************************************************/

typedef struct Jackoviz
{
    /* JACK */
    jack_client_t* client;
    jack_port_t* in_port;
    jvz_jack_ringbuffer_t* audio_rb;
    jack_nframes_t sample_rate;

    /* FFT */
    uint32_t fft_size;
    uint32_t n_bins;
    double kaiser_beta;
    double* window;
    double* time_buf;
    fftw_complex* freq_buf;
    fftw_plan plan;
    double* mag_db; /* one spectrum column */

    /* Spectrogram history */
    SpecRing spec;
    uint32_t history;
    uint32_t n_plot_bins; /* bins kept: 0 … plot_freq_limit */
    double plot_freq_limit; /* requested max display frequency (Hz) */
    double plot_freq_max; /* Hz covered by n_plot_bins */
    uint32_t plot_freq_index; /* index into PLOT_FREQ_OPTIONS when cycling */
    bool plot_freq_locked; /* true if CLI -f fixed the limit */
    double* heights;      /* 3D: row-major time × freq, normalized [0,1] */
    DvzColor* colors;     /* 3D: viridis colors matching heights */
    float* field_values;  /* 2D: row-major freq × time, dB */
    vec3* spectrum_pos;   /* 1D: freq × dB line vertices */
    DvzColor* spectrum_color;
    float* spectrum_width;
    float* scope_wave;    /* latest raw time-domain frame */
    vec3* scope_pos;      /* scope: time × amplitude */
    DvzColor* scope_color;
    float* scope_width;
    bool scope_have_data;
    double db_floor; /* display floor; cycled with key f */
    uint32_t db_floor_index;
    double db_ceil; /* display ceiling; cycled with key c */
    uint32_t db_ceil_index;
    double db_tick_values[DB_TICK_MAX];
    uint32_t db_tick_count;

    /* Datoviz */
    DvzScene* scene;
    DvzFigure* figure;
    DvzPanel* panel_3d;
    DvzPanel* panel_2d;
    DvzPanel* panel_1d;
    DvzPanel* panel_scope;
    DvzVisual* mesh;
    DvzGeometry* geometry;
    DvzVisual* image;
    DvzSampledField* field;
    DvzVisual* spectrum; /* 1D path */
    DvzVisual* scope;    /* oscilloscope path */
    DvzScale* db_scale;  /* shared 2D color scale */
    DvzColorbar* colorbar;
    DvzApp* app;
    DvzView* view;
    DvzAnimation* timer;
    ViewMode view_mode;

    uint32_t frame_limit;
    uint32_t frames_drawn;
    bool running;
    bool fast; /* --fast: scope + 1D only; skip 2D/3D spectrogram */
    float line_width_px; /* 1D spectrum + scope stroke; toggled with key w */
    bool paused; /* key p: freeze FFT/visuals; JACK ringbuffer keeps writing */
} Jackoviz;

static int jack_process(jack_nframes_t nframes, void* arg)
{
    Jackoviz* app = (Jackoviz*)arg;
    float* in = (float*)jack_port_get_buffer(app->in_port, nframes);
    if (in == NULL)
        return 0;

    const size_t bytes = (size_t)nframes * sizeof(float);
    if (jvz_jack_ringbuffer_write_space(app->audio_rb) >= bytes)
        (void)jvz_jack_ringbuffer_write(app->audio_rb, (const char*)in, bytes);
    /* else drop — prefer realtime safety over completeness */

    return 0;
}

static void jack_shutdown(void* arg)
{
    Jackoviz* app = (Jackoviz*)arg;
    app->running = false;
    if (app->app != NULL)
        dvz_app_stop(app->app);
}

/*
 * Convert a complex FFT bin to decibels before the spectrogram ringbuffer.
 * Equivalent forms: 20*log10(|X|)  ==  10*log10(|X|^2)  (power spectrum).
 */
static void spectrum_to_db(const fftw_complex* freq, double* mag_db, uint32_t n_bins, uint32_t fft_size)
{
    const double scale = 1.0 / (double)fft_size;
    for (uint32_t k = 0; k < n_bins; k++)
    {
        const double re = freq[k][0] * scale;
        const double im = freq[k][1] * scale;
        const double mag = hypot(re, im);
        double db = 20.0 * log10(mag + 1e-20);
        if (db < DB_STORAGE_FLOOR)
            db = DB_STORAGE_FLOOR;
        if (db > DB_STORAGE_CEIL)
            db = DB_STORAGE_CEIL;
        mag_db[k] = db;
    }
}

static void process_available_audio(Jackoviz* app)
{
    const size_t frame_bytes = (size_t)app->fft_size * sizeof(float);
    float scratch[8192]; /* max supported FFT size */

    if (app->fft_size > 8192u)
        return;

    size_t newbytes = jvz_jack_ringbuffer_read_space(app->audio_rb);
    if (newbytes > 0)
    {
        /* Peek the newest FFT window (history / future overlap); then consume a hop. */
        if (jvz_jack_ringbuffer_read_lastn(app->audio_rb, (char*)scratch, frame_bytes) != frame_bytes)
            return;
        jvz_jack_ringbuffer_read_advance(app->audio_rb, newbytes);

        // Keep the newest raw frame for the oscilloscope view.
        if (app->scope_wave != NULL)
        {
            memcpy(app->scope_wave, scratch, frame_bytes);
            app->scope_have_data = true;
        }

        for (uint32_t i = 0; i < app->fft_size; i++)
            app->time_buf[i] = (double)scratch[i] * app->window[i];

        fftw_execute(app->plan);
        spectrum_to_db(app->freq_buf, app->mag_db, app->n_bins, app->fft_size);
        spec_ring_push(&app->spec, app->mag_db);
    }
}

static void fill_spectrogram_buffers(Jackoviz* app)
{
    const uint32_t n_time = app->history;
    const uint32_t n_freq = app->n_plot_bins;
    const uint32_t available =
        app->spec.write_col < n_time ? app->spec.write_col : n_time;
    const uint32_t surface_count = n_time * n_freq;

    viridis_lut_init();
    memset(app->heights, 0, (size_t)surface_count * sizeof(double));
    const DvzColor floor_color = g_viridis_lut[0];
    for (uint32_t i = 0; i < surface_count; i++)
    {
        app->field_values[i] = (float)app->db_floor;
        app->colors[i] = floor_color;
    }

    if (available == 0u)
        return;

    const double* hist = spec_ring_history(&app->spec, available);
    if (hist == NULL)
        return;

    /* Newest spectrum at high time index for the 3D surface (far edge).
     * 2D columns are mirrored so on-screen time increases to the right. */
    const uint32_t t0 = n_time - available;
    const double db_span = app->db_ceil - app->db_floor;
    for (uint32_t t = 0; t < available; t++)
    {
        const double* src = hist + (size_t)t * (size_t)app->spec.stride;
        const uint32_t col_2d = n_time - 1u - (t0 + t);
        double* hdst = app->heights + (size_t)(t0 + t) * (size_t)n_freq;
        DvzColor* coldst = app->colors + (size_t)(t0 + t) * (size_t)n_freq;
        for (uint32_t f = 0; f < n_freq; f++)
        {
            const double db = src[f];
            double v = db_span > 0.0 ? (db - app->db_floor) / db_span : 0.0;
            if (v < 0.0)
                v = 0.0;
            if (v > 1.0)
                v = 1.0;
            hdst[f] = v;
            coldst[f] = viridis_lut_sample(v);
            app->field_values[(size_t)f * (size_t)n_time + (size_t)col_2d] = (float)db;
        }
    }
}

static bool upload_spectrogram(Jackoviz* app)
{
    fill_spectrogram_buffers(app);
    const uint32_t vertex_count = app->history * app->n_plot_bins;

    if (dvz_geometry_surface_grid_update_heights(
            app->geometry, app->heights, vertex_count) != DVZ_OK)
        return false;
    if (app->geometry->colors != NULL)
        memcpy(app->geometry->colors, app->colors, (size_t)vertex_count * sizeof(DvzColor));
    if (dvz_mesh_set_geometry(app->mesh, app->geometry) != DVZ_OK)
        return false;

    if (app->field != NULL)
    {
        DvzFieldDataView view = dvz_field_data_view();
        view.data = app->field_values;
        view.bytes_per_row = (uint64_t)app->history * sizeof(float);
        view.rows_per_image = app->n_plot_bins;
        if (dvz_sampled_field_set_data(app->field, &view) != DVZ_OK)
            return false;
    }
    return true;
}

static bool upload_spectrum(Jackoviz* app)
{
    if (app->spectrum == NULL || app->spectrum_pos == NULL || app->n_plot_bins == 0u)
        return false;

    const double hz_per_bin =
        (double)app->sample_rate / (double)(app->fft_size > 0u ? app->fft_size : 1u);
    const double* latest = spec_ring_latest(&app->spec);

    for (uint32_t k = 0; k < app->n_plot_bins; k++)
    {
        const double hz = (double)k * hz_per_bin;
        double db = app->db_floor;
        if (latest != NULL)
            db = latest[k];
        app->spectrum_pos[k][0] = (float)hz;
        app->spectrum_pos[k][1] = (float)db;
        app->spectrum_pos[k][2] = 0.0f;
    }

    DvzVisualDataUpdate updates[] = {
        {.attr_name = "position", .data = app->spectrum_pos, .item_count = app->n_plot_bins},
        {.attr_name = "color", .data = app->spectrum_color, .item_count = app->n_plot_bins},
        {.attr_name = "stroke_width_px", .data = app->spectrum_width,
            .item_count = app->n_plot_bins},
    };
    return dvz_visual_set_data_many(app->spectrum, updates, 3) == DVZ_OK;
}

static bool upload_scope(Jackoviz* app)
{
    if (app->scope == NULL || app->scope_pos == NULL || app->fft_size == 0u)
        return false;

    const double ms_per_sample =
        1000.0 / (double)(app->sample_rate > 0u ? app->sample_rate : 1u);

    for (uint32_t i = 0; i < app->fft_size; i++)
    {
        float y = 0.0f;
        if (app->scope_have_data && app->scope_wave != NULL)
            y = app->scope_wave[i];
        app->scope_pos[i][0] = (float)((double)i * ms_per_sample);
        app->scope_pos[i][1] = y;
        app->scope_pos[i][2] = 0.0f;
    }

    DvzVisualDataUpdate updates[] = {
        {.attr_name = "position", .data = app->scope_pos, .item_count = app->fft_size},
        {.attr_name = "color", .data = app->scope_color, .item_count = app->fft_size},
        {.attr_name = "stroke_width_px", .data = app->scope_width, .item_count = app->fft_size},
    };
    return dvz_visual_set_data_many(app->scope, updates, 3) == DVZ_OK;
}

static void rebuild_db_ticks(Jackoviz* app)
{
    app->db_tick_count = 0u;
    for (double v = app->db_floor; v <= app->db_ceil + 1e-9 && app->db_tick_count < DB_TICK_MAX;
         v += 10.0)
        app->db_tick_values[app->db_tick_count++] = v;
}

static void cycle_db_floor(Jackoviz* app);
static void cycle_db_ceil(Jackoviz* app);
static void cycle_plot_freq(Jackoviz* app);

static bool apply_db_range(Jackoviz* app)
{
    if (app == NULL)
        return false;

    rebuild_db_ticks(app);

    if (app->db_scale != NULL)
    {
        if (dvz_scale_set_domain(app->db_scale, app->db_floor, app->db_ceil) != DVZ_OK)
            return false;
        if (dvz_scale_set_view_range(app->db_scale, app->db_floor, app->db_ceil) != DVZ_OK)
            return false;
    }
    if (app->colorbar != NULL)
    {
        DvzColorbarTicks ticks = dvz_colorbar_ticks();
        ticks.count = app->db_tick_count;
        ticks.values = app->db_tick_values;
        if (dvz_colorbar_set_ticks(app->colorbar, &ticks) != DVZ_OK)
            return false;
    }
    if (app->panel_1d != NULL)
    {
        if (dvz_panel_set_domain(app->panel_1d, DVZ_DIM_Y, app->db_floor, app->db_ceil) !=
            DVZ_OK)
            return false;
    }

    fprintf(
        stderr, "dB range: %.0f … %.0f dB\n", app->db_floor, app->db_ceil);
    return true;
}

void set_db_floor(Jackoviz* app, double floor) {
    if (app == NULL || floor < -200.0 || floor > -20.0)
        return;
    app->db_floor = floor;
    (void)apply_db_range(app);  
}

static void cycle_db_floor(Jackoviz* app)
{
    if (app == NULL)
        return;
    app->db_floor_index = (app->db_floor_index + 1u) % DB_FLOOR_OPTION_COUNT;
    app->db_floor = DB_FLOOR_OPTIONS[app->db_floor_index];
    (void)apply_db_range(app);
}

void set_db_ceil(Jackoviz* app, double ceil)
{
    if (app == NULL || ceil > 0.0 || ceil < -20.0)
        return;
    app->db_ceil = ceil;
    (void)apply_db_range(app);
}

static void cycle_db_ceil(Jackoviz* app)
{
    if (app == NULL)
        return;
    app->db_ceil_index = (app->db_ceil_index + 1u) % DB_CEIL_OPTION_COUNT;
    app->db_ceil = DB_CEIL_OPTIONS[app->db_ceil_index];
    (void)apply_db_range(app);
}

void set_view_mode(Jackoviz* app, ViewMode mode)
{
    if (app == NULL || app->panel_1d == NULL || app->panel_scope == NULL)
        return;
    /* --fast disables spectrogram views; keys 2/3 are intentional no-ops. */
    if (app->fast && (mode == VIEW_MODE_2D || mode == VIEW_MODE_3D))
        return;
    if ((mode == VIEW_MODE_2D && app->panel_2d == NULL) ||
        (mode == VIEW_MODE_3D && app->panel_3d == NULL))
        return;
    if (app->view_mode == mode)
        return;

    app->view_mode = mode;

    /* Datoviz rejects zero-extent panels; park inactive panels in a tiny corner. */
    DvzPanelDesc full = dvz_panel_desc();
    full.x = 0.0f;
    full.y = 0.0f;
    full.width = 1.0f;
    full.height = 1.0f;
    DvzPanelDesc parked = full;
    parked.width = 0.001f;
    parked.height = 0.001f;

    (void)dvz_panel_set_desc(app->panel_scope, &parked);
    (void)dvz_panel_set_desc(app->panel_1d, &parked);
    if (app->panel_2d != NULL)
        (void)dvz_panel_set_desc(app->panel_2d, &parked);
    if (app->panel_3d != NULL)
        (void)dvz_panel_set_desc(app->panel_3d, &parked);
    if (app->mesh != NULL)
        (void)dvz_visual_set_visible(app->mesh, false);
    if (app->image != NULL)
        (void)dvz_visual_set_visible(app->image, false);
    (void)dvz_visual_set_visible(app->spectrum, false);
    (void)dvz_visual_set_visible(app->scope, false);
    (void)dvz_panel_connect_input(app->panel_scope, NULL);
    (void)dvz_panel_connect_input(app->panel_1d, NULL);
    if (app->panel_2d != NULL)
        (void)dvz_panel_connect_input(app->panel_2d, NULL);
    if (app->panel_3d != NULL)
        (void)dvz_panel_connect_input(app->panel_3d, NULL);

    if (mode == VIEW_MODE_SCOPE)
    {
        (void)dvz_panel_set_desc(app->panel_scope, &full);
        (void)dvz_visual_set_visible(app->scope, true);
        if (app->view != NULL)
            (void)dvz_view_connect_panel(app->view, app->panel_scope);
        fprintf(stderr, "view: oscilloscope (time × amplitude)\n");
    }
    else if (mode == VIEW_MODE_1D)
    {
        (void)dvz_panel_set_desc(app->panel_1d, &full);
        (void)dvz_visual_set_visible(app->spectrum, true);
        if (app->view != NULL)
            (void)dvz_view_connect_panel(app->view, app->panel_1d);
        fprintf(stderr, "view: 1D spectrum (freq × dB)\n");
    }
    else if (mode == VIEW_MODE_2D)
    {
        (void)dvz_panel_set_desc(app->panel_2d, &full);
        (void)dvz_visual_set_visible(app->image, true);
        if (app->view != NULL)
            (void)dvz_view_connect_panel(app->view, app->panel_2d);
        fprintf(stderr, "view: 2D STFT spectrogram\n");
    }
    else
    {
        (void)dvz_panel_set_desc(app->panel_3d, &full);
        (void)dvz_visual_set_visible(app->mesh, true);
        if (app->view != NULL)
            (void)dvz_view_connect_panel(app->view, app->panel_3d);
        fprintf(stderr, "view: 3D surface\n");
    }
}

static void apply_line_width(Jackoviz* app)
{
    if (app == NULL)
        return;
    const float w = app->line_width_px;
    if (app->spectrum_width != NULL)
    {
        for (uint32_t k = 0; k < app->n_plot_bins; k++)
            app->spectrum_width[k] = w;
    }
    if (app->scope_width != NULL)
    {
        for (uint32_t i = 0; i < app->fft_size; i++)
            app->scope_width[i] = w;
    }
}

void set_line_width(Jackoviz* app, float width)
{
    if (app == NULL || width < 1.0f || width > 3.0f)
        return;
    app->line_width_px = width;
    apply_line_width(app);
    fprintf(stderr, "line width: %.0f px\n", (double)app->line_width_px);
}

static void toggle_line_width(Jackoviz* app)
{
    if (app == NULL)
        return;
    app->line_width_px = (app->line_width_px < 1.5f) ? 2.0f : 1.0f;
    apply_line_width(app);
    fprintf(stderr, "line width: %.0f px\n", (double)app->line_width_px);
}

void set_pause(Jackoviz* app, bool paused)
{
    if (app == NULL)
        return;
    app->paused = paused;
    fprintf(stderr, "processing: %s\n", app->paused ? "paused" : "running");
}

static void toggle_pause(Jackoviz* app)
{
    if (app == NULL)
        return;
    app->paused = !app->paused;
    fprintf(stderr, "processing: %s\n", app->paused ? "paused" : "running");
}

static void on_keyboard(DvzInputRouter* router, const DvzKeyboardEvent* event, void* user_data)
{
    (void)router;
    Jackoviz* app = (Jackoviz*)user_data;
    if (app == NULL || event == NULL || event->type != DVZ_KEYBOARD_EVENT_PRESS)
        return;

    if (event->key == DVZ_KEY_0)
        set_view_mode(app, VIEW_MODE_SCOPE);
    else if (event->key == DVZ_KEY_1)
        set_view_mode(app, VIEW_MODE_1D);
    else if (event->key == DVZ_KEY_2)
    {
        if (!app->fast)
            set_view_mode(app, VIEW_MODE_2D);
    }
    else if (event->key == DVZ_KEY_3)
    {
        if (!app->fast)
            set_view_mode(app, VIEW_MODE_3D);
    }
    else if (event->key == DVZ_KEY_F)
        cycle_db_floor(app);
    else if (event->key == DVZ_KEY_C)
        cycle_db_ceil(app);
    else if (event->key == DVZ_KEY_M)
        cycle_plot_freq(app);
    else if (event->key == DVZ_KEY_W)
        toggle_line_width(app);
    else if (event->key == DVZ_KEY_P)
        toggle_pause(app);
}

static void on_timer(DvzAnimation* animation, double t, double dt, uint64_t tick, void* user_data)
{
    (void)animation;
    (void)t;
    (void)dt;
    (void)tick;

    Jackoviz* app = (Jackoviz*)user_data;
    if (!app->running)
        return;

    /* JACK process callback keeps filling audio_rb; freeze FFT + visual uploads. */
    if (!app->paused)
    {
        process_available_audio(app);
        if (!app->fast)
            (void)upload_spectrogram(app);
        (void)upload_spectrum(app);
        (void)upload_scope(app);
    }

    app->frames_drawn++;
    if (app->frame_limit != 0u && app->frames_drawn >= app->frame_limit)
    {
        app->running = false;
        dvz_app_stop(app->app);
    }
}

/*************************************************************************************************/
/*  Setup / teardown                                                                             */
/*************************************************************************************************/

static void usage(const char* prog)
{
    fprintf(
        stderr,
        "Usage: %s [-n 1024|2048|4096|8192] [-f hz] [-b beta] [-c client] [-s jack_source] "
        "[--frames N] [--fast]\n"
        "  -n     FFT size (default %u)\n"
        "  -f     max plot frequency in Hz (locks key m; default %.0f, else key m cycles "
        "8/12/16/20/4 kHz)\n"
        "  -b     Kaiser beta (default %.1f)\n"
        "  -c     JACK client name (default jackoviz)\n"
        "  -s     auto-connect from this JACK port (e.g. system:capture_1)\n"
        "  --frames N  exit after N rendered frames (smoke / CI)\n"
        "  --fast      scope + 1D spectrum only (skip 2D/3D spectrogram uploads)\n"
        "Keys: 0 = oscilloscope, 1 = spectrum XY, 2 = 2D STFT image, 3 = 3D surface, "
        "f = cycle dB floor, c = cycle dB ceiling, "
        "m = cycle max plot frequency (unless -f given), "
        "w = toggle line width (1↔2 px), p = pause/resume processing\n"
        "      (keys 2/3 are disabled with --fast)\n",
        prog, DEFAULT_FFT_SIZE, DEFAULT_PLOT_FREQ_MAX_HZ, DEFAULT_KAISER_BETA);
}

static int parse_args(
    int argc, char** argv, uint32_t* fft_size, double* beta, double* plot_freq_limit,
    const char** client_name, const char** source, uint32_t* frame_limit, bool* fast,
    bool* plot_freq_locked)
{
    *fft_size = DEFAULT_FFT_SIZE;
    *beta = DEFAULT_KAISER_BETA;
    *plot_freq_limit = DEFAULT_PLOT_FREQ_MAX_HZ;
    *client_name = "jackoviz";
    *source = NULL;
    *frame_limit = 0u;
    *fast = false;
    *plot_freq_locked = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
        {
            unsigned long v = strtoul(argv[++i], NULL, 10);
            if (v != 1024ul && v != 2048ul && v != 4096ul && v != 8192ul)
            {
                fprintf(stderr, "FFT size must be 1024, 2048, 4096, or 8192\n");
                return -1;
            }
            *fft_size = (uint32_t)v;
        }
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
        {
            *plot_freq_limit = strtod(argv[++i], NULL);
            if (!(*plot_freq_limit >= 1000.0 && *plot_freq_limit <= 48000.))
            {
                fprintf(stderr, "max plot frequency must be between 1000 and 48000Hz\n");
                return -1;
            }
            *plot_freq_locked = true;
        }
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
        {
            *beta = strtod(argv[++i], NULL);
            if (!(*beta > 0.0 && *beta <= 10.0))
            {
                fprintf(stderr, "Kaiser beta must be above 0.0 and below or equal 8.0\n");
                return -1;
            }
        }
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
        {
            *client_name = argv[++i];
        }
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
        {
            *source = argv[++i];
        }
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
        {
            *frame_limit = (uint32_t)strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--fast") == 0)
        {
            *fast = true;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            usage(argv[0]);
            return 1;
        }
        else
        {
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

static bool setup_jack(Jackoviz* app, const char* client_name, const char* source)
{
    jack_status_t status = 0;
    app->client = jack_client_open(client_name, JackNullOption, &status);
    if (app->client == NULL)
    {
        fprintf(stderr, "jack_client_open failed (status 0x%x)\n", (unsigned)status);
        return false;
    }

    app->sample_rate = jack_get_sample_rate(app->client);
    app->in_port = jack_port_register(
        app->client, "input", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (app->in_port == NULL)
    {
        fprintf(stderr, "jack_port_register failed\n");
        return false;
    }

    app->audio_rb = jvz_jack_ringbuffer_create(AUDIO_RB_BYTES);
    if (app->audio_rb == NULL)
    {
        fprintf(stderr, "jvz_jack_ringbuffer_create failed\n");
        return false;
    }
    jvz_jack_ringbuffer_mlock(app->audio_rb);

    jack_set_process_callback(app->client, jack_process, app);
    jack_on_shutdown(app->client, jack_shutdown, app);

    if (jack_activate(app->client) != 0)
    {
        fprintf(stderr, "jack_activate failed\n");
        return false;
    }

    if (source != NULL)
    {
        const char* dst = jack_port_name(app->in_port);
        if (jack_connect(app->client, source, dst) != 0)
            fprintf(stderr, "warning: could not connect %s → %s\n", source, dst);
        else
            fprintf(stderr, "connected %s → %s\n", source, dst);
    }

    fprintf(
        stderr, "JACK client '%s' @ %u Hz, FFT %u, Kaiser β=%.2f\n", client_name,
        (unsigned)app->sample_rate, app->fft_size, app->kaiser_beta);
    return true;
}

static bool setup_fft(Jackoviz* app)
{
    app->n_bins = app->fft_size / 2u + 1u;
    app->window = (double*)fftw_malloc(sizeof(double) * app->fft_size);
    app->time_buf = (double*)fftw_malloc(sizeof(double) * app->fft_size);
    app->freq_buf = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * app->n_bins);
    app->mag_db = (double*)calloc(app->n_bins, sizeof(double));
    app->scope_wave = (float*)calloc(app->fft_size, sizeof(float));
    app->scope_pos = (vec3*)calloc(app->fft_size, sizeof(vec3));
    app->scope_color = (DvzColor*)calloc(app->fft_size, sizeof(DvzColor));
    app->scope_width = (float*)calloc(app->fft_size, sizeof(float));
    if (app->window == NULL || app->time_buf == NULL || app->freq_buf == NULL ||
        app->mag_db == NULL || app->scope_wave == NULL || app->scope_pos == NULL ||
        app->scope_color == NULL || app->scope_width == NULL)
        return false;

    {
        const DvzColor line_color = dvz_color_rgba(250, 183, 3, 255);
        for (uint32_t i = 0; i < app->fft_size; i++)
        {
            app->scope_color[i] = line_color;
            app->scope_width[i] = app->line_width_px > 0.0f ? app->line_width_px : 2.0f;
            app->scope_pos[i][0] = 0.0f;
            app->scope_pos[i][1] = 0.0f;
            app->scope_pos[i][2] = 0.0f;
        }
    }

    kaiser_window(app->window, app->fft_size, app->kaiser_beta);
    app->plan = fftw_plan_dft_r2c_1d(
        (int)app->fft_size, app->time_buf, app->freq_buf, FFTW_MEASURE);
    if (app->plan == NULL)
        return false;

    app->history = DEFAULT_HISTORY;
    if (spec_ring_create(&app->spec, app->history, app->n_bins) != 0)
    {
        fprintf(stderr, "doubly-mapped spectrogram ringbuffer failed: %s\n", strerror(errno));
        return false;
    }

    return true;
}

/*
 * Number of r2c bins whose centre frequency is ≤ max_hz (inclusive), given fs.
 * bin k ↔ k * fs / fft_size.
 */
static uint32_t bins_up_to_hz(uint32_t fft_size, uint32_t n_bins, jack_nframes_t fs, double max_hz)
{
    if (fs == 0u || fft_size == 0u)
        return n_bins;
    const double nyquist = 0.5 * (double)fs;
    if (max_hz >= nyquist)
        return n_bins;
    const uint32_t last = (uint32_t)floor(max_hz * (double)fft_size / (double)fs);
    uint32_t count = last + 1u;
    if (count < 2u)
        count = 2u;
    if (count > n_bins)
        count = n_bins;
    return count;
}

static bool configure_plot_band(Jackoviz* app)
{
    app->n_plot_bins =
        bins_up_to_hz(app->fft_size, app->n_bins, app->sample_rate, app->plot_freq_limit);

    free(app->heights);
    free(app->colors);
    free(app->field_values);
    free(app->spectrum_pos);
    free(app->spectrum_color);
    free(app->spectrum_width);
    app->heights = NULL;
    app->colors = NULL;
    app->field_values = NULL;
    app->spectrum_pos = (vec3*)calloc(app->n_plot_bins, sizeof(vec3));
    app->spectrum_color = (DvzColor*)calloc(app->n_plot_bins, sizeof(DvzColor));
    app->spectrum_width = (float*)calloc(app->n_plot_bins, sizeof(float));
    if (app->spectrum_pos == NULL || app->spectrum_color == NULL || app->spectrum_width == NULL)
        return false;

    if (!app->fast)
    {
        app->heights =
            (double*)calloc((size_t)app->history * (size_t)app->n_plot_bins, sizeof(double));
        app->colors =
            (DvzColor*)calloc((size_t)app->history * (size_t)app->n_plot_bins, sizeof(DvzColor));
        app->field_values =
            (float*)calloc((size_t)app->history * (size_t)app->n_plot_bins, sizeof(float));
        if (app->heights == NULL || app->colors == NULL || app->field_values == NULL)
            return false;

        viridis_lut_init();
        const DvzColor floor_color = g_viridis_lut[0];
        for (uint32_t i = 0; i < app->history * app->n_plot_bins; i++)
        {
            app->field_values[i] = (float)app->db_floor;
            app->colors[i] = floor_color;
        }
    }

    const DvzColor line_color = dvz_color_rgba(94, 213, 220, 255);
    for (uint32_t k = 0; k < app->n_plot_bins; k++)
    {
        app->spectrum_color[k] = line_color;
        app->spectrum_width[k] = app->line_width_px;
        app->spectrum_pos[k][0] = 0.0f;
        app->spectrum_pos[k][1] = (float)app->db_floor;
        app->spectrum_pos[k][2] = 0.0f;
    }

    app->plot_freq_max =
        (double)(app->n_plot_bins - 1u) * (double)app->sample_rate / (double)app->fft_size;
    fprintf(
        stderr, "plot band: 0 … %.1f Hz (%u of %u bins @ %u Hz)\n", app->plot_freq_max,
        app->n_plot_bins, app->n_bins, (unsigned)app->sample_rate);
    return true;
}

static bool rebuild_surface_geometry(Jackoviz* app)
{
    if (app == NULL || app->mesh == NULL || app->heights == NULL || app->colors == NULL)
        return false;

    if (app->geometry != NULL)
    {
        dvz_geometry_destroy(app->geometry);
        app->geometry = NULL;
    }

    DvzGeometrySurfaceGridDesc desc = dvz_geometry_surface_grid_desc();
    desc.rows = app->history;
    desc.cols = app->n_plot_bins;
    desc.heights = app->heights;
    desc.colors = app->colors;
    desc.origin[0] = -2.6;
    desc.origin[1] = 0.0;
    desc.origin[2] = +1.8;
    desc.col_basis[0] = 5.2 / (double)(app->n_plot_bins > 1u ? app->n_plot_bins - 1u : 1u);
    desc.col_basis[1] = 0.0;
    desc.col_basis[2] = 0.0;
    desc.row_basis[0] = 0.0;
    desc.row_basis[1] = 0.0;
    desc.row_basis[2] = -3.6 / (double)(app->history > 1u ? app->history - 1u : 1u);
    desc.height_axis[0] = 0.0;
    desc.height_axis[1] = 1.0;
    desc.height_axis[2] = 0.0;
    desc.height_scale = 1.35;

    app->geometry = dvz_geometry_surface_grid(&desc);
    if (app->geometry == NULL)
        return false;
    return dvz_mesh_set_geometry(app->mesh, app->geometry) == DVZ_OK;
}

static bool apply_plot_freq_limit(Jackoviz* app)
{
    if (app == NULL)
        return false;
    if (!configure_plot_band(app))
        return false;

    if (app->panel_1d != NULL)
    {
        if (dvz_panel_set_domain(app->panel_1d, DVZ_DIM_X, 0.0, app->plot_freq_max) != DVZ_OK)
            return false;
    }
    if (app->spectrum != NULL)
    {
        const uint32_t subpath_len = app->n_plot_bins;
        DvzVisualDataUpdate updates[] = {
            {.attr_name = "position", .data = app->spectrum_pos, .item_count = app->n_plot_bins},
            {.attr_name = "color", .data = app->spectrum_color, .item_count = app->n_plot_bins},
            {.attr_name = "stroke_width_px", .data = app->spectrum_width,
                .item_count = app->n_plot_bins},
        };
        if (dvz_visual_set_data_many(app->spectrum, updates, 3) != DVZ_OK)
            return false;
        if (dvz_path_set_subpaths(app->spectrum, 1, &subpath_len) != DVZ_OK)
            return false;
    }

    if (!app->fast)
    {
        const double x_max = (double)app->history;
        const double y_max = app->plot_freq_max;
        if (app->panel_2d != NULL)
        {
            if (dvz_panel_set_domain(app->panel_2d, DVZ_DIM_Y, 0.0, y_max) != DVZ_OK)
                return false;
        }
        if (app->image != NULL)
        {
            vec3 positions[4] = {
                {0.0f, 0.0f, 0.0f},
                {0.0f, (float)y_max, 0.0f},
                {(float)x_max, 0.0f, 0.0f},
                {(float)x_max, (float)y_max, 0.0f},
            };
            if (dvz_visual_set_data(app->image, "position", positions, 4) != DVZ_OK)
                return false;
        }
        if (app->field != NULL && app->field_values != NULL)
        {
            DvzFieldDataView view = dvz_field_data_view();
            view.data = app->field_values;
            view.bytes_per_row = (uint64_t)app->history * sizeof(float);
            view.rows_per_image = app->n_plot_bins;
            if (dvz_sampled_field_resize(
                    app->field, app->history, app->n_plot_bins, 1u, &view) != DVZ_OK)
                return false;
        }
        if (app->mesh != NULL && !rebuild_surface_geometry(app))
            return false;
    }

    return true;
}

void set_plot_freq(Jackoviz* app, double freq) {
    app->plot_freq_limit = freq;
    if (!apply_plot_freq_limit(app))
    {
        fprintf(stderr, "setting max plot frequency failed\n");
        return;
    }
    fprintf(
        stderr, "max plot frequency: %.0f Hz (0 … %.1f Hz, %u bins)\n", app->plot_freq_limit,
        app->plot_freq_max, app->n_plot_bins);
}

static void cycle_plot_freq(Jackoviz* app)
{
    if (app == NULL || app->plot_freq_locked)
        return;

    app->plot_freq_index = (app->plot_freq_index + 1u) % PLOT_FREQ_OPTION_COUNT;
    app->plot_freq_limit = PLOT_FREQ_OPTIONS[app->plot_freq_index];
    if (!apply_plot_freq_limit(app))
    {
        fprintf(stderr, "max plot frequency cycle failed\n");
        return;
    }
    fprintf(
        stderr, "max plot frequency: %.0f Hz (0 … %.1f Hz, %u bins)\n", app->plot_freq_limit,
        app->plot_freq_max, app->n_plot_bins);
}

void set_kaiser_beta(Jackoviz* app, double beta)
{
    if (app == NULL || app->window == NULL || beta < 1.0 || beta > 10.0)
        return;
    app->kaiser_beta = beta;
    kaiser_window(app->window, app->fft_size, app->kaiser_beta);
}

static bool setup_datoviz(Jackoviz* app)
{
    app->scene = dvz_scene();
    if (app->scene == NULL)
        return false;

    app->figure = dvz_figure(app->scene, WINDOW_WIDTH, WINDOW_HEIGHT, 0);
    if (app->figure == NULL)
        return false;

    /* Datoviz rejects zero-extent panels; park inactive panels in a tiny corner. */
    DvzPanelDesc parked_desc = dvz_panel_desc();
    parked_desc.x = 0.0f;
    parked_desc.y = 0.0f;
    parked_desc.width = 0.001f;
    parked_desc.height = 0.001f;

    if (app->fast)
    {
        /* Default view is 1D spectrum; scope starts parked. No 2D/3D panels. */
        app->panel_1d = dvz_panel_full(app->figure);
        app->panel_scope = dvz_panel(app->figure, &parked_desc);
        if (app->panel_1d == NULL || app->panel_scope == NULL)
            return false;
    }
    else
    {
        app->panel_3d = dvz_panel_full(app->figure);
        if (app->panel_3d == NULL)
            return false;
        /* 2D / 1D / scope panels start parked; shown on keys 2 / 1 / 0. */
        app->panel_2d = dvz_panel(app->figure, &parked_desc);
        app->panel_1d = dvz_panel(app->figure, &parked_desc);
        app->panel_scope = dvz_panel(app->figure, &parked_desc);
        if (app->panel_2d == NULL || app->panel_1d == NULL || app->panel_scope == NULL)
            return false;
    }

    DvzColor bg = dvz_color_rgba(12, 14, 20, 255);
    if (app->panel_3d != NULL && dvz_panel_set_background_color(app->panel_3d, bg) != DVZ_OK)
        return false;
    if (app->panel_2d != NULL && dvz_panel_set_background_color(app->panel_2d, bg) != DVZ_OK)
        return false;
    if (dvz_panel_set_background_color(app->panel_1d, bg) != DVZ_OK)
        return false;
    if (dvz_panel_set_background_color(app->panel_scope, bg) != DVZ_OK)
        return false;

    DvzPanelBorderDesc border = dvz_panel_border_desc();
    border.color = dvz_color_rgba(160, 170, 190, 220);
    border.width_px = 1.5f;
    border.inset_px = 0.0f;

    if (!app->fast)
    {
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
        if (dvz_panel_set_camera_desc(app->panel_3d, &camera) != DVZ_OK)
            return false;

        /* ---- 3D surface ---- */
        DvzGeometrySurfaceGridDesc desc = dvz_geometry_surface_grid_desc();
        desc.rows = app->history;
        desc.cols = app->n_plot_bins;
        desc.heights = app->heights;
        desc.colors = app->colors;
        desc.origin[0] = -2.6;
        desc.origin[1] = 0.0;
        desc.origin[2] = +1.8;
        desc.col_basis[0] = 5.2 / (double)(app->n_plot_bins > 1u ? app->n_plot_bins - 1u : 1u);
        desc.col_basis[1] = 0.0;
        desc.col_basis[2] = 0.0;
        desc.row_basis[0] = 0.0;
        desc.row_basis[1] = 0.0;
        desc.row_basis[2] = -3.6 / (double)(app->history > 1u ? app->history - 1u : 1u);
        desc.height_axis[0] = 0.0;
        desc.height_axis[1] = 1.0;
        desc.height_axis[2] = 0.0;
        desc.height_scale = 1.35;

        app->geometry = dvz_geometry_surface_grid(&desc);
        if (app->geometry == NULL)
            return false;

        app->mesh = dvz_mesh(app->scene, 0);
        if (app->mesh == NULL)
            return false;

        DvzMaterialDesc material = dvz_phong_material_desc();
        material.light_direction[0] = -1.0f;
        material.light_direction[1] = 1.0f;
        material.light_direction[2] = 1.0f;
        material.phong.ambient = 0.28f;
        material.phong.diffuse = 0.72f;
        material.phong.specular = 0.42f;
        material.phong.shininess = 48.0f;
        if (dvz_visual_set_material(app->mesh, &material) != DVZ_OK)
            return false;
        if (dvz_mesh_set_geometry(app->mesh, app->geometry) != DVZ_OK)
            return false;
        if (dvz_panel_add_visual(app->panel_3d, app->mesh, NULL) != DVZ_OK)
            return false;

        DvzController* arcball = dvz_arcball(app->scene, NULL);
        if (arcball == NULL ||
            dvz_panel_bind_controller(app->panel_3d, arcball, DVZ_DIM_MASK_XYZ) != DVZ_OK)
            return false;

        /* ---- 2D STFT image (time × frequency) with axes, grid, colorbar ---- */
        const double x_max = (double)app->history;
        const double y_max = app->plot_freq_max;
        if (dvz_panel_set_domain(app->panel_2d, DVZ_DIM_X, 0.0, x_max) != DVZ_OK)
            return false;
        if (dvz_panel_set_domain(app->panel_2d, DVZ_DIM_Y, 0.0, y_max) != DVZ_OK)
            return false;
        if (dvz_panel_set_border(app->panel_2d, &border) != DVZ_OK)
            return false;

        DvzScaleDesc scale_desc = dvz_scale_desc();
        scale_desc.kind = DVZ_SCALE_CONTINUOUS;
        scale_desc.label = "level";
        scale_desc.unit = "dB";
        app->db_scale = dvz_scale(app->scene, &scale_desc);
        if (app->db_scale == NULL)
            return false;
        if (dvz_scale_set_domain(app->db_scale, app->db_floor, app->db_ceil) != DVZ_OK)
            return false;
        if (dvz_scale_set_view_range(app->db_scale, app->db_floor, app->db_ceil) != DVZ_OK)
            return false;
        if (dvz_scale_set_format(
                app->db_scale,
                &(DvzFormatDesc){DVZ_STRUCT_INIT_FIELDS(DvzFormatDesc), .precision = 0,
                    .trim_trailing_zeros = true}) != DVZ_OK)
            return false;
        DvzColormap* cmap = dvz_colormap_builtin(app->scene, DVZ_BUILTIN_COLORMAP_VIRIDIS);
        if (cmap == NULL || dvz_scale_set_colormap(app->db_scale, cmap) != DVZ_OK)
            return false;

        /* Data-space quad matching panel domain: X = time, Y = frequency. */
        vec3 positions[4] = {
            {0.0f, 0.0f, 0.0f},
            {0.0f, (float)y_max, 0.0f},
            {(float)x_max, 0.0f, 0.0f},
            {(float)x_max, (float)y_max, 0.0f},
        };
        vec2 texcoords[4] = {
            {0.0f, 0.0f},
            {0.0f, 1.0f},
            {1.0f, 0.0f},
            {1.0f, 1.0f},
        };

        app->image = dvz_image(app->scene, 0);
        if (app->image == NULL)
            return false;
        if (dvz_visual_set_data(app->image, "position", positions, 4) != DVZ_OK)
            return false;
        if (dvz_visual_set_data(app->image, "texcoords", texcoords, 4) != DVZ_OK)
            return false;
        if (dvz_visual_set_scale(app->image, "color", app->db_scale) != DVZ_OK)
            return false;
        if (dvz_image_set_sampling(app->image, DVZ_IMAGE_SAMPLING_NEAREST) != DVZ_OK)
            return false;
        if (dvz_visual_set_depth_test(app->image, false) != DVZ_OK)
            return false;
        if (dvz_visual_set_visible(app->image, false) != DVZ_OK)
            return false;

        DvzSampledFieldDesc field_desc = dvz_sampled_field_desc();
        field_desc.dim = DVZ_FIELD_DIM_2D;
        field_desc.format = DVZ_FIELD_FORMAT_R32_FLOAT;
        field_desc.semantic = DVZ_FIELD_SEMANTIC_SCALAR;
        field_desc.color_role = DVZ_COLOR_ROLE_DATA;
        field_desc.width = app->history;
        field_desc.height = app->n_plot_bins;
        field_desc.depth = 1;
        app->field = dvz_sampled_field(app->scene, &field_desc);
        if (app->field == NULL)
            return false;

        DvzFieldDataView field_view = dvz_field_data_view();
        field_view.data = app->field_values;
        field_view.bytes_per_row = (uint64_t)app->history * sizeof(float);
        field_view.rows_per_image = app->n_plot_bins;
        if (dvz_sampled_field_set_data(app->field, &field_view) != DVZ_OK)
            return false;
        if (dvz_visual_set_field(app->image, "field", app->field) != DVZ_OK)
            return false;
        if (dvz_panel_add_visual(app->panel_2d, app->image, NULL) != DVZ_OK)
            return false;

        DvzPanelAxes2DDesc axes = dvz_panel_axes_2d_desc();
        axes.x_label = "time (frames)";
        axes.y_label = "frequency (Hz)";
        if (dvz_panel_set_axes_2d(app->panel_2d, &axes) != DVZ_OK)
            return false;

        DvzAxis* x_axis = dvz_panel_axis(app->panel_2d, DVZ_DIM_X);
        DvzAxis* y_axis = dvz_panel_axis(app->panel_2d, DVZ_DIM_Y);
        if (x_axis == NULL || y_axis == NULL)
            return false;
        if (dvz_axis_set_grid(x_axis, true) != DVZ_OK || dvz_axis_set_grid(y_axis, true) != DVZ_OK)
            return false;

        /* Viridis colorbar with explicit dB ticks every 10 dB. */
        rebuild_db_ticks(app);
        app->colorbar = dvz_colorbar(
            app->panel_2d, app->db_scale,
            &(DvzColorbarDesc){DVZ_STRUCT_INIT_FIELDS(DvzColorbarDesc),
                .orientation = DVZ_COLORBAR_ORIENTATION_VERTICAL,
                .anchor = DVZ_SCENE_ANCHOR_PANEL_RIGHT,
                .title = "dB",
                .reserve_px = 96.0f,
                .ramp_width_px = 18.0f,
                .plot_gap_px = 12.0f,
                .tick_length_px = 5.0f,
                .label_gap_px = 6.0f,
            });
        if (app->colorbar == NULL)
            return false;
        if (dvz_colorbar_set_format(
                app->colorbar,
                &(DvzFormatDesc){DVZ_STRUCT_INIT_FIELDS(DvzFormatDesc), .precision = 0,
                    .trim_trailing_zeros = true}) != DVZ_OK)
            return false;
        DvzColorbarTicks cb_ticks = dvz_colorbar_ticks();
        cb_ticks.count = app->db_tick_count;
        cb_ticks.values = app->db_tick_values;
        if (dvz_colorbar_set_ticks(app->colorbar, &cb_ticks) != DVZ_OK)
            return false;

        DvzController* panzoom = dvz_panzoom(app->scene, NULL);
        if (panzoom == NULL ||
            dvz_panel_bind_controller(app->panel_2d, panzoom, DVZ_DIM_MASK_XY) != DVZ_OK)
            return false;
    }

    /* ---- 1D spectrum XY plot (frequency × dB) ---- */
    if (dvz_panel_set_domain(app->panel_1d, DVZ_DIM_X, 0.0, app->plot_freq_max) != DVZ_OK)
        return false;
    if (dvz_panel_set_domain(app->panel_1d, DVZ_DIM_Y, app->db_floor, app->db_ceil) != DVZ_OK)
        return false;
    if (dvz_panel_set_border(app->panel_1d, &border) != DVZ_OK)
        return false;

    DvzPanelAxes2DDesc axes_1d = dvz_panel_axes_2d_desc();
    axes_1d.x_label = "frequency (Hz)";
    axes_1d.y_label = "magnitude (dB)";
    if (dvz_panel_set_axes_2d(app->panel_1d, &axes_1d) != DVZ_OK)
        return false;
    DvzAxis* x_axis_1d = dvz_panel_axis(app->panel_1d, DVZ_DIM_X);
    DvzAxis* y_axis_1d = dvz_panel_axis(app->panel_1d, DVZ_DIM_Y);
    if (x_axis_1d == NULL || y_axis_1d == NULL)
        return false;
    if (dvz_axis_set_grid(x_axis_1d, true) != DVZ_OK ||
        dvz_axis_set_grid(y_axis_1d, true) != DVZ_OK)
        return false;

    app->spectrum = dvz_path(app->scene, 0);
    if (app->spectrum == NULL)
        return false;
    {
        const uint32_t subpath_len = app->n_plot_bins;
        DvzVisualDataUpdate updates[] = {
            {.attr_name = "position", .data = app->spectrum_pos, .item_count = app->n_plot_bins},
            {.attr_name = "color", .data = app->spectrum_color, .item_count = app->n_plot_bins},
            {.attr_name = "stroke_width_px", .data = app->spectrum_width,
                .item_count = app->n_plot_bins},
        };
        if (dvz_visual_set_data_many(app->spectrum, updates, 3) != DVZ_OK)
            return false;
        if (dvz_path_set_subpaths(app->spectrum, 1, &subpath_len) != DVZ_OK)
            return false;
    }
    if (dvz_path_set_caps(app->spectrum, DVZ_SEGMENT_CAP_ROUND, DVZ_SEGMENT_CAP_ROUND) != DVZ_OK)
        return false;
    if (dvz_path_set_join(app->spectrum, DVZ_PATH_JOIN_ROUND, 4.0f) != DVZ_OK)
        return false;
    if (dvz_visual_set_depth_test(app->spectrum, false) != DVZ_OK)
        return false;
    if (dvz_visual_set_visible(app->spectrum, app->fast) != DVZ_OK)
        return false;
    if (dvz_panel_add_visual(app->panel_1d, app->spectrum, NULL) != DVZ_OK)
        return false;

    DvzController* panzoom_1d = dvz_panzoom(app->scene, NULL);
    if (panzoom_1d == NULL ||
        dvz_panel_bind_controller(app->panel_1d, panzoom_1d, DVZ_DIM_MASK_XY) != DVZ_OK)
        return false;

    /* ---- Oscilloscope (time × amplitude) ---- */
    const double scope_ms =
        1000.0 * (double)app->fft_size / (double)(app->sample_rate > 0u ? app->sample_rate : 1u);
    if (dvz_panel_set_domain(app->panel_scope, DVZ_DIM_X, 0.0, scope_ms) != DVZ_OK)
        return false;
    if (dvz_panel_set_domain(app->panel_scope, DVZ_DIM_Y, -1.0, 1.0) != DVZ_OK)
        return false;
    if (dvz_panel_set_border(app->panel_scope, &border) != DVZ_OK)
        return false;

    DvzPanelAxes2DDesc axes_scope = dvz_panel_axes_2d_desc();
    axes_scope.x_label = "time (ms)";
    axes_scope.y_label = "amplitude";
    if (dvz_panel_set_axes_2d(app->panel_scope, &axes_scope) != DVZ_OK)
        return false;
    DvzAxis* x_axis_scope = dvz_panel_axis(app->panel_scope, DVZ_DIM_X);
    DvzAxis* y_axis_scope = dvz_panel_axis(app->panel_scope, DVZ_DIM_Y);
    if (x_axis_scope == NULL || y_axis_scope == NULL)
        return false;
    if (dvz_axis_set_grid(x_axis_scope, true) != DVZ_OK ||
        dvz_axis_set_grid(y_axis_scope, true) != DVZ_OK)
        return false;

    app->scope = dvz_path(app->scene, 0);
    if (app->scope == NULL)
        return false;
    {
        const uint32_t subpath_len = app->fft_size;
        DvzVisualDataUpdate updates[] = {
            {.attr_name = "position", .data = app->scope_pos, .item_count = app->fft_size},
            {.attr_name = "color", .data = app->scope_color, .item_count = app->fft_size},
            {.attr_name = "stroke_width_px", .data = app->scope_width, .item_count = app->fft_size},
        };
        if (dvz_visual_set_data_many(app->scope, updates, 3) != DVZ_OK)
            return false;
        if (dvz_path_set_subpaths(app->scope, 1, &subpath_len) != DVZ_OK)
            return false;
    }
    if (dvz_path_set_caps(app->scope, DVZ_SEGMENT_CAP_ROUND, DVZ_SEGMENT_CAP_ROUND) != DVZ_OK)
        return false;
    if (dvz_path_set_join(app->scope, DVZ_PATH_JOIN_ROUND, 4.0f) != DVZ_OK)
        return false;
    if (dvz_visual_set_depth_test(app->scope, false) != DVZ_OK)
        return false;
    if (dvz_visual_set_visible(app->scope, false) != DVZ_OK)
        return false;
    if (dvz_panel_add_visual(app->panel_scope, app->scope, NULL) != DVZ_OK)
        return false;

    DvzController* panzoom_scope = dvz_panzoom(app->scene, NULL);
    if (panzoom_scope == NULL ||
        dvz_panel_bind_controller(app->panel_scope, panzoom_scope, DVZ_DIM_MASK_XY) != DVZ_OK)
        return false;

    DvzAnimTimerDesc timer_desc = dvz_anim_timer_desc();
    timer_desc.mode = DVZ_TIMER_EVERY_FRAME;
    timer_desc.callback = on_timer;
    timer_desc.user_data = app;
    app->timer = dvz_anim_timer(app->scene, &timer_desc);
    if (app->timer == NULL)
        return false;
    if (dvz_anim_start(app->timer, 0.0) != DVZ_OK)
        return false;

    DvzAppConfig cfg = dvz_app_config();
    cfg.schedule_mode = DVZ_APP_SCHEDULE_CONTINUOUS;
    cfg.fps_cap = 60.0;
    app->app = dvz_app_with_config(app->scene, &cfg);
    if (app->app == NULL)
        return false;

    app->view = dvz_view_window(app->app, app->figure, WINDOW_WIDTH, WINDOW_HEIGHT, "jackoviz");
    if (app->view == NULL)
        return false;

    app->view_mode = app->fast ? VIEW_MODE_1D : VIEW_MODE_3D;
    (void)dvz_panel_connect_input(app->panel_scope, NULL);
    (void)dvz_panel_connect_input(app->panel_1d, NULL);
    if (app->panel_2d != NULL)
        (void)dvz_panel_connect_input(app->panel_2d, NULL);
    if (app->fast && app->view != NULL)
        (void)dvz_view_connect_panel(app->view, app->panel_1d);

    DvzInputRouter* input = dvz_view_input(app->view);
    if (input == NULL ||
        dvz_input_subscribe_keyboard(input, on_keyboard, app) == DVZ_CALLBACK_ID_NONE)
        return false;

    return true;
}

static void teardown(Jackoviz* app)
{
    if (app->client != NULL)
    {
        jack_deactivate(app->client);
        jack_client_close(app->client);
        app->client = NULL;
    }
    if (app->audio_rb != NULL)
    {
        jvz_jack_ringbuffer_free(app->audio_rb);
        app->audio_rb = NULL;
    }

    if (app->app != NULL)
    {
        dvz_app_destroy(app->app);
        app->app = NULL;
    }
    if (app->geometry != NULL)
    {
        dvz_geometry_destroy(app->geometry);
        app->geometry = NULL;
    }
    if (app->scene != NULL)
    {
        dvz_scene_destroy(app->scene);
        app->scene = NULL;
    }

    if (app->plan != NULL)
        fftw_destroy_plan(app->plan);
    fftw_free(app->window);
    fftw_free(app->time_buf);
    fftw_free(app->freq_buf);
    free(app->mag_db);
    free(app->heights);
    free(app->colors);
    free(app->field_values);
    free(app->spectrum_pos);
    free(app->spectrum_color);
    free(app->spectrum_width);
    free(app->scope_wave);
    free(app->scope_pos);
    free(app->scope_color);
    free(app->scope_width);
    spec_ring_destroy(&app->spec);
}

/*************************************************************************************************/
/*  main                                                                                         */
/*************************************************************************************************/

int main(int argc, char** argv)
{
    uint32_t fft_size = DEFAULT_FFT_SIZE;
    double beta = DEFAULT_KAISER_BETA;
    double plot_freq_limit = DEFAULT_PLOT_FREQ_MAX_HZ;
    const char* client_name = "jackoviz";
    const char* source = NULL;
    uint32_t frame_limit = 0u;
    bool fast = false;
    bool plot_freq_locked = false;

    const int parg = parse_args(
        argc, argv, &fft_size, &beta, &plot_freq_limit, &client_name, &source, &frame_limit,
        &fast, &plot_freq_locked);
    if (parg != 0)
        return parg < 0 ? EXIT_FAILURE : EXIT_SUCCESS;

    Jackoviz app;
    memset(&app, 0, sizeof(app));
    app.fft_size = fft_size;
    app.kaiser_beta = beta;
    app.plot_freq_limit = plot_freq_limit;
    app.plot_freq_locked = plot_freq_locked;
    app.plot_freq_index = 0u;
    if (!plot_freq_locked)
    {
        for (uint32_t i = 0; i < PLOT_FREQ_OPTION_COUNT; i++)
        {
            if (PLOT_FREQ_OPTIONS[i] == plot_freq_limit)
            {
                app.plot_freq_index = i;
                break;
            }
        }
    }
    app.db_floor = DB_FLOOR_DEFAULT;
    app.db_floor_index = 0u;
    app.db_ceil = DB_CEIL_DEFAULT;
    app.db_ceil_index = 2u; /* -20 dB in DB_CEIL_OPTIONS */
    app.frame_limit = frame_limit;
    app.fast = fast;
    app.line_width_px = 2.0f;
    app.running = true;

    int rc = EXIT_FAILURE;
    if (!setup_fft(&app))
    {
        fprintf(stderr, "FFT / spectrogram setup failed\n");
        goto done;
    }
    if (!setup_jack(&app, client_name, source))
    {
        fprintf(stderr, "JACK setup failed (is jackd running?)\n");
        goto done;
    }
    if (!configure_plot_band(&app))
    {
        fprintf(stderr, "plot band setup failed\n");
        goto done;
    }
    if (!setup_datoviz(&app))
    {
        fprintf(stderr, "Datoviz setup failed\n");
        goto done;
    }

    if (app.fast)
    {
        fprintf(
            stderr,
            "Fast mode: scope + 1D spectrum only (0–%.0f Hz, %u bins). "
            "Keys: 0=scope, 1=spectrum, f=dB floor, c=dB ceiling, m=plot Hz, w=line width, "
            "p=pause (2/3 disabled). Close window to quit.\n",
            app.plot_freq_max, app.n_plot_bins);
    }
    else
    {
        fprintf(
            stderr,
            "Spectrogram %u × %u (time × bins, 0–%.0f Hz). "
            "Keys: 0=scope, 1=spectrum, 2=2D, 3=3D, f=dB floor, c=dB ceiling, "
            "m=plot Hz, w=line width, p=pause. Close window to quit.\n",
            app.history, app.n_plot_bins, app.plot_freq_max);
    }

#if defined(JVZ_HAS_GRPC)
    jvz_grpc_serve_async(&app, NULL);
#endif

    dvz_app_run(app.app, frame_limit == 0u ? 0u : frame_limit);
    rc = EXIT_SUCCESS;

done:
    teardown(&app);
    return rc;
}
