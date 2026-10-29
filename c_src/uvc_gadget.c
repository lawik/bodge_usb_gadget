// SPDX-FileCopyrightText: 2026 Lars Wikman
//
// SPDX-License-Identifier: Apache-2.0
//
// uvc_gadget: the userspace half of a UVC gadget function, for
// BodgeUSBGadget.UVC.
//
//     uvc_gadget /dev/videoN MAX_PACKET
//
// The kernel's f_uvc does the USB side of a webcam but hands two things to
// userspace through its V4L2 output device: the class's control requests
// (the host picks a format, size and frame rate with probe and commit) and
// the frames. This program answers the requests from the formats the device
// enumerates (the function's configfs tree, as the kernel describes it to
// the host) and queues frames it reads from stdin.
//
// Erlang port protocol, {packet, 4} both ways:
//   stdin:  one frame per packet, in the format the host committed to.
//   stdout: text events: "connect SPEED", "disconnect",
//           "stream FOURCC WIDTH HEIGHT INTERVAL" (100 ns units), "stop",
//           "log TEXT", "error TEXT".
//
// The kernel keeps the gadget off the bus until a program subscribes to the
// function's SETUP events; this one does at start, and closing the device
// when stdin closes takes the camera off the bus again.

#define _GNU_SOURCE

#include <stdio.h>

#ifndef __linux__
int main(void) {
    fprintf(stderr, "uvc_gadget: Linux only\n");
    return 1;
}
#else

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/usb/ch9.h>
#include <linux/usb/g_uvc.h>
#include <linux/usb/video.h>
#include <linux/videodev2.h>

// f_uvc rewrites wIndex so its interfaces read as these.
#define INTF_CONTROL 0
#define INTF_STREAMING 1

#define MAX_FORMATS 8
#define MAX_FRAMES 16
#define MAX_INTERVALS 8
#define NUM_BUFFERS 4
#define MAX_FRAME_BYTES (32u << 20)

struct frame {
    uint32_t width, height, size;
    uint32_t intervals[MAX_INTERVALS]; // 100 ns units, fastest first
    int n_intervals;
};

struct format {
    uint32_t fcc;
    struct frame frames[MAX_FRAMES];
    int n_frames;
};

struct buffer {
    void *mem;
    size_t length;
    int free;
};

static int fd = -1;
static struct format formats[MAX_FORMATS];
static int n_formats;
static uint32_t max_packet;
static uint32_t max_payload;

static struct uvc_streaming_control probe, commit;
static int pending_control; // UVC_VS_PROBE_CONTROL or _COMMIT_ awaiting its data stage
static uint8_t error_code;  // for VC_REQUEST_ERROR_CODE_CONTROL

static struct buffer buffers[NUM_BUFFERS];
static int n_buffers;
static int streaming;
static int poll_out; // cleared when the queue reports an error until the next stream
static unsigned long dropped;

// ---- port I/O ----------------------------------------------------------------

static void emit(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof msg) n = sizeof msg - 1;

    uint8_t packet[4 + sizeof msg];
    packet[0] = 0;
    packet[1] = 0;
    packet[2] = (uint8_t)(n >> 8);
    packet[3] = (uint8_t)n;
    memcpy(packet + 4, msg, n);

    size_t off = 0, len = 4 + (size_t)n;
    while (off < len) {
        ssize_t w = write(STDOUT_FILENO, packet + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) exit(1); // the BEAM is gone
        off += (size_t)w;
    }
}

static void fail(const char *what) { emit("error %s: %s", what, strerror(errno)); }

