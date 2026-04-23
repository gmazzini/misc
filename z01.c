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

struct subj {
    char fnv1a[8];
    uint32_t nistat;
};

int cmp_rec(const void *a, const void *b)
{
    const struct rec *x = (const struct rec *)a;
    const struct rec *y = (const struct rec *)b;
    int c;

    c = memcmp(x->fnv1a, y->fnv1a, 8);
    if (c) return c;
    if (x->istat < y->istat) return -1;
    if (x->istat > y->istat) return 1;
    return 0;
}

int cmp_subj(const void *a, const void *b)
{
    const struct subj *x = (const struct subj *)a;
    const struct subj *y = (const struct subj *)b;

    if (x->nistat < y->nistat) return 1;
    if (x->nistat > y->nistat) return -1;
    return memcmp(x->fnv1a, y->fnv1a, 8);
}

int main(int argc, char *argv[])
{
    FILE *fp;
    struct rec *v;
    struct subj *s;
    long nrec, i, ns;
    int n, k;
    char lastfnv[8];
    uint32_t lastistat;
    int first;

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
    s = (struct subj *)malloc(nrec * sizeof(struct subj));
    if (!v || !s) return 1;

    fread(v, sizeof(struct rec), nrec, fp);
    fclose(fp);

    qsort(v, nrec, sizeof(struct rec), cmp_rec);

    ns = -1;
    first = 1;

    for (i = 0; i < nrec; i++) {
        if (first || memcmp(v[i].fnv1a, lastfnv, 8) != 0) {
            ns++;
            memcpy(s[ns].fnv1a, v[i].fnv1a, 8);
            s[ns].nistat = 1;
            memcpy(lastfnv, v[i].fnv1a, 8);
            lastistat = v[i].istat;
            first = 0;
        } else if (v[i].istat != lastistat) {
            s[ns].nistat++;
            lastistat = v[i].istat;
        }
    }

    ns++;

    qsort(s, ns, sizeof(struct subj), cmp_subj);

    if ((long)n > ns) n = (int)ns;

    for (i = 0; i < n; i++) {
        for (k = 0; k < 8; k++)
            printf("%02x", (unsigned char)s[i].fnv1a[k]);
        printf(" %u\n", (unsigned)s[i].nistat);
    }

    free(v);
    free(s);
    return 0;
}
