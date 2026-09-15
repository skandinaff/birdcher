/*
 * DS1 -> stdout bridge, for feeding ffmpeg.
 *
 * ffmpeg cannot open the DS1 stream itself. Stream ids on /dev/video1 are
 * assigned by open order -- 0=FR, 1=META, 2=DS1 -- so a single open() lands on
 * FR at 3864x2192. This opens the node three times, streams the third handle,
 * and writes raw NV12 frames to stdout.
 *
 *   ds1stream <seconds> <out_fps> [width] [height] [native_fps]
 *
 * Capture always runs at the sensor's native rate; out_fps decimates on the
 * way to stdout, because 1920x1080 NV12 at 60 fps is ~186 MB/s down the pipe
 * and the encoder does not need it.
 *
 * native_fps is what the sensor is actually producing (default 60). It is not
 * fixed: the imx415 subdev's vertical_blanking control sets the frame period,
 * and lowering the sensor to the rate you actually want is much cheaper than
 * capturing at 60 and discarding frames. At VBLANK 6808 the sensor runs at
 * 15 fps, and then out_fps 15 needs no decimation at all. Pass out_fps 0 (or
 * >= native) to disable decimation outright.
 *
 * Capture statistics go to stderr, never stdout: sequence-gap drops, short
 * frames, select timeouts, and a mean-absolute-difference against the previous
 * frame so a frozen pipeline is distinguishable from a static scene (a real
 * sensor never reads exactly 0.000).
 *
 * DS1_EXIT_ON_SINK_LOSS=1 makes a vanished consumer end the run. The two
 * callers want opposite things: a soak measures capture and must not be ended
 * by a dropped network client, while the preview server must let the pipeline
 * finish so it can re-listen for the next browser. Default is to keep
 * capturing, which is the soak's need.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

#define NBUF 4
#define NATIVE_FPS 60   /* default only; see native_fps argument */
#define MAD_STRIDE 64

static volatile sig_atomic_t stop_now = 0;
static void on_sig(int s) { (void)s; stop_now = 1; }

static int openv(void)
{
    int fd = open("/dev/video1", O_RDWR);
    if (fd < 0) { perror("open /dev/video1"); exit(1); }
    return fd;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

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

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int secs    = argc > 1 ? atoi(argv[1]) : 60;
    int out_fps = argc > 2 ? atoi(argv[2]) : 15;
    int w       = argc > 3 ? atoi(argv[3]) : 1920;
    int h       = argc > 4 ? atoi(argv[4]) : 1080;
    int native  = argc > 5 ? atoi(argv[5]) : NATIVE_FPS;
    int decim;
    if (native < 1) native = NATIVE_FPS;
    decim = (out_fps > 0 && out_fps < native) ? (native + out_fps / 2) / out_fps : 1;
    int fr, meta, ds1;
    struct v4l2_format f;
    struct v4l2_requestbuffers rb;
    void *map[NBUF][2];
    unsigned int len[NBUF][2];
    int i, type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    unsigned long got = 0, wrote = 0, drops = 0, frozen = 0, timeouts = 0, shorts = 0;
    int sink_ok = 1;
    int exit_on_sink_loss = getenv("DS1_EXIT_ON_SINK_LOSS") != NULL &&
                            getenv("DS1_EXIT_ON_SINK_LOSS")[0] == '1';
    unsigned int prev_seq = 0; int have_prev = 0;
    unsigned char *prev_y = NULL; int have_prev_y = 0;
    double mad_sum = 0.0, mad_min = 1e9, mad_max = 0.0;
    double t0, tlast, tend;
    unsigned long got_last = 0;

    if (decim < 1) decim = 1;
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    fr = openv(); meta = openv(); ds1 = openv();
    (void)fr; (void)meta;

    memset(&f, 0, sizeof f);
    f.type = type;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (ioctl(ds1, VIDIOC_S_FMT, &f) < 0) { perror("S_FMT"); return 1; }
    fprintf(stderr, "ds1stream: %ux%u NV12 bpl=%u planes %u+%u, native %d fps -> every %dth frame (~%d fps out), %d s\n",
            f.fmt.pix_mp.width, f.fmt.pix_mp.height,
            f.fmt.pix_mp.plane_fmt[0].bytesperline,
            f.fmt.pix_mp.plane_fmt[0].sizeimage, f.fmt.pix_mp.plane_fmt[1].sizeimage,
            native, decim, native / decim, secs);

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

    while (!stop_now && now() < tend) {
        struct v4l2_buffer b; struct v4l2_plane p[2];
        fd_set fds; struct timeval tv; double t;
        FD_ZERO(&fds); FD_SET(ds1, &fds);
        tv.tv_sec = 5; tv.tv_usec = 0;
        i = select(ds1 + 1, &fds, NULL, NULL, &tv);
        if (i < 0) { if (errno == EINTR) continue; perror("select"); break; }
        if (i == 0) {
            timeouts++;
            fprintf(stderr, "[%6.0fs] SELECT TIMEOUT (#%lu) after %lu frames\n", now() - t0, timeouts, got);
            if (timeouts >= 3) { fprintf(stderr, "giving up after 3 timeouts\n"); break; }
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

        /* Capture is the primary measurement; the consumer is secondary. If the
         * encoder or the network client goes away, stop writing and keep
         * capturing, so a dropped client cannot end a soak run. */
        if (sink_ok && got % (unsigned long)decim == 0) {
            if (write_all(STDOUT_FILENO, map[b.index][0], len[b.index][0]) < 0 ||
                write_all(STDOUT_FILENO, map[b.index][1], len[b.index][1]) < 0) {
                fprintf(stderr, "[%6.0fs] stdout closed after %lu written frames; %s\n",
                        now() - t0, wrote,
                        exit_on_sink_loss ? "ending run" : "continuing capture");
                sink_ok = 0;
                if (exit_on_sink_loss) {
                    ioctl(ds1, VIDIOC_QBUF, &b);
                    break;
                }
            } else {
                wrote++;
            }
        }

        if (ioctl(ds1, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); break; }
        got++;

        t = now();
        if (t - tlast >= 60.0) {
            fprintf(stderr, "[%6.0fs] cap=%-7lu out=%-7lu %.2f fps in (last min %.2f)  drops=%lu frozen=%lu short=%lu timeouts=%lu  mad avg=%.3f min=%.3f max=%.3f\n",
                    t - t0, got, wrote, got / (t - t0), (got - got_last) / (t - tlast),
                    drops, frozen, shorts, timeouts,
                    got > 1 ? mad_sum / (got - 1) : 0.0, mad_min, mad_max);
            tlast = t; got_last = got;
        }
    }

    ioctl(ds1, VIDIOC_STREAMOFF, &type);
    fprintf(stderr, "DS1STREAM DONE: %.0f s, sink=%s, captured=%lu written=%lu, %.2f fps in, drops=%lu frozen=%lu short=%lu timeouts=%lu, mad avg=%.3f min=%.3f max=%.3f\n",
            now() - t0, sink_ok ? "ok" : "LOST", got, wrote, got / (now() - t0), drops, frozen, shorts, timeouts,
            got > 1 ? mad_sum / (got - 1) : 0.0,
            mad_min > 1e8 ? 0.0 : mad_min, mad_max);
    return (timeouts || shorts || frozen) ? 1 : 0;
}
