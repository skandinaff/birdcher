/* DS1 capture: open FR/META/DS1 handles, set 1920x1080 NV12 on DS1, stream. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
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

int main(int argc, char **argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 1920;
    int h = argc > 2 ? atoi(argv[2]) : 1080;
    int nframes = argc > 3 ? atoi(argv[3]) : 6;
    const char *out = argc > 4 ? argv[4] : NULL;
    int fr = openv(), meta = openv(), ds1 = openv();
    struct v4l2_format f;
    struct v4l2_requestbuffers rb;
    void *map[NBUF][2];
    unsigned int len[NBUF][2];
    int i, type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, got = 0;
    FILE *fp = NULL;

    memset(&f, 0, sizeof f);
    f.type = type;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (ioctl(ds1, VIDIOC_S_FMT, &f) < 0) { perror("S_FMT"); return 1; }
    printf("DS1 negotiated %ux%u planes=%u bpl=%u size0=%u size1=%u\n",
           f.fmt.pix_mp.width, f.fmt.pix_mp.height, f.fmt.pix_mp.num_planes,
           f.fmt.pix_mp.plane_fmt[0].bytesperline,
           f.fmt.pix_mp.plane_fmt[0].sizeimage, f.fmt.pix_mp.plane_fmt[1].sizeimage);
    fflush(stdout);

    memset(&rb, 0, sizeof rb);
    rb.count = NBUF;
    rb.type = type;
    rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(ds1, VIDIOC_REQBUFS, &rb) < 0) { perror("REQBUFS"); return 1; }
    printf("buffers: %u\n", rb.count);
    fflush(stdout);

    for (i = 0; i < (int)rb.count; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane p[2];
        int j;
        memset(&b, 0, sizeof b);
        memset(p, 0, sizeof p);
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i; b.length = 2; b.m.planes = p;
        if (ioctl(ds1, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return 1; }
        for (j = 0; j < 2; j++) {
            len[i][j] = p[j].length;
            map[i][j] = mmap(NULL, p[j].length, PROT_READ, MAP_SHARED, ds1, p[j].m.mem_offset);
            if (map[i][j] == MAP_FAILED) { perror("mmap"); return 1; }
        }
        if (ioctl(ds1, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return 1; }
    }

    printf("about to STREAMON\n");
    fflush(stdout);
    sync();
    if (ioctl(ds1, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }
    printf("STREAMON returned ok\n");
    fflush(stdout);
    sync();

    if (out)
        fp = fopen(out, "wb");

    while (got < nframes) {
        struct v4l2_buffer b;
        struct v4l2_plane p[2];
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(ds1, &fds);
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        i = select(ds1 + 1, &fds, NULL, NULL, &tv);
        if (i <= 0) { printf("select returned %d (%s)\n", i, strerror(errno)); break; }
        memset(&b, 0, sizeof b);
        memset(p, 0, sizeof p);
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.length = 2; b.m.planes = p;
        if (ioctl(ds1, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }
        printf("frame %d: idx=%u bytesused=%u/%u seq=%u\n", got, b.index,
               p[0].bytesused, p[1].bytesused, b.sequence);
        fflush(stdout);
        if (fp && got == nframes - 1) {
            fwrite(map[b.index][0], 1, len[b.index][0], fp);
            fwrite(map[b.index][1], 1, len[b.index][1], fp);
        }
        if (ioctl(ds1, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); break; }
        got++;
    }
    if (fp)
        fclose(fp);
    ioctl(ds1, VIDIOC_STREAMOFF, &type);
    printf("STREAMOFF done, got %d frames\n", got);
    close(ds1);
    close(meta);
    close(fr);
    return got == nframes ? 0 : 2;
}
