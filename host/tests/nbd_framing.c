// Exercise stream segmentation independently of how a particular Linux kernel sends I/O.
#include "pager.h"
static size_t range_calls, range_pages;
static int observed_range(struct store *s, size_t first, size_t n, uint8_t *out)
{
    range_calls++;
    range_pages += n;
    return store_read_pages(s, first, n, out);
}

#define store_read_pages observed_range
#define main vram_host_main
#include "../vram_host.c"
#undef main
#undef store_read_pages
#include <assert.h>

static void split_headers(void)
{
    int fd[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    struct nbd_request req = {.magic = htonl(NBD_REQUEST_MAGIC), .type = htonl(NBD_CMD_WRITE),
                              .from = htobe64(PAGE), .len = htonl(PAGE)};
    uint8_t stream[sizeof(req) + PAGE + sizeof(req)], page[PAGE];
    memcpy(stream, &req, sizeof(req));
    for (size_t i = 0; i < PAGE; ++i) stream[sizeof(req)+i] = (uint8_t)(i ^ (i >> 8));
    req.type = htonl(NBD_CMD_READ);
    memcpy(stream + sizeof(req) + PAGE, &req, sizeof(req));
    size_t splits[] = {0, 1, sizeof(req)-1, sizeof(req), sizeof(req)+1, sizeof(req)+PAGE};
    for (size_t i = 0; i < sizeof(splits)/sizeof(splits[0]); ++i) {
        struct input in = {.pos = 0, .end = splits[i]};
        memcpy(in.block, stream, in.end);
        assert(!transfer(fd[0], stream + in.end, sizeof(stream) - in.end, true));
        struct nbd_request got;
        assert(!header(fd[1], &in, &got) && !memcmp(&got, stream, sizeof(got)));
        const uint8_t *body = payload(fd[1], &in, page, PAGE);
        assert(body && !memcmp(body, stream + sizeof(req), PAGE));
        assert(!header(fd[1], &in, &got) && !memcmp(&got, &req, sizeof(req)));
    }
    close(fd[0]); close(fd[1]);
}

struct session {
    int fd, result;
    size_t extent;
    struct store *store;
    struct io_counts counts;
};

static void *run_session(void *arg)
{
    struct session *s = arg;
    /* The last advertised page injects a store read failure. */
    s->result = serve(s->fd, s->store, s->extent, &s->counts);
    close(s->fd);
    return NULL;
}

static void request(int fd, unsigned command, size_t offset, uint32_t length)
{
    struct nbd_request req = {.magic = htonl(NBD_REQUEST_MAGIC), .type = htonl(command),
                              .from = htobe64(offset), .len = htonl(length)};
    memcpy(req.handle, "block-io", sizeof(req.handle));
    assert(!transfer(fd, &req, sizeof(req), true));
}

static void reply(int fd, unsigned error)
{
    struct nbd_reply rep;
    assert(!transfer(fd, &rep, sizeof(rep), false));
    assert(ntohl(rep.magic) == NBD_REPLY_MAGIC && ntohl(rep.error) == error);
    assert(!memcmp(rep.handle, "block-io", sizeof(rep.handle)));
}

static void wire_requests(bool partial)
{
    int fd[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    struct timeval timeout = {.tv_sec = 5};
    assert(!setsockopt(fd[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    struct session s = {.fd = fd[1], .extent = 4, .store = store_new(3)};
    assert(s.store);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, run_session, &s));
    uint8_t want[2*PAGE], got[2*PAGE];
    for (size_t i = 0; i < sizeof(want); ++i) want[i] = (uint8_t)(i ^ (i >> 7));

    if (partial) {
        request(fd[0], NBD_CMD_READ, 2*PAGE, 2*PAGE);
        reply(fd[0], 0);
        assert(!transfer(fd[0], got, PAGE, false));
        assert(read(fd[0], got, PAGE) == 0);
    } else {
        request(fd[0], NBD_CMD_WRITE, PAGE, sizeof(want));
        assert(!transfer(fd[0], want, sizeof(want), true));
        reply(fd[0], 0);
        request(fd[0], NBD_CMD_READ, PAGE, sizeof(want));
        reply(fd[0], 0);
        assert(!transfer(fd[0], got, sizeof(got), false) && !memcmp(want, got, sizeof(got)));
        request(fd[0], NBD_CMD_WRITE, 1, PAGE+1);
        assert(!transfer(fd[0], want, PAGE+1, true));
        request(fd[0], NBD_CMD_FLUSH, 0, 0);
        reply(fd[0], EINVAL);
        reply(fd[0], 0);
        request(fd[0], NBD_CMD_FLUSH, 0, PAGE);
        reply(fd[0], EINVAL);
        request(fd[0], NBD_CMD_WRITE, 0, 0);
        reply(fd[0], EINVAL);
        request(fd[0], NBD_CMD_READ, UINT64_MAX - (PAGE - 1), PAGE);
        reply(fd[0], EINVAL);
        request(fd[0], 0xffff, 0, PAGE);
        reply(fd[0], EOPNOTSUPP);
        request(fd[0], NBD_CMD_READ, 3*PAGE, PAGE);
        reply(fd[0], EIO);
        request(fd[0], NBD_CMD_TRIM, PAGE, sizeof(want));
        reply(fd[0], 0);
        request(fd[0], NBD_CMD_READ, PAGE, sizeof(want));
        reply(fd[0], 0);
        memset(want, 0, sizeof(want));
        assert(!transfer(fd[0], got, sizeof(got), false) && !memcmp(want, got, sizeof(got)));
        request(fd[0], NBD_CMD_DISC, 0, 0);
    }
    assert(!pthread_join(thread, NULL));
    assert(s.result == (partial ? -1 : 0));
    assert(s.counts.reads == (partial ? 1 : 4));
    assert(s.counts.writes == (partial ? 0 : 2));
    assert(s.counts.trims == (partial ? 0 : 2));
    close(fd[0]);
    store_free(s.store);
}

static void read_windows(bool partial)
{
    const size_t n = 2 * READ_FRAME_PAGES + 1;
    int fd[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    struct timeval timeout = {.tv_sec = 5};
    assert(!setsockopt(fd[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    struct session s = {.fd = fd[1], .extent = n + 1, .store = store_new(n)};
    assert(s.store);
    uint8_t page[PAGE];
    for (size_t i = 0; i < n; i++) {
        memset(page, i + 1, PAGE);
        assert(!store_put(s.store, i, page));
    }
    range_calls = range_pages = 0;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, run_session, &s));
    request(fd[0], NBD_CMD_READ, 0, (n + partial) * PAGE);
    reply(fd[0], 0);
    for (size_t i = 0; i < n; i++) {
        assert(!transfer(fd[0], page, PAGE, false));
        for (size_t j = 0; j < PAGE; j++) assert(page[j] == (uint8_t)(i + 1));
    }
    if (partial) assert(read(fd[0], page, PAGE) == 0);
    else request(fd[0], NBD_CMD_DISC, 0, 0);
    assert(!pthread_join(thread, NULL));
    assert(s.result == (partial ? -1 : 0) && s.counts.reads == n);
    assert(range_calls == 2u + partial);
    assert(range_pages == 2u * READ_FRAME_PAGES + 2u * partial);
    close(fd[0]);
    store_free(s.store);
}

int main(void)
{
    read_windows(false);
    read_windows(true);
    split_headers();
    wire_requests(false);
    wire_requests(true);
}
