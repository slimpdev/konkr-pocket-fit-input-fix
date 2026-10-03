#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VIRTUAL_NAME "KONKR Filtered Gamepad"
#define READY_FILE "/run/konkr-input-fix.ready"
#define R3_DEBOUNCE_MS 50
#define FF_MAP_SIZE 64
#define VERSION "0.2.0"

#define BITS_PER_LONG (8U * (unsigned)sizeof(unsigned long))
#define NBITS(x) (((x) + BITS_PER_LONG - 1U) / BITS_PER_LONG)
#define TEST_BIT(bit, array) (((array)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

static volatile sig_atomic_t running = 1;

struct source_desc {
    const char *name_prefix;
    unsigned short vendor;
    unsigned short product;
    int trigger_left;
    int trigger_right;
};

struct trigger_filter {
    bool enabled;
    int abs_code;
    int key_code;
    int raw_value;
    int raw_key;
    int output_value;
    int output_key;
    bool pending;
    unsigned pending_frames;
};

static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int write_event(int fd, unsigned short type, unsigned short code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ssize_t n = write(fd, &ev, sizeof(ev));
    return n == (ssize_t)sizeof(ev) ? 0 : -1;
}

static int forward_event(int fd, const struct input_event *ev) {
    ssize_t n = write(fd, ev, sizeof(*ev));
    if (n == (ssize_t)sizeof(*ev))
        return 0;
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    return -1;
}

static bool have_key(int fd, unsigned int code) {
    unsigned long bits[NBITS(KEY_CNT)];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
        return false;
    return code < KEY_CNT && TEST_BIT(code, bits);
}

static bool have_abs(int fd, unsigned int code) {
    unsigned long bits[NBITS(ABS_CNT)];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) < 0)
        return false;
    return code < ABS_CNT && TEST_BIT(code, bits);
}

static int get_name(int fd, char *buf, size_t len) {
    memset(buf, 0, len);
    return ioctl(fd, EVIOCGNAME(len), buf);
}

static int find_source_event(const struct source_desc *wanted, char *out, size_t out_len) {
    char path[128];
    char name[256];

    for (int i = 0; i < 128; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        bool match = true;
        if (wanted->name_prefix) {
            if (get_name(fd, name, sizeof(name)) < 0 ||
                strncmp(name, wanted->name_prefix, strlen(wanted->name_prefix)) != 0)
                match = false;
        }
        if (match && (wanted->vendor || wanted->product)) {
            struct input_id id;
            memset(&id, 0, sizeof(id));
            if (ioctl(fd, EVIOCGID, &id) < 0 ||
                id.vendor != wanted->vendor || id.product != wanted->product)
                match = false;
        }

        close(fd);
        if (match) {
            snprintf(out, out_len, "%s", path);
            return 0;
        }
    }
    return -1;
}

static int wait_for_source(const char *variant, char *path, size_t path_len, struct source_desc *desc) {
    static const struct source_desc elite[] = {
        { "AYANEO MCU Gamepad", 0, 0, ABS_Z, ABS_RZ },
    };
    static const struct source_desc fit[] = {
        { "AYANEO Controller", 0x4001, 0x0428, ABS_BRAKE, ABS_GAS },
        { "Microsoft X-Box 360 pad", 0x045e, 0x028e, ABS_Z, ABS_RZ },
    };

    const struct source_desc *list = NULL;
    size_t count = 0;
    if (strcmp(variant, "elite") == 0) {
        list = elite;
        count = sizeof(elite) / sizeof(elite[0]);
    } else if (strcmp(variant, "fit") == 0) {
        list = fit;
        count = sizeof(fit) / sizeof(fit[0]);
    } else {
        fprintf(stderr, "unknown KONKR_VARIANT=%s\n", variant);
        return -1;
    }

    while (running) {
        for (size_t i = 0; i < count; i++) {
            if (find_source_event(&list[i], path, path_len) == 0) {
                *desc = list[i];
                return 0;
            }
        }
        usleep(100000);
    }
    return -1;
}

static int setup_abs_from_source(int ufd, int sfd, unsigned int code) {
    struct input_absinfo info;
    if (ioctl(sfd, EVIOCGABS(code), &info) < 0)
        return -1;
    if (ioctl(ufd, UI_SET_ABSBIT, code) < 0)
        return -1;

    struct uinput_abs_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.code = code;
    setup.absinfo = info;
    return ioctl(ufd, UI_ABS_SETUP, &setup);
}

