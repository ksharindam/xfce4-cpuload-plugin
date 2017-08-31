#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <stdlib.h>

#include <libxfce4panel/xfce-panel-plugin.h>

#define BORDER_SIZE 1
#define PLUGIN_WIDTH 42
#define FG_COLOR "rgb(0,205,0)"                 // Color names are found in X11 rgb.txt
#define BG_COLOR "white"
#define FONT_SIZE 12

typedef unsigned long long CPUTick;		/* Value from /proc/stat */
typedef float CPUSample;			/* Saved CPU utilization value as 0.0..1.0 */

struct cpu_stat {
    CPUTick u, n, s, i;				/* User, nice, system, idle */
};

/* Private context for CPU plugin. */
typedef struct {
    GdkRGBA foreground_color;			/* Foreground color for drawing area */
    GdkRGBA background_color;			/* Background color for drawing area */
    GtkWidget * da;				        /* Drawing area */
    cairo_surface_t * pixmap;			/* Pixmap to be drawn on drawing area */

    guint timer;				        /* Timer for periodic update */
    CPUSample * stats_cpu;			    /* Ring buffer of CPU utilization values */
    unsigned int ring_cursor;			/* Cursor for ring buffer */
    guint pixmap_width;				    /* Width of drawing area pixmap; also size of ring buffer; does not include border size */
    guint pixmap_height;			    /* Height of drawing area pixmap; does not include border size */
    struct cpu_stat previous_cpu_stat;	/* Previous value of cpu_stat */
    gboolean show_percentage;			/* Display usage as a percentage */
} CPUPlugin;

static void redraw_pixmap(CPUPlugin * c);
static gboolean cpu_update(CPUPlugin * c);
static gboolean configure_event(GtkWidget * widget, GdkEventConfigure * event, CPUPlugin * c);
static gboolean expose_event(GtkWidget * widget, GdkEventExpose * event, CPUPlugin * c);
static gboolean on_size_change(XfcePanelPlugin *plugin, gint size, void *data);
static gboolean on_button_press(GtkWidget *button, GdkEvent *event, gpointer data);
static void cpu_constructor(XfcePanelPlugin *plugin);
static void cpu_destructor(XfcePanelPlugin *plugin, gpointer user_data);

XFCE_PANEL_PLUGIN_REGISTER_INTERNAL(cpu_constructor);

/* Redraw after timer callback or resize. */
static void
redraw_pixmap (CPUPlugin * c)
{
    cairo_t * cr = cairo_create(c->pixmap);
    cairo_set_line_width (cr, 1.0);
    /* Erase pixmap. */
    cairo_rectangle(cr, 0, 0, c->pixmap_width, c->pixmap_height);
    gdk_cairo_set_source_rgba(cr, &c->background_color);
    cairo_fill(cr);

    /* Recompute pixmap. */
    unsigned int i;
    unsigned int drawing_cursor = c->ring_cursor;
    gdk_cairo_set_source_rgba(cr, &c->foreground_color);
    for (i = 0; i < c->pixmap_width; i++)
    {
        /* Draw one bar of the CPU usage graph. */
        if (c->stats_cpu[drawing_cursor] != 0.0)
        {
            cairo_move_to(cr, i + 0.5, c->pixmap_height);
            cairo_line_to(cr, i + 0.5, c->pixmap_height - c->stats_cpu[drawing_cursor] * c->pixmap_height);
            cairo_stroke(cr);
        }

        /* Increment and wrap drawing cursor. */
        drawing_cursor += 1;
        if (drawing_cursor >= c->pixmap_width)
            drawing_cursor = 0;
    }

    /* draw a border in black */
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, 0, 0);
    cairo_line_to(cr, 0, c->pixmap_height);
    cairo_line_to(cr, c->pixmap_width, c->pixmap_height);
    cairo_line_to(cr, c->pixmap_width, 0);
    cairo_line_to(cr, 0, 0);
    cairo_stroke(cr);

	if (c->show_percentage)
	{
    	char buffer[10];
		int val = 100 * c->stats_cpu[c->ring_cursor ? c->ring_cursor - 1 : c->pixmap_width - 1];
    	sprintf (buffer, "%3d %%", val);
        cairo_select_font_face (cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    	cairo_set_font_size (cr, FONT_SIZE);
        /*// Shadow effect
    	cairo_set_source_rgb (cr, 255, 255, 255);
    	cairo_move_to (cr, 0, ((c->pixmap_height + FONT_SIZE) / 2) - 2);
    	cairo_show_text (cr, buffer);
    	cairo_move_to (cr, 2, ((c->pixmap_height + FONT_SIZE) / 2) );
    	cairo_show_text (cr, buffer);*/
        // End shadow effect
    	cairo_set_source_rgb (cr, 0, 0, 0);
    	cairo_move_to (cr, 1, ((c->pixmap_height + FONT_SIZE) / 2) - 1);
    	cairo_show_text (cr, buffer);
	}

    /* check_cairo_status(cr); */
    cairo_destroy(cr);

    /* Redraw pixmap. */
    gtk_widget_queue_draw(c->da);
}