static int xioctl(unsigned long request, void *arg) {
    int r;
    do r = ioctl(fd, request, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

static const char *fourcc(uint32_t fcc) {
    static char s[5];
    for (int i = 0; i < 4; i++) s[i] = (char)((fcc >> (8 * i)) & 0xff);
    s[4] = 0;
    return s;
}

// ---- formats -------------------------------------------------------------------

// What the function offers, as the kernel enumerates its configfs tree: the
// same order, so the same format and frame indices, as the descriptors the
// host reads.
static int enumerate(void) {
    for (int i = 0; i < MAX_FORMATS; i++) {
        struct v4l2_fmtdesc desc = {.index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
        if (xioctl(VIDIOC_ENUM_FMT, &desc) < 0) break;

        struct format *f = &formats[n_formats++];
        f->fcc = desc.pixelformat;

        for (int j = 0; j < MAX_FRAMES; j++) {
            struct v4l2_frmsizeenum size = {.index = j, .pixel_format = f->fcc};
            if (xioctl(VIDIOC_ENUM_FRAMESIZES, &size) < 0) break;
            if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;

            struct frame *fr = &f->frames[f->n_frames++];
            fr->width = size.discrete.width;
            fr->height = size.discrete.height;

            for (int k = 0; k < MAX_INTERVALS; k++) {
                struct v4l2_frmivalenum ival = {
                    .index = k, .pixel_format = f->fcc,
                    .width = fr->width, .height = fr->height};
                if (xioctl(VIDIOC_ENUM_FRAMEINTERVALS, &ival) < 0) break;
                if (ival.type != V4L2_FRMIVAL_TYPE_DISCRETE || ival.discrete.denominator == 0)
                    break;
                fr->intervals[fr->n_intervals++] = (uint32_t)(
                    (uint64_t)ival.discrete.numerator * 10000000 / ival.discrete.denominator);
            }

            // The buffer a frame of this size needs, as the kernel works it out.
            struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
            fmt.fmt.pix.pixelformat = f->fcc;
            fmt.fmt.pix.width = fr->width;
            fmt.fmt.pix.height = fr->height;
            fmt.fmt.pix.field = V4L2_FIELD_NONE;
            fr->size = xioctl(VIDIOC_TRY_FMT, &fmt) == 0 ? fmt.fmt.pix.sizeimage
                                                        : fr->width * fr->height * 2;
        }

        if (f->n_frames == 0) n_formats--;
    }

    for (int i = 0; i < n_formats; i++)
        for (int j = 0; j < formats[i].n_frames; j++) {
            struct frame *fr = &formats[i].frames[j];
            emit("log format %d %s %ux%u, %d intervals from %u, %u bytes", i + 1,
                 fourcc(formats[i].fcc), fr->width, fr->height, fr->n_intervals,
                 fr->n_intervals ? fr->intervals[0] : 0, fr->size);
        }

    return n_formats;
}

static uint32_t closest_interval(const struct frame *fr, uint32_t wanted) {
    if (fr->n_intervals == 0) return 333333;
    uint32_t best = fr->intervals[0];
    for (int i = 0; i < fr->n_intervals; i++) {
        uint32_t d = fr->intervals[i] > wanted ? fr->intervals[i] - wanted : wanted - fr->intervals[i];
        uint32_t bd = best > wanted ? best - wanted : wanted - best;
        if (d < bd) best = fr->intervals[i];
    }
    return best;
}

// A probe/commit structure for format `fi` and frame `fr` (0-based, clamped).
static void fill_control(struct uvc_streaming_control *ctrl, int fi, int ri, uint32_t interval) {
    if (fi < 0) fi = 0;
    if (fi >= n_formats) fi = n_formats - 1;
    const struct format *f = &formats[fi];
    if (ri < 0) ri = 0;
    if (ri >= f->n_frames) ri = f->n_frames - 1;
    const struct frame *fr = &f->frames[ri];

    memset(ctrl, 0, sizeof *ctrl);
    ctrl->bmHint = 1;
    ctrl->bFormatIndex = (uint8_t)(fi + 1);
    ctrl->bFrameIndex = (uint8_t)(ri + 1);
    ctrl->dwFrameInterval = interval ? closest_interval(fr, interval)
                                     : (fr->n_intervals ? fr->intervals[0] : 333333);
    ctrl->dwMaxVideoFrameSize = fr->size;
    ctrl->dwMaxPayloadTransferSize = max_payload;
    ctrl->bmFramingInfo = 3;
    ctrl->bPreferedVersion = 1;
    ctrl->bMaxVersion = 1;
}

static const struct frame *committed(const struct format **f) {
    *f = &formats[commit.bFormatIndex - 1];
    return &(*f)->frames[commit.bFrameIndex - 1];
}

static void set_format(void) {
    const struct format *f;
    const struct frame *fr = committed(&f);
    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    fmt.fmt.pix.pixelformat = f->fcc;
    fmt.fmt.pix.width = fr->width;
    fmt.fmt.pix.height = fr->height;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.sizeimage = fr->size;
    if (xioctl(VIDIOC_S_FMT, &fmt) < 0) fail("S_FMT");

    // The kernel spreads each frame over this long (its default is 15 fps),
    // so uncompressed video at 30 fps needs to be told.
    struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    parm.parm.output.timeperframe.numerator = commit.dwFrameInterval;
    parm.parm.output.timeperframe.denominator = 10000000;
    if (xioctl(VIDIOC_S_PARM, &parm) < 0) fail("S_PARM");
}

// ---- streaming -----------------------------------------------------------------

static void free_buffers(void) {
    for (int i = 0; i < n_buffers; i++) munmap(buffers[i].mem, buffers[i].length);
    n_buffers = 0;
    struct v4l2_requestbuffers req = {
        .count = 0, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP};
    xioctl(VIDIOC_REQBUFS, &req);
}

static void stream_on(void) {
    const struct format *f;
    const struct frame *fr = committed(&f);

    set_format();

    struct v4l2_requestbuffers req = {
        .count = NUM_BUFFERS, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(VIDIOC_REQBUFS, &req) < 0) {
        fail("REQBUFS");
        return;
    }

    for (unsigned i = 0; i < req.count && i < NUM_BUFFERS; i++) {
        struct v4l2_buffer buf = {
            .index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP};
        if (xioctl(VIDIOC_QUERYBUF, &buf) < 0) {
            fail("QUERYBUF");
            free_buffers();
            return;
        }
        void *mem = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (mem == MAP_FAILED) {
            fail("mmap");
            free_buffers();
            return;
        }
        buffers[i] = (struct buffer){.mem = mem, .length = buf.length, .free = 1};
        n_buffers = (int)i + 1;
    }

    // Also completes the host's SET_INTERFACE, which the kernel holds until now.
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (xioctl(VIDIOC_STREAMON, &type) < 0) {
        fail("STREAMON");
        free_buffers();
        return;
    }

    streaming = 1;
    poll_out = 1;
    dropped = 0;
    emit("stream %s %u %u %u", fourcc(f->fcc), fr->width, fr->height, commit.dwFrameInterval);
}

static void stream_off(void) {
    if (!streaming) return;
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (xioctl(VIDIOC_STREAMOFF, &type) < 0) fail("STREAMOFF");
    free_buffers();
    streaming = 0;
    if (dropped) emit("log dropped %lu frames", dropped);
    emit("stop");
}

static void reclaim(void) {
    for (;;) {
        struct v4l2_buffer buf = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP};
        if (xioctl(VIDIOC_DQBUF, &buf) < 0) return;
        if (buf.index < (unsigned)n_buffers) buffers[buf.index].free = 1;
    }
}

static void queue_frame(const uint8_t *data, size_t len) {
    if (!streaming) return;
    reclaim();

    for (int i = 0; i < n_buffers; i++) {
        if (!buffers[i].free) continue;
        if (len > buffers[i].length) {
            emit("error a %zu byte frame is larger than the %zu byte buffer", len,
                 buffers[i].length);
            return;
        }
        memcpy(buffers[i].mem, data, len);
        struct v4l2_buffer buf = {
            .index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP,
            .bytesused = (uint32_t)len, .field = V4L2_FIELD_NONE};
        if (xioctl(VIDIOC_QBUF, &buf) < 0) {
            fail("QBUF");
            return;
        }
        buffers[i].free = 0;
        return;
    }

    dropped++;
}

// ---- control requests ------------------------------------------------------------

static void control_request(const struct usb_ctrlrequest *ctrl, struct uvc_request_data *resp) {
    uint8_t entity = ctrl->wIndex >> 8, cs = ctrl->wValue >> 8;

    if (entity == 0 && cs == UVC_VC_REQUEST_ERROR_CODE_CONTROL) {
        if (ctrl->bRequest == UVC_GET_CUR) {
            resp->data[0] = error_code;
            resp->length = 1;
        } else if (ctrl->bRequest == UVC_GET_INFO) {
            resp->data[0] = 0x01; // GET only
            resp->length = 1;
        }
        return;
    }

    // There are no controls (bmControls are all zero); a host asking anyway
    // gets a stall, and "invalid control" if it asks why.
    error_code = 0x06;
}

static void streaming_request(const struct usb_ctrlrequest *ctrl, struct uvc_request_data *resp) {
    uint8_t cs = ctrl->wValue >> 8;
    if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL) return;

    struct uvc_streaming_control *out = (void *)resp->data;
    const struct format *last = &formats[n_formats - 1];

    switch (ctrl->bRequest) {
    case UVC_SET_CUR:
        pending_control = cs;
        resp->length = sizeof *out;
        break;
    case UVC_GET_CUR:
        memcpy(out, cs == UVC_VS_PROBE_CONTROL ? &probe : &commit, sizeof *out);
        resp->length = sizeof *out;
        break;
    case UVC_GET_MIN:
    case UVC_GET_DEF:
        fill_control(out, 0, 0, 0);
        resp->length = sizeof *out;
        break;
    case UVC_GET_MAX:
        fill_control(out, n_formats - 1, last->n_frames - 1, 0);
        resp->length = sizeof *out;
        break;
    case UVC_GET_RES:
        memset(out, 0, sizeof *out);
        resp->length = sizeof *out;
        break;
    case UVC_GET_LEN:
        resp->data[0] = sizeof *out; // little endian
        resp->data[1] = 0;
        resp->length = 2;
        break;
    case UVC_GET_INFO:
        resp->data[0] = 0x03; // GET and SET
        resp->length = 1;
        break;
    }
}

static void setup(const struct usb_ctrlrequest *ctrl) {
    struct uvc_request_data resp;
    memset(&resp, 0, sizeof resp);
    resp.length = -EL2HLT; // a stall unless answered

    if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS &&
        (ctrl->bRequestType & USB_RECIP_MASK) == USB_RECIP_INTERFACE) {
        switch (ctrl->wIndex & 0xff) {
        case INTF_CONTROL: control_request(ctrl, &resp); break;
        case INTF_STREAMING: streaming_request(ctrl, &resp); break;
        }
    }

    if (resp.length >= 0) error_code = 0;
    if (xioctl(UVCIOC_SEND_RESPONSE, &resp) < 0) fail("SEND_RESPONSE");
}

