/*
 * jackoviz.c — realtime mono JACK capture → Kaiser-windowed FFTW3 → Datoviz spectrogram
 *
 * POSIX (macOS / Linux). Build with the accompanying Makefile.
 *
 * Usage:
 *   ./jackoviz [-n 2048|8192] [-b beta] [-c client] [-s source] [--frames N]
 *
 * Connect a mono source to "jackoviz:input", or pass -s system:capture_1.
 * Keys: 2 = 2D STFT image, 3 = 3D surface.
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
#include <jack/ringbuffer.h>

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

#define DEFAULT_FFT_SIZE     2048u
#define DEFAULT_KAISER_BETA  4.5
#define DEFAULT_HISTORY      128u /* spectrogram time columns (power of two) */
#define AUDIO_RB_BYTES       (1u << 18) /* 256 KiB of float samples */
#define WINDOW_WIDTH         1280u
#define WINDOW_HEIGHT        720u
#define DB_FLOOR             (-100.0)
#define DB_CEIL              (-20.0)
#define PLOT_FREQ_MAX_HZ     8000.0

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

typedef enum
{
    VIEW_MODE_3D = 0,
    VIEW_MODE_2D = 1,
} ViewMode;

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
    jack_ringbuffer_t* audio_rb;
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
    uint32_t n_plot_bins; /* bins kept: 0 … PLOT_FREQ_MAX_HZ */
    double plot_freq_max; /* Hz covered by n_plot_bins */
    double* heights;      /* 3D: row-major time × freq, normalized [0,1] */
    DvzColor* colors;     /* 3D: viridis colors matching heights */
    float* field_values;  /* 2D: row-major freq × time, dB */

    /* Datoviz */
    DvzScene* scene;
    DvzFigure* figure;
    DvzPanel* panel_3d;
    DvzPanel* panel_2d;
    DvzVisual* mesh;
    DvzGeometry* geometry;
    DvzVisual* image;
    DvzSampledField* field;
    DvzApp* app;
    DvzView* view;
    DvzAnimation* timer;
    ViewMode view_mode;

    uint32_t frame_limit;
    uint32_t frames_drawn;
    bool running;
} Jackoviz;

static int jack_process(jack_nframes_t nframes, void* arg)
{
    Jackoviz* app = (Jackoviz*)arg;
    float* in = (float*)jack_port_get_buffer(app->in_port, nframes);
    if (in == NULL)
        return 0;

    const size_t bytes = (size_t)nframes * sizeof(float);
    if (jack_ringbuffer_write_space(app->audio_rb) >= bytes)
        (void)jack_ringbuffer_write(app->audio_rb, (const char*)in, bytes);
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
        if (db < DB_FLOOR)
            db = DB_FLOOR;
        if (db > DB_CEIL)
            db = DB_CEIL;
        mag_db[k] = db;
    }
}