/* Periodic timer callback. */
static gboolean
cpu_update (CPUPlugin * c)
{
    if (g_source_is_destroyed(g_main_current_source()))
        return FALSE;
    if ((c->stats_cpu != NULL) && (c->pixmap != NULL))
    {
        /* Open statistics file and scan out CPU usage. */
        struct cpu_stat cpu;
        FILE * stat = fopen("/proc/stat", "r");
        if (stat == NULL)
            return TRUE;
        int fscanf_result = fscanf(stat, "cpu %llu %llu %llu %llu", &cpu.u, &cpu.n, &cpu.s, &cpu.i);
        fclose(stat);

        /* Ensure that fscanf succeeded. */
        if (fscanf_result == 4)
        {
            /* Compute delta from previous statistics. */
            struct cpu_stat cpu_delta;
            cpu_delta.u = cpu.u - c->previous_cpu_stat.u;
            cpu_delta.n = cpu.n - c->previous_cpu_stat.n;
            cpu_delta.s = cpu.s - c->previous_cpu_stat.s;
            cpu_delta.i = cpu.i - c->previous_cpu_stat.i;

            /* Copy current to previous. */
            memcpy(&c->previous_cpu_stat, &cpu, sizeof(struct cpu_stat));

            /* Compute user+nice+system as a fraction of total.
             * Introduce this sample to ring buffer, increment and wrap ring buffer cursor. */
            float cpu_uns = cpu_delta.u + cpu_delta.n + cpu_delta.s;
            c->stats_cpu[c->ring_cursor] = cpu_uns / (cpu_uns + cpu_delta.i);
            c->ring_cursor += 1;
            if (c->ring_cursor >= c->pixmap_width)
                c->ring_cursor = 0;

            /* Redraw with the new sample. */
            redraw_pixmap(c);
        }
    }
    return TRUE;
}

/* Handler for configure_event on drawing area. */
static gboolean
configure_event (GtkWidget * widget, GdkEventConfigure * event, CPUPlugin * c)
{
    GtkAllocation allocation;

    gtk_widget_get_allocation(widget, &allocation);
    /* Allocate pixmap and statistics buffer without border pixels. */
    guint new_pixmap_width = MAX(allocation.width - BORDER_SIZE * 2, 0);
    guint new_pixmap_height = MAX(allocation.height - BORDER_SIZE * 2, 0);
    if ((new_pixmap_width > 0) && (new_pixmap_height > 0))
    {
        /* If statistics buffer does not exist or it changed size, reallocate and preserve existing data. */
        if ((c->stats_cpu == NULL) || (new_pixmap_width != c->pixmap_width))
        {
            CPUSample * new_stats_cpu = g_new0(typeof(*c->stats_cpu), new_pixmap_width);
            if (c->stats_cpu != NULL)
            {
                if (new_pixmap_width > c->pixmap_width)
                {
                    /* New allocation is larger.
                     * Introduce new "oldest" samples of zero following the cursor. */
                    memcpy(&new_stats_cpu[0],
                        &c->stats_cpu[0], c->ring_cursor * sizeof(CPUSample));
                    memcpy(&new_stats_cpu[new_pixmap_width - c->pixmap_width + c->ring_cursor],
                        &c->stats_cpu[c->ring_cursor], (c->pixmap_width - c->ring_cursor) * sizeof(CPUSample));
                }
                else if (c->ring_cursor <= new_pixmap_width)
                {
                    /* New allocation is smaller, but still larger than the ring buffer cursor.
                     * Discard the oldest samples following the cursor. */
                    memcpy(&new_stats_cpu[0],
                        &c->stats_cpu[0], c->ring_cursor * sizeof(CPUSample));
                    memcpy(&new_stats_cpu[c->ring_cursor],
                        &c->stats_cpu[c->pixmap_width - new_pixmap_width + c->ring_cursor], (new_pixmap_width - c->ring_cursor) * sizeof(CPUSample));
                }
                else
                {
                    /* New allocation is smaller, and also smaller than the ring buffer cursor.
                     * Discard all oldest samples following the ring buffer cursor and additional samples at the beginning of the buffer. */
                    memcpy(&new_stats_cpu[0],
                        &c->stats_cpu[c->ring_cursor - new_pixmap_width], new_pixmap_width * sizeof(CPUSample));
                    c->ring_cursor = 0;
                }
                g_free(c->stats_cpu);
            }
            c->stats_cpu = new_stats_cpu;
        }

        /* Allocate or reallocate pixmap. */
        c->pixmap_width = new_pixmap_width;
        c->pixmap_height = new_pixmap_height;
        if (c->pixmap)
            cairo_surface_destroy(c->pixmap);
        c->pixmap = cairo_image_surface_create(CAIRO_FORMAT_RGB24, c->pixmap_width, c->pixmap_height);

        /* Redraw pixmap at the new size. */
        redraw_pixmap(c);
    }
    return TRUE;
}