static int clone_keys(int ufd, int sfd) {
    unsigned long bits[NBITS(KEY_CNT)];
    memset(bits, 0, sizeof(bits));
    if (ioctl(sfd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
        return -1;
    if (ioctl(ufd, UI_SET_EVBIT, EV_KEY) < 0)
        return -1;
    for (unsigned int code = 0; code < KEY_CNT; code++) {
        if (TEST_BIT(code, bits) && ioctl(ufd, UI_SET_KEYBIT, code) < 0)
            return -1;
    }
    return 0;
}

static int clone_abs(int ufd, int sfd) {
    unsigned long bits[NBITS(ABS_CNT)];
    memset(bits, 0, sizeof(bits));
    if (ioctl(sfd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) < 0)
        return -1;
    if (ioctl(ufd, UI_SET_EVBIT, EV_ABS) < 0)
        return -1;
    for (unsigned int code = 0; code < ABS_CNT; code++) {
        if (TEST_BIT(code, bits) && setup_abs_from_source(ufd, sfd, code) < 0)
            return -1;
    }
    return 0;
}

static bool source_has_rumble(int sfd) {
    unsigned long bits[NBITS(FF_CNT)];
    memset(bits, 0, sizeof(bits));
    if (ioctl(sfd, EVIOCGBIT(EV_FF, sizeof(bits)), bits) < 0)
        return false;
    return FF_RUMBLE < FF_CNT && TEST_BIT(FF_RUMBLE, bits);
}

static int create_virtual_gamepad(int sfd) {
    int ufd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (ufd < 0) {
        perror("open /dev/uinput");
        return -1;
    }

    if (clone_keys(ufd, sfd) < 0 || clone_abs(ufd, sfd) < 0) {
        perror("clone input capabilities");
        close(ufd);
        return -1;
    }

    bool have_rumble = source_has_rumble(sfd);
    if (have_rumble) {
        if (ioctl(ufd, UI_SET_EVBIT, EV_FF) < 0 || ioctl(ufd, UI_SET_FFBIT, FF_RUMBLE) < 0) {
            perror("enable FF_RUMBLE");
            close(ufd);
            return -1;
        }
    }

    struct input_id src_id;
    memset(&src_id, 0, sizeof(src_id));
    (void)ioctl(sfd, EVIOCGID, &src_id);

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, sizeof(setup.name), "%s", VIRTUAL_NAME);

    /* InputPlumber 0.81 skips ordinary uinput evdev nodes.  Preserve the
     * source VID/PID but mark the virtual node as BUS_BLUETOOTH so it is
     * eligible for source matching.  No Bluetooth connection is created. */
    setup.id = src_id;
    setup.id.bustype = BUS_BLUETOOTH;
    setup.ff_effects_max = have_rumble ? FF_MAP_SIZE : 0;

    if (ioctl(ufd, UI_DEV_SETUP, &setup) < 0) {
        perror("UI_DEV_SETUP");
        close(ufd);
        return -1;
    }
    if (ioctl(ufd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        close(ufd);
        return -1;
    }

    usleep(100000);
    return ufd;
}

static void trigger_abs(struct trigger_filter *t, int value, int ufd) {
    if (!t->enabled)
        return;
    t->raw_value = value;
    if (value <= 0) {
        t->pending = false;
        t->pending_frames = 0;
        if (t->output_value != 0) {
            write_event(ufd, EV_ABS, t->abs_code, 0);
            t->output_value = 0;
        }
        if (t->key_code >= 0 && t->output_key != 0) {
            write_event(ufd, EV_KEY, t->key_code, 0);
            t->output_key = 0;
        }
        return;
    }
    if (t->output_value != 0) {
        write_event(ufd, EV_ABS, t->abs_code, value);
        t->output_value = value;
        return;
    }
    if (!t->pending) {
        t->pending = true;
        t->pending_frames = 0;
    }
}

static void trigger_key(struct trigger_filter *t, int value, int ufd) {
    if (!t->enabled || t->key_code < 0)
        return;
    t->raw_key = value ? 1 : 0;
    if (t->output_value == 0)
        return;
    if (t->output_key != t->raw_key) {
        write_event(ufd, EV_KEY, t->key_code, t->raw_key);
        t->output_key = t->raw_key;
    }
}

static void trigger_on_syn(struct trigger_filter *t, int ufd) {
    if (!t->enabled || !t->pending || t->raw_value <= 0)
        return;
    t->pending_frames++;
    if (t->pending_frames < 2)
        return;
    write_event(ufd, EV_ABS, t->abs_code, t->raw_value);
    t->output_value = t->raw_value;
    if (t->key_code >= 0 && t->raw_key != t->output_key) {
        write_event(ufd, EV_KEY, t->key_code, t->raw_key);
        t->output_key = t->raw_key;
    }
    t->pending = false;
    t->pending_frames = 0;
}

static void ff_upload(int ufd, int sfd, int *map, int request_id) {
    struct uinput_ff_upload upload;
    memset(&upload, 0, sizeof(upload));
    upload.request_id = request_id;
    if (ioctl(ufd, UI_BEGIN_FF_UPLOAD, &upload) < 0)
        return;

    int virtual_id = upload.effect.id;
    struct ff_effect effect = upload.effect;
    effect.id = -1;
    if (ioctl(sfd, EVIOCSFF, &effect) < 0) {
        upload.retval = -errno;
    } else {
        if (virtual_id >= 0 && virtual_id < FF_MAP_SIZE)
            map[virtual_id] = effect.id;
        upload.retval = 0;
    }
    ioctl(ufd, UI_END_FF_UPLOAD, &upload);
}

static void ff_erase(int ufd, int sfd, int *map, int request_id) {
    struct uinput_ff_erase erase;
    memset(&erase, 0, sizeof(erase));
    erase.request_id = request_id;
    if (ioctl(ufd, UI_BEGIN_FF_ERASE, &erase) < 0)
        return;

    int virtual_id = (int)erase.effect_id;
    int physical_id = (virtual_id >= 0 && virtual_id < FF_MAP_SIZE) ? map[virtual_id] : -1;
    if (physical_id >= 0) {
        if (ioctl(sfd, EVIOCRMFF, physical_id) < 0)
            erase.retval = -errno;
        else {
            erase.retval = 0;
            map[virtual_id] = -1;
        }
    } else {
        erase.retval = 0;
    }
    ioctl(ufd, UI_END_FF_ERASE, &erase);
}

static void process_uinput_ff(int ufd, int sfd, int *map) {
    struct input_event ev;
    while (read(ufd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD)
            ff_upload(ufd, sfd, map, ev.value);
        else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE)
            ff_erase(ufd, sfd, map, ev.value);
        else if (ev.type == EV_FF) {
            int virtual_id = ev.code;
            if (virtual_id >= 0 && virtual_id < FF_MAP_SIZE && map[virtual_id] >= 0)
                write_event(sfd, EV_FF, map[virtual_id], ev.value);
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("konkr-input-fix %s\n", VERSION);
        return 0;
    }

    const char *variant = getenv("KONKR_VARIANT");
    if (!variant || !*variant)
        variant = "elite";

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    unlink(READY_FILE);

    char source_path[128];
    struct source_desc source;
    if (wait_for_source(variant, source_path, sizeof(source_path), &source) < 0)
        return 1;

    int sfd = open(source_path, O_RDWR | O_NONBLOCK);
    if (sfd < 0) {
        perror("open source");
        return 1;
    }

    char source_name[256];
    if (get_name(sfd, source_name, sizeof(source_name)) < 0)
        snprintf(source_name, sizeof(source_name), "%s", source.name_prefix ? source.name_prefix : "KONKR controller");

    struct input_id sid;
    memset(&sid, 0, sizeof(sid));
    (void)ioctl(sfd, EVIOCGID, &sid);
    fprintf(stderr, "source: %s (%s, %04x:%04x, variant=%s)\n",
            source_path, source_name, sid.vendor, sid.product, variant);

    if (ioctl(sfd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB");
        close(sfd);
        return 1;
    }

    int ufd = create_virtual_gamepad(sfd);
    if (ufd < 0) {
        ioctl(sfd, EVIOCGRAB, 0);
        close(sfd);
        return 1;
    }

    fprintf(stderr, "created virtual device: %s\n", VIRTUAL_NAME);

    FILE *ready = fopen(READY_FILE, "w");
    if (ready) {
        fputs("ready\n", ready);
        fclose(ready);
        chmod(READY_FILE, 0644);
    }

    int lt_key = have_key(sfd, BTN_TL2) ? BTN_TL2 : -1;
    int rt_key = have_key(sfd, BTN_TR2) ? BTN_TR2 : -1;
    struct trigger_filter lt = {
        .enabled = have_abs(sfd, (unsigned)source.trigger_left),
        .abs_code = source.trigger_left,
        .key_code = lt_key,
    };
    struct trigger_filter rt = {
        .enabled = have_abs(sfd, (unsigned)source.trigger_right),
        .abs_code = source.trigger_right,
        .key_code = rt_key,
    };

    fprintf(stderr, "filter ready: R3=%dms, triggers=%s/%s 2-frame, FF=%s\n",
            R3_DEBOUNCE_MS,
            lt.enabled ? "on" : "off",
            rt.enabled ? "on" : "off",
            source_has_rumble(sfd) ? "proxy" : "source-none");

    bool r3_pending = false;
    bool r3_output = false;
    uint64_t r3_started = 0;

    int ff_map[FF_MAP_SIZE];
    for (int i = 0; i < FF_MAP_SIZE; i++)
        ff_map[i] = -1;

    struct pollfd pfds[2] = {
        { .fd = sfd, .events = POLLIN },
        { .fd = ufd, .events = POLLIN },
    };

    while (running) {
        if (r3_pending && !r3_output && monotonic_ms() - r3_started >= R3_DEBOUNCE_MS) {
            write_event(ufd, EV_KEY, BTN_THUMBR, 1);
            write_event(ufd, EV_SYN, SYN_REPORT, 0);
            r3_output = true;
        }

        int timeout = r3_pending && !r3_output ? 5 : 50;
        int pr = poll(pfds, 2, timeout);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (pfds[1].revents & POLLIN)
            process_uinput_ff(ufd, sfd, ff_map);

        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "source device disappeared\n");
            break;
        }
        if (!(pfds[0].revents & POLLIN))
            continue;

        struct input_event events[64];
        ssize_t bytes = 0;
        while ((bytes = read(sfd, events, sizeof(events))) > 0) {
            size_t count = (size_t)bytes / sizeof(events[0]);
            for (size_t i = 0; i < count; i++) {
                struct input_event *ev = &events[i];

                if (ev->type == EV_KEY && ev->code == BTN_THUMBR) {
                    if (ev->value) {
                        if (!r3_pending && !r3_output) {
                            r3_pending = true;
                            r3_started = monotonic_ms();
                        }
                    } else {
                        if (r3_output)
                            write_event(ufd, EV_KEY, BTN_THUMBR, 0);
                        r3_pending = false;
                        r3_output = false;
                    }
                    continue;
                }

                if (lt.enabled && ev->type == EV_ABS && ev->code == lt.abs_code) {
                    trigger_abs(&lt, ev->value, ufd);
                    continue;
                }
                if (rt.enabled && ev->type == EV_ABS && ev->code == rt.abs_code) {
                    trigger_abs(&rt, ev->value, ufd);
                    continue;
                }
                if (lt.key_code >= 0 && ev->type == EV_KEY && ev->code == lt.key_code) {
                    trigger_key(&lt, ev->value, ufd);
                    continue;
                }
                if (rt.key_code >= 0 && ev->type == EV_KEY && ev->code == rt.key_code) {
                    trigger_key(&rt, ev->value, ufd);
                    continue;
                }

                if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
                    trigger_on_syn(&lt, ufd);
                    trigger_on_syn(&rt, ufd);
                    forward_event(ufd, ev);
                    continue;
                }

                if (ev->type == EV_SYN || ev->type == EV_KEY || ev->type == EV_ABS)
                    forward_event(ufd, ev);
            }
        }

        if (bytes < 0 && errno != EAGAIN && errno != EINTR) {
            perror("read source");
            break;
        }
    }

    unlink(READY_FILE);
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    ioctl(sfd, EVIOCGRAB, 0);
    close(sfd);
    return 0;
}
