#include "v4l2cap.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/videodev2.h>

#define MAXC   V4L2CAP_MAX_CAMS
#define NSPOT  8

static volatile sig_atomic_t stop_flag = 0;
static v4l2cap *g_cap = NULL;

static void cleanup(void)
{
    if (g_cap) { v4l2cap_close(g_cap); g_cap = NULL; }
}

static void on_signal(int s)
{
    (void)s;
    if (stop_flag) { cleanup(); _exit(130); }
    stop_flag = 1;
}

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

typedef struct {
    uint32_t w, h, bpl, fourcc;
} geom_t;

static geom_t G[MAXC];

static int get_geom(v4l2cap *c, int i, geom_t *g)
{
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(c->dev[i].fd, VIDIOC_G_FMT, &f) == 0) {
        g->w = f.fmt.pix.width;
        g->h = f.fmt.pix.height;
        g->bpl = f.fmt.pix.bytesperline ? f.fmt.pix.bytesperline : g->w * 2;
        g->fourcc = f.fmt.pix.pixelformat;
    } else {
        g->w = c->dev[i].width;
        g->h = c->dev[i].height;
        g->bpl = g->w * 2;
        g->fourcc = c->dev[i].pixfmt;
    }
    if (g->fourcc != V4L2_PIX_FMT_UYVY && g->fourcc != V4L2_PIX_FMT_YUYV) {
        fprintf(stderr, "camera %d: pixel format is not UYVY/YUYV; "
                        "ledcap cannot read it\n", i);
        return -1;
    }
    return 0;
}

static inline int red_at(const uint8_t *p, const geom_t *g, uint32_t used,
                         int x, int y)
{
    size_t base;
    int Y, V, r;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x >= g->w) x = (int)g->w - 1;
    if ((uint32_t)y >= g->h) y = (int)g->h - 1;
    base = (size_t)y * g->bpl + (size_t)(x & ~1) * 2;
    if (base + 3 >= used) return 0;
    if (g->fourcc == V4L2_PIX_FMT_UYVY) {
        Y = p[base + 1 + (x & 1) * 2];
        V = p[base + 2];
    } else {
        Y = p[base + (x & 1) * 2];
        V = p[base + 3];
    }
    r = Y + ((V - 128) * 359) / 256;
    return r < 0 ? 0 : (r > 255 ? 255 : r);
}

static void write_pgm(const char *path, const uint8_t *img, uint32_t w,
                      uint32_t h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P5\n%u %u\n255\n", w, h);
    fwrite(img, 1, (size_t)w * h, f);
    fclose(f);
}