/* Handler for expose_event on drawing area. */
static gboolean
expose_event (GtkWidget * widget, GdkEventExpose * event, CPUPlugin * c)
{
    /* Draw the requested part of the pixmap onto the drawing area.
     * Translate it in both x and y by the border size. */
    if (c->pixmap != NULL)
    {
        /*GdkDrawingContext *dc = gdk_window_begin_draw_frame(gtk_widget_get_window(widget),
                                gdk_window_get_visible_region(gtk_widget_get_window(widget)));
        cairo_t * cr = gdk_drawing_context_get_cairo_context(dc); */
        cairo_t * cr = gdk_cairo_create(gtk_widget_get_window(widget)); // deprecated
        gdk_cairo_region(cr, event->region);
        cairo_clip(cr);
        gdk_cairo_set_source_rgba(cr, &c->foreground_color);
        cairo_set_source_surface(cr, c->pixmap, BORDER_SIZE, BORDER_SIZE);
        cairo_paint(cr);
        /* check_cairo_status(cr); */
        cairo_destroy(cr);
    }
    return FALSE;
}

static gboolean
on_size_change (XfcePanelPlugin *plugin, gint size, void *data)
{
  GtkOrientation orientation;

  orientation = xfce_panel_plugin_get_orientation (plugin);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    gtk_widget_set_size_request (GTK_WIDGET (plugin), PLUGIN_WIDTH, size);
  else
    gtk_widget_set_size_request (GTK_WIDGET (plugin), size, PLUGIN_WIDTH);

  return TRUE;
}


//  Execute command on button press
static gboolean
on_button_press(GtkWidget *button, GdkEvent *event, gpointer data)
{
    if ( event->button.button == 1 ) //When left click
    {
    	if ( fork() == 0) {
    		wait(NULL);
    	} else {
            char arg0[7] = "lxtask";
            char* const  argv[2] = {arg0, NULL};
    		execvp("lxtask", argv);
    		perror("execvp");
    	}
        return TRUE;
    }
    return FALSE;
}

/* Plugin constructor. */
static void
cpu_constructor (XfcePanelPlugin *plugin)
{
    /* Allocate plugin context and set into Plugin private data pointer. */
    CPUPlugin * c = g_new0(CPUPlugin, 1);
    GtkWidget * p;

    c->show_percentage = TRUE;
    gdk_rgba_parse(&c->foreground_color, FG_COLOR);
    gdk_rgba_parse(&c->background_color, BG_COLOR);
    
    /* Allocate top level widget and set into Plugin widget pointer. */
    p = gtk_event_box_new();
    gtk_container_set_border_width(GTK_CONTAINER(p), 1);

    /* Allocate drawing area as a child of top level widget.  Enable button press events. */
    c->da = gtk_drawing_area_new();
    gtk_widget_add_events(c->da, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(p), c->da);
    gtk_container_add(GTK_CONTAINER(plugin), p);
    xfce_panel_plugin_add_action_widget (plugin, p);

    /* Connect signals. */
    g_signal_connect(G_OBJECT(c->da), "configure-event", G_CALLBACK(configure_event), (gpointer) c);
    g_signal_connect(G_OBJECT(c->da), "expose-event", G_CALLBACK(expose_event), (gpointer) c);
    g_signal_connect (G_OBJECT (plugin), "size-changed", G_CALLBACK(on_size_change), NULL);
    g_signal_connect (G_OBJECT (plugin), "free-data", G_CALLBACK (cpu_destructor), (gpointer) c);
    g_signal_connect (G_OBJECT (c->da), "button-press-event", G_CALLBACK (on_button_press), NULL);

    /* Show the widget.  Connect a timer to refresh the statistics. */
    gtk_widget_show_all(p);
    c->timer = g_timeout_add(1500, (GSourceFunc) cpu_update, (gpointer) c);
}

/* Plugin destructor. */
static void
cpu_destructor (XfcePanelPlugin *plugin, gpointer user_data)
{
    CPUPlugin * c = (CPUPlugin *)user_data;

    /* Disconnect the timer. */
    g_source_remove(c->timer);

    /* Deallocate memory. */
    cairo_surface_destroy(c->pixmap);
    g_free(c->stats_cpu);
    g_free(c);
}


