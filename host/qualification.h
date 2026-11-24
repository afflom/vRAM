/* Full-capacity service measurement through the same store as the FUSE mount. */
enum { QUALIFY_PAGES = 64 };

static void qualification_page(uint64_t page, uint8_t dst[PAGE])
{
    /* A bijective 64-bit mixer keeps the first word unique at every address.
       Use memcpy so pattern generation also works on unaligned byte buffers. */
    uint64_t x = page;
    for (size_t i = 0; i < PAGE; i += sizeof(x)) {
        x += UINT64_C(0x9e3779b97f4a7c15);
        uint64_t z = x;
        z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
        z ^= z >> 31;
        memcpy(dst + i, &z, sizeof(z));
    }
}

static int qualify_store(struct store *s, size_t pages)
{
    uint8_t (*batch)[PAGE] = malloc(QUALIFY_PAGES * PAGE);
    if (!batch) return 1;
    uint64_t elapsed[2] = {0}, slowest[2] = {0};
    int rc = 1;
    for (unsigned phase = 0; phase < 2; ++phase) {
        for (size_t first = 0; first < pages; first += QUALIFY_PAGES) {
            size_t n = pages - first < QUALIFY_PAGES ? pages - first : QUALIFY_PAGES;
            if (phase == 0)
                for (size_t i = 0; i < n; ++i) qualification_page(first + i, batch[i]);
            uint64_t start = monotonic_ns();
            int failed = phase == 0 ? store_write_pages(s, first, n, batch[0])
                                    : store_read_pages(s, first, n, batch[0]);
            uint64_t ns = monotonic_ns() - start;
            if (failed) { fprintf(stderr, "store I/O failed at page %zu\n", first); goto done; }
            elapsed[phase] += ns;
            if (ns > slowest[phase]) slowest[phase] = ns;
            if (phase == 1) {
                uint8_t expected[PAGE];
                for (size_t i = 0; i < n; ++i) {
                    qualification_page(first + i, expected);
                    if (memcmp(batch[i], expected, PAGE)) {
                        fprintf(stderr, "failed readback at page %zu\n", first + i);
                        goto done;
                    }
                }
            }
        }
    }
    uint64_t reads, writes;
    store_file_stats(s, &reads, &writes);
    if (!elapsed[0] || !elapsed[1] || writes != pages) {
        fprintf(stderr, "full dense workload was not exercised\n");
        goto done;
    }
    printf("{\"verified_bytes\":%" PRIu64 ",\"request_pages\":%d,\"write_ns\":%" PRIu64
           ",\"read_ns\":%" PRIu64 ",\"max_write_request_ns\":%" PRIu64
           ",\"max_read_request_ns\":%" PRIu64 ",\"file_reads\":%" PRIu64
           ",\"file_writes\":%" PRIu64 "}\n",
           (uint64_t)pages * PAGE, QUALIFY_PAGES, elapsed[0], elapsed[1], slowest[0], slowest[1], reads, writes);
    rc = 0;
done:
    free(batch);
    return rc;
}
