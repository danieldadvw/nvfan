/*
    MIT License

    Copyright (c) 2026 Daniel_Dad Vince Wang

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

/*
    nvfan_gtk3_v3_crossplatform.c

    NVIDIA GPU Fan Monitor + Individual Fan Controller
    GTK3 + C + NVML + Cairo animation

    Cross-platform:
      - Linux:   dlopen("libnvidia-ml.so.1")
      - Windows: LoadLibraryA("nvml.dll") and common NVIDIA NVSMI paths

    Features:
      - Enumerates NVIDIA GPUs through NVML.
      - Enumerates total number of fans per selected GPU.
      - Controls selected fan individually using nvmlDeviceSetFanSpeed_v2().
      - Restores selected fan to automatic control using nvmlDeviceSetDefaultFanSpeed_v2().
      - Optional "Apply to All Fans" and "Restore All Fans Auto".
      - Live temperature, utilization, fan speed display.
      - Small animated Cairo fan graphic.
      - Animation speed follows selected fan speed percentage.
      - Uses dynamic loading, so it does not require nvml.h at build time.

    Linux build:
      gcc nvfan_gtk3_v3_crossplatform.c -o nvfan_gtk3_v3_crossplatform $(pkg-config --cflags --libs gtk+-3.0) -ldl -lm

    Windows MSYS2 MinGW64 build:
      gcc nvfan_gtk3_v3_crossplatform.c -o nvfan_gtk3_v3_crossplatform.exe $(pkg-config --cflags --libs gtk+-3.0) -lm

    Linux run:
      ./nvfan_gtk3_v3_crossplatform

    Windows run:
      nvfan_gtk3_v3_crossplatform.exe

    WARNING:
      Manual fan control can overheat or damage hardware if the fan is set too low.
      Restore automatic fan control before exiting.
*/

#include <gtk/gtk.h>
#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef void *nvmlDevice_t;
typedef int nvmlReturn_t;

#define NVML_SUCCESS 0
#define NVML_TEMPERATURE_GPU 0

typedef struct
{
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;

typedef nvmlReturn_t (*p_nvmlInit_v2)(void);
typedef nvmlReturn_t (*p_nvmlShutdown)(void);
typedef const char *(*p_nvmlErrorString)(nvmlReturn_t);

typedef nvmlReturn_t (*p_nvmlDeviceGetCount_v2)(unsigned int *);
typedef nvmlReturn_t (*p_nvmlDeviceGetHandleByIndex_v2)(unsigned int, nvmlDevice_t *);
typedef nvmlReturn_t (*p_nvmlDeviceGetName)(nvmlDevice_t, char *, unsigned int);

typedef nvmlReturn_t (*p_nvmlDeviceGetTemperature)(nvmlDevice_t, unsigned int, unsigned int *);
typedef nvmlReturn_t (*p_nvmlDeviceGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t *);

typedef nvmlReturn_t (*p_nvmlDeviceGetNumFans)(nvmlDevice_t, unsigned int *);
typedef nvmlReturn_t (*p_nvmlDeviceGetFanSpeed)(nvmlDevice_t, unsigned int *);
typedef nvmlReturn_t (*p_nvmlDeviceGetFanSpeed_v2)(nvmlDevice_t, unsigned int, unsigned int *);
typedef nvmlReturn_t (*p_nvmlDeviceGetMinMaxFanSpeed)(nvmlDevice_t, unsigned int *, unsigned int *);

typedef nvmlReturn_t (*p_nvmlDeviceSetFanSpeed_v2)(nvmlDevice_t, unsigned int, unsigned int);
typedef nvmlReturn_t (*p_nvmlDeviceSetDefaultFanSpeed_v2)(nvmlDevice_t, unsigned int);

typedef struct
{
#ifdef _WIN32
    HMODULE lib;
#else
    void *lib;
#endif

    p_nvmlInit_v2 nvmlInit_v2;
    p_nvmlShutdown nvmlShutdown;
    p_nvmlErrorString nvmlErrorString;

    p_nvmlDeviceGetCount_v2 nvmlDeviceGetCount_v2;
    p_nvmlDeviceGetHandleByIndex_v2 nvmlDeviceGetHandleByIndex_v2;
    p_nvmlDeviceGetName nvmlDeviceGetName;

    p_nvmlDeviceGetTemperature nvmlDeviceGetTemperature;
    p_nvmlDeviceGetUtilizationRates nvmlDeviceGetUtilizationRates;

    p_nvmlDeviceGetNumFans nvmlDeviceGetNumFans;
    p_nvmlDeviceGetFanSpeed nvmlDeviceGetFanSpeed;
    p_nvmlDeviceGetFanSpeed_v2 nvmlDeviceGetFanSpeed_v2;
    p_nvmlDeviceGetMinMaxFanSpeed nvmlDeviceGetMinMaxFanSpeed;

    p_nvmlDeviceSetFanSpeed_v2 nvmlDeviceSetFanSpeed_v2;
    p_nvmlDeviceSetDefaultFanSpeed_v2 nvmlDeviceSetDefaultFanSpeed_v2;

    unsigned int gpu_count;
} NvmlApi;

typedef struct
{
    NvmlApi nvml;

    GtkWidget *window;

    GtkWidget *gpu_combo;
    GtkWidget *fan_combo;

    GtkWidget *name_label;
    GtkWidget *temp_label;
    GtkWidget *util_label;
    GtkWidget *fan_count_label;
    GtkWidget *selected_fan_label;
    GtkWidget *status_label;

    GtkWidget *fan_scale;
    GtkWidget *apply_selected_button;
    GtkWidget *auto_selected_button;
    GtkWidget *apply_all_button;
    GtkWidget *auto_all_button;

    GtkWidget *fan_drawing_area;

    unsigned int current_gpu;
    unsigned int current_fan;
    unsigned int current_gpu_fan_count;

    unsigned int selected_fan_speed_percent;
    unsigned int min_fan_speed_percent;
    unsigned int max_fan_speed_percent;

    double fan_angle_deg;

    gboolean rebuilding_fan_combo;
    gboolean closing;
} App;

static const char *nvml_error(App *app, nvmlReturn_t r)
{
    if (app->nvml.nvmlErrorString)
        return app->nvml.nvmlErrorString(r);

    return "Unknown NVML error";
}

static void set_status(App *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->status_label), text);
}

