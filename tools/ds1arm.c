/* Safe DS1 probe: everything up to but NOT including STREAMON.
 * Arming the DMA writer happens at QBUF, so this exercises the path that
 * was corrupting memory, without ever letting the sensor produce a frame. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static int openv(void) {
    int fd = open("/dev/video1", O_RDWR);
    if (fd < 0) { perror("open"); exit(1); }
    return fd;
}

int main(int argc, char **argv) {
    int w = argc > 1 ? atoi(argv[1]) : 1920;
    int h = argc > 2 ? atoi(argv[2]) : 1080;
    int fr = openv();   /* stream 0 = FR   */
    int meta = openv(); /* stream 1 = META */
    int ds1 = openv();  /* stream 2 = DS1  */
    struct v4l2_format f;
    struct v4l2_requestbuffers rb;
    int i;

    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (ioctl(ds1, VIDIOC_S_FMT, &f) < 0) { perror("S_FMT ds1"); return 1; }
    printf("DS1 negotiated %ux%u planes=%u bpl=%u size0=%u size1=%u\n",
           f.fmt.pix_mp.width, f.fmt.pix_mp.height, f.fmt.pix_mp.num_planes,
           f.fmt.pix_mp.plane_fmt[0].bytesperline,
           f.fmt.pix_mp.plane_fmt[0].sizeimage,
           f.fmt.pix_mp.plane_fmt[1].sizeimage);

    memset(&rb, 0, sizeof rb);
    rb.count = 4;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(ds1, VIDIOC_REQBUFS, &rb) < 0) { perror("REQBUFS"); return 1; }
    printf("got %u buffers\n", rb.count);

    for (i = 0; i < (int)rb.count; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane p[2];
        memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = 2;
        b.m.planes = p;
        if (ioctl(ds1, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return 1; }
        if (ioctl(ds1, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return 1; }
        printf("qbuf %d ok (plane0 len=%u plane1 len=%u)\n", i, p[0].length, p[1].length);
    }

    printf("STOPPING BEFORE STREAMON -- no DMA will run\n");
    close(ds1); close(meta); close(fr);
    return 0;
}
