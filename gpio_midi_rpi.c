#include "gpio_midi.h"
#ifndef GPIO_CHIP
#define GPIO_CHIP "/dev/gpiochip0"
#endif
#include <linux/gpio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>

#define APP_NAME "gpio-midi"
#define UNUSED __attribute__((unused))
#define PACKED __attribute__((packed))
#define UNLIKELY(x) __builtin_expect(x, 0)

typedef struct {
    const char *    log_path;
    const char *    pid_path;
    const char *    server_ip;
    int             server_fd;
    int             chip_fd;
    int             out_fd;
    int             in_fd;
    short           server_port;
} common_t;

static common_t common = {
    .log_path       = APP_NAME ".log",
    .pid_path       = APP_NAME ".pid",
    .server_ip      = NULL,
    .server_fd      = -1,
    .chip_fd        = -1,
    .out_fd         = -1,
    .in_fd          = -1,
    .server_port    = 9001,
};

typedef enum PACKED {
    SUCCESS_ACTION_CODE,
    UNDEFINED_PROCESS_ACTION_CODE = -128,

    OPEN_LOG_FILE_ACTION_CODE,
    READ_LOG_FILE_ACTION_CODE,
    WRITE_LOG_FILE_ACTION_CODE,

    SIGSEGV_ACTION_CODE,
    SIGTERM_ACTION_CODE,

    OPEN_PID_FILE_ACTION_CODE,
    READ_PID_FILE_ACTION_CODE,
    WRITE_PID_FILE_ACTION_CODE,

    FORK_ACTION_CODE,
    OPEN_GPIO_CHIP_ACTION_CODE,
    IOCTL_GPIO_OUT_ACTION_CODE,
    IOCTL_GPIO_IN_ACTION_CODE,

    CREATE_SERVER_SOCKET_ACTION_CODE,
    IOCTL_GPIO_SET_ACTION_CODE,
    IOCTL_GPIO_GET_ACTION_CODE,
    SEND_EVENTS_ACTION_CODE,

    CONNECT_SERVER_ACTION_CODE,
} action_code_t;

action_code_t main_loop(common_t * const restrict common) {
    const int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (UNLIKELY(server_fd < 0)) {
        return CREATE_SERVER_SOCKET_ACTION_CODE;
    } else {
        common->server_fd = server_fd;
    }

    struct sockaddr_in sockaddr = {
        .sin_family         = AF_INET,
        .sin_port           = htons(common->server_port),
        .sin_addr.s_addr    = htonl(INADDR_LOOPBACK),
    };

    const char * const server_ip = common->server_ip;

    if (server_ip != NULL) {
        inet_pton(AF_INET, server_ip, &sockaddr.sin_addr);
    }

    int gpio_timeout = 1;
    uint8_t keys[37] = { [0 ... 36] = 0 };

    while (1) {
        int result = connect(server_fd, (struct sockaddr *)&sockaddr, sizeof(sockaddr));

        if (UNLIKELY(result < 0)) {
            sleep(CONFIG_CONNECT_TIMEOUT);
            continue;
        }

        while (1) {
            static uint8_t key_hash[5][8] = {
                [2][7] = 0,
                [2][2] = 1,
                [2][6] = 2,
                [2][0] = 3,
                [2][4] = 4,
                [2][1] = 5,
                [2][3] = 6,
                [2][5] = 7,
                [1][7] = 8,
                [1][2] = 9,
                [1][6] = 10,
                [1][0] = 11,
                [1][4] = 12,
                [1][1] = 13,
                [1][3] = 14,
                [1][5] = 15,
                [3][7] = 16,
                [3][2] = 17,
                [3][6] = 18,
                [3][0] = 19,
                [3][4] = 20,
                [3][1] = 21,
                [3][3] = 22,
                [3][5] = 23,
                [4][7] = 24,
                [4][2] = 25,
                [4][6] = 26,
                [4][0] = 27,
                [4][4] = 28,
                [4][1] = 29,
                [4][3] = 30,
                [4][5] = 31,
                [0][7] = 32,
                [0][2] = 33,
                [0][6] = 34,
                [0][0] = 35,
                [0][4] = 36,
            };

            uint8_t midi_event_count = 0;
            midi_event_t midi_events[CONFIG_MAX_MIDI_EVENTS];

            for (uint8_t i = 0; i < 5; i++) {
                struct gpiohandle_data data = { .values[0 ... 4] = 0 };
                data.values[i] = 1;

                result = ioctl(common->out_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);

                if (UNLIKELY(result < 0)) {
                    return IOCTL_GPIO_SET_ACTION_CODE;
                }

                result = ioctl(common->in_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data);

                if (UNLIKELY(result < 0)) {
                    return IOCTL_GPIO_GET_ACTION_CODE;
                }

                for (uint8_t j = 0; j < 8; j++) {
                    const uint8_t key = key_hash[i][j];
                    const uint8_t value = data.values[j];

                    if (keys[key] != value) {
                        keys[key] = value;

                        midi_events[midi_event_count++] = (const midi_event_t) {
                            .key        = key + 3 * 12, // 3 octave offset
                            .velocity   = value * 100
                        };
                    }
                }
            }

            if (midi_event_count > 0) {
                const int events_size = midi_event_count * sizeof(midi_events[0]);
                result = write(server_fd, midi_events, events_size);

                if (result != events_size) {
                    return SEND_EVENTS_ACTION_CODE;
                } else {
                    gpio_timeout = 1;
                }
            } else {
                usleep(gpio_timeout);

                if (gpio_timeout < CONFIG_MAX_GPIO_TIMEOUT) {
                    gpio_timeout <<= 1;
                }
            }
        }
    }
}