#ifdef _WIN32

static HMODULE load_nvml_windows(void)
{
    HMODULE h = NULL;
    char path[MAX_PATH * 2];

    h = LoadLibraryA("nvml.dll");
    if (h)
        return h;

    const char *program_w6432 = getenv("ProgramW6432");
    if (program_w6432 && program_w6432[0])
    {
        snprintf(path,
                 sizeof(path),
                 "%s\\NVIDIA Corporation\\NVSMI\\nvml.dll",
                 program_w6432);

        h = LoadLibraryA(path);
        if (h)
            return h;
    }

    const char *program_files = getenv("ProgramFiles");
    if (program_files && program_files[0])
    {
        snprintf(path,
                 sizeof(path),
                 "%s\\NVIDIA Corporation\\NVSMI\\nvml.dll",
                 program_files);

        h = LoadLibraryA(path);
        if (h)
            return h;
    }

    h = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (h)
        return h;

    h = LoadLibraryA("C:\\Windows\\System32\\nvml.dll");
    if (h)
        return h;

    return NULL;
}

static void *load_sym(HMODULE lib, const char *name, gboolean required)
{
    void *p = (void *)GetProcAddress(lib, name);

    if (!p && required)
        fprintf(stderr, "Missing required NVML symbol: %s\n", name);

    return p;
}

#else

static void *load_sym(void *lib, const char *name, gboolean required)
{
    void *p = dlsym(lib, name);

    if (!p && required)
        fprintf(stderr, "Missing required NVML symbol: %s\n", name);

    return p;
}

#endif

static void unload_nvml_library(App *app)
{
    if (!app->nvml.lib)
        return;

#ifdef _WIN32
    FreeLibrary(app->nvml.lib);
#else
    dlclose(app->nvml.lib);
#endif

    app->nvml.lib = 0;
}

static gboolean nvml_load(App *app)
{
    NvmlApi *n = &app->nvml;
    memset(n, 0, sizeof(*n));

#ifdef _WIN32
    n->lib = load_nvml_windows();

    if (!n->lib)
    {
        DWORD err = GetLastError();

        fprintf(stderr,
                "Failed to load nvml.dll. GetLastError=%lu\n"
                "Checked common locations:\n"
                "  nvml.dll through PATH\n"
                "  %%ProgramW6432%%\\NVIDIA Corporation\\NVSMI\\nvml.dll\n"
                "  %%ProgramFiles%%\\NVIDIA Corporation\\NVSMI\\nvml.dll\n"
                "  C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll\n"
                "  C:\\Windows\\System32\\nvml.dll\n",
                (unsigned long)err);

        return FALSE;
    }
#else
    n->lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);

    if (!n->lib)
        n->lib = dlopen("libnvidia-ml.so", RTLD_NOW | RTLD_LOCAL);

    if (!n->lib)
    {
        fprintf(stderr, "Failed to load libnvidia-ml: %s\n", dlerror());
        return FALSE;
    }
