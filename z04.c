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
    uint32_t ndays;
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

    if (x->istat < y->istat) return -1;
    if (x->istat > y->istat) return 1;

    return 0;
}

int cmp_subj(const void *a, const void *b)
{
    const struct subj *x = (const struct subj *)a;
    const struct subj *y = (const struct subj *)b;

    if (x->ndays < y->ndays) return 1;
    if (x->ndays > y->ndays) return -1;

    return memcmp(x->fnv1a, y->fnv1a, 8);
}

int main(int argc, char *argv[])
{
    FILE *fp;
    struct rec *v;
    struct subj *s;
    long nrec, i, j, ns;
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
    s = (struct subj *)malloc(nrec * sizeof(struct subj));
    if (!v || !s) return 1;

    fread(v, sizeof(struct rec), nrec, fp);
    fclose(fp);

    qsort(v, nrec, sizeof(struct rec), cmp_rec);

    ns = 0;
    i = 0;

    while (i < nrec) {
        char curfnv[8];
        uint32_t subj_days = 0;

        memcpy(curfnv, v[i].fnv1a, 8);

        while (i < nrec && memcmp(v[i].fnv1a, curfnv, 8) == 0) {
            uint16_t curtt = v[i].tt;
            int lastprov = -1;
            int nprov = 0;

            j = i;
            while (j < nrec &&
                   memcmp(v[j].fnv1a, curfnv, 8) == 0 &&
                   v[j].tt == curtt) {

                if (v[j].istat >= 33000 && v[j].istat <= 99999) {
                    int prov = (int)(v[j].istat / 1000);
                    if (prov != lastprov) {
                        nprov++;
                        lastprov = prov;
                    }
                }
                j++;
            }

            if (nprov >= 2) subj_days++;

            i = j;
        }

        memcpy(s[ns].fnv1a, curfnv, 8);
        s[ns].ndays = subj_days;
        ns++;
    }

    qsort(s, ns, sizeof(struct subj), cmp_subj);

    if ((long)n > ns) n = (int)ns;

    for (i = 0; i < n; i++) {
        for (k = 0; k < 8; k++)
            printf("%02x", (unsigned char)s[i].fnv1a[k]);
        printf(" %u\n", (unsigned)s[i].ndays);
    }

    free(v);
    free(s);
    return 0;
}