// The data stage of a SET_CUR: what the host proposes (probe) or settles on
// (commit), made into something this device does.
static void data(const struct uvc_request_data *d) {
    struct uvc_streaming_control *target =
        pending_control == UVC_VS_PROBE_CONTROL ? &probe
        : pending_control == UVC_VS_COMMIT_CONTROL ? &commit : NULL;
    if (!target) return;

    struct uvc_streaming_control asked;
    memset(&asked, 0, sizeof asked);
    memcpy(&asked, d->data, d->length < (int)sizeof asked ? (size_t)d->length : sizeof asked);

    fill_control(target, asked.bFormatIndex - 1, asked.bFrameIndex - 1, asked.dwFrameInterval);
    if (target == &commit) set_format();
    pending_control = 0;
}

static void event(void) {
    struct v4l2_event ev;
    while (xioctl(VIDIOC_DQEVENT, &ev) == 0) {
        struct uvc_event *uev = (void *)&ev.u.data;

        switch (ev.type) {
        case UVC_EVENT_CONNECT:
            max_payload = uev->speed >= USB_SPEED_HIGH ? max_packet
                          : max_packet < 1023      ? max_packet : 1023;
            fill_control(&probe, 0, 0, 0);
            fill_control(&commit, 0, 0, 0);
            emit("connect %s", uev->speed >= USB_SPEED_SUPER ? "super"
                               : uev->speed == USB_SPEED_HIGH ? "high" : "full");
            break;
        case UVC_EVENT_DISCONNECT:
            stream_off();
            emit("disconnect");
            break;
        case UVC_EVENT_STREAMON: stream_on(); break;
        case UVC_EVENT_STREAMOFF: stream_off(); break;
        case UVC_EVENT_SETUP: setup(&uev->req); break;
        case UVC_EVENT_DATA: data(&uev->data); break;
        }
    }
}