#endif

    n->nvmlInit_v2 =
        (p_nvmlInit_v2)load_sym(n->lib, "nvmlInit_v2", TRUE);

    n->nvmlShutdown =
        (p_nvmlShutdown)load_sym(n->lib, "nvmlShutdown", TRUE);

    n->nvmlErrorString =
        (p_nvmlErrorString)load_sym(n->lib, "nvmlErrorString", FALSE);

    n->nvmlDeviceGetCount_v2 =
        (p_nvmlDeviceGetCount_v2)load_sym(n->lib, "nvmlDeviceGetCount_v2", TRUE);

    n->nvmlDeviceGetHandleByIndex_v2 =
        (p_nvmlDeviceGetHandleByIndex_v2)load_sym(n->lib, "nvmlDeviceGetHandleByIndex_v2", TRUE);

    n->nvmlDeviceGetName =
        (p_nvmlDeviceGetName)load_sym(n->lib, "nvmlDeviceGetName", TRUE);

    n->nvmlDeviceGetTemperature =
        (p_nvmlDeviceGetTemperature)load_sym(n->lib, "nvmlDeviceGetTemperature", FALSE);

    n->nvmlDeviceGetUtilizationRates =
        (p_nvmlDeviceGetUtilizationRates)load_sym(n->lib, "nvmlDeviceGetUtilizationRates", FALSE);

    n->nvmlDeviceGetNumFans =
        (p_nvmlDeviceGetNumFans)load_sym(n->lib, "nvmlDeviceGetNumFans", FALSE);

    n->nvmlDeviceGetFanSpeed =
        (p_nvmlDeviceGetFanSpeed)load_sym(n->lib, "nvmlDeviceGetFanSpeed", FALSE);

    n->nvmlDeviceGetFanSpeed_v2 =
        (p_nvmlDeviceGetFanSpeed_v2)load_sym(n->lib, "nvmlDeviceGetFanSpeed_v2", FALSE);

    n->nvmlDeviceGetMinMaxFanSpeed =
        (p_nvmlDeviceGetMinMaxFanSpeed)load_sym(n->lib, "nvmlDeviceGetMinMaxFanSpeed", FALSE);

    n->nvmlDeviceSetFanSpeed_v2 =
        (p_nvmlDeviceSetFanSpeed_v2)load_sym(n->lib, "nvmlDeviceSetFanSpeed_v2", FALSE);

    n->nvmlDeviceSetDefaultFanSpeed_v2 =
        (p_nvmlDeviceSetDefaultFanSpeed_v2)load_sym(n->lib, "nvmlDeviceSetDefaultFanSpeed_v2", FALSE);

    if (!n->nvmlInit_v2 ||
        !n->nvmlShutdown ||
        !n->nvmlDeviceGetCount_v2 ||
        !n->nvmlDeviceGetHandleByIndex_v2 ||
        !n->nvmlDeviceGetName)
    {
        unload_nvml_library(app);
        return FALSE;
    }

    nvmlReturn_t r = n->nvmlInit_v2();
    if (r != NVML_SUCCESS)
    {
        fprintf(stderr,
                "nvmlInit_v2 failed: %s\n",
                n->nvmlErrorString ? n->nvmlErrorString(r) : "unknown error");

        unload_nvml_library(app);
        return FALSE;
    }

    r = n->nvmlDeviceGetCount_v2(&n->gpu_count);
    if (r != NVML_SUCCESS || n->gpu_count == 0)
    {
        fprintf(stderr, "No NVIDIA GPU found through NVML.\n");

        if (n->nvmlShutdown)
            n->nvmlShutdown();

        unload_nvml_library(app);
        return FALSE;
    }

    return TRUE;
}

static gboolean get_current_device(App *app, nvmlDevice_t *dev)
{
    nvmlReturn_t r;

    if (app->current_gpu >= app->nvml.gpu_count)
    {
        set_status(app, "Invalid GPU index.");
        return FALSE;
    }

    r = app->nvml.nvmlDeviceGetHandleByIndex_v2(app->current_gpu, dev);
    if (r != NVML_SUCCESS)
    {
        char msg[256];

        snprintf(msg,
                 sizeof(msg),
                 "Failed to get GPU handle: %s",
                 nvml_error(app, r));

        set_status(app, msg);
        return FALSE;
    }

    return TRUE;
}

static unsigned int query_fan_count(App *app, nvmlDevice_t dev)
{
    unsigned int count = 0;

    if (!app->nvml.nvmlDeviceGetNumFans)
        return 0;

    nvmlReturn_t r = app->nvml.nvmlDeviceGetNumFans(dev, &count);
    if (r != NVML_SUCCESS)
        return 0;

    return count;
}

static gboolean query_fan_speed(App *app,
                                nvmlDevice_t dev,
                                unsigned int fan_index,
                                unsigned int *speed)
{
    nvmlReturn_t r;

    if (app->nvml.nvmlDeviceGetFanSpeed_v2)
    {
        r = app->nvml.nvmlDeviceGetFanSpeed_v2(dev, fan_index, speed);
        return r == NVML_SUCCESS;
    }

    if (app->nvml.nvmlDeviceGetFanSpeed)
    {
        r = app->nvml.nvmlDeviceGetFanSpeed(dev, speed);
        return r == NVML_SUCCESS;
    }

    return FALSE;
}

