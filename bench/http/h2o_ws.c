/* WebSocket echo server on h2o (libh2o-evloop + wslay) for the
 * harness in bench/http/run.sh: the C row measures a real, known
 * library rather than a hand-written epoll loop. Adapted from
 * h2o's examples/libh2o/websocket.c (MIT, DeNA) with SSL stripped
 * and one event loop per thread over SO_REUSEPORT listeners.
 * Text/binary messages are echoed verbatim.
 * Usage: h2o_ws [port] [threads] (threads defaults to the online
 * CPU count). */
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "h2o.h"
#include "h2o/websocket.h"

static h2o_globalconf_t config;
static int g_port;

static void on_ws_message(h2o_websocket_conn_t *conn, const struct wslay_event_on_msg_recv_arg *arg)
{
    if (arg == NULL) {
        h2o_websocket_close(conn);
        return;
    }
    if (!wslay_is_ctrl_frame(arg->opcode)) {
        struct wslay_event_msg msgarg = {arg->opcode, arg->msg, arg->msg_length};
        wslay_event_queue_msg(conn->ws_ctx, &msgarg);
    }
}

static int on_req(h2o_handler_t *self, h2o_req_t *req)
{
    const char *client_key;
    (void)self;
    if (h2o_is_websocket_handshake(req, &client_key) != 0 || client_key == NULL)
        return -1;
    h2o_upgrade_to_websocket(req, client_key, NULL, on_ws_message);
    return 0;
}

typedef struct {
    h2o_context_t ctx;
    h2o_accept_ctx_t accept_ctx;
} loop_t;

static void on_accept(h2o_socket_t *listener, const char *err)
{
    h2o_socket_t *sock;
    if (err != NULL)
        return;
    if ((sock = h2o_evloop_socket_accept(listener)) == NULL)
        return;
    h2o_accept((h2o_accept_ctx_t *)listener->data, sock);
}

static void *loop_main(void *arg)
{
    loop_t *loop = (loop_t *)arg;
    struct sockaddr_in addr;
    int fd, one = 1;
    h2o_socket_t *sock;

    h2o_context_init(&loop->ctx, h2o_evloop_create(), &config);
    loop->accept_ctx.ctx = &loop->ctx;
    loop->accept_ctx.hosts = config.hosts;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001);
    addr.sin_port = htons((unsigned short)g_port);

    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, SOMAXCONN) != 0) {
        fprintf(stderr, "failed to listen on 127.0.0.1:%d: %s\n", g_port, strerror(errno));
        exit(1);
    }

    sock = h2o_evloop_socket_create(loop->ctx.loop, fd, H2O_SOCKET_FLAG_DONT_READ);
    sock->data = &loop->accept_ctx;
    h2o_socket_read_start(sock, on_accept);

    while (h2o_evloop_run(loop->ctx.loop, INT32_MAX) == 0)
        ;
    return NULL;
}

int main(int argc, char **argv)
{
    int threads, i;
    loop_t *loops;
    pthread_t *tids;
    h2o_hostconf_t *hostconf;
    h2o_pathconf_t *pathconf;

    g_port = argc > 1 ? atoi(argv[1]) : 18311;
    threads = argc > 2 ? atoi(argv[2]) : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (threads < 1)
        threads = 1;

    signal(SIGPIPE, SIG_IGN);

    h2o_config_init(&config);
    hostconf = h2o_config_register_host(&config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    pathconf = h2o_config_register_path(hostconf, "/", 0);
    h2o_create_handler(pathconf, sizeof(h2o_handler_t))->on_req = on_req;

    loops = (loop_t *)calloc((size_t)threads, sizeof(*loops));
    tids = (pthread_t *)calloc((size_t)threads, sizeof(*tids));
    for (i = 1; i < threads; i++)
        pthread_create(&tids[i], NULL, loop_main, &loops[i]);
    loop_main(&loops[0]);
    return 0;
}
