// Exercise stream segmentation independently of how a particular Linux kernel sends I/O.
#define main vram_host_main
#include "../vram_host.c"
#undef main
#include <assert.h>

int main(void)
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