static void query_fan_min_max(App *app, nvmlDevice_t dev)
{
    unsigned int min_speed = 0;
    unsigned int max_speed = 100;

    if (app->nvml.nvmlDeviceGetMinMaxFanSpeed)
    {
        nvmlReturn_t r =
            app->nvml.nvmlDeviceGetMinMaxFanSpeed(dev, &min_speed, &max_speed);

        if (r != NVML_SUCCESS)
        {
            min_speed = 0;
            max_speed = 100;
        }
    }

    if (max_speed <= min_speed || max_speed > 200)
    {
        min_speed = 0;
        max_speed = 100;
    }

    app->min_fan_speed_percent = min_speed;
    app->max_fan_speed_percent = max_speed;

    gtk_range_set_range(GTK_RANGE(app->fan_scale), min_speed, max_speed);
}

static GtkWidget *make_left_label(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

static void rebuild_gpu_combo(App *app)
{
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->gpu_combo));

    for (unsigned int i = 0; i < app->nvml.gpu_count; i++)
    {
        nvmlDevice_t dev = NULL;
        char name[128] = {0};
        char item[256];

        if (app->nvml.nvmlDeviceGetHandleByIndex_v2(i, &dev) == NVML_SUCCESS &&
            app->nvml.nvmlDeviceGetName(dev, name, sizeof(name)) == NVML_SUCCESS)
        {
            snprintf(item, sizeof(item), "GPU %u: %s", i, name);
        }
        else
        {
            snprintf(item, sizeof(item), "GPU %u", i);
        }

        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->gpu_combo), item);
    }

    gtk_combo_box_set_active(GTK_COMBO_BOX(app->gpu_combo), 0);
}

static void rebuild_fan_combo(App *app)
{
    nvmlDevice_t dev = NULL;

    app->rebuilding_fan_combo = TRUE;

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->fan_combo));

    app->current_gpu_fan_count = 0;
    app->current_fan = 0;

    if (!get_current_device(app, &dev))
    {
        app->rebuilding_fan_combo = FALSE;
        return;
    }

    app->current_gpu_fan_count = query_fan_count(app, dev);

    if (app->current_gpu_fan_count == 0)
    {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->fan_combo),
                                       "No controllable fan exposed");
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->fan_combo), 0);
    }
    else
    {
        for (unsigned int i = 0; i < app->current_gpu_fan_count; i++)
        {
            char item[64];
            snprintf(item, sizeof(item), "Fan %u", i);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->fan_combo),
                                           item);
        }

        gtk_combo_box_set_active(GTK_COMBO_BOX(app->fan_combo), 0);
    }

    app->rebuilding_fan_combo = FALSE;
}

static gboolean draw_fan(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    App *app = (App *)user_data;

    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);

    double w = (double)a.width;
    double h = (double)a.height;
    double cx = w * 0.5;
    double cy = h * 0.5;

    double r = fmin(w, h) * 0.38;
    double hub = r * 0.16;

    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_paint(cr);

    cairo_arc(cr, cx, cy, r * 1.12, 0, 2.0 * G_PI);
    cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
    cairo_fill(cr);

    cairo_arc(cr, cx, cy, r * 1.04, 0, 2.0 * G_PI);
    cairo_set_source_rgb(cr, 0.98, 0.98, 0.98);
    cairo_fill(cr);

    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, app->fan_angle_deg * G_PI / 180.0);

    for (int i = 0; i < 5; i++)
    {
        cairo_save(cr);

        cairo_rotate(cr, i * 2.0 * G_PI / 5.0);

        cairo_move_to(cr, hub * 0.6, 0.0);

        cairo_curve_to(cr,
                       r * 0.26, -r * 0.17,
                       r * 0.73, -r * 0.22,
                       r * 0.98, -r * 0.03);

        cairo_curve_to(cr,
                       r * 0.78, r * 0.23,
                       r * 0.32, r * 0.18,
                       hub * 0.6, 0.0);

        cairo_close_path(cr);

        cairo_set_source_rgb(cr, 0.18, 0.18, 0.18);
        cairo_fill_preserve(cr);

        cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        cairo_restore(cr);
    }

    cairo_restore(cr);

    cairo_arc(cr, cx, cy, hub, 0, 2.0 * G_PI);
    cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
    cairo_fill(cr);

    cairo_arc(cr, cx, cy, hub * 0.55, 0, 2.0 * G_PI);
    cairo_set_source_rgb(cr, 0.42, 0.42, 0.42);
    cairo_fill(cr);

    cairo_arc(cr, cx, cy, r * 1.13, 0, 2.0 * G_PI);
    cairo_set_source_rgb(cr, 0.20, 0.20, 0.20);
    cairo_set_line_width(cr, 4.0);
    cairo_stroke(cr);

    char text[64];
    snprintf(text, sizeof(text), "%u%%", app->selected_fan_speed_percent);

    cairo_select_font_face(cr,
                           "Sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);

    cairo_set_font_size(cr, 18.0);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);

    cairo_move_to(cr,
                  cx - ext.width / 2.0 - ext.x_bearing,
                  cy + r * 1.48);

    cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
    cairo_show_text(cr, text);

    return FALSE;
}

