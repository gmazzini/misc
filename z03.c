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

struct net {
    uint16_t start;
    uint16_t end;
};

struct row {
    char fnv1a[8];
    uint32_t istat;
    uint16_t netstart;
};

struct best {
    char fnv1a[8];
    uint32_t istat;
    uint32_t nnet;
};

int cmp_net(const void *a, const void *b)
{
    const struct net *x = (const struct net *)a;
    const struct net *y = (const struct net *)b;

    if (x->start < y->start) return -1;
    if (x->start > y->start) return 1;
    if (x->end < y->end) return -1;
    if (x->end > y->end) return 1;
    return 0;
}

int cmp_row(const void *a, const void *b)
{
    const struct row *x = (const struct row *)a;
    const struct row *y = (const struct row *)b;
    int c;

    c = memcmp(x->fnv1a, y->fnv1a, 8);
    if (c) return c;

    if (x->istat < y->istat) return -1;
    if (x->istat > y->istat) return 1;

    if (x->netstart < y->netstart) return -1;
    if (x->netstart > y->netstart) return 1;

    return 0;
}

int cmp_best(const void *a, const void *b)
{
    const struct best *x = (const struct best *)a;
    const struct best *y = (const struct best *)b;

    if (x->nnet < y->nnet) return 1;
    if (x->nnet > y->nnet) return -1;

    if (x->istat < y->istat) return -1;
    if (x->istat > y->istat) return 1;

    return memcmp(x->fnv1a, y->fnv1a, 8);
}

uint16_t find_netstart(uint16_t ip, struct net *nets, long nnets)
{
    long l, r, m;

    l = 0;
    r = nnets - 1;

    while (l <= r) {
        m = (l + r) / 2;

        if (ip < nets[m].start) r = m - 1;
        else if (ip > nets[m].end) l = m + 1;
        else return nets[m].start;
    }

    return ip;
}

int main(int argc, char *argv[])
{
    FILE *fp;
    struct rec *v;
    struct net *nets;
    struct row *rows;
    struct best *best;
    long nrec, nnets, capnets, nrows, nbest;
    long i, j;
    int n, k;
    char line[256];
    unsigned int a, b, c, d, e;
    uint16_t start, end;

    if (argc != 2) {
        printf("uso: %s n\n", argv[0]);
        return 1;
    }

    n = atoi(argv[1]);
    if (n < 0) n = 0;

    /* leggi reti.csv */
    fp = fopen("reti.csv", "r");
    if (!fp) return 1;

    capnets = 1024;
    nnets = 0;
    nets = (struct net *)malloc(capnets * sizeof(struct net));
    if (!nets) return 1;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%u.%u.%u.%u/%u", &a, &b, &c, &d, &e) == 5) {
            if (nnets >= capnets) {
                capnets *= 2;
                nets = (struct net *)realloc(nets, capnets * sizeof(struct net));
                if (!nets) return 1;
            }

            start = (uint16_t)((a << 8) | b);
            end = (uint16_t)(start + ((1U << (24 - e)) - 1));

            nets[nnets].start = start;
            nets[nnets].end = end;
            nnets++;
        }
    }
    fclose(fp);

    qsort(nets, nnets, sizeof(struct net), cmp_net);

    /* leggi bkpwifi.bin */
    fp = fopen("/home/bkpwifi.bin", "rb");
    if (!fp) return 1;

    fseek(fp, 0, SEEK_END);
    nrec = ftell(fp) / (long)sizeof(struct rec);
    fseek(fp, 0, SEEK_SET);

    v = (struct rec *)malloc(nrec * sizeof(struct rec));
    rows = (struct row *)malloc(nrec * sizeof(struct row));
    best = (struct best *)malloc(nrec * sizeof(struct best));
    if (!v || !rows || !best) return 1;

    fread(v, sizeof(struct rec), nrec, fp);
    fclose(fp);

    /* normalizza ip -> start rete */
    nrows = 0;
    for (i = 0; i < nrec; i++) {
        memcpy(rows[nrows].fnv1a, v[i].fnv1a, 8);
        rows[nrows].istat = v[i].istat;
        rows[nrows].netstart = find_netstart(v[i].ip, nets, nnets);
        nrows++;
    }

    qsort(rows, nrows, sizeof(struct row), cmp_row);

    /* conta reti distinte per (fnv1a, istat) */
    nbest = 0;
    for (i = 0; i < nrows; ) {
        uint32_t cnt = 1;
        j = i + 1;

        while (j < nrows &&
               memcmp(rows[j].fnv1a, rows[i].fnv1a, 8) == 0 &&
               rows[j].istat == rows[i].istat) {
            if (rows[j].netstart != rows[j - 1].netstart) cnt++;
            j++;
        }

        memcpy(best[nbest].fnv1a, rows[i].fnv1a, 8);
        best[nbest].istat = rows[i].istat;
        best[nbest].nnet = cnt;
        nbest++;

        i = j;
    }

    /* per ogni soggetto tieni solo il massimo istat */
    {
        long outn = 0;

        for (i = 0; i < nbest; ) {
            struct best cur;
            memcpy(cur.fnv1a, best[i].fnv1a, 8);
            cur.istat = best[i].istat;
            cur.nnet = best[i].nnet;

            j = i + 1;
            while (j < nbest && memcmp(best[j].fnv1a, best[i].fnv1a, 8) == 0) {
                if (best[j].nnet > cur.nnet) {
                    cur.istat = best[j].istat;
                    cur.nnet = best[j].nnet;
                }
                j++;
            }

            best[outn] = cur;
            outn++;
            i = j;
        }

        nbest = outn;
    }

    qsort(best, nbest, sizeof(struct best), cmp_best);

    if ((long)n > nbest) n = (int)nbest;

    for (i = 0; i < n; i++) {
        for (k = 0; k < 8; k++)
            printf("%02x", (unsigned char)best[i].fnv1a[k]);
        printf(" %u %u\n",
               (unsigned)best[i].istat,
               (unsigned)best[i].nnet);
    }

    free(v);
    free(nets);
    free(rows);
    free(best);

    return 0;
}
