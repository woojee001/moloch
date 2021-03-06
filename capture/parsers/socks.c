#include <string.h>
#include "moloch.h"

/******************************************************************************/
typedef struct socksinfo {
    char     *user;
    char     *host;
    uint32_t  ip;
    uint16_t  port;
    uint16_t  userlen;
    uint16_t  hostlen;
    uint8_t   which;
    uint8_t   state4;
    uint8_t   state5[2];
} SocksInfo_t;

/******************************************************************************/
#define SOCKS4_STATE_REPLY        0
#define SOCKS4_STATE_DATA         1
int socks4_parser(MolochSession_t *session, void *uw, const unsigned char *data, int remaining)
{
    SocksInfo_t            *socks          = uw;

    switch(socks->state4) {
    case SOCKS4_STATE_REPLY:
        if (session->which == socks->which)
            return 0;
        if (remaining >= 8 && data[0] == 0 && data[1] >= 0x5a && data[1] <= 0x5d) {
            if (socks->ip)
                moloch_field_int_add(MOLOCH_FIELD_SOCKS_IP, session, socks->ip);
            moloch_field_int_add(MOLOCH_FIELD_SOCKS_PORT, session, socks->port);
            moloch_nids_add_tag(session, MOLOCH_FIELD_TAGS, "protocol:socks");

            if (socks->user) {
                if (!moloch_field_string_add(MOLOCH_FIELD_SOCKS_USER, session, socks->user, socks->userlen, FALSE)) {
                    g_free(socks->user);
                }
                socks->user = 0;
            }
            if (socks->host) {
                if (!moloch_field_string_add(MOLOCH_FIELD_SOCKS_HOST, session, socks->host, socks->hostlen, FALSE)) {
                    g_free(socks->host);
                }
                socks->host = 0;
            }
            moloch_parsers_classify_tcp(session, data+8, remaining-8);
            socks->state4 = SOCKS4_STATE_DATA;
            return 8;
        }
        break;
    case SOCKS4_STATE_DATA:
        if (session->which != socks->which)
            return 0;
        moloch_parsers_classify_tcp(session, data, remaining);
        moloch_parsers_unregister(session, uw);
        break;
    }

    return 0;
}
/******************************************************************************/
#define SOCKS5_STATE_VER_REQUEST    1
#define SOCKS5_STATE_VER_REPLY      2
#define SOCKS5_STATE_USER_REQUEST   3
#define SOCKS5_STATE_USER_REPLY     4
#define SOCKS5_STATE_CONN_REQUEST   5
#define SOCKS5_STATE_CONN_REPLY     6
#define SOCKS5_STATE_CONN_DATA      7
int socks5_parser(MolochSession_t *session, void *uw, const unsigned char *data, int remaining)
{
    SocksInfo_t            *socks          = uw;

    //LOG("%d %d %d", session->which, socks->which, socks->state5[session->which]);
    //moloch_print_hex_string(data, remaining);

    switch(socks->state5[session->which]) {
    case SOCKS5_STATE_VER_REQUEST:
        if (data[2] == 0) {
            socks->state5[session->which] = SOCKS5_STATE_CONN_REQUEST;
        } else {
            socks->state5[session->which] = SOCKS5_STATE_USER_REQUEST;
        }
        socks->state5[(session->which+1)%2] = SOCKS5_STATE_VER_REPLY;
        break;
    case SOCKS5_STATE_VER_REPLY:
        if (remaining != 2 || data[0] != 5 || data[1] > 2) {
            moloch_parsers_unregister(session, uw);
            return 0;
        }

        moloch_nids_add_tag(session, MOLOCH_FIELD_TAGS, "protocol:socks");

        if (socks->state5[socks->which] == SOCKS5_STATE_CONN_DATA) {
            // Other side of connection already in data state
            socks->state5[session->which] = SOCKS5_STATE_CONN_REPLY;
        } else if (data[1] == 0) {
            socks->state5[socks->which] = SOCKS5_STATE_CONN_REQUEST;
            socks->state5[session->which] = SOCKS5_STATE_CONN_REPLY;
        } else if (data[1] == 2) {
            socks->state5[socks->which] = SOCKS5_STATE_USER_REQUEST;
            socks->state5[session->which] = SOCKS5_STATE_USER_REPLY;
        } else {
            // We don't handle other auth methods
            moloch_parsers_unregister(session, uw);
        }


        return 2;
    case SOCKS5_STATE_USER_REQUEST:
        if ((2 + data[1] > (int)remaining) || (2 + data[1] + 1 + data[data[1]+2]  > (int)remaining)) {
            moloch_parsers_unregister(session, uw);
            return 0;
        }

        moloch_field_string_add(MOLOCH_FIELD_SOCKS_USER, session, (char *)data + 2, data[1], TRUE);
        moloch_nids_add_tag(session, MOLOCH_FIELD_TAGS, "socks:password");
        socks->state5[session->which] = SOCKS5_STATE_CONN_REQUEST;
        return data[1] + 1 + data[data[1]+2];
    case SOCKS5_STATE_USER_REPLY:
        socks->state5[session->which] = SOCKS5_STATE_CONN_REPLY;
        return 2;
    case SOCKS5_STATE_CONN_REQUEST:
        if (remaining < 6 || data[0] != 5 || data[1] != 1 || data[2] != 0) {
            moloch_parsers_unregister(session, uw);
            return 0;
        }

        socks->state5[session->which] = SOCKS5_STATE_CONN_DATA;
        if (data[3] == 1) { // IPV4
            socks->port = (data[8]&0xff) << 8 | (data[9]&0xff);
            memcpy(&socks->ip, data+4, 4);
            moloch_field_int_add(MOLOCH_FIELD_SOCKS_IP, session, socks->ip);
            moloch_field_int_add(MOLOCH_FIELD_SOCKS_PORT, session, socks->port);
            return 4 + 4 + 2;
        } else if (data[3] == 3) { // Domain Name
            socks->port = (data[5+data[4]]&0xff) << 8 | (data[6+data[4]]&0xff);
            char *lower = g_ascii_strdown((char*)data+5, data[4]);
            if (!moloch_field_string_add(MOLOCH_FIELD_SOCKS_HOST, session, lower, data[4], FALSE)) {
                g_free(lower);
            }
            moloch_field_int_add(MOLOCH_FIELD_SOCKS_PORT, session, socks->port);
            return 4 + 1 + data[4] + 2;
        } else if (data[3] == 4) { // IPV6
            return 4 + 16 + 2;
        }
        break;
    case SOCKS5_STATE_CONN_REPLY:
        if (remaining < 6) {
            moloch_parsers_unregister(session, uw);
            return 0;
        }

        socks->state5[session->which] = SOCKS5_STATE_CONN_DATA;
        if (data[3] == 1) { // IPV4
            return 4 + 4 + 2;
        } else if (data[3] == 3) { // Domain Name
            return 4 + 1 + data[4] + 2;
        } else if (data[3] == 4) { // IPV6
            return 4 + 16 + 2;
        }
    case SOCKS5_STATE_CONN_DATA:
        moloch_parsers_classify_tcp(session, data, remaining);
        moloch_parsers_unregister(session, uw);
        return 0;
    default:
        moloch_parsers_unregister(session, uw);
    }

    return 0;
}