static gboolean animate_fan(gpointer user_data)
{
    App *app = (App *)user_data;

    if (app->closing)
        return FALSE;

    double pct = (double)app->selected_fan_speed_percent;

    if (pct <= 0.5)
        app->fan_angle_deg += 0.15;
    else
        app->fan_angle_deg += 0.6 + pct * 0.75;

    while (app->fan_angle_deg >= 360.0)
        app->fan_angle_deg -= 360.0;

    if (app->fan_drawing_area)
        gtk_widget_queue_draw(app->fan_drawing_area);

    return TRUE;
}

static gboolean refresh_ui(gpointer user_data)
{
    App *app = (App *)user_data;

    if (app->closing)
        return FALSE;

    nvmlDevice_t dev = NULL;
    char msg[512];

    if (!get_current_device(app, &dev))
        return TRUE;

    char name[128] = {0};
    nvmlReturn_t r = app->nvml.nvmlDeviceGetName(dev, name, sizeof(name));

    if (r == NVML_SUCCESS)
        gtk_label_set_text(GTK_LABEL(app->name_label), name);
    else
        gtk_label_set_text(GTK_LABEL(app->name_label), "Unknown NVIDIA GPU");

    if (app->nvml.nvmlDeviceGetTemperature)
    {
        unsigned int temp = 0;

        r = app->nvml.nvmlDeviceGetTemperature(dev,
                                               NVML_TEMPERATURE_GPU,
                                               &temp);

        if (r == NVML_SUCCESS)
            snprintf(msg, sizeof(msg), "Temperature: %u °C", temp);
        else
            snprintf(msg,
                     sizeof(msg),
                     "Temperature: unsupported (%s)",
                     nvml_error(app, r));

        gtk_label_set_text(GTK_LABEL(app->temp_label), msg);
    }
    else
    {
        gtk_label_set_text(GTK_LABEL(app->temp_label),
                           "Temperature: API unavailable");
    }

    if (app->nvml.nvmlDeviceGetUtilizationRates)
    {
        nvmlUtilization_t util;
        memset(&util, 0, sizeof(util));

        r = app->nvml.nvmlDeviceGetUtilizationRates(dev, &util);

        if (r == NVML_SUCCESS)
        {
            snprintf(msg,
                     sizeof(msg),
                     "Utilization: GPU %u%% / Memory %u%%",
                     util.gpu,
                     util.memory);
        }
        else
        {
            snprintf(msg,
                     sizeof(msg),
                     "Utilization: unsupported (%s)",
                     nvml_error(app, r));
        }

        gtk_label_set_text(GTK_LABEL(app->util_label), msg);
    }
    else
    {
        gtk_label_set_text(GTK_LABEL(app->util_label),
                           "Utilization: API unavailable");
    }

    unsigned int fan_count_now = query_fan_count(app, dev);

    if (fan_count_now != app->current_gpu_fan_count)
    {
        rebuild_fan_combo(app);
        fan_count_now = app->current_gpu_fan_count;
    }

    if (fan_count_now == 0)
    {
        gtk_label_set_text(GTK_LABEL(app->fan_count_label),
                           "Fans exposed by NVML: 0");

        gtk_label_set_text(GTK_LABEL(app->selected_fan_label),
                           "Selected fan: unavailable");

        app->selected_fan_speed_percent = 0;

        gtk_widget_set_sensitive(app->fan_combo, FALSE);
        gtk_widget_set_sensitive(app->fan_scale, FALSE);
        gtk_widget_set_sensitive(app->apply_selected_button, FALSE);
        gtk_widget_set_sensitive(app->auto_selected_button, FALSE);
        gtk_widget_set_sensitive(app->apply_all_button, FALSE);
        gtk_widget_set_sensitive(app->auto_all_button, FALSE);

        return TRUE;
    }

    gtk_widget_set_sensitive(app->fan_combo, TRUE);

    if (app->current_fan >= fan_count_now)
        app->current_fan = 0;

    query_fan_min_max(app, dev);

    snprintf(msg, sizeof(msg), "Fans exposed by NVML: %u", fan_count_now);
    gtk_label_set_text(GTK_LABEL(app->fan_count_label), msg);

    unsigned int fan_speed = 0;
    gboolean have_fan_speed =
        query_fan_speed(app, dev, app->current_fan, &fan_speed);

    if (have_fan_speed)
    {
        app->selected_fan_speed_percent = fan_speed;

        snprintf(msg,
                 sizeof(msg),
                 "Selected fan: Fan %u / speed %u%% / settable range %u%%–%u%%",
                 app->current_fan,
                 fan_speed,
                 app->min_fan_speed_percent,
                 app->max_fan_speed_percent);

        gtk_label_set_text(GTK_LABEL(app->selected_fan_label), msg);

        if (!gtk_widget_has_focus(app->fan_scale))
            gtk_range_set_value(GTK_RANGE(app->fan_scale), fan_speed);
    }
    else
    {
        app->selected_fan_speed_percent = 0;

        snprintf(msg,
                 sizeof(msg),
                 "Selected fan: Fan %u / speed unavailable",
                 app->current_fan);

        gtk_label_set_text(GTK_LABEL(app->selected_fan_label), msg);
    }

    gboolean can_set_selected =
        app->nvml.nvmlDeviceSetFanSpeed_v2 &&
        fan_count_now > 0;

    gboolean can_restore_selected =
        app->nvml.nvmlDeviceSetDefaultFanSpeed_v2 &&
        fan_count_now > 0;

    gtk_widget_set_sensitive(app->fan_scale, can_set_selected);
    gtk_widget_set_sensitive(app->apply_selected_button, can_set_selected);
    gtk_widget_set_sensitive(app->auto_selected_button, can_restore_selected);
    gtk_widget_set_sensitive(app->apply_all_button, can_set_selected);
    gtk_widget_set_sensitive(app->auto_all_button, can_restore_selected);

    return TRUE;
}

