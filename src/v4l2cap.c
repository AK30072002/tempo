#define _GNU_SOURCE
#include "v4l2cap.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#ifndef V4L2_PIX_FMT_UYVY
#define V4L2_PIX_FMT_UYVY v4l2_fourcc('U','Y','V','Y')
#endif

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static const uint32_t ACCEPT_FMT[] = {
    v4l2_fourcc('U','Y','V','Y'),
    v4l2_fourcc('Y','U','Y','V'),
    v4l2_fourcc('N','V','1','6'),
    v4l2_fourcc('N','V','1','2'),
    v4l2_fourcc('Y','4','2','0'),
    v4l2_fourcc('G','R','E','Y'),
    0
};

static const char *fourcc_str(uint32_t f, char out[5])
{
    out[0] = (char)(f & 0xff);
    out[1] = (char)((f >> 8) & 0xff);
    out[2] = (char)((f >> 16) & 0xff);
    out[3] = (char)((f >> 24) & 0xff);
    out[4] = 0;
    return out;
}

static int fmt_supported(int fd, uint32_t fourcc)
{
    struct v4l2_fmtdesc fd_desc;
    memset(&fd_desc, 0, sizeof fd_desc);
    fd_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (fd_desc.index = 0; ; fd_desc.index++) {
        if (xioctl(fd, VIDIOC_ENUM_FMT, &fd_desc) < 0) return 0;
        if (fd_desc.pixelformat == fourcc) return 1;
    }
}

static uint32_t pick_format(int fd)
{
    int i;
    for (i = 0; ACCEPT_FMT[i]; i++)
        if (fmt_supported(fd, ACCEPT_FMT[i])) return ACCEPT_FMT[i];
    return 0;
}

static int open_one(v4l2cap_dev *d, int idx, uint32_t w, uint32_t h,
                    uint32_t fps, uint32_t pixfmt, uint32_t nbufs)
{
    char path[32];
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_streamparm parm;
    struct v4l2_requestbuffers req;
    uint32_t i;

    memset(d, 0, sizeof(*d));
    d->fd = -1;
    d->index = idx;

    snprintf(path, sizeof path, "/dev/video%d", idx);
    d->fd = open(path, O_RDWR | O_NONBLOCK, 0);
    if (d->fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno));
                     return -1; }

    memset(&cap, 0, sizeof cap);
    if (xioctl(d->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "%s: QUERYCAP: %s\n", path, strerror(errno));
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s: not a video-capture node "
                "(UVC cameras expose a metadata node too; try the other "
                "index -- 'v4l2-ctl --list-devices' shows the pairing)\n",
                path);
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s: no streaming support\n", path); return -1; }

    {
        uint32_t want = pixfmt;
        char a[5], b[5];

        if (want && !fmt_supported(d->fd, want)) {
            uint32_t alt = pick_format(d->fd);
            if (!alt) {
                fprintf(stderr,
                        "%s: requested %s unsupported and no uncompressed "
                        "format offered\n", path, fourcc_str(want, a));
                return -1;
            }
            fprintf(stderr, "%s: %s unsupported, using %s\n",
                    path, fourcc_str(want, a), fourcc_str(alt, b));
            want = alt;
        } else if (!want) {
            want = pick_format(d->fd);
            if (!want) {
                fprintf(stderr, "%s: no uncompressed format offered "
                        "(metadata node?)\n", path);
                return -1;
            }
        }

        memset(&fmt, 0, sizeof fmt);
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = w;
        fmt.fmt.pix.height      = h;
        fmt.fmt.pix.pixelformat = want;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
        if (xioctl(d->fd, VIDIOC_S_FMT, &fmt) < 0) {
            fprintf(stderr, "%s: S_FMT %s %ux%u: %s\n", path,
                    fourcc_str(want, a), w, h, strerror(errno));
            return -1;
        }
    }

    d->width  = fmt.fmt.pix.width;
    d->height = fmt.fmt.pix.height;
    d->pixfmt = fmt.fmt.pix.pixelformat;
    {
        char a[5];
        if (d->width != w || d->height != h)
            fprintf(stderr, "%s: driver chose %ux%u (asked %ux%u)\n",
                    path, d->width, d->height, w, h);
        fprintf(stderr, "%s: %s %ux%u\n", path,
                fourcc_str(d->pixfmt, a), d->width, d->height);
    }

    if (fps) {
        memset(&parm, 0, sizeof parm);
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = fps;
        if (xioctl(d->fd, VIDIOC_S_PARM, &parm) < 0)
            fprintf(stderr, "%s: S_PARM: %s (continuing)\n",
                    path, strerror(errno));
    }

    memset(&req, 0, sizeof req);
    req.count  = nbufs ? nbufs : 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(d->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "%s: REQBUFS: %s\n", path, strerror(errno));
        return -1;
    }
    if (req.count < 2) { fprintf(stderr, "%s: too few buffers\n", path);
                         return -1; }
    if (req.count > V4L2CAP_MAX_BUFS) req.count = V4L2CAP_MAX_BUFS;
    d->nbufs = req.count;

    for (i = 0; i < d->nbufs; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = i;
        if (xioctl(d->fd, VIDIOC_QUERYBUF, &b) < 0) {
            fprintf(stderr, "%s: QUERYBUF %u: %s\n", path, i, strerror(errno));
            return -1;
        }
        d->bufs[i].length = b.length;
        d->bufs[i].start  = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, d->fd, b.m.offset);
        if (d->bufs[i].start == MAP_FAILED) {
            fprintf(stderr, "%s: mmap %u: %s\n", path, i, strerror(errno));
            d->bufs[i].start = NULL;
            return -1;
        }
    }
    return 0;
}

