/* PC check of src/ta_roots.h: does a server's certificate chain (PEM file, e.g. from
 * openssl s_client -showcerts) validate against RoboOS's built-in roots, for that name, now?
 * build: see tools/test_wpa.sh (host BearSSL);  usage: test_ta chain.pem server.name */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bearssl.h"
#include "ta_roots.h"

static unsigned char der[8][4096]; static size_t dlen[8]; static int n;
static void dest(void *c, const void *d, size_t l) { (void)c; memcpy(der[n] + dlen[n], d, l); dlen[n] += l; }

int main(int argc, char **argv)
{
    static char pem[65536];
    FILE *f = fopen(argv[1], "rb");
    size_t pl = fread(pem, 1, sizeof pem, f);
    br_pem_decoder_context pc; br_pem_decoder_init(&pc);
    size_t off = 0;
    while (off < pl) {
        off += br_pem_decoder_push(&pc, pem + off, pl - off);
        int e = br_pem_decoder_event(&pc);
        if (e == BR_PEM_BEGIN_OBJ) { dlen[n] = 0; br_pem_decoder_setdest(&pc, dest, 0); }
        if (e == BR_PEM_END_OBJ) n++;
    }
    br_x509_minimal_context xc;
    br_x509_minimal_init_full(&xc, ta_roots, sizeof ta_roots / sizeof ta_roots[0]);
    time_t t = time(0);
    br_x509_minimal_set_time(&xc, (uint32_t)(t / 86400 + 719528), (uint32_t)(t % 86400));
    xc.vtable->start_chain(&xc.vtable, argv[2]);
    for (int i = 0; i < n; i++) { xc.vtable->start_cert(&xc.vtable, dlen[i]); xc.vtable->append(&xc.vtable, der[i], dlen[i]); xc.vtable->end_cert(&xc.vtable); }
    unsigned r = xc.vtable->end_chain(&xc.vtable);
    printf("%d certificates, %u roots: %s (%u)\n", n, (unsigned)(sizeof ta_roots / sizeof ta_roots[0]), r ? "NOT valid" : "VALID", r);
    return r != 0;
}