static void on_gpu_changed(GtkComboBox *combo, gpointer user_data)
{
    App *app = (App *)user_data;

    int idx = gtk_combo_box_get_active(combo);
    if (idx < 0)
        return;

    app->current_gpu = (unsigned int)idx;
    app->current_fan = 0;

    rebuild_fan_combo(app);
    refresh_ui(app);
}

static void on_fan_changed(GtkComboBox *combo, gpointer user_data)
{
    App *app = (App *)user_data;

    if (app->rebuilding_fan_combo)
        return;

    int idx = gtk_combo_box_get_active(combo);
    if (idx < 0)
        return;

    app->current_fan = (unsigned int)idx;

    refresh_ui(app);
}

static void on_scale_value_changed(GtkRange *range, gpointer user_data)
{
    App *app = (App *)user_data;

    unsigned int v = (unsigned int)gtk_range_get_value(range);
    app->selected_fan_speed_percent = v;

    if (app->fan_drawing_area)
        gtk_widget_queue_draw(app->fan_drawing_area);
}

static unsigned int get_requested_fan_speed(App *app)
{
    double v = gtk_range_get_value(GTK_RANGE(app->fan_scale));

    if (v < app->min_fan_speed_percent)
        v = app->min_fan_speed_percent;

    if (v > app->max_fan_speed_percent)
        v = app->max_fan_speed_percent;

    return (unsigned int)(v + 0.5);
}

static void apply_speed_to_fan(App *app,
                               nvmlDevice_t dev,
                               unsigned int fan_index,
                               unsigned int speed)
{
    char msg[256];

    if (!app->nvml.nvmlDeviceSetFanSpeed_v2)
    {
        set_status(app, "Manual fan-speed API is unavailable.");
        return;
    }

    nvmlReturn_t r =
        app->nvml.nvmlDeviceSetFanSpeed_v2(dev, fan_index, speed);

    if (r != NVML_SUCCESS)
    {
        snprintf(msg,
                 sizeof(msg),
                 "Failed to set GPU %u Fan %u to %u%%: %s",
                 app->current_gpu,
                 fan_index,
                 speed,
                 nvml_error(app, r));

        set_status(app, msg);
        return;
    }

    snprintf(msg,
             sizeof(msg),
             "Applied manual control: GPU %u Fan %u = %u%%",
             app->current_gpu,
             fan_index,
             speed);

    set_status(app, msg);
}

static void restore_auto_for_fan(App *app,
                                 nvmlDevice_t dev,
                                 unsigned int fan_index)
{
    char msg[256];

    if (!app->nvml.nvmlDeviceSetDefaultFanSpeed_v2)
    {
        set_status(app, "Auto fan restore API is unavailable.");
        return;
    }

    nvmlReturn_t r =
        app->nvml.nvmlDeviceSetDefaultFanSpeed_v2(dev, fan_index);

    if (r != NVML_SUCCESS)
    {
        snprintf(msg,
                 sizeof(msg),
                 "Failed to restore GPU %u Fan %u auto control: %s",
                 app->current_gpu,
                 fan_index,
                 nvml_error(app, r));

        set_status(app, msg);
        return;
    }

    snprintf(msg,
             sizeof(msg),
             "Restored automatic control: GPU %u Fan %u",
             app->current_gpu,
             fan_index);

    set_status(app, msg);
}

static void on_apply_selected(GtkButton *button, gpointer user_data)
{
    (void)button;

    App *app = (App *)user_data;
    nvmlDevice_t dev = NULL;

    if (!get_current_device(app, &dev))
        return;

    if (app->current_gpu_fan_count == 0)
    {
        set_status(app, "No controllable fan exposed by NVML.");
        return;
    }

    unsigned int speed = get_requested_fan_speed(app);
    apply_speed_to_fan(app, dev, app->current_fan, speed);

    refresh_ui(app);
}