// ---- main loop -----------------------------------------------------------------

static uint8_t *in;
static size_t in_len, in_cap;

// Frames from stdin; returns 0 at EOF.
static int read_stdin(void) {
    if (in_cap - in_len < (1u << 16)) {
        size_t cap = in_cap ? in_cap * 2 : (1u << 20);
        uint8_t *grown = realloc(in, cap);
        if (!grown) {
            emit("error out of memory");
            exit(1);
        }
        in = grown;
        in_cap = cap;
    }

    ssize_t r = read(STDIN_FILENO, in + in_len, in_cap - in_len);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR || errno == EAGAIN;
    in_len += (size_t)r;

    size_t off = 0;
    while (in_len - off >= 4) {
        uint32_t len = (uint32_t)in[off] << 24 | (uint32_t)in[off + 1] << 16 |
                       (uint32_t)in[off + 2] << 8 | in[off + 3];
        if (len > MAX_FRAME_BYTES) {
            emit("error a %u byte frame is too large", len);
            exit(1);
        }
        if (in_len - off - 4 < len) {
            // Room for the rest of this frame.
            while (in_cap < 4 + (size_t)len) {
                uint8_t *grown = realloc(in, in_cap * 2);
                if (!grown) exit(1);
                in = grown;
                in_cap *= 2;
            }
            break;
        }
        queue_frame(in + off + 4, len);
        off += 4 + (size_t)len;
    }

    memmove(in, in + off, in_len - off);
    in_len -= off;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: uvc_gadget /dev/videoN MAX_PACKET\n");
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    max_packet = max_payload = (uint32_t)strtoul(argv[2], NULL, 10);

    fd = open(argv[1], O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fail(argv[1]);
        return 1;
    }

    if (enumerate() == 0) {
        emit("error %s offers no formats", argv[1]);
        return 1;
    }
    fill_control(&probe, 0, 0, 0);
    fill_control(&commit, 0, 0, 0);

    static const unsigned events[] = {UVC_EVENT_CONNECT, UVC_EVENT_DISCONNECT, UVC_EVENT_STREAMON,
                                      UVC_EVENT_STREAMOFF, UVC_EVENT_DATA,
                                      // Last: subscribing to SETUP puts the camera on the bus.
                                      UVC_EVENT_SETUP};
    for (size_t i = 0; i < sizeof events / sizeof events[0]; i++) {
        struct v4l2_event_subscription sub = {.type = events[i]};
        if (xioctl(VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
            fail("SUBSCRIBE_EVENT");
            return 1;
        }
    }

    for (;;) {
        struct pollfd fds[2] = {
            {.fd = fd, .events = POLLPRI | (streaming && poll_out ? POLLOUT : 0)},
            {.fd = STDIN_FILENO, .events = POLLIN},
        };

        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            fail("poll");
            break;
        }

        if (fds[0].revents & POLLPRI) event();
        if (fds[0].revents & POLLOUT) reclaim();
        if (fds[0].revents & POLLERR && streaming) {
            emit("log the video queue reports an error");
            poll_out = 0;
        }
        if (fds[1].revents & (POLLIN | POLLHUP))
            if (!read_stdin()) break;
    }

    stream_off();
    close(fd); // unsubscribes, which takes the camera off the bus
    return 0;
}

#endif