action_code_t init_gpio(common_t * const restrict common) {
    const int chip_fd = open(GPIO_CHIP, 0);

    if (UNLIKELY(chip_fd < 0)) {
        return OPEN_GPIO_CHIP_ACTION_CODE;
    } else {
        common->chip_fd = chip_fd;
    }

    struct gpiohandle_request out_request = {
        .lineoffsets    = { 7, 8, 15, 17, 27 },
        .flags          = GPIOHANDLE_REQUEST_OUTPUT,
        .default_values = { 0, 0, 0, 0, 0 },
        .consumer_label = APP_NAME,
        .lines          = 5,
    };

    int result = ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &out_request);

    if (UNLIKELY(result < 0)) {
        return IOCTL_GPIO_OUT_ACTION_CODE;
    } else {
        common->out_fd = out_request.fd;
    }

    struct gpiohandle_request in_request = {
        .lineoffsets    = { 11, 9, 25, 10, 24, 23, 22, 18 },
        .flags          = GPIOHANDLE_REQUEST_INPUT,
        .default_values = { 0, 0, 0, 0, 0, 0, 0, 0 },
        .consumer_label = APP_NAME,
        .lines          = 8,
    };

    result = ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &in_request);

    if (UNLIKELY(result < 0)) {
        return IOCTL_GPIO_IN_ACTION_CODE;
    } else {
        common->in_fd = in_request.fd;
    }

    close(chip_fd);
    common->chip_fd = -1;

    return main_loop(common);
}

action_code_t quit_proc(const common_t * const restrict common) {
    const int pid_fd = open(common->pid_path, O_RDONLY);

    if (UNLIKELY(pid_fd < 0)) {
        return OPEN_PID_FILE_ACTION_CODE;
    }

    pid_t pid;
    const int result = read(pid_fd, &pid, sizeof(pid));
    close(pid_fd);

    if (UNLIKELY(result != sizeof(pid))) {
        return READ_PID_FILE_ACTION_CODE;
    }

    kill(pid, SIGTERM);
    return SUCCESS_ACTION_CODE;
}