static void on_auto_selected(GtkButton *button, gpointer user_data)
{
    (void)button;

    App *app = (App *)user_data;
    nvmlDevice_t dev = NULL;

    if (!get_current_device(app, &dev))
        return;

    if (app->current_gpu_fan_count == 0)
    {
        set_status(app, "No controllable fan exposed by NVML.");
        return;
    }

    restore_auto_for_fan(app, dev, app->current_fan);

    refresh_ui(app);
}

static void on_apply_all(GtkButton *button, gpointer user_data)
{
    (void)button;

    App *app = (App *)user_data;
    nvmlDevice_t dev = NULL;

    if (!get_current_device(app, &dev))
        return;

    if (app->current_gpu_fan_count == 0)
    {
        set_status(app, "No controllable fan exposed by NVML.");
        return;
    }

    if (!app->nvml.nvmlDeviceSetFanSpeed_v2)
    {
        set_status(app, "Manual fan-speed API is unavailable.");
        return;
    }

    unsigned int speed = get_requested_fan_speed(app);
    unsigned int ok_count = 0;
    unsigned int fail_count = 0;

    for (unsigned int i = 0; i < app->current_gpu_fan_count; i++)
    {
        nvmlReturn_t r =
            app->nvml.nvmlDeviceSetFanSpeed_v2(dev, i, speed);

        if (r == NVML_SUCCESS)
        {
            ok_count++;
        }
        else
        {
            fail_count++;

            fprintf(stderr,
                    "Fan %u set failed: %s (%d)\n",
                    i,
                    nvml_error(app, r),
                    r);
        }
    }

    char msg[256];

    snprintf(msg,
             sizeof(msg),
             "Apply all fans complete: %u succeeded, %u failed, requested speed %u%%",
             ok_count,
             fail_count,
             speed);

    set_status(app, msg);

    refresh_ui(app);
}

static void on_auto_all(GtkButton *button, gpointer user_data)
{
    (void)button;

    App *app = (App *)user_data;
    nvmlDevice_t dev = NULL;

    if (!get_current_device(app, &dev))
        return;

    if (app->current_gpu_fan_count == 0)
    {
        set_status(app, "No controllable fan exposed by NVML.");
        return;
    }

    if (!app->nvml.nvmlDeviceSetDefaultFanSpeed_v2)
    {
        set_status(app, "Auto fan restore API is unavailable.");
        return;
    }

    unsigned int ok_count = 0;
    unsigned int fail_count = 0;

    for (unsigned int i = 0; i < app->current_gpu_fan_count; i++)
    {
        nvmlReturn_t r =
            app->nvml.nvmlDeviceSetDefaultFanSpeed_v2(dev, i);

        if (r == NVML_SUCCESS)
        {
            ok_count++;
        }
        else
        {
            fail_count++;

            fprintf(stderr,
                    "Fan %u restore-auto failed: %s (%d)\n",
                    i,
                    nvml_error(app, r),
                    r);
        }
    }

    char msg[256];

    snprintf(msg,
             sizeof(msg),
             "Restore all fans auto complete: %u succeeded, %u failed",
             ok_count,
             fail_count);

    set_status(app, msg);

    refresh_ui(app);
}

static void on_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;

    App *app = (App *)user_data;

    app->closing = TRUE;

    if (app->nvml.nvmlShutdown)
        app->nvml.nvmlShutdown();

    unload_nvml_library(app);

    gtk_main_quit();
}

