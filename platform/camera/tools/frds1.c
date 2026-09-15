/*
 * Observe the DS1 scaler configuration without ever arming the DS1 DMA writer.
 *
 * Opens the three handles in order (FR, META, DS1), sets 1920x1080 on DS1 so
 * the crop/scaler FSM is programmed for it, then streams FR only. DS1 gets no
 * REQBUFS and no QBUF, so its DMA writer is never armed and cannot write
 * anywhere -- but the ISP is running, so the scaler configuration is live and
 * the driver traces report what it actually holds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <linux/videodev2.h>

#define NBUF 4

static int openv(void)
{
    int fd = open("/dev/video1", O_RDWR);
    if (fd < 0) { perror("open"); exit(1); }
    return fd;
}

static int set_fmt(int fd, const char *tag, int w, int h)
{
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &f) < 0) { perror("S_FMT"); return -1; }
    printf("%s negotiated %ux%u bpl=%u size0=%u size1=%u\n", tag,
           f.fmt.pix_mp.width, f.fmt.pix_mp.height,
           f.fmt.pix_mp.plane_fmt[0].bytesperline,
           f.fmt.pix_mp.plane_fmt[0].sizeimage,
           f.fmt.pix_mp.plane_fmt[1].sizeimage);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    int dw = argc > 1 ? atoi(argv[1]) : 1920;
    int dh = argc > 2 ? atoi(argv[2]) : 1080;
    int nframes = argc > 3 ? atoi(argv[3]) : 4;
    int fr = openv(), meta = openv(), ds1 = openv();
    struct v4l2_requestbuffers rb;
    int i, type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, got = 0;

    (void)meta;

    /* DS1 first: this is the S_FMT that programs the downscaler. */
    if (set_fmt(ds1, "DS1", dw, dh) < 0) return 1;
    if (set_fmt(fr, "FR", 3864, 2192) < 0) return 1;

    memset(&rb, 0, sizeof rb);
    rb.count = NBUF; rb.type = type; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fr, VIDIOC_REQBUFS, &rb) < 0) { perror("REQBUFS fr"); return 1; }
    printf("FR buffers: %u (DS1 deliberately has none)\n", rb.count);
    fflush(stdout);

    for (i = 0; i < (int)rb.count; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane p[2];
        memset(&b, 0, sizeof b);
        memset(p, 0, sizeof p);
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i; b.length = 2; b.m.planes = p;
        if (ioctl(fr, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return 1; }
        if (ioctl(fr, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return 1; }
    }

    printf("FR STREAMON\n");
    fflush(stdout);
    sync();
    if (ioctl(fr, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }

    while (got < nframes) {
        struct v4l2_buffer b;
        struct v4l2_plane p[2];
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(fr, &fds);
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        i = select(fr + 1, &fds, NULL, NULL, &tv);
        if (i <= 0) { printf("FR select returned %d (%s)\n", i, strerror(errno)); break; }
        memset(&b, 0, sizeof b);
        memset(p, 0, sizeof p);
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.length = 2; b.m.planes = p;
        if (ioctl(fr, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }
        printf("FR frame %d: idx=%u bytesused=%u/%u\n", got, b.index, p[0].bytesused, p[1].bytesused);
        fflush(stdout);
        if (ioctl(fr, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); break; }
        got++;
    }

    ioctl(fr, VIDIOC_STREAMOFF, &type);
    printf("done, FR frames %d\n", got);
    close(ds1);
    close(meta);
    close(fr);
    return 0;
}