action_code_t view_log(const common_t * const restrict common) {
    const int log_fd = open(common->log_path, O_RDONLY);

    if (UNLIKELY(log_fd < 0)) {
        return OPEN_LOG_FILE_ACTION_CODE;
    }

    action_code_t action_code;
    const int result = read(log_fd, &action_code, sizeof(action_code));
    close(log_fd);

    if (UNLIKELY(result != sizeof(action_code))) {
        return READ_LOG_FILE_ACTION_CODE;
    }

    printf("Log: %d\n", action_code);
    return SUCCESS_ACTION_CODE;
}

action_code_t destroy(const action_code_t action_code) {
    unlink(common.pid_path);

    if (common.in_fd >= 0) {
        close(common.in_fd);
    }

    if (common.out_fd >= 0) {
        close(common.out_fd);
    }

    if (common.chip_fd >= 0) {
        close(common.chip_fd);
    }

    if (common.server_fd >= 0) {
        close(common.server_fd);
    }

    const int log_fd = open(common.log_path,
        O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP);

    if (UNLIKELY(log_fd < 0)) {
        return OPEN_LOG_FILE_ACTION_CODE;
    }

    const int result = write(log_fd, &action_code, sizeof(action_code));
    close(log_fd);

    if (UNLIKELY(result != sizeof(action_code))) {
        return WRITE_LOG_FILE_ACTION_CODE;
    }

    return SUCCESS_ACTION_CODE;
}

void sig_proc(const int code) {
    switch (code) {
        case SIGSEGV: return (void)destroy(SIGSEGV_ACTION_CODE);
        case SIGTERM: return (void)destroy(SIGTERM_ACTION_CODE);
    }
}

action_code_t init(common_t * const restrict common) {
    pid_t pid = fork();

    if (pid == SUCCESS_ACTION_CODE) {
        signal(SIGSEGV, sig_proc);
        signal(SIGINT, sig_proc);
        signal(SIGPIPE, SIG_IGN);
        signal(SIGHUP, SIG_IGN);

        close(STDERR_FILENO);
        close(STDOUT_FILENO);
        close(STDIN_FILENO);

        return destroy(init_gpio(common));
    } else if (pid > 0) {
        const int pid_fd = open(common->pid_path,
            O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP);

        if (UNLIKELY(pid_fd < 0)) {
            return OPEN_PID_FILE_ACTION_CODE;
        }

        const int result = write(pid_fd, &pid, sizeof(pid));
        close(pid_fd);

        if (UNLIKELY(result != sizeof(pid))) {
            kill(pid, SIGTERM);
            return WRITE_PID_FILE_ACTION_CODE;
        }

        return SUCCESS_ACTION_CODE;
    }

    return FORK_ACTION_CODE;
}

action_code_t test(common_t * const restrict common, const uint8_t key) {
    const int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (UNLIKELY(server_fd < 0)) {
        return CREATE_SERVER_SOCKET_ACTION_CODE;
    }

    struct sockaddr_in sockaddr = {
        .sin_family         = AF_INET,
        .sin_port           = htons(common->server_port),
        .sin_addr.s_addr    = htonl(INADDR_LOOPBACK),
    };

    const char * const server_ip = common->server_ip;

    if (server_ip != NULL) {
        inet_pton(AF_INET, server_ip, &sockaddr.sin_addr);
    }

    int result = connect(server_fd, (struct sockaddr *)&sockaddr, sizeof(sockaddr));

    if (UNLIKELY(result < 0)) {
        return CONNECT_SERVER_ACTION_CODE;
    }

    midi_event_t event = {
        .key        = key,
        .velocity   = 100,
    };

    result = write(server_fd, &event, sizeof(event));

    if (UNLIKELY(result != sizeof(event))) {
        close(server_fd);
        return SEND_EVENTS_ACTION_CODE;
    }

    event.velocity = 0;
    sleep(CONFIG_TEST_KEY_TIMEOUT);

    result = write(server_fd, &event, sizeof(event));
    close(server_fd);

    if (UNLIKELY(result != sizeof(event))) {
        return SEND_EVENTS_ACTION_CODE;
    }

    return SUCCESS_ACTION_CODE;
}