static void build_ui(App *app)
{
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window),
                         "NVIDIA GPU Fan Monitor / Controller V3");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 780, 440);
    gtk_container_set_border_width(GTK_CONTAINER(app->window), 12);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_container_add(GTK_CONTAINER(app->window), main_box);

    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    gtk_box_pack_start(GTK_BOX(main_box), left_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), right_box, FALSE, FALSE, 0);

    GtkWidget *selector_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(selector_grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(selector_grid), 8);
    gtk_box_pack_start(GTK_BOX(left_box), selector_grid, FALSE, FALSE, 0);

    GtkWidget *gpu_label = make_left_label("GPU:");
    GtkWidget *fan_label = make_left_label("Fan:");

    app->gpu_combo = gtk_combo_box_text_new();
    app->fan_combo = gtk_combo_box_text_new();

    gtk_widget_set_hexpand(app->gpu_combo, TRUE);
    gtk_widget_set_hexpand(app->fan_combo, TRUE);

    gtk_grid_attach(GTK_GRID(selector_grid), gpu_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(selector_grid), app->gpu_combo, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(selector_grid), fan_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(selector_grid), app->fan_combo, 1, 1, 1, 1);

    app->name_label = make_left_label("GPU: -");
    app->temp_label = make_left_label("Temperature: -");
    app->util_label = make_left_label("Utilization: -");
    app->fan_count_label = make_left_label("Fans exposed by NVML: -");
    app->selected_fan_label = make_left_label("Selected fan: -");

    gtk_box_pack_start(GTK_BOX(left_box), app->name_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_box), app->temp_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_box), app->util_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_box), app->fan_count_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_box), app->selected_fan_label, FALSE, FALSE, 0);

    GtkWidget *manual_frame = gtk_frame_new("Manual fan control");
    gtk_box_pack_start(GTK_BOX(left_box), manual_frame, FALSE, FALSE, 0);

    GtkWidget *manual_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(manual_box), 10);
    gtk_container_add(GTK_CONTAINER(manual_frame), manual_box);

    app->fan_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                              0,
                                              100,
                                              1);

    gtk_scale_set_draw_value(GTK_SCALE(app->fan_scale), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(app->fan_scale), GTK_POS_RIGHT);
    gtk_range_set_value(GTK_RANGE(app->fan_scale), 50);
    gtk_widget_set_hexpand(app->fan_scale, TRUE);
    gtk_box_pack_start(GTK_BOX(manual_box), app->fan_scale, FALSE, FALSE, 0);

    GtkWidget *button_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(button_grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(button_grid), 8);
    gtk_box_pack_start(GTK_BOX(manual_box), button_grid, FALSE, FALSE, 0);

    app->apply_selected_button =
        gtk_button_new_with_label("Apply to Selected Fan");

    app->auto_selected_button =
        gtk_button_new_with_label("Restore Selected Fan Auto");

    app->apply_all_button =
        gtk_button_new_with_label("Apply Same Speed to All Fans");

    app->auto_all_button =
        gtk_button_new_with_label("Restore All Fans Auto");

    gtk_grid_attach(GTK_GRID(button_grid), app->apply_selected_button, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(button_grid), app->auto_selected_button, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(button_grid), app->apply_all_button, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(button_grid), app->auto_all_button, 1, 1, 1, 1);

    app->status_label = make_left_label("Ready.");
    gtk_box_pack_end(GTK_BOX(left_box), app->status_label, FALSE, FALSE, 0);

    GtkWidget *fan_frame = gtk_frame_new("Selected fan animation");
    gtk_box_pack_start(GTK_BOX(right_box), fan_frame, FALSE, FALSE, 0);

    app->fan_drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(app->fan_drawing_area, 220, 260);
    gtk_container_add(GTK_CONTAINER(fan_frame), app->fan_drawing_area);

    g_signal_connect(app->window,
                     "destroy",
                     G_CALLBACK(on_destroy),
                     app);

    g_signal_connect(app->gpu_combo,
                     "changed",
                     G_CALLBACK(on_gpu_changed),
                     app);

    g_signal_connect(app->fan_combo,
                     "changed",
                     G_CALLBACK(on_fan_changed),
                     app);

    g_signal_connect(app->fan_scale,
                     "value-changed",
                     G_CALLBACK(on_scale_value_changed),
                     app);

    g_signal_connect(app->apply_selected_button,
                     "clicked",
                     G_CALLBACK(on_apply_selected),
                     app);

    g_signal_connect(app->auto_selected_button,
                     "clicked",
                     G_CALLBACK(on_auto_selected),
                     app);

    g_signal_connect(app->apply_all_button,
                     "clicked",
                     G_CALLBACK(on_apply_all),
                     app);

    g_signal_connect(app->auto_all_button,
                     "clicked",
                     G_CALLBACK(on_auto_all),
                     app);

    g_signal_connect(app->fan_drawing_area,
                     "draw",
                     G_CALLBACK(draw_fan),
                     app);

    rebuild_gpu_combo(app);
    rebuild_fan_combo(app);
}

int main(int argc, char **argv)
{
    App app;
    memset(&app, 0, sizeof(app));

    app.min_fan_speed_percent = 0;
    app.max_fan_speed_percent = 100;
    app.selected_fan_speed_percent = 0;
    app.fan_angle_deg = 0.0;

    gtk_init(&argc, &argv);

    if (!nvml_load(&app))
    {
#ifdef _WIN32
        const char *msg =
            "Failed to initialize NVIDIA NVML.\n\n"
            "Check that the NVIDIA driver is installed and nvml.dll is available.\n\n"
            "Common path:\n"
            "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll";
#else
        const char *msg =
            "Failed to initialize NVIDIA NVML.\n\n"
            "Check that the NVIDIA proprietary driver is installed and that libnvidia-ml.so.1 is available.";
#endif

        GtkWidget *dialog =
            gtk_message_dialog_new(NULL,
                                   GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_ERROR,
                                   GTK_BUTTONS_CLOSE,
                                   "%s",
                                   msg);

        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);

        return 1;
    }

    build_ui(&app);
    refresh_ui(&app);

    g_timeout_add(33, animate_fan, &app);
    g_timeout_add_seconds(1, refresh_ui, &app);

    gtk_widget_show_all(app.window);
    gtk_main();

    return 0;
}