static void write_ppm(const char *path, const uint8_t *p, const geom_t *g,
                      uint32_t used)
{
    FILE *f = fopen(path, "wb");
    uint32_t x, y;
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P6\n%u %u\n255\n", g->w, g->h);
    for (y = 0; y < g->h; y++)
        for (x = 0; x < g->w; x++) {
            size_t base = (size_t)y * g->bpl + (size_t)(x & ~1u) * 2;
            int Y, U, V, r, gg, b;
            unsigned char px[3];
            if (base + 3 >= used) { px[0] = px[1] = px[2] = 0; }
            else {
                if (g->fourcc == V4L2_PIX_FMT_UYVY) {
                    U = p[base]; Y = p[base + 1 + (x & 1) * 2]; V = p[base + 2];
                } else {
                    Y = p[base + (x & 1) * 2]; U = p[base + 1]; V = p[base + 3];
                }
                r  = Y + ((V - 128) * 359) / 256;
                gg = Y - ((U - 128) * 88 + (V - 128) * 183) / 256;
                b  = Y + ((U - 128) * 454) / 256;
                px[0] = (unsigned char)(r  < 0 ? 0 : r  > 255 ? 255 : r);
                px[1] = (unsigned char)(gg < 0 ? 0 : gg > 255 ? 255 : gg);
                px[2] = (unsigned char)(b  < 0 ? 0 : b  > 255 ? 255 : b);
            }
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

static void red_plane(const uint8_t *p, const geom_t *g, uint32_t used,
                      uint8_t *out)
{
    uint32_t x, y;
    int uyvy = (g->fourcc == V4L2_PIX_FMT_UYVY);
    for (y = 0; y < g->h; y++) {
        const uint8_t *row = p + (size_t)y * g->bpl;
        uint8_t *o = out + (size_t)y * g->w;
        if ((size_t)y * g->bpl + (size_t)g->w * 2 > used) {
            memset(o, 0, g->w);
            continue;
        }
        for (x = 0; x + 1 < g->w; x += 2, row += 4) {
            int Y0, Y1, V, dv, r0, r1;
            if (uyvy) { Y0 = row[1]; V = row[2]; Y1 = row[3]; }
            else      { Y0 = row[0]; Y1 = row[2]; V = row[3]; }
            dv = ((V - 128) * 359) / 256;
            r0 = Y0 + dv; r1 = Y1 + dv;
            o[x]     = (uint8_t)(r0 < 0 ? 0 : r0 > 255 ? 255 : r0);
            o[x + 1] = (uint8_t)(r1 < 0 ? 0 : r1 > 255 ? 255 : r1);
        }
    }
}

static int roi_x[MAXC][NSPOT], roi_y[MAXC][NSPOT], roi_ok[MAXC];

static int load_roi(const char *path, int ncams)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int i, n = 0;
    if (!f) { fprintf(stderr, "cannot open ROI file %s\n", path); return -1; }
    memset(roi_ok, 0, sizeof roi_ok);
    while (fgets(line, sizeof line, f)) {
        int c, v[2 * NSPOT], k = 0, off = 0, used;
        char *s = line;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(s, "%d%n", &c, &used) != 1) continue;
        off += used;
        for (k = 0; k < 2 * NSPOT; k++) {
            if (sscanf(s + off, "%d%n", &v[k], &used) != 1) break;
            off += used;
        }
        if (k != 2 * NSPOT || c < 0 || c >= ncams) continue;
        for (i = 0; i < NSPOT; i++) {
            roi_x[c][i] = v[2 * i];
            roi_y[c][i] = v[2 * i + 1];
        }
        roi_ok[c] = 1;
        n++;
    }
    fclose(f);
    for (i = 0; i < ncams; i++)
        if (!roi_ok[i]) {
            fprintf(stderr, "ROI file has no line for camera %d\n", i);
            return -1;
        }
    return n;
}

