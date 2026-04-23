#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

struct rec {
    uint16_t ip;
    uint16_t tt;
    uint32_t istat;
    char fnv1a[8];
    uint32_t req;
};

struct worker {
    char fnv1a[8];
    uint32_t days;
};

struct rel {
    uint32_t istat;
    uint32_t cnt;
};

int cmp_rec(const void *a, const void *b)
{
    const struct rec *x = (const struct rec *)a;
    const struct rec *y = (const struct rec *)b;
    int c;

    c = memcmp(x->fnv1a, y->fnv1a, 8);
    if (c) return c;
    if (x->tt < y->tt) return -1;
    if (x->tt > y->tt) return 1;
    if (x->ip < y->ip) return -1;
    if (x->ip > y->ip) return 1;
    if (x->istat < y->istat) return -1;
    if (x->istat > y->istat) return 1;
    return 0;
}

int cmp_worker(const void *a, const void *b)
{
    const struct worker *x = (const struct worker *)a;
    const struct worker *y = (const struct worker *)b;
    return memcmp(x->fnv1a, y->fnv1a, 8);
}

int cmp_rel_istat(const void *a, const void *b)
{
    const struct rel *x = (const struct rel *)a;
    const struct rel *y = (const struct rel *)b;
    if (x->istat < y->istat) return -1;
    if (x->istat > y->istat) return 1;
    return 0;
}

int cmp_rel_cnt(const void *a, const void *b)
{
    const struct rel *x = (const struct rel *)a;
    const struct rel *y = (const struct rel *)b;
    if (x->cnt < y->cnt) return 1;
    if (x->cnt > y->cnt) return -1;
    if (x->istat < y->istat) return -1;
    if (x->istat > y->istat) return 1;
    return 0;
}

int is_airport(uint16_t ip)
{
    return ip >= ((54 << 8) | 192) && ip <= ((54 << 8) | 223);
}

int is_worker(char fnv1a[8], struct worker *w, long nw)
{
    long l, r, m, c;
    l = 0;
    r = nw - 1;

    while (l <= r) {
        m = (l + r) / 2;
        c = memcmp(fnv1a, w[m].fnv1a, 8);
        if (c == 0) return 1;
        if (c < 0) r = m - 1;
        else l = m + 1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    FILE *fp;
    struct rec *v;
    struct worker *w;
    struct rel *rel;
    long nrec, i, j, nw, nrel;
    char curfnv[8], lastfnv[8];
    uint32_t curtt, lasttt;
    int first, has_air;
    uint32_t air_days;
    int n, k;

    if (argc != 2) {
        printf("uso: %s n\n", argv[0]);
        return 1;
    }

    n = atoi(argv[1]);
    if (n < 0) n = 0;

    fp = fopen("/home/bkpwifi.bin", "rb");
    if (!fp) return 1;

    fseek(fp, 0, SEEK_END);
    nrec = ftell(fp) / (long)sizeof(struct rec);
    fseek(fp, 0, SEEK_SET);

    v = (struct rec *)malloc(nrec * sizeof(struct rec));
    w = (struct worker *)malloc(nrec * sizeof(struct worker));
    rel = (struct rel *)malloc(nrec * sizeof(struct rel));
    if (!v || !w || !rel) return 1;

    fread(v, sizeof(struct rec), nrec, fp);
    fclose(fp);

    qsort(v, nrec, sizeof(struct rec), cmp_rec);

    /* trova lavoratori aeroportuali */
    nw = 0;
    first = 1;
    air_days = 0;
    has_air = 0;

    for (i = 0; i < nrec; i++) {
        if (first || memcmp(v[i].fnv1a, lastfnv, 8) != 0 || v[i].tt != lasttt) {
            if (!first && has_air) air_days++;

            if (first || memcmp(v[i].fnv1a, lastfnv, 8) != 0) {
                if (!first && air_days > 90) {
                    memcpy(w[nw].fnv1a, lastfnv, 8);
                    w[nw].days = air_days;
                    nw++;
                }
                memcpy(lastfnv, v[i].fnv1a, 8);
                air_days = 0;
            }

            lasttt = v[i].tt;
            has_air = 0;
            first = 0;
        }

        if (is_airport(v[i].ip)) has_air = 1;
    }

    if (!first && has_air) air_days++;
    if (!first && air_days > 90) {
        memcpy(w[nw].fnv1a, lastfnv, 8);
        w[nw].days = air_days;
        nw++;
    }

    qsort(w, nw, sizeof(struct worker), cmp_worker);

    /* costruisci rapporti soggetto-giorno-istat */
    nrel = 0;

    for (i = 0; i < nrec; ) {
        memcpy(curfnv, v[i].fnv1a, 8);
        curtt = v[i].tt;

        j = i;
        has_air = 0;

        while (j < nrec && memcmp(v[j].fnv1a, curfnv, 8) == 0 && v[j].tt == curtt) {
            if (is_airport(v[j].ip)) has_air = 1;
            j++;
        }

        if (has_air && !is_worker(curfnv, w, nw)) {
            uint32_t lastistat = 0;
            int firstistat = 1;

            while (i < j) {
                if (!is_airport(v[i].ip)) {
                    if (firstistat || v[i].istat != lastistat) {
                        rel[nrel].istat = v[i].istat;
                        rel[nrel].cnt = 1;
                        nrel++;
                        lastistat = v[i].istat;
                        firstistat = 0;
                    }
                }
                i++;
            }
        } else {
            i = j;
        }
    }

    /* aggrega per istat */
    qsort(rel, nrel, sizeof(struct rel), cmp_rel_istat);

    {
        long outn;
        outn = 0;

        for (i = 0; i < nrel; ) {
            uint32_t ist = rel[i].istat;
            uint32_t c = 0;
            j = i;

            while (j < nrel && rel[j].istat == ist) {
                c += rel[j].cnt;
                j++;
            }

            rel[outn].istat = ist;
            rel[outn].cnt = c;
            outn++;
            i = j;
        }

        nrel = outn;
    }

    qsort(rel, nrel, sizeof(struct rel), cmp_rel_cnt);

    if ((long)n > nrel) n = (int)nrel;

    for (k = 0; k < n; k++) {
        printf("%u %u\n",
               (unsigned)rel[k].istat,
               (unsigned)rel[k].cnt);
    }

    free(v);
    free(w);
    free(rel);
    return 0;
}
