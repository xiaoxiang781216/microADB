#include <arpa/inet.h>

#include "adb.h"
#include "hal_uv_priv.h"
#include <uv.h>

typedef struct adb_client_tcp_s {
    adb_client_uv_t uc;
    /* libuv handle must be right after adb_client_uv_t */
    uv_tcp_t socket;
} adb_client_tcp_t;

static void tcp_uv_allocate_frame(uv_handle_t* handle,
                       size_t suggested_size, uv_buf_t* buf) {
    UNUSED(handle);
    UNUSED(suggested_size);

    adb_client_tcp_t *client = container_of(handle, adb_client_tcp_t, socket);
    adb_uv_allocate_frame(&client->uc, buf);
}

static void tcp_uv_on_data_available(uv_stream_t* handle,
        ssize_t nread, const uv_buf_t* buf) {
    UNUSED(buf);

    adb_client_tcp_t *client = container_of(handle, adb_client_tcp_t, socket);

    adb_uv_on_data_available(&client->uc, (uv_stream_t*)handle, nread, buf);
}

static int tcp_uv_write(adb_client_t *c, apacket *p) {
    uv_buf_t buf;
    apacket_uv_t *up = container_of(p, apacket_uv_t, p);
    adb_client_tcp_t *client = container_of(c, adb_client_tcp_t, uc.client);

    buf = uv_buf_init((char*)&p->msg,
        sizeof(p->msg) + p->msg.data_length);

    /* Packet is now tracked by libuv */
    up->wr.data = &client->uc;

    if (uv_write(&up->wr, (uv_stream_t*)&client->socket, &buf, 1, adb_uv_after_write)) {
        adb_log("write %d %p %d\n", buf.len, buf.base, client->socket.io_watcher.fd);
        fatal("uv_write failed");
        return -1;
    }

    return 0;
}

static void tcp_uv_kick(adb_client_t *c) {
    adb_client_tcp_t *client = container_of(c, adb_client_tcp_t, uc.client);

    if (!uv_is_active((uv_handle_t*)&client->socket)) {
        adb_log("RESTART READ EVENTS\n");
        /* Restart read events */
        int ret = uv_read_start((uv_stream_t*)&client->socket,
            tcp_uv_allocate_frame,
            tcp_uv_on_data_available);
        /* TODO check return code */
        assert(ret == 0);
    }

    adb_client_kick_services(c);
}

static void tcp_uv_close(adb_client_t *c) {
    adb_client_tcp_t *client = (adb_client_tcp_t*)c;

    /* Close socket and cancel all pending write requests if any */
    uv_close((uv_handle_t*)&client->socket, NULL);

    adb_uv_close_client(&client->uc);
}

static const adb_client_ops_t adb_tcp_uv_ops = {
    .write = tcp_uv_write,
    .kick  = tcp_uv_kick,
    .close = tcp_uv_close
};

static void tcp_on_connection(uv_stream_t* server, int status) {
    int ret;
    adb_client_tcp_t *client;
    adb_context_uv_t *adbd = (adb_context_uv_t*)server->data;

    if (status < 0) {
        adb_log("connect failed %d\n", status);
        return;
    }

    client = (adb_client_tcp_t*)adb_uv_create_client(sizeof(*client));
    if (client == NULL) {
        adb_log("failed to allocate stream\n");
        return;
    }

    /* Setup adb_client */
    client->uc.client.ops = &adb_tcp_uv_ops;

    ret = uv_tcp_init(adbd->loop, &client->socket);
    assert(ret == 0);

    client->socket.data = server;


    ret = uv_accept(server, (uv_stream_t*)&client->socket);
    /* TODO check return code */
    assert(ret == 0);

    ret = uv_read_start((uv_stream_t*)&client->socket,
        tcp_uv_allocate_frame,
        tcp_uv_on_data_available);
    /* TODO check return code */
    assert(ret == 0);

    /* Insert client in context */
    client->uc.client.next = adbd->context.clients;
    adbd->context.clients = &client->uc.client;
}

int adb_uv_tcp_setup(adb_context_uv_t *adbd) {
    int ret;
    struct sockaddr_in addr;

    ret = uv_tcp_init(adbd->loop, &adbd->tcp_server);
    adbd->tcp_server.data = adbd;
    if (ret) {
        /* TODO: Error codes */
        adb_log("tcp server init error %d %d\n", ret, errno);
        return ret;
    }

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(CONFIG_SYSTEM_ADB_TCP_SERVER_PORT);

    ret = uv_tcp_bind(&adbd->tcp_server, (const struct sockaddr*)&addr, 0);
    if (ret) {
        /* TODO: Error codes */
        adb_log("tcp server bind error %d %d\n", ret, errno);
        return ret;
    }

    ret = uv_listen((uv_stream_t*)&adbd->tcp_server,
        SOMAXCONN, tcp_on_connection);
    if (ret) {
        /* TODO: Error codes */
        adb_log("tcp server listen error %d %d\n", ret, errno);
        return ret;
    }
    return 0;
}