int v4l2cap_open(v4l2cap *c, const int *cams, int ncams,
                 uint32_t width, uint32_t height, uint32_t fps,
                 uint32_t pixfmt, uint32_t nbufs)
{
    int i;
    if (!c || !cams || ncams < 1 || ncams > V4L2CAP_MAX_CAMS) return -1;
    memset(c, 0, sizeof(*c));
    c->ncams = ncams;
    for (i = 0; i < ncams; i++) {
        if (open_one(&c->dev[i], cams[i], width, height, fps,
                     pixfmt, nbufs) < 0) {
            v4l2cap_close(c);
            return -1;
        }
    }
    return 0;
}

int v4l2cap_start(v4l2cap *c)
{
    int i;
    uint32_t j;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (i = 0; i < c->ncams; i++) {
        v4l2cap_dev *d = &c->dev[i];
        for (j = 0; j < d->nbufs; j++) {
            struct v4l2_buffer b;
            memset(&b, 0, sizeof b);
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = j;
            if (xioctl(d->fd, VIDIOC_QBUF, &b) < 0) {
                fprintf(stderr, "video%d: QBUF %u: %s\n",
                        d->index, j, strerror(errno));
                return -1;
            }
        }
    }
    for (i = 0; i < c->ncams; i++) {
        if (xioctl(c->dev[i].fd, VIDIOC_STREAMON, &type) < 0) {
            fprintf(stderr, "video%d: STREAMON: %s\n",
                    c->dev[i].index, strerror(errno));
            return -1;
        }
    }
    c->streaming = 1;
    return 0;
}

int v4l2cap_stop(v4l2cap *c)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int i;
    if (!c->streaming) return 0;
    for (i = 0; i < c->ncams; i++)
        xioctl(c->dev[i].fd, VIDIOC_STREAMOFF, &type);
    c->streaming = 0;
    return 0;
}

void v4l2cap_close(v4l2cap *c)
{
    int i;
    uint32_t j;
    if (!c) return;

    v4l2cap_stop(c);

    for (i = 0; i < c->ncams; i++) {
        v4l2cap_dev *d = &c->dev[i];
        struct v4l2_requestbuffers req;

        for (j = 0; j < d->nbufs; j++)
            if (d->bufs[j].start) {
                munmap(d->bufs[j].start, d->bufs[j].length);
                d->bufs[j].start = NULL;
            }

        if (d->fd >= 0) {
            memset(&req, 0, sizeof req);
            req.count  = 0;
            req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            req.memory = V4L2_MEMORY_MMAP;
            xioctl(d->fd, VIDIOC_REQBUFS, &req);
        }
        d->nbufs = 0;

        if (d->fd >= 0) { close(d->fd); d->fd = -1; }
    }
    c->ncams = 0;
}

int v4l2cap_reset(const int *cams, int ncams)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int i, cleared = 0;

    for (i = 0; i < ncams; i++) {
        char path[32];
        int fd;
        struct v4l2_requestbuffers req;

        snprintf(path, sizeof path, "/dev/video%d", cams[i]);
        fd = open(path, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "  %s: %s\n", path, strerror(errno));
            continue;
        }
        xioctl(fd, VIDIOC_STREAMOFF, &type);
        memset(&req, 0, sizeof req);
        req.count  = 0;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        xioctl(fd, VIDIOC_REQBUFS, &req);
        close(fd);
        printf("  %s: streamoff + buffers released\n", path);
        cleared++;
    }
    return cleared;
}

