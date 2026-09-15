/*
 * DS1 long-run soak: stream 1920x1080 NV12 continuously for a fixed duration
 * and report what a short capture cannot show -- dropped frames, sustained
 * rate, and whether the content stays live.
 *
 * Usage: ds1soak <seconds> [width] [height]
 *
 * Drops are counted from v4l2_buffer.sequence, which the driver increments per
 * frame the ISP completes, so a gap means a frame was produced and never
 * delivered. A "static" frame (checksum identical to the previous one) would
 * mean the pipeline stopped updating while still handing buffers back, which a
 * frame count alone would not catch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

#define NBUF 4

static int openv(void)
{
    int fd = open("/dev/video1", O_RDWR);
    if (fd < 0) { perror("open"); exit(1); }
    return fd;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/*
 * Mean absolute difference against the previous frame, over a strided sample.
 *
 * A plain equal/not-equal hash cannot tell a frozen pipeline from a static
 * scene: on a dark, still scene most sampled pixels genuinely repeat. The
 * magnitude does distinguish them -- a live sensor with temper bypassed (so no
 * temporal denoise) always carries some frame-to-frame noise, while a pipeline
 * handing back a stale buffer gives exactly 0.000 every time.
 *
 * Sampled every 64 bytes rather than every 4096: ~32k points per frame, enough
 * to be stable, still ~1% of the plane.
 */
#define MAD_STRIDE 64
static double frame_mad(const unsigned char *cur, const unsigned char *prev, unsigned int len)
{
    unsigned long sum = 0, n = 0;
    unsigned int i;
    for (i = 0; i < len; i += MAD_STRIDE) {
        int d = (int)cur[i] - (int)prev[i];
        sum += (d < 0 ? -d : d);
        n++;
    }
    return n ? (double)sum / n : 0.0;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 1800;
    int w = argc > 2 ? atoi(argv[2]) : 1920;
    int h = argc > 3 ? atoi(argv[3]) : 1080;
    int fr = openv(), meta = openv(), ds1 = openv();
    struct v4l2_format f;
    struct v4l2_requestbuffers rb;
    void *map[NBUF][2];
    unsigned int len[NBUF][2];
    int i, type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    unsigned long got = 0, drops = 0, frozen = 0, timeouts = 0, shorts = 0;
    unsigned int prev_seq = 0; int have_prev = 0;
    unsigned char *prev_y = NULL; int have_prev_y = 0;
    double mad_sum = 0.0, mad_min = 1e9, mad_max = 0.0;
    double t0, tlast, tend;
    unsigned long got_last = 0;

    (void)fr; (void)meta;

    memset(&f, 0, sizeof f);
    f.type = type;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (ioctl(ds1, VIDIOC_S_FMT, &f) < 0) { perror("S_FMT"); return 1; }
    printf("DS1 %ux%u bpl=%u size0=%u size1=%u, soaking %d s\n",
           f.fmt.pix_mp.width, f.fmt.pix_mp.height,
           f.fmt.pix_mp.plane_fmt[0].bytesperline,
           f.fmt.pix_mp.plane_fmt[0].sizeimage, f.fmt.pix_mp.plane_fmt[1].sizeimage, secs);
    fflush(stdout);

    memset(&rb, 0, sizeof rb);
    rb.count = NBUF; rb.type = type; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(ds1, VIDIOC_REQBUFS, &rb) < 0) { perror("REQBUFS"); return 1; }

    for (i = 0; i < (int)rb.count; i++) {
        struct v4l2_buffer b; struct v4l2_plane p[2]; int j;
        memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i; b.length = 2; b.m.planes = p;
        if (ioctl(ds1, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return 1; }
        for (j = 0; j < 2; j++) {
            len[i][j] = p[j].length;
            map[i][j] = mmap(NULL, p[j].length, PROT_READ, MAP_SHARED, ds1, p[j].m.mem_offset);
            if (map[i][j] == MAP_FAILED) { perror("mmap"); return 1; }
        }
        if (ioctl(ds1, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return 1; }
    }

    if (ioctl(ds1, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }
    t0 = tlast = now(); tend = t0 + secs;

    while (now() < tend) {
        struct v4l2_buffer b; struct v4l2_plane p[2];
        fd_set fds; struct timeval tv;
        double t;
        FD_ZERO(&fds); FD_SET(ds1, &fds);
        tv.tv_sec = 5; tv.tv_usec = 0;
        i = select(ds1 + 1, &fds, NULL, NULL, &tv);
        if (i < 0) { if (errno == EINTR) continue; perror("select"); break; }
        if (i == 0) {
            timeouts++;
            printf("[%6.0fs] SELECT TIMEOUT (#%lu) after %lu frames\n", now() - t0, timeouts, got);
            fflush(stdout);
            if (timeouts >= 3) { printf("giving up after 3 timeouts\n"); break; }
            continue;
        }
        memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.length = 2; b.m.planes = p;
        if (ioctl(ds1, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }

        if (p[0].bytesused != len[b.index][0] || p[1].bytesused != len[b.index][1])
            shorts++;

        if (have_prev && b.sequence > prev_seq + 1)
            drops += b.sequence - prev_seq - 1;
        prev_seq = b.sequence; have_prev = 1;

        if (!prev_y) prev_y = malloc(len[b.index][0]);
        if (prev_y) {
            if (have_prev_y) {
                double mad = frame_mad(map[b.index][0], prev_y, len[b.index][0]);
                if (mad == 0.0) frozen++;
                mad_sum += mad;
                if (mad < mad_min) mad_min = mad;
                if (mad > mad_max) mad_max = mad;
            }
            memcpy(prev_y, map[b.index][0], len[b.index][0]);
            have_prev_y = 1;
        }

        if (ioctl(ds1, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); break; }
        got++;

        t = now();
        if (t - tlast >= 60.0) {
            printf("[%6.0fs] frames=%-8lu %.2f fps (last min %.2f)  drops=%lu frozen=%lu short=%lu timeouts=%lu  mad avg=%.3f min=%.3f max=%.3f\n",
                   t - t0, got, got / (t - t0), (got - got_last) / (t - tlast),
                   drops, frozen, shorts, timeouts,
                   got > 1 ? mad_sum / (got - 1) : 0.0, mad_min, mad_max);
            fflush(stdout);
            tlast = t; got_last = got;
        }
    }

    ioctl(ds1, VIDIOC_STREAMOFF, &type);
    printf("SOAK DONE: %.0f s, frames=%lu, %.2f fps avg, drops=%lu, frozen=%lu, short=%lu, timeouts=%lu, mad avg=%.3f min=%.3f max=%.3f\n",
           now() - t0, got, got / (now() - t0), drops, frozen, shorts, timeouts,
           got > 1 ? mad_sum / (got - 1) : 0.0, mad_min, mad_max);
    return (timeouts || shorts || frozen) ? 1 : 0;
}
