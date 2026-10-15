#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gstnvdsmeta.h"
#include "nvdsmeta.h"

#define MAXCAM 8

typedef struct {
    guint64 n_batches;
    guint64 n_complete;
    guint64 n_short;
    guint64 n_frames;

    double  sum_spread_ms;
    double  max_spread_ms;
    guint32 hist[257];

    double  first_sum, last_sum;
    guint32 first_n, last_n;
    double  t_first, t_last;

    guint32 per_src[MAXCAM];
    int     ncams;
    FILE   *csv;
} probe_state;

static probe_state ST;

static double now_s(void)
{
    return (double)g_get_monotonic_time() / 1e6;
}

static void record(double spread_ms, double t_s)
{
    int b;
    ST.sum_spread_ms += spread_ms;
    if (spread_ms > ST.max_spread_ms) ST.max_spread_ms = spread_ms;

    b = (int)(spread_ms / 0.25);
    if (b < 0) b = 0;
    if (b > 256) b = 256;
    ST.hist[b]++;

    if (ST.first_n < 512) {
        ST.first_sum += spread_ms;
        ST.first_n++;
        ST.t_first = t_s;
    }
    if (ST.last_n < 512) { ST.last_sum += spread_ms; ST.last_n++; }
    else ST.last_sum += spread_ms - ST.last_sum / ST.last_n;
    ST.t_last = t_s;
}

static double pctile(double q)
{
    guint64 target = (guint64)(q * (double)ST.n_batches), acc = 0;
    int i;
    for (i = 0; i <= 256; i++) {
        acc += ST.hist[i];
        if (acc >= target) return (i + 0.5) * 0.25;
    }
    return ST.max_spread_ms;
}

static GstPadProbeReturn
mux_probe(GstPad *pad, GstPadProbeInfo *info, gpointer udata)
{
    GstBuffer *buf = (GstBuffer *)info->data;
    NvDsBatchMeta *bmeta;
    NvDsMetaList *l;
    gint64 lo = G_MAXINT64, hi = G_MININT64;
    int count = 0;
    double t = now_s();

    (void)pad; (void)udata;
    if (!buf) return GST_PAD_PROBE_OK;

    bmeta = gst_buffer_get_nvds_batch_meta(buf);
    if (!bmeta) return GST_PAD_PROBE_OK;

    for (l = bmeta->frame_meta_list; l != NULL; l = l->next) {
        NvDsFrameMeta *fm = (NvDsFrameMeta *)l->data;
        gint64 pts;
        if (!fm) continue;

        pts = (gint64)fm->buf_pts;
        if (pts <= 0) continue;

        if (pts < lo) lo = pts;
        if (pts > hi) hi = pts;
        count++;

        if (fm->source_id < MAXCAM) ST.per_src[fm->source_id]++;
        if (ST.csv)
            fprintf(ST.csv, "%u,%.9f\n",
                    fm->source_id, (double)pts / 1e9);
    }

    if (count >= 2) {
        ST.n_batches++;
        ST.n_frames += count;
        if (count >= ST.ncams) ST.n_complete++;
        else ST.n_short++;
        record((double)(hi - lo) / 1e6, t);
    }
    return GST_PAD_PROBE_OK;
}

static GMainLoop *loop;