uint8_t get_key(const char * const restrict arg) {
    uint8_t key = 0;

    switch (arg[0]) {
        case 'C': key = 0; break;
        case 'D': key = 2; break;
        case 'E': key = 4; break;
        case 'F': key = 5; break;
        case 'G': key = 7; break;
        case 'A': key = 9; break;
        case 'B': key = 11; break;
    }

    char next_c = arg[2];

    switch (arg[1]) {
        case '#': key++; break;
        case 'b': key--; break;
        default: next_c = arg[1];
    }

    return key + (next_c - '0') * 12;
}

typedef enum {
    STANDARD_PROCESS,
    VIEW_LOG_PROCESS,
    QUIT_PROCESS,
    TEST_PROCESS,
} process_t;

int main(const int argc, char * const argv[]) {
    process_t process = STANDARD_PROCESS;
    uint8_t test_key = 0;

    while (1) {
        static const struct option options[] = {
            {
                .name       = "server",
                .has_arg    = required_argument,
                .flag       = NULL,
                .val        = 's',
            },
            {
                .name       = "log-file",
                .has_arg    = required_argument,
                .flag       = NULL,
                .val        = 'l',
            },
            {
                .name       = "pid-file",
                .has_arg    = required_argument,
                .flag       = NULL,
                .val        = 'p',
            },
            {
                .name       = "quit",
                .has_arg    = no_argument,
                .flag       = NULL,
                .val        = 'q',
            },
            {
                .name       = "view-log",
                .has_arg    = no_argument,
                .flag       = NULL,
                .val        = 'v',
            },
            {
                .name       = "test",
                .has_arg    = required_argument,
                .flag       = NULL,
                .val        = 't',
            },
            {
                .name       = "help",
                .has_arg    = no_argument,
                .flag       = NULL,
                .val        = 'h',
            },
            {   NULL, 0, NULL, 0    }
        };

        const int opt = getopt_long(argc, argv, "s:l:p:qvt:h", options, NULL);

        if (UNLIKELY(opt < 0)) {
            break;
        }

        switch (opt) {
            case 's': {
                char * restrict port = strchr(optarg, ':');

                if (port != NULL) {
                    *port++ = '\0';
                    common.server_port = atoi(port);
                }

                common.server_ip = optarg;
            } break;
            case 'l': common.log_path = optarg; break;
            case 'p': common.pid_path = optarg; break;
            case 'v': process = VIEW_LOG_PROCESS; break;
            case 'q': process = QUIT_PROCESS; break;
            case 't': {
                process = TEST_PROCESS;
                test_key = get_key(optarg);
            } break;
            case '?': case 'h': {
                static const char help[] =
                    "GPIO-MIDI RPI client v0.0.1\n"
                    "-s, --server\t:\tServer IP and port (127.0.0.1:9001)\n"
                    "-l, --log-file\t:\tLog file (" APP_NAME ".log)\n"
                    "-p, --pid-file\t:\tPid file (" APP_NAME ".pid)\n"
                    "-q, --quit\t:\tQuit daemod\n"
                    "-v, --view-log\t:\tView log action code\n"
                    "-t, --test\t:\tPlay test note (-t C#3 or -t Db4 or -t E5)\n"
                    "-h, --help\t:\tPrint this help info\n";

                write(STDOUT_FILENO, help, sizeof(help) - 1);
            } return SUCCESS_ACTION_CODE;
        }
    }

    switch (process) {
        case STANDARD_PROCESS: return init(&common);
        case VIEW_LOG_PROCESS: return view_log(&common);
        case QUIT_PROCESS: return quit_proc(&common);
        case TEST_PROCESS: return test(&common, test_key);
    }

    return UNDEFINED_PROCESS_ACTION_CODE;
}
