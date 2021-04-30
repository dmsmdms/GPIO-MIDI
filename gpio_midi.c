#include "gpio_midi.h"
#ifndef SND_SEQ
#define SND_SEQ "/dev/snd/seq"
#endif
#define __USE_GNU
#include <sound/asequencer.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
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
    const char *        log_path;
    const char *        pid_path;
    const char *        server_ip;
    int                 epoll_fd;
    int                 server_fd;
    int                 seq_fd;
    short               server_port;
    struct snd_seq_addr seq_addr;
} common_t;

static common_t common = {
    .log_path           = APP_NAME ".log",
    .pid_path           = APP_NAME ".pid",
    .server_ip          = NULL,
    .epoll_fd           = -1,
    .server_fd          = -1,
    .seq_fd             = -1,
    .server_port        = 9001,
    .seq_addr.client    = 14,
    .seq_addr.port      = 0,
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
    CREATE_SERVER_SOCKET_ACTION_CODE,
    BIND_SERVER_SOCKET_ACTION_CODE,
    LISTEN_SERVER_SOCKET_ACTION_CODE,
    EPOLL_CREATE_ACTION_CODE,
    EPOLL_ADD_SERVER_SOCKET_ACTION_CODE,
    OPEN_SND_SEQ_ACTION_CODE,

    EPOLL_WAIT_ACTION_CODE,
    ACCEPT_CLIENT_ACTION_CODE,
    EPOLL_ADD_CLIENT_SOCKET_ACTION_CODE,
    READ_EVENTS_ACTION_CODE,
    WRITE_SEQ_EVENTS_ACTION_CODE,

    CONNECT_SERVER_ACTION_CODE,
    SEND_EVENTS_ACTION_CODE,
} action_code_t;

action_code_t main_loop(common_t * const restrict common) {
    while (1) {
        struct epoll_event events[CONFIG_MAX_EPOLL_EVENTS];
        const int N = epoll_wait(common->epoll_fd, events, CONFIG_MAX_EPOLL_EVENTS, -1);

        if (UNLIKELY(N < 0)) {
            return EPOLL_WAIT_ACTION_CODE;
        }

        for (int i = 0; i < N; i++) {
            struct epoll_event * const restrict event = events + i;
            const uint32_t epoll_events = event->events;
            const int fd = event->data.fd;

            if (fd == common->server_fd) {
                const int client_fd = accept4(fd, NULL, NULL, O_NONBLOCK);

                if (UNLIKELY(client_fd < 0)) {
                    return ACCEPT_CLIENT_ACTION_CODE;
                }

                event->events = EPOLLIN | EPOLLET;
                event->data.fd = client_fd;

                const int result = epoll_ctl(common->epoll_fd, EPOLL_CTL_ADD, client_fd, event);

                if (UNLIKELY(result < 0)) {
                    return EPOLL_ADD_CLIENT_SOCKET_ACTION_CODE;
                }
            } else if (epoll_events & EPOLLIN) {
                midi_event_t midi_events[CONFIG_MAX_MIDI_EVENTS];
                int result = read(fd, midi_events, sizeof(midi_events));

                if (UNLIKELY(result <= 0)) {
                    continue;
                }

                result /= sizeof(midi_event_t);
                struct snd_seq_event seq_events[result];

                for (int i = 0; i < result; i++) {
                    const midi_event_t * const restrict event = midi_events + i;
                    struct snd_seq_event * const restrict seq_event = seq_events + i;

                    memset(seq_event, 0, sizeof(seq_event[0]));

                    seq_event->type = (event->velocity > 0 ? SNDRV_SEQ_EVENT_NOTEON : SNDRV_SEQ_EVENT_NOTEOFF);
                    seq_event->flags = SNDRV_SEQ_EVENT_LENGTH_FIXED;
                    seq_event->queue = SNDRV_SEQ_QUEUE_DIRECT;
                    seq_event->dest = common->seq_addr;

                    seq_event->data.note.channel = 0;
                    seq_event->data.note.note = event->key;
                    seq_event->data.note.velocity = event->velocity;
                }

                const int seq_events_size = result * sizeof(struct snd_seq_event);
                result = write(common->seq_fd, seq_events, seq_events_size);

                if (UNLIKELY(result != seq_events_size)) {
                    return WRITE_SEQ_EVENTS_ACTION_CODE;
                }
            } else {
                close(fd);
            }
        }
    }
}

action_code_t init_server(common_t * const restrict common) {
    const int server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);

    if (UNLIKELY(server_fd < 0)) {
        return CREATE_SERVER_SOCKET_ACTION_CODE;
    } else {
        common->server_fd = server_fd;
    }

    struct sockaddr_in sockaddr = {
        .sin_family         = AF_INET,
        .sin_port           = htons(common->server_port),
        .sin_addr.s_addr    = htonl(INADDR_ANY),
    };

    const char * const server_ip = common->server_ip;

    if (server_ip != NULL) {
        inet_pton(AF_INET, server_ip, &sockaddr.sin_addr);
    }

    int result = bind(server_fd, (struct sockaddr *)&sockaddr, sizeof(sockaddr));

    if (UNLIKELY(result < 0)) {
        return BIND_SERVER_SOCKET_ACTION_CODE;
    }

    result = listen(server_fd, SOMAXCONN);

    if (UNLIKELY(result < 0)) {
        return LISTEN_SERVER_SOCKET_ACTION_CODE;
    }

    const int epoll_fd = epoll_create(1);

    if (UNLIKELY(epoll_fd < 0)) {
        return EPOLL_CREATE_ACTION_CODE;
    } else {
        common->epoll_fd = epoll_fd;
    }

    struct epoll_event event = {
        .events     = EPOLLIN,
        .data.fd    = server_fd,
    };

    result = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    if (UNLIKELY(result < 0)) {
        return EPOLL_ADD_SERVER_SOCKET_ACTION_CODE;
    }

    result = open(SND_SEQ, O_WRONLY);

    if (UNLIKELY(result < 0)) {
        return OPEN_SND_SEQ_ACTION_CODE;
    } else {
        common->seq_fd = result;
    }

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

    if (common.seq_fd >= 0) {
        close(common.seq_fd);
    }

    if (common.server_fd >= 0) {
        close(common.server_fd);
    }

    if (common.epoll_fd >= 0) {
        close(common.epoll_fd);
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

        return destroy(init_server(common));
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
                    "GPIO-MIDI server v0.0.1\n"
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