static gboolean on_timeout(gpointer d)
{
    (void)d;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gboolean on_bus(GstBus *bus, GstMessage *msg, gpointer d)
{
    (void)bus; (void)d;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *e = NULL; gchar *dbg = NULL;
        gst_message_parse_error(msg, &e, &dbg);
        g_printerr("ERROR: %s\n", e->message);
        if (dbg) g_printerr("  %s\n", dbg);
        g_clear_error(&e); g_free(dbg);
        g_main_loop_quit(loop);
        break;
    }
    case GST_MESSAGE_EOS:
        g_main_loop_quit(loop);
        break;
    default: break;
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    GstElement *pipeline, *mux, *sink;
    GstPad *srcpad;
    GstBus *bus;
    int cams[MAXCAM] = {0, 1, 2, 3};
    int ncams = 4, i;
    int width = 640, height = 480, fps = 100;
    double seconds = 60.0;
    const char *csv_path = NULL;
    int push_timeout_us = -1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cams") && i + 1 < argc) {
            char *s = argv[++i], *tok;
            ncams = 0;
            for (tok = strtok(s, ","); tok && ncams < MAXCAM;
                 tok = strtok(NULL, ",")) cams[ncams++] = atoi(tok);
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &width, &height);
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            fps = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--csv") && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (!strcmp(argv[i], "--push-timeout") && i + 1 < argc) {
            push_timeout_us = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--cams 0,1,2,3] [--size WxH] [--fps N]\n"
                   "          [--seconds N] [--csv out.csv]\n"
                   "          [--push-timeout US]\n"
                   "\n"
                   "Measures the real temporal alignment of nvstreammux\n"
                   "batches. With --csv, writes per-frame timestamps so the\n"
                   "SAME frames can be replayed through tempo_replay.\n",
                   argv[0]);
            return 0;
        }
    }

    gst_init(&argc, &argv);
    memset(&ST, 0, sizeof ST);
    ST.ncams = ncams;
    if (csv_path) {
        ST.csv = fopen(csv_path, "w");
        if (ST.csv) fprintf(ST.csv, "camera,wall_monotonic_s\n");
        else g_printerr("warning: cannot write %s\n", csv_path);
    }

    pipeline = gst_pipeline_new("muxprobe");
    mux  = gst_element_factory_make("nvstreammux", "mux");
    sink = gst_element_factory_make("fakesink", "sink");
    if (!pipeline || !mux || !sink) {
        g_printerr("failed to create elements (is DeepStream installed?)\n");
        return 1;
    }

    g_object_set(G_OBJECT(mux),
                 "batch-size", ncams,
                 "width", width, "height", height,
                 "live-source", 1,
                 NULL);
    if (push_timeout_us >= 0)
        g_object_set(G_OBJECT(mux), "batched-push-timeout",
                     push_timeout_us, NULL);
    g_object_set(G_OBJECT(sink), "sync", FALSE, "async", FALSE, NULL);

    gst_bin_add_many(GST_BIN(pipeline), mux, sink, NULL);
    if (!gst_element_link(mux, sink)) {
        g_printerr("cannot link mux -> sink\n"); return 1;
    }

    for (i = 0; i < ncams; i++) {
        GstElement *src, *conv, *q;
        GstCaps *caps;
        gchar name[32], padname[32];
        GstPad *sinkpad, *srcpad_q;

        g_snprintf(name, sizeof name, "src%d", i);
        src  = gst_element_factory_make("nvv4l2camerasrc", name);
        g_snprintf(name, sizeof name, "conv%d", i);
        conv = gst_element_factory_make("nvvideoconvert", name);
        g_snprintf(name, sizeof name, "q%d", i);
        q    = gst_element_factory_make("queue", name);
        if (!src || !conv || !q) {
            g_printerr("cannot create source chain %d\n", i); return 1;
        }

        g_snprintf(name, sizeof name, "/dev/video%d", cams[i]);
        g_object_set(G_OBJECT(src), "device", name, NULL);
        g_object_set(G_OBJECT(q), "max-size-buffers", 1,
                     "leaky", 2  , NULL);

        gst_bin_add_many(GST_BIN(pipeline), src, conv, q, NULL);

        caps = gst_caps_from_string(
            g_strdup_printf("video/x-raw(memory:NVMM),format=UYVY,"
                            "width=%d,height=%d,framerate=%d/1",
                            width, height, fps));
        if (!gst_element_link_filtered(src, conv, caps)) {
            g_printerr("cannot link src%d -> conv (mode unsupported?)\n", i);
            return 1;
        }
        gst_caps_unref(caps);

        caps = gst_caps_from_string(
            "video/x-raw(memory:NVMM),format=NV12");
        if (!gst_element_link_filtered(conv, q, caps)) {
            g_printerr("cannot link conv%d -> queue\n", i); return 1;
        }
        gst_caps_unref(caps);

        g_snprintf(padname, sizeof padname, "sink_%d", i);
        sinkpad = gst_element_get_request_pad(mux, padname);
        srcpad_q = gst_element_get_static_pad(q, "src");
        if (!sinkpad || !srcpad_q ||
            gst_pad_link(srcpad_q, sinkpad) != GST_PAD_LINK_OK) {
            g_printerr("cannot link queue%d -> mux.%s\n", i, padname);
            return 1;
        }
        gst_object_unref(sinkpad);
        gst_object_unref(srcpad_q);
    }

    srcpad = gst_element_get_static_pad(mux, "src");
    gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER, mux_probe,
                      NULL, NULL);
    gst_object_unref(srcpad);

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, on_bus, NULL);
    gst_object_unref(bus);

    {
        gint bt = 0;
        g_object_get(G_OBJECT(mux), "batched-push-timeout", &bt, NULL);
        printf("nvmux_probe -- measuring real nvstreammux alignment\n\n");
        printf("  cameras %d   %dx%d @ %d fps\n", ncams, width, height, fps);
        printf("  nvstreammux batch-size=%d  batched-push-timeout=%d us%s\n",
               ncams, bt,
               push_timeout_us >= 0 ? " (set)" : " (element default)");
        printf("  duration %.0f s\n\n", seconds);
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add((guint)(seconds * 1000), on_timeout, NULL);
    g_main_loop_run(loop);
    gst_element_set_state(pipeline, GST_STATE_NULL);

    printf("================================================================\n");
    printf("NVSTREAMMUX MEASURED ALIGNMENT\n");
    printf("================================================================\n");
    if (!ST.n_batches) {
        printf("  no batches captured -- check the camera modes\n");
    } else {
        printf("  batches        %llu  (complete %llu, short %llu = %.2f%%)\n",
               (unsigned long long)ST.n_batches,
               (unsigned long long)ST.n_complete,
               (unsigned long long)ST.n_short,
               100.0 * (double)ST.n_short / (double)ST.n_batches);
        printf("  frames         %llu\n", (unsigned long long)ST.n_frames);
        printf("  per-source     ");
        for (i = 0; i < ncams; i++) printf("%u ", ST.per_src[i]);
        printf("\n\n");
        printf("  intra-batch timestamp spread (ms)\n");
        printf("    mean %8.4f   p50 %8.4f   p95 %8.4f   max %8.4f\n",
               ST.sum_spread_ms / (double)ST.n_batches,
               pctile(0.50), pctile(0.95), ST.max_spread_ms);
        if (ST.first_n >= 64 && ST.last_n >= 64) {
            double f = ST.first_sum / ST.first_n;
            double l = ST.last_sum  / ST.last_n;
            double mins = (ST.t_last - ST.t_first) / 60.0;
            printf("    first 512 %.3f -> last 512 %.3f ms   %+.4f ms/min\n",
                   f, l, mins > 0 ? (l - f) / mins : 0.0);
        }
        printf("\n  NOTE: a SHORT batch is nvstreammux forwarding before every\n");
        printf("        source delivered, which bounds latency but means the\n");
        printf("        batch does not contain all cameras. Both counts are\n");
        printf("        reported because ignoring short batches would flatter\n");
        printf("        the measured spread.\n");
    }

    if (ST.csv) {
        fclose(ST.csv);
        printf("\n  wrote %s\n", csv_path);
        printf("  compare on the SAME frames:  ./tempo_replay %s\n", csv_path);
    }

    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    return 0;
}