int main(int argc, char **argv)
{
    int cams[MAXC] = {0, 1, 2, 3}, ncams = 4;
    uint32_t w = 640, h = 480, fps = 100;
    double seconds = 60.0;
    int snap = 0, box = 2, save_every = 6000;
    const char *roi_path = NULL, *out = NULL;
    v4l2cap cap;
    v4l2cap_frame fr;
    char path[1024];
    FILE *csv = NULL;
    uint8_t *maxp[MAXC] = {0}, *tmp = NULL;
    uint64_t got[MAXC] = {0}, proc[MAXC] = {0};
    int i, rc;
    int64_t t_end, t_check;
    int checked = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cams") && i + 1 < argc) {
            char *s = argv[++i], *tok;
            ncams = 0;
            for (tok = strtok(s, ","); tok && ncams < MAXC;
                 tok = strtok(NULL, ","))
                cams[ncams++] = atoi(tok);
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            sscanf(argv[++i], "%ux%u", &w, &h);
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            fps = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--snap") && i + 1 < argc) {
            snap = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--roi") && i + 1 < argc) {
            roi_path = argv[++i];
        } else if (!strcmp(argv[i], "--box") && i + 1 < argc) {
            box = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--save-every") && i + 1 < argc) {
            save_every = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            out = argv[++i];
        } else {
            fprintf(stderr,
                "usage: %s --out PREFIX [--cams 0,1,2,3] [--size WxH] "
                "[--fps N]\n"
                "          (--snap N | --roi roi.txt --seconds S "
                "[--save-every K] [--box R])\n", argv[0]);
            return 2;
        }
    }
    if (!out || (!snap && !roi_path) || ncams < 1) {
        fprintf(stderr, "need --out and one of --snap / --roi\n");
        return 2;
    }
    if (box < 0) box = 0;
    if (box > 6) box = 6;
    if (roi_path && load_roi(roi_path, ncams) < 0) return 1;

    if (v4l2cap_open(&cap, cams, ncams, w, h, fps, 0, 6) < 0) {
        fprintf(stderr, "capture setup failed (try: ./tempo_sync --reset)\n");
        return 1;
    }
    g_cap = &cap;
    atexit(cleanup);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    for (i = 0; i < ncams; i++)
        if (get_geom(&cap, i, &G[i]) < 0) return 1;

    if (snap) {
        for (i = 0; i < ncams; i++) {
            maxp[i] = calloc((size_t)G[i].w * G[i].h, 1);
            if (!maxp[i]) { fprintf(stderr, "out of memory\n"); return 1; }
        }
        tmp = malloc((size_t)G[0].w * G[0].h * 4);
        if (!tmp) { fprintf(stderr, "out of memory\n"); return 1; }
        seconds = 60.0;
    } else {
        snprintf(path, sizeof path, "%s.csv", out);
        csv = fopen(path, "w");
        if (!csv) { fprintf(stderr, "cannot write %s\n", path); return 1; }
        fprintf(csv, "camera,wall_monotonic_s,seq");
        for (i = 0; i < NSPOT; i++) fprintf(csv, ",v%d", i);
        fprintf(csv, "\n");
        if (save_every > 0) {
            snprintf(path, sizeof path, "%s_frames", out);
            mkdir(path, 0775);
        }
        tmp = malloc((size_t)G[0].w * G[0].h * 4);
    }

    {
        FILE *m;
        snprintf(path, sizeof path, "%s_meta.txt", out);
        m = fopen(path, "w");
        if (m) {
            uint32_t sid = v4l2cap_find_ctrl(cams[0], "frame_sync");
            uint32_t eid = v4l2cap_find_ctrl(cams[0], "exposure_time_absolute");
            uint32_t gid = v4l2cap_find_ctrl(cams[0], "gain");
            int32_t v;
            fprintf(m, "mode %s\nrequested %ux%u @ %u fps\n",
                    snap ? "snap" : "measure", w, h, fps);
            for (i = 0; i < ncams; i++)
                fprintf(m, "cam%d /dev/video%d %ux%u bpl=%u fourcc=%c%c%c%c\n",
                        i, cams[i], G[i].w, G[i].h, G[i].bpl,
                        G[i].fourcc & 0xff, (G[i].fourcc >> 8) & 0xff,
                        (G[i].fourcc >> 16) & 0xff, (G[i].fourcc >> 24) & 0xff);
            if (sid && v4l2cap_get_ctrl(cams[0], sid, &v) == 0)
                fprintf(m, "frame_sync %d\n", v);
            if (eid && v4l2cap_get_ctrl(cams[0], eid, &v) == 0)
                fprintf(m, "exposure_time_absolute %d\n", v);
            if (gid && v4l2cap_get_ctrl(cams[0], gid, &v) == 0)
                fprintf(m, "gain %d\n", v);
            fprintf(m, "box_radius %d\nsave_every %d\n", box, save_every);
            if (roi_path) {
                int s;
                for (i = 0; i < ncams; i++) {
                    fprintf(m, "roi cam%d", i);
                    for (s = 0; s < NSPOT; s++)
                        fprintf(m, " %d,%d", roi_x[i][s], roi_y[i][s]);
                    fprintf(m, "\n");
                }
            }
            fclose(m);
        }
    }

    if (v4l2cap_start(&cap) < 0) return 1;
    printf("ledcap: %s, %d cameras, %ux%u @ %u fps%s\n",
           snap ? "SETUP snapshot" : "MEASUREMENT", ncams, G[0].w, G[0].h,
           fps, snap ? "" : "");
    fflush(stdout);

    t_end = mono_ns() + (int64_t)(seconds * 1e9);
    t_check = mono_ns() + 5000000000LL;

    while (!stop_flag && mono_ns() < t_end) {
        const uint8_t *p;
        const geom_t *g;
        rc = v4l2cap_next(&cap, &fr, 500);
        if (rc < 0) { fprintf(stderr, "capture error\n"); break; }
        if (rc == 0) continue;
        p = (const uint8_t *)fr.data;
        g = &G[fr.cam];
        got[fr.cam]++;

        if (snap) {
            int c = fr.cam, k, use = 1;
            uint64_t minp = (uint64_t)-1;
            for (k = 0; k < ncams; k++)
                if (got[k] > 0 && proc[k] < (uint64_t)snap && proc[k] < minp)
                    minp = proc[k];
            if (proc[c] >= (uint64_t)snap || proc[c] > minp) use = 0;
            if (use) {
                uint8_t *mp = maxp[c];
                size_t n = (size_t)g->w * g->h, j;
                red_plane(p, g, fr.bytesused, tmp);
                for (j = 0; j < n; j++) if (tmp[j] > mp[j]) mp[j] = tmp[j];
                proc[c]++;
                if (proc[c] == (uint64_t)snap) {
                    snprintf(path, sizeof path, "%s_cam%d_last.ppm", out, c);
                    write_ppm(path, p, g, fr.bytesused);
                }
            }
            v4l2cap_release(&cap, &fr);
            {
                int done = 1;
                for (i = 0; i < ncams; i++)
                    if (proc[i] < (uint64_t)snap) done = 0;
                if (done) break;
            }
            continue;
        }

        {
            int s, dx, dy;
            fprintf(csv, "%d,%.9f,%u", fr.cam, (double)fr.t_ns / 1e9,
                    fr.sequence);
            for (s = 0; s < NSPOT; s++) {
                int acc = 0, cnt = 0;
                for (dy = -box; dy <= box; dy++)
                    for (dx = -box; dx <= box; dx++) {
                        acc += red_at(p, g, fr.bytesused,
                                      roi_x[fr.cam][s] + dx,
                                      roi_y[fr.cam][s] + dy);
                        cnt++;
                    }
                fprintf(csv, ",%d", (acc + cnt / 2) / cnt);
            }
            fprintf(csv, "\n");
        }
        if (save_every > 0 && tmp &&
            (got[fr.cam] == 1 || got[fr.cam] % (uint64_t)save_every == 0)) {
            red_plane(p, g, fr.bytesused, tmp);
            snprintf(path, sizeof path, "%s_frames/cam%d_seq%08u.pgm", out,
                     fr.cam, fr.sequence);
            write_pgm(path, tmp, g->w, g->h);
        }
        v4l2cap_release(&cap, &fr);

        if (!checked && mono_ns() > t_check) {
            uint64_t hi = 0, lo = (uint64_t)-1;
            int bad = -1;
            checked = 1;
            for (i = 0; i < ncams; i++) {
                if (got[i] > hi) hi = got[i];
                if (got[i] < lo) { lo = got[i]; bad = i; }
            }
            if (hi && lo * 2 < hi) {
                fprintf(stderr, "HARDWARE FAULT: cam%d delivered %llu frames, "
                        "fastest %llu. Check cables; run ./tempo_sync --reset; "
                        "power-cycle if needed.\n", bad,
                        (unsigned long long)lo, (unsigned long long)hi);
                break;
            }
        }
    }

    v4l2cap_stop(&cap);

    if (snap) {
        for (i = 0; i < ncams; i++) {
            snprintf(path, sizeof path, "%s_cam%d_max.pgm", out, i);
            write_pgm(path, maxp[i], G[i].w, G[i].h);
            if (proc[i] < (uint64_t)snap)
                fprintf(stderr, "WARNING: cam%d: only %llu of %d frames used "
                        "(%llu delivered) -- snapshot incomplete\n", i,
                        (unsigned long long)proc[i], snap,
                        (unsigned long long)got[i]);
        }
        printf("ledcap: wrote %s_camN_max.pgm and _last.ppm\n", out);
    } else {
        fclose(csv);
        printf("ledcap: wrote %s.csv\n", out);
    }
    for (i = 0; i < ncams; i++)
        printf("  cam%d: %llu frames, %llu kernel sequence gaps, %s\n", i,
               (unsigned long long)got[i],
               (unsigned long long)cap.dev[i].seq_gaps,
               v4l2cap_ts_desc(&cap, i));
    cleanup();
    return 0;
}
