/*
 * WPA2-PSK supplicant (the 4-way and group key handshakes, IEEE 802.11-2016 12.7) for WiFi chips whose
 * firmware has no built-in supplicant (the Pi 3B+ CYW43455 firmware answers "sup_wpa" with
 * BCME_UNSUPPORTED). Plain C + BearSSL only, so tools/test_wpa.c can run it on the PC.
 * Key descriptor version 2 (HMAC-SHA1 MIC, AES key wrap), pairwise CCMP, group CCMP or TKIP.
 */
#ifndef WPA_H
#define WPA_H

struct wpa {
    unsigned char pmk[32], spa[6], aa[6];
    unsigned char anonce[32], snonce[32], ptk[48];   /* KCK 0-15, KEK 16-31, TK 32-47 */
    unsigned char ie[64];                            /* our RSN element, as sent in the association request */
    int ielen, have_ptk, have_replay, done;
    unsigned char replay[8];
    /* results for the driver: a frame to send and keys to install */
    unsigned char out[160];
    int outlen;
    int set_ptk;                                     /* TK = ptk + 32 (16 bytes) for the AP aa */
    int set_gtk, gtk_idx, gtk_len;
    unsigned char gtk[32], gtk_rsc[8];
    const char *err;                                 /* why the last frame was dropped */
    int last;                                        /* the last message handled: 1, 3, or 5 = group rekey */
};

/* PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32) */
void wpa_pmk(const char *pass, const char *ssid, int ssid_len, unsigned char pmk[32]);
/* new association: our MAC address, the PMK and our RSN element (NULL: the usual WPA2-PSK CCMP one) */
void wpa_start(struct wpa *s, const unsigned char pmk[32], const unsigned char own_mac[6],
               const unsigned char *ie, int ielen);
/* one received EAPOL frame (whole Ethernet frame, type 0x888E); returns 1 when it was used */
int wpa_rx(struct wpa *s, const unsigned char *frame, int len);

/* 30 14 01 00 | 00-0F-AC-04 group | 1 x 00-0F-AC-04 pairwise | 1 x 00-0F-AC-02 PSK | caps 0 */
extern const unsigned char wpa_rsn_default[22];

/* building blocks, exported for the tests */
void wpa_hmac_sha1(const unsigned char *key, int klen, const unsigned char *d, int dlen, unsigned char out[20]);
void wpa_prf(const unsigned char *key, int klen, const char *label, const unsigned char *data, int dlen,
             unsigned char *out, int olen);
int  wpa_unwrap(const unsigned char kek[16], const unsigned char *in, int len, unsigned char *out);

/* supplied by the driver (or the test): random bytes for the SNonce */
void wpa_random(unsigned char *out, int n);

#endif