int v4l2cap_next(v4l2cap *c, v4l2cap_frame *out, int timeout_ms)
{
    struct pollfd pfd[V4L2CAP_MAX_CAMS];
    int i, n;

    for (i = 0; i < c->ncams; i++) {
        pfd[i].fd = c->dev[i].fd;
        pfd[i].events = POLLIN;
        pfd[i].revents = 0;
    }

    n = poll(pfd, (nfds_t)c->ncams, timeout_ms);
    if (n < 0) { if (errno == EINTR) return 0; return -1; }
    if (n == 0) return 0;

    for (i = 0; i < c->ncams; i++) {
        v4l2cap_dev *d = &c->dev[i];
        struct v4l2_buffer b;

        if (!(pfd[i].revents & POLLIN)) continue;

        memset(&b, 0, sizeof b);
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(d->fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) continue;
            d->errors++;
            return -1;
        }

        if (!d->ts_flags_known) {
            d->ts_monotonic = (b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK)
                              == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
            d->ts_src_eof   = (b.flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK)
                              == V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
            d->ts_flags_known = 1;
        }

        if (d->have_seq && b.sequence > d->last_sequence + 1)
            d->seq_gaps += b.sequence - d->last_sequence - 1;
        d->last_sequence = b.sequence;
        d->have_seq = 1;

        d->frames++;

        out->cam       = i;
        out->t_ns      = (int64_t)b.timestamp.tv_sec * 1000000000LL
                       + (int64_t)b.timestamp.tv_usec * 1000LL;
        out->sequence  = b.sequence;
        out->bytesused = b.bytesused;
        out->data      = d->bufs[b.index].start;
        out->buf_index = b.index;
        return 1;
    }
    return 0;
}

int v4l2cap_release(v4l2cap *c, const v4l2cap_frame *f)
{
    struct v4l2_buffer b;
    if (!c || !f || f->cam < 0 || f->cam >= c->ncams) return -1;
    memset(&b, 0, sizeof b);
    b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index  = f->buf_index;
    if (xioctl(c->dev[f->cam].fd, VIDIOC_QBUF, &b) < 0) return -1;
    return 0;
}

int v4l2cap_get_ctrl(int cam_index, uint32_t ctrl_id, int32_t *out)
{
    char path[32];
    struct v4l2_control ctrl;
    int fd, rc;

    if (!ctrl_id || !out) return -1;
    snprintf(path, sizeof path, "/dev/video%d", cam_index);
    fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    memset(&ctrl, 0, sizeof ctrl);
    ctrl.id = ctrl_id;
    rc = xioctl(fd, VIDIOC_G_CTRL, &ctrl);
    if (rc == 0) *out = ctrl.value;
    close(fd);
    return rc;
}

static char norm_ch(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int is_sep(char c)
{
    return c == ' ' || c == '_' || c == '-' || c == ',' || c == '.';
}

static int name_matches(const char *have, const char *want)
{
    size_t i = 0, j = 0;
    for (;;) {
        while (is_sep(have[i])) i++;
        while (is_sep(want[j])) j++;
        if (!have[i] || !want[j]) break;
        if (norm_ch(have[i]) != norm_ch(want[j])) return 0;
        i++; j++;
    }
    while (is_sep(have[i])) i++;
    while (is_sep(want[j])) j++;
    return !have[i] && !want[j];
}

uint32_t v4l2cap_find_ctrl(int cam_index, const char *name)
{
    char path[32];
    struct v4l2_queryctrl q;
    int fd;
    uint32_t found = 0;

    snprintf(path, sizeof path, "/dev/video%d", cam_index);
    fd = open(path, O_RDWR);
    if (fd < 0) return 0;

    memset(&q, 0, sizeof q);
    q.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (xioctl(fd, VIDIOC_QUERYCTRL, &q) == 0) {
        if (!(q.flags & V4L2_CTRL_FLAG_DISABLED)
            && name_matches((const char *)q.name, name)) {
            found = q.id;
            break;
        }
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    close(fd);
    return found;
}

const char *v4l2cap_ts_desc(const v4l2cap *c, int cam)
{
    const v4l2cap_dev *d;
    if (!c || cam < 0 || cam >= c->ncams) return "invalid";
    d = &c->dev[cam];
    if (!d->ts_flags_known) return "unknown (no frame yet)";
    if (d->ts_monotonic && d->ts_src_eof)  return "MONOTONIC + EOF";
    if (d->ts_monotonic && !d->ts_src_eof) return "MONOTONIC + SoE";
    if (!d->ts_monotonic && d->ts_src_eof) return "COPY + EOF";
    return "COPY + SoE";
}
