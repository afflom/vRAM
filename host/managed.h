/* Linux swap activation must run on a different thread from NBD I/O: swapon
 * reads the swap header, and swapoff can fault every occupied page back in. */
#include <stddef.h>
#include <sys/swap.h>
#include <sys/un.h>

struct managed {
    int device, socket, error;
    const char *path;
    struct store *store;
    sigset_t signals;
};

static unsigned long available_mib(void)
{
    FILE *info = fopen("/proc/meminfo", "r");
    if (!info) die("read available memory");
    char line[160];
    unsigned long kib;
    while (fgets(line, sizeof(line), info)) {
        if (sscanf(line, "MemAvailable: %lu kB", &kib) == 1) {
            fclose(info);
            return vram_host_auto_mib(kib / 1024);
        }
    }
    fclose(info);
    errno = ENODATA;
    die("MemAvailable");
    return 0;
}

static int notify_manager(const char *message)
{
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path) return 0;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    size_t len = strlen(path);
    if (!len || len >= sizeof(addr.sun_path)) { errno = EINVAL; return -1; }
    memcpy(addr.sun_path, path, len + 1);
    if (*path == '@') addr.sun_path[0] = 0;
    int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;
    int rc = sendto(sock, message, strlen(message), MSG_NOSIGNAL, (struct sockaddr *)&addr,
                    offsetof(struct sockaddr_un, sun_path) + len + (*path != '@'));
    int saved = errno;
    close(sock);
    errno = saved;
    return rc < 0 ? -1 : 0;
}

static void report_store(struct store *s)
{
    uint64_t counts[5], pages, hits, skips, residuals;
    store_stats(s, counts);
    store_literal_stats(s, &pages, &hits, &skips, &residuals);
    printf("VRAM_LIVE {\"store_bytes\":%lu,\"raw_pages\":%lu,\"unique_literals\":%lu,\"literal_hits\":%lu,\"literal_fast_writes\":%lu,\"residual_captures\":%lu}\n",
           counts[4], counts[2], pages, hits, skips, residuals);
    fflush(stdout);
}

static void *manage_swap(void *arg)
{
    struct managed *m = arg;
    struct stat st;
    if (fstat(m->device, &st)) goto failed;
    char pidfile[96];
    snprintf(pidfile, sizeof(pidfile), "/sys/dev/block/%u:%u/pid", major(st.st_rdev), minor(st.st_rdev));
    int attempts = 0;
    while (access(pidfile, F_OK) && attempts++ < 200) usleep(50000);
    /* Linux swap UAPI bit 18 requests freed-cluster discard, without the
       whole-device discard at activation. Some libc headers omit this flag. */
    if (swapon(m->path, SWAP_FLAG_PREFER | 100 | SWAP_FLAG_DISCARD | 0x40000)) goto failed;
    if (notify_manager("READY=1\nSTATUS=Serving Verdant swap")) {
        m->error = errno;
        perror("systemd readiness");
    } else {
        int signal_number;
        for (;;) {
            int rc = sigwait(&m->signals, &signal_number);
            if (rc) { m->error = rc; break; }
            if (signal_number != SIGUSR1) break;
            report_store(m->store);
        }
    }
    notify_manager("STOPPING=1\nSTATUS=Returning swap pages to Linux");
    while (swapoff(m->path)) {
        if (errno == EINVAL) break; /* Already disabled by the operator. */
        perror("swapoff; keeping the server online and retrying");
        sleep(5);
    }
    if (ioctl(m->device, NBD_DISCONNECT)) goto failed;
    return NULL;
failed:
    m->error = errno;
    perror("manage Verdant swap");
    shutdown(m->socket, SHUT_RDWR);
    return NULL;
}