static void process_available_audio(Jackoviz* app)
{
    const size_t frame_bytes = (size_t)app->fft_size * sizeof(float);
    float scratch[8192]; /* max supported FFT size */

    if (app->fft_size > 8192u)
        return;

    while (jack_ringbuffer_read_space(app->audio_rb) >= frame_bytes)
    {
        if (jack_ringbuffer_read(app->audio_rb, (char*)scratch, frame_bytes) != frame_bytes)
            break;

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

    memset(app->heights, 0, (size_t)surface_count * sizeof(double));
    for (size_t i = 0; i < (size_t)surface_count; i++)
        app->field_values[i] = (float)DB_FLOOR;
    for (uint32_t i = 0; i < surface_count; i++)
        (void)dvz_colormap_builtin_sample(
            DVZ_BUILTIN_COLORMAP_VIRIDIS, 0.0, &app->colors[i]);

    if (available == 0u)
        return;

    const double* hist = spec_ring_history(&app->spec, available);
    if (hist == NULL)
        return;

    /* Newest spectrum at high time index for the 3D surface (far edge).
     * 2D columns are mirrored so on-screen time increases to the right. */
    const uint32_t t0 = n_time - available;
    const double db_span = DB_CEIL - DB_FLOOR;
    for (uint32_t t = 0; t < available; t++)
    {
        const double* src = hist + (size_t)t * (size_t)app->spec.stride;
        const uint32_t col_2d = n_time - 1u - (t0 + t);
        double* hdst = app->heights + (size_t)(t0 + t) * (size_t)n_freq;
        DvzColor* coldst = app->colors + (size_t)(t0 + t) * (size_t)n_freq;
        for (uint32_t f = 0; f < n_freq; f++)
        {
            const double db = src[f];
            double v = (db - DB_FLOOR) / db_span;
            if (v < 0.0)
                v = 0.0;
            if (v > 1.0)
                v = 1.0;
            hdst[f] = v;
            (void)dvz_colormap_builtin_sample(DVZ_BUILTIN_COLORMAP_VIRIDIS, v, &coldst[f]);
            coldst[f].a = 255;
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

static void set_view_mode(Jackoviz* app, ViewMode mode)
{
    if (app == NULL || app->panel_3d == NULL || app->panel_2d == NULL)
        return;
    if (app->view_mode == mode)
        return;

    app->view_mode = mode;

    /* Datoviz rejects zero-extent panels; park the inactive one in a tiny corner. */
    DvzPanelDesc full = dvz_panel_desc();
    full.x = 0.0f;
    full.y = 0.0f;
    full.width = 1.0f;
    full.height = 1.0f;
    DvzPanelDesc parked = full;
    parked.width = 0.001f;
    parked.height = 0.001f;

    if (mode == VIEW_MODE_2D)
    {
        (void)dvz_panel_set_desc(app->panel_2d, &full);
        (void)dvz_panel_set_desc(app->panel_3d, &parked);
        (void)dvz_visual_set_visible(app->mesh, false);
        (void)dvz_visual_set_visible(app->image, true);
        (void)dvz_panel_connect_input(app->panel_3d, NULL);
        if (app->view != NULL)
            (void)dvz_view_connect_panel(app->view, app->panel_2d);
        fprintf(stderr, "view: 2D STFT spectrogram\n");
    }
    else
    {
        (void)dvz_panel_set_desc(app->panel_3d, &full);
        (void)dvz_panel_set_desc(app->panel_2d, &parked);
        (void)dvz_visual_set_visible(app->mesh, true);
        (void)dvz_visual_set_visible(app->image, false);
        (void)dvz_panel_connect_input(app->panel_2d, NULL);
        if (app->view != NULL)
            (void)dvz_view_connect_panel(app->view, app->panel_3d);
        fprintf(stderr, "view: 3D surface\n");
    }
}

static void on_keyboard(DvzInputRouter* router, const DvzKeyboardEvent* event, void* user_data)
{
    (void)router;
    Jackoviz* app = (Jackoviz*)user_data;
    if (app == NULL || event == NULL || event->type != DVZ_KEYBOARD_EVENT_PRESS)
        return;

    if (event->key == DVZ_KEY_2)
        set_view_mode(app, VIEW_MODE_2D);
    else if (event->key == DVZ_KEY_3)
        set_view_mode(app, VIEW_MODE_3D);
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

    process_available_audio(app);
    (void)upload_spectrogram(app);

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
        "Usage: %s [-n 2048|8192] [-b beta] [-c client] [-s jack_source] [--frames N]\n"
        "  -n     FFT size (default %u)\n"
        "  -b     Kaiser beta (default %.1f)\n"
        "  -c     JACK client name (default jackoviz)\n"
        "  -s     auto-connect from this JACK port (e.g. system:capture_1)\n"
        "  --frames N  exit after N rendered frames (smoke / CI)\n"
        "Keys: 2 = 2D STFT image, 3 = 3D surface\n",
        prog, DEFAULT_FFT_SIZE, DEFAULT_KAISER_BETA);
}

static int parse_args(
    int argc, char** argv, uint32_t* fft_size, double* beta, const char** client_name,
    const char** source, uint32_t* frame_limit)
{
    *fft_size = DEFAULT_FFT_SIZE;
    *beta = DEFAULT_KAISER_BETA;
    *client_name = "jackoviz";
    *source = NULL;
    *frame_limit = 0u;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
        {
            unsigned long v = strtoul(argv[++i], NULL, 10);
            if (v != 2048ul && v != 8192ul)
            {
                fprintf(stderr, "FFT size must be 2048 or 8192\n");
                return -1;
            }
            *fft_size = (uint32_t)v;
        }
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
        {
            *beta = strtod(argv[++i], NULL);
            if (!(*beta > 0.0))
            {
                fprintf(stderr, "Kaiser beta must be > 0\n");
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

    app->audio_rb = jack_ringbuffer_create(AUDIO_RB_BYTES);
    if (app->audio_rb == NULL)
    {
        fprintf(stderr, "jack_ringbuffer_create failed\n");
        return false;
    }
    jack_ringbuffer_mlock(app->audio_rb);

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
    if (app->window == NULL || app->time_buf == NULL || app->freq_buf == NULL ||
        app->mag_db == NULL)
        return false;

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
        bins_up_to_hz(app->fft_size, app->n_bins, app->sample_rate, PLOT_FREQ_MAX_HZ);

    free(app->heights);
    free(app->colors);
    free(app->field_values);
    app->heights =
        (double*)calloc((size_t)app->history * (size_t)app->n_plot_bins, sizeof(double));
    app->colors =
        (DvzColor*)calloc((size_t)app->history * (size_t)app->n_plot_bins, sizeof(DvzColor));
    app->field_values =
        (float*)calloc((size_t)app->history * (size_t)app->n_plot_bins, sizeof(float));
    if (app->heights == NULL || app->colors == NULL || app->field_values == NULL)
        return false;

    for (uint32_t i = 0; i < app->history * app->n_plot_bins; i++)
    {
        app->field_values[i] = (float)DB_FLOOR;
        (void)dvz_colormap_builtin_sample(
            DVZ_BUILTIN_COLORMAP_VIRIDIS, 0.0, &app->colors[i]);
    }

    app->plot_freq_max =
        (double)(app->n_plot_bins - 1u) * (double)app->sample_rate / (double)app->fft_size;
    fprintf(
        stderr, "plot band: 0 … %.1f Hz (%u of %u bins @ %u Hz)\n", app->plot_freq_max,
        app->n_plot_bins, app->n_bins, (unsigned)app->sample_rate);
    return true;
}

static bool setup_datoviz(Jackoviz* app)
{
    app->scene = dvz_scene();
    if (app->scene == NULL)
        return false;

    app->figure = dvz_figure(app->scene, WINDOW_WIDTH, WINDOW_HEIGHT, 0);
    if (app->figure == NULL)
        return false;

    app->panel_3d = dvz_panel_full(app->figure);
    if (app->panel_3d == NULL)
        return false;

    /* 2D panel starts parked; shown when the user presses "2". */
    DvzPanelDesc panel_2d_desc = dvz_panel_desc();
    panel_2d_desc.x = 0.0f;
    panel_2d_desc.y = 0.0f;
    panel_2d_desc.width = 0.001f;
    panel_2d_desc.height = 0.001f;
    app->panel_2d = dvz_panel(app->figure, &panel_2d_desc);
    if (app->panel_2d == NULL)
        return false;

    DvzColor bg = dvz_color_rgba(12, 14, 20, 255);
    if (dvz_panel_set_background_color(app->panel_3d, bg) != DVZ_OK)
        return false;
    if (dvz_panel_set_background_color(app->panel_2d, bg) != DVZ_OK)
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

    DvzPanelBorderDesc border = dvz_panel_border_desc();
    border.color = dvz_color_rgba(160, 170, 190, 220);
    border.width_px = 1.5f;
    border.inset_px = 0.0f;
    if (dvz_panel_set_border(app->panel_2d, &border) != DVZ_OK)
        return false;

    DvzScaleDesc scale_desc = dvz_scale_desc();
    scale_desc.kind = DVZ_SCALE_CONTINUOUS;
    scale_desc.label = "level";
    scale_desc.unit = "dB";
    DvzScale* scale = dvz_scale(app->scene, &scale_desc);
    if (scale == NULL)
        return false;
    if (dvz_scale_set_domain(scale, DB_FLOOR, DB_CEIL) != DVZ_OK)
        return false;
    if (dvz_scale_set_view_range(scale, DB_FLOOR, DB_CEIL) != DVZ_OK)
        return false;
    if (dvz_scale_set_format(
            scale, &(DvzFormatDesc){DVZ_STRUCT_INIT_FIELDS(DvzFormatDesc), .precision = 0,
                       .trim_trailing_zeros = true}) != DVZ_OK)
        return false;
    DvzColormap* cmap = dvz_colormap_builtin(app->scene, DVZ_BUILTIN_COLORMAP_VIRIDIS);
    if (cmap == NULL || dvz_scale_set_colormap(scale, cmap) != DVZ_OK)
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
    if (dvz_visual_set_scale(app->image, "color", scale) != DVZ_OK)
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
    static const double db_tick_values[] = {
        -100.0, -90.0, -80.0, -70.0, -60.0, -50.0, -40.0, -30.0, -20.0};
    DvzColorbar* colorbar = dvz_colorbar(
        app->panel_2d, scale,
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
    if (colorbar == NULL)
        return false;
    if (dvz_colorbar_set_format(
            colorbar, &(DvzFormatDesc){DVZ_STRUCT_INIT_FIELDS(DvzFormatDesc), .precision = 0,
                          .trim_trailing_zeros = true}) != DVZ_OK)
        return false;
    DvzColorbarTicks cb_ticks = dvz_colorbar_ticks();
    cb_ticks.count = (uint32_t)(sizeof(db_tick_values) / sizeof(db_tick_values[0]));
    cb_ticks.values = db_tick_values;
    if (dvz_colorbar_set_ticks(colorbar, &cb_ticks) != DVZ_OK)
        return false;

    DvzController* panzoom = dvz_panzoom(app->scene, NULL);
    if (panzoom == NULL ||
        dvz_panel_bind_controller(app->panel_2d, panzoom, DVZ_DIM_MASK_XY) != DVZ_OK)
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

    app->view_mode = VIEW_MODE_3D;
    (void)dvz_panel_connect_input(app->panel_2d, NULL);

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
        jack_ringbuffer_free(app->audio_rb);
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
    spec_ring_destroy(&app->spec);
}

/*************************************************************************************************/
/*  main                                                                                         */
/*************************************************************************************************/

int main(int argc, char** argv)
{
    uint32_t fft_size = DEFAULT_FFT_SIZE;
    double beta = DEFAULT_KAISER_BETA;
    const char* client_name = "jackoviz";
    const char* source = NULL;
    uint32_t frame_limit = 0u;

    const int parg = parse_args(argc, argv, &fft_size, &beta, &client_name, &source, &frame_limit);
    if (parg != 0)
        return parg < 0 ? EXIT_FAILURE : EXIT_SUCCESS;

    Jackoviz app;
    memset(&app, 0, sizeof(app));
    app.fft_size = fft_size;
    app.kaiser_beta = beta;
    app.frame_limit = frame_limit;
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

    fprintf(
        stderr,
        "Spectrogram %u × %u (time × bins, 0–%.0f Hz). Keys: 2=2D, 3=3D. Close window to quit.\n",
        app.history, app.n_plot_bins, PLOT_FREQ_MAX_HZ);

    dvz_app_run(app.app, frame_limit == 0u ? 0u : frame_limit);
    rc = EXIT_SUCCESS;

done:
    teardown(&app);
    return rc;
}