/******************************************************************************/
void socks_free(MolochSession_t UNUSED(*session), void *uw)
{
    SocksInfo_t            *socks          = uw;

    if (socks->user)
        g_free(socks->user);
    if (socks->host)
        g_free(socks->host);
    MOLOCH_TYPE_FREE(SocksInfo_t, socks);
}
/******************************************************************************/
void socks4_classify(MolochSession_t *session, const unsigned char *data, int len)
{
    if (data[len-1] == 0)  {
        SocksInfo_t *socks;

        socks = MOLOCH_TYPE_ALLOC0(SocksInfo_t);
        socks->which = session->which;
        socks->port = (data[2]&0xff) << 8 | (data[3]&0xff);
        if (data[4] == 0 && data[5] == 0 && data[6] == 0 && data[7] != 0) {
            socks->ip = 0;
        } else {
            memcpy(&socks->ip, data+4, 4);
        }

        int i;
        for(i = 8; i < len && data[i]; i++);
        if (i > 8 && i != len ) {
            socks->user = g_strndup((char *)data+8, i-8);
            socks->userlen = i - 8;
        }

        if (socks->ip == 0) {
            i++;
            int start;
            for(start = i; i < len && data[i]; i++);
            if (i > start && i != len ) {
                socks->hostlen = i-start;
                socks->host = g_ascii_strdown((char*)data+start, i-start);
            }
        }

        moloch_parsers_register(session, socks4_parser, socks, socks_free);
    }
}

/******************************************************************************/
void socks5_classify(MolochSession_t *session, const unsigned char *data, int len)
{
    if ((len >=3 && len <= 5) && data[1] == len - 2 && data[2] <= 3) {
        SocksInfo_t *socks;

        socks = MOLOCH_TYPE_ALLOC0(SocksInfo_t);
        socks->which = session->which;
        socks->state5[session->which] = SOCKS5_STATE_VER_REQUEST;
        moloch_parsers_register(session, socks5_parser, socks, socks_free);
        return;
    }
    return;
}
/******************************************************************************/
void moloch_parser_init()
{
    moloch_field_define_internal(MOLOCH_FIELD_SOCKS_IP,      "socksip",MOLOCH_FIELD_TYPE_IP,        0);
    moloch_field_define_internal(MOLOCH_FIELD_SOCKS_HOST,    "socksho",MOLOCH_FIELD_TYPE_STR,       0);
    moloch_field_define_internal(MOLOCH_FIELD_SOCKS_PORT,    "sockspo",MOLOCH_FIELD_TYPE_INT,       0);
    moloch_field_define_internal(MOLOCH_FIELD_SOCKS_USER,    "socksuser",MOLOCH_FIELD_TYPE_STR,     0);

    moloch_parsers_classifier_register_tcp("socks5", 0, (unsigned char*)"\005", 1, socks5_classify);
    moloch_parsers_classifier_register_tcp("socks4", 0, (unsigned char*)"\004\000", 2, socks4_classify);
    moloch_parsers_classifier_register_tcp("socks4", 0, (unsigned char*)"\004\001", 2, socks4_classify);
}

