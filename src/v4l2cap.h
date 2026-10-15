#ifndef V4L2CAP_H
#define V4L2CAP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define V4L2CAP_MAX_CAMS 8
#define V4L2CAP_MAX_BUFS 8

typedef struct {
    void   *start;
    size_t  length;
} v4l2cap_buf;

typedef struct {
    int          fd;
    int          index;
    uint32_t     width, height;
    uint32_t     pixfmt;
    v4l2cap_buf  bufs[V4L2CAP_MAX_BUFS];
    uint32_t     nbufs;

    uint8_t      ts_monotonic;
    uint8_t      ts_src_eof;
    uint8_t      ts_flags_known;

    uint64_t     frames;
    uint64_t     errors;
    uint64_t     seq_gaps;
    uint32_t     last_sequence;
    uint8_t      have_seq;
} v4l2cap_dev;

typedef struct {
    v4l2cap_dev dev[V4L2CAP_MAX_CAMS];
    int         ncams;
    int         streaming;
} v4l2cap;

typedef struct {
    int      cam;
    int64_t  t_ns;
    uint32_t sequence;
    uint32_t bytesused;
    void    *data;
    uint32_t buf_index;
} v4l2cap_frame;

int v4l2cap_open(v4l2cap *c, const int *cams, int ncams,
                 uint32_t width, uint32_t height, uint32_t fps,
                 uint32_t pixfmt, uint32_t nbufs);

int v4l2cap_start(v4l2cap *c);
int v4l2cap_stop(v4l2cap *c);
void v4l2cap_close(v4l2cap *c);

int v4l2cap_next(v4l2cap *c, v4l2cap_frame *out, int timeout_ms);

int v4l2cap_release(v4l2cap *c, const v4l2cap_frame *f);

int v4l2cap_get_ctrl(int cam_index, uint32_t ctrl_id, int32_t *out);

uint32_t v4l2cap_find_ctrl(int cam_index, const char *name);

int v4l2cap_reset(const int *cams, int ncams);

const char *v4l2cap_ts_desc(const v4l2cap *c, int cam);

#ifdef __cplusplus
}
#endif
#endif
