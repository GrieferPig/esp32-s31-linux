// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "frames.h"

struct queue {
    unsigned int type;
    unsigned int len;
    unsigned char *data;
};

static void check(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "%s: %s\n", message, strerror(errno));
        exit(1);
    }
}

static void queue_init(int fd, struct queue *q, unsigned int w, unsigned int h,
                       unsigned int size)
{
    int output = q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT;
    struct v4l2_format fmt = {
        .type = q->type,
        .fmt.pix = { .width = w, .height = h, .sizeimage = size,
            .pixelformat = output ? V4L2_PIX_FMT_JPEG : V4L2_PIX_FMT_GREY },
    };
    struct v4l2_requestbuffers req = {
        .type = q->type, .memory = V4L2_MEMORY_MMAP, .count = 1,
    };
    struct v4l2_buffer buf = { .type = q->type, .memory = req.memory };

    check(ioctl(fd, VIDIOC_S_FMT, &fmt) == 0, "set format");
    check(fmt.fmt.pix.width == w && fmt.fmt.pix.height == h, "format dimensions");
    check(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count >= 1, "request buffers");
    check(ioctl(fd, VIDIOC_QUERYBUF, &buf) == 0, "query buffer");
    q->len = buf.length;
    q->data = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    check(q->data != MAP_FAILED, "map buffer");
}

static void queue_frame(int fd, const struct queue *q, unsigned int bytes)
{
    struct v4l2_buffer buf = { .type = q->type, .memory = V4L2_MEMORY_MMAP,
        .bytesused = bytes };
    check(ioctl(fd, VIDIOC_QBUF, &buf) == 0, "queue buffer");
}

static struct v4l2_buffer dequeue(int fd, const struct queue *q)
{
    struct v4l2_buffer buf = { .type = q->type, .memory = V4L2_MEMORY_MMAP };
    check(ioctl(fd, VIDIOC_DQBUF, &buf) == 0, "dequeue buffer");
    return buf;
}

static void run_frame(int fd, struct queue *out, struct queue *cap,
                      const unsigned char *data, size_t len, unsigned int w,
                      unsigned int h, unsigned int fixture, int stream_on, int bad)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    struct v4l2_buffer cbuf, obuf;
    unsigned int i;

    check(len <= out->len, "fixture fits");
    memcpy(out->data, data, len);
    memset(cap->data, 0xa5, cap->len);
    queue_frame(fd, cap, 0);
    queue_frame(fd, out, len);
    if (stream_on) {
        check(ioctl(fd, VIDIOC_STREAMON, &out->type) == 0, "start output");
        check(ioctl(fd, VIDIOC_STREAMON, &cap->type) == 0, "start capture");
    }

    check(poll(&pfd, 1, 2000) == 1 && pfd.revents & POLLIN, "wait for capture");
    cbuf = dequeue(fd, cap);
    obuf = dequeue(fd, out);
    printf("%ux%u fixture=%u invalid=%d capture_flags=%#x output_flags=%#x bytes=%u\n",
           w, h, fixture, bad, cbuf.flags, obuf.flags, cbuf.bytesused);
    if (bad) {
        check((cbuf.flags & obuf.flags & V4L2_BUF_FLAG_ERROR) &&
              cbuf.bytesused == 0, "bad frame error");
        return;
    }

    check(!(cbuf.flags & V4L2_BUF_FLAG_ERROR) && cbuf.bytesused == w * h,
          "capture result");
    for (i = 0; i < w * h; i++) {
        unsigned char expected = 17 + 47 * ((i % w) / 8) +
                                 71 * ((i / w) / 8) + 83 * fixture;
        if (cap->data[i] != expected) {
            fprintf(stderr, "pixel %u: got %u expected %u\n", i, cap->data[i], expected);
            exit(1);
        }
    }
    puts("pixels matched");
}

int main(int argc, char **argv)
{
    static const unsigned char *const data[] = { frame_0, frame_1, frame_2, frame_3 };
    static const unsigned int lens[] = { sizeof(frame_0), sizeof(frame_1), sizeof(frame_2), sizeof(frame_3) };
    static const unsigned int widths[] = { 16, 32, 320, 1024 };
    static const unsigned int heights[] = { 16, 24, 240, 1024 };
    unsigned int i, repeated;
    unsigned int count = argc == 3 ? 5 : 1;

    check(argc == 2 || argc == 3, "usage: p4-jpeg-v4l2-test device [stress]");
    for (i = 0; i < 4; i++) {
        struct queue out = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
        struct queue cap = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        int fd = open(argv[1], O_RDWR | O_NONBLOCK);

        check(fd >= 0, "open codec");
        queue_init(fd, &out, widths[i], heights[i], lens[i]);
        queue_init(fd, &cap, widths[i], heights[i], 0);
        if (i == 1)
            run_frame(fd, &out, &cap, bad_frame, sizeof(bad_frame), widths[i], heights[i], i, 1, 1);
        for (repeated = 0; repeated < count; repeated++)
            run_frame(fd, &out, &cap, data[i], lens[i], widths[i], heights[i], i,
                      i != 1 && repeated == 0, 0);
        check(ioctl(fd, VIDIOC_STREAMOFF, &out.type) == 0, "stop output");
        check(ioctl(fd, VIDIOC_STREAMOFF, &cap.type) == 0, "stop capture");
        check(munmap(out.data, out.len) == 0, "unmap output");
        check(munmap(cap.data, cap.len) == 0, "unmap capture");
        check(close(fd) == 0, "close codec");
    }
    return 0;
}
