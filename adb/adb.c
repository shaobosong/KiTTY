/*
 * "Adb" backend.
 */
#ifdef MOD_ADB

#include <stdio.h>
#include <stdlib.h>

#include "putty.h"

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

#define ADB_MAX_BACKLOG 4096

typedef enum {
    STATE_WARMING_UP,
    STATE_SENT_HELLO,
    STATE_ASKED_FOR_SHELL,
    STATE_CONNECTED,
    STATE_WAITING_FOR_ERROR_MESSAGE,
} adb_state;

size_t win_seat_output_local(Seat *seat, bool is_stderr, const void *data, size_t len) ;

typedef struct adb_backend_data {
    //const struct plug_function_table *fn;
    /* the above field _must_ be first in the structure */

    size_t bufsize;
    Socket *s;
    Seat *seat;
    LogContext *logctx;
    
    adb_state state;
    //void *frontend;
    Conf *conf;

    Plug plug;
    Backend backend;
} Adb;

static void adb_size(Backend *be, int width, int height);

static void c_write(Adb *adb, const char *buf, size_t len)
{
    //int backlog = from_backend(adb->frontend, 0, buf, len);
    size_t backlog = win_seat_output_local(adb->seat,false,buf,len);
    sk_set_frozen(adb->s, backlog > ADB_MAX_BACKLOG);
}

static void adb_log(Plug *plug, PlugLogType type, SockAddr *addr, int port,
                    const char *error_msg, int error_code)
{
    //Adb adb = (Adb) plug;
    Adb *adb = container_of(plug, Adb, plug);
    char addrbuf[256], *msg;

    sk_getaddr(addr, addrbuf, lenof(addrbuf));

    if (type == PLUGLOG_CONNECT_TRYING)
        msg = dupprintf("Connecting to %s port %d", addrbuf, port);
    else if (type == PLUGLOG_CONNECT_SUCCESS)
        msg = dupprintf("Connected to %s port %d", addrbuf, port);
    else
        msg = dupprintf("Failed to connect to %s: %s", addrbuf, error_msg);

    logevent(adb->logctx, msg);
    sfree(msg);
}

static void adb_closing(Plug * plug, const char *error_msg, int error_code,
                       bool calling_back)
{
    //Adb adb = (Adb) plug;
    Adb *adb = container_of(plug, Adb, plug);

    if (adb->s) {
        sk_close(adb->s);
        adb->s = NULL;
        //notify_remote_exit(adb->frontend);
	seat_notify_remote_exit(adb->seat);
    }
    if (error_msg) {
        /* A socket error has occurred. */
        logevent(adb->logctx, error_msg);
        //connection_fatal(adb->frontend, "%s", error_msg);
	seat_connection_fatal(adb->seat, "%s", error_msg);
    } /* Otherwise, the remote side closed the connection normally. */
    return ;
}

static void do_fatal(Adb * adb, const char *data, int len) {
    char* d = (char*)smalloc(len+1);
    memcpy(d, data, len);
    d[len] = '\0';
    //connection_fatal(adb->frontend, "adb failure message: '%s'", d);
    seat_connection_fatal(adb->seat, "adb failure message: '%s'", d);
    sfree(d);
}

/** the error might not be available when the error occurs; wait
  * a bit for more data to show up then assume that's the error message.
  */
static void handle_fail(Adb *adb, const char *data, int len) {
    // FAIL0003abc
    char message_length_hex[5];
    unsigned long expected;
    memcpy(message_length_hex, data+4, 4);
    message_length_hex[4] = 0;
    expected = strtoul(message_length_hex, NULL, 16);

    if (len == expected + 8)
        do_fatal(adb, data+8, expected);
    else
        adb->state = STATE_WAITING_FOR_ERROR_MESSAGE;
}

static void adb_receive(Plug *plug, int urgent, const char *data, size_t len)
{
    //Adb adb = (Adb) plug;
    Adb *adb = container_of(plug, Adb, plug);
    if (adb->state == STATE_SENT_HELLO) {
        if (data[0]=='O') { // OKAY
            sk_write(adb->s,"0006shell:",10);
            adb->state = STATE_ASKED_FOR_SHELL; // wait for shell start response
        } else {
             if (data[0]=='F') {
                handle_fail(adb, data, len);
            } else {
                //connection_fatal(adb->frontend, "Bad response after initial send");
		seat_connection_fatal(adb->seat, "%s", "Bad response after initial send");
            }
            return ;
        }
    } else if (adb->state == STATE_ASKED_FOR_SHELL) {
        if (data[0]=='O') { //OKAY
            adb->state = STATE_CONNECTED; // shell started, switch to terminal mode
        } else {
            if (data[0]=='F') {
                handle_fail(adb, data, len);
            } else {
                //connection_fatal(adb->frontend, "Bad response waiting for shell start");
		seat_connection_fatal(adb->seat, "Bad response waiting for shell start");
            }
            return ;
        }
    } else if (adb->state == STATE_WAITING_FOR_ERROR_MESSAGE) {
        do_fatal(adb, data, len);
    } else {
        c_write(adb, data, len);
    }
    return ;
}

static void adb_sent(Plug *plug, size_t bufsize)
{
    //Adb adb = (Adb) plug;
    Adb *adb = container_of(plug, Adb, plug);
    adb->bufsize = bufsize;
}

/*
 * Called to set up the adb connection.
 * 
 * Returns an error message, or NULL on success.
 *
 * Also places the canonical host name into `realhost'. It must be
 * freed by the caller.
 */
		    
static const struct PlugVtable Adb_plugvt = {
        .log = adb_log,
        .closing = adb_closing,
        .receive = adb_receive,
        .sent = adb_sent,
    };

/*
static const char *adb_init(void *frontend_handle, void **backend_handle,
                            Conf *conf,
                            const char *host, int port, char **realhost, int nodelay,
                            int keepalive)
*/	
static char *adb_init(const BackendVtable *vt, Seat *seat,
                      Backend **backend_handle, LogContext *logctx,
                      Conf *conf, const char *host, int port,
                      char **realhost, bool nodelay, bool keepalive)
{
    SockAddr *addr;
    const char *err;
    char *loghost;
    Adb *adb;

    adb = snew(Adb);
    adb->plug.vt = &Adb_plugvt;
    adb->backend.vt = vt;
    
    adb->s = NULL;
    *backend_handle = &adb->backend;
    adb->bufsize = 0;
    adb->conf = conf_copy(conf);

    adb->seat = seat;
    adb->logctx = logctx;
    
    adb->state = STATE_WARMING_UP;

    /*
     * Try to find host.
     */
    {
        char *buf;
        buf = dupprintf("Looking up host \"%s\"%s", "localhost",
                (conf_get_int(conf, CONF_addressfamily) == ADDRTYPE_IPV4 ? " (IPv4)" :
                 (conf_get_int(conf, CONF_addressfamily) == ADDRTYPE_IPV6 ? " (IPv6)" :
                  "")));
        logevent(adb->logctx, buf);
        sfree(buf);
    }
    addr = name_lookup("localhost", port, realhost, conf, conf_get_int(conf, CONF_addressfamily),  adb->logctx , "ADB connection");

    if ((err = sk_addr_error(addr)) != NULL) {
        sk_addr_free(addr);
        return dupstr(err);
    }

    if (port < 0)
        port = 5037; /* default adb port */

    /*
     * Open socket.
     */
    adb->s = new_connection(addr, *realhost, port, false, true, nodelay, keepalive,
                            &adb->plug, conf);
	    
    if ((err = sk_socket_error(adb->s)) != NULL)
        return dupstr(err);

    loghost = conf_get_str(conf, CONF_loghost);
    if (*loghost) {
        char *colon;

        sfree(*realhost);
        *realhost = dupstr(loghost);
        colon = host_strrchr(*realhost, ':');
        if (colon)
            *colon++ = '\0';
    }

    /* send initial data to adb server */
#define ADB_SHELL_DEFAULT_STR "0012" "host:transport-any"
#define ADB_SHELL_DEFAULT_STR_LEN (sizeof(ADB_SHELL_DEFAULT_STR)-1)
#define ADB_SHELL_USB_STR "0012" "host:transport-usb"
#define ADB_SHELL_USB_STR_LEN (sizeof(ADB_SHELL_USB_STR)-1)
#define ADB_SHELL_LOCAL_STR "0015" "host:transport-local"
#define ADB_SHELL_LOCAL_STR_LEN (sizeof(ADB_SHELL_LOCAL_STR)-1)
#define ADB_SHELL_SERIAL_PREFIX "host:transport:"
#define ADB_SHELL_SERIAL_PREFIX_LEN (sizeof(ADB_SHELL_SERIAL_PREFIX)-1)

#   define write_hello(str, len) \
        sk_write(adb->s, str, len); \
        adb->state = STATE_SENT_HELLO;

    do {
        size_t len;
        if (host[0] == ':')
            ++host;

        len = strlen(host);

        if (len == 0 || !strcmp("-a", host) || !strcmp(host, "transport-any")) {
            write_hello(ADB_SHELL_DEFAULT_STR, ADB_SHELL_DEFAULT_STR_LEN);
        } else if (!strcmp("-d", host) || !strcmp(host, "transport-usb")) {
            write_hello(ADB_SHELL_USB_STR, ADB_SHELL_USB_STR_LEN);
        } else if (!strcmp("-e", host) || !strcmp(host, "transport-local")) {
            write_hello(ADB_SHELL_LOCAL_STR, ADB_SHELL_LOCAL_STR_LEN);
        } else {
            char sendbuf[512];
#           define ADB_SHELL_HOST_MAX_LEN (sizeof(sendbuf)-4-ADB_SHELL_SERIAL_PREFIX_LEN)
            if (len > ADB_SHELL_HOST_MAX_LEN)
                len = ADB_SHELL_HOST_MAX_LEN;
            sprintf(sendbuf,"%04lx" ADB_SHELL_SERIAL_PREFIX, (unsigned long)(len+ADB_SHELL_SERIAL_PREFIX_LEN));
            memcpy(sendbuf+4+ADB_SHELL_SERIAL_PREFIX_LEN, host, len);
            write_hello(sendbuf, len+4+ADB_SHELL_SERIAL_PREFIX_LEN);
        }
    } while (0);
    return NULL;
}

static void adb_free(Backend *be)
{
    //Adb adb = (Adb) be;
    Adb *adb = container_of(be, Adb, backend);

    if (adb->s)
        sk_close(adb->s);
    conf_free(adb->conf);
    sfree(adb);
}

/*
 * Stub routine (we don't have any need to reconfigure this backend).
 */
static void adb_reconfig(Backend *be, Conf *conf)
{
}

/*
 * Called to send data down the adb connection.
 */
static size_t adb_send(Backend *be, const char *buf, size_t len)
{
    //Adb adb = (Adb) be;
    Adb *adb = container_of(be, Adb, backend);
    
    if (adb->s == NULL)
        return 0;

    adb->bufsize = sk_write(adb->s, buf, len);

    return adb->bufsize;
}

/*
 * Called to query the current socket sendability status.
 */
static size_t adb_sendbuffer(Backend *be)
{
    //Adb adb = (Adb) be;
    Adb *adb = container_of(be, Adb, backend);
    return adb->bufsize;
}

/*
 * Called to set the size of the window
 */
static void adb_size(Backend *be, int width, int height)
{
    /* Do nothing! */
    return;
}

/*
 * Send adb special codes.
 */
//static void adb_special(void *handle, Telnet_Special code)
static void adb_special(Backend *be, SessionSpecialCode code, int arg)
{
    /* Do nothing! */
    return;
}

/*
 * Return a list of the special codes that make sense in this
 * protocol.
 */
static const SessionSpecial *adb_get_specials(Backend *be)
{
    return NULL;
}

static bool adb_connected(Backend *be)
{
    //Adb adb = (Adb) be;
    Adb *adb = container_of(be, Adb, backend);
    return adb->s != NULL;
}

static bool adb_sendok(Backend *be)
{
    return 1;
}

static void adb_unthrottle(Backend *be, size_t backlog)
{
    //Adb adb = (Adb) be;
    Adb *adb = container_of(be, Adb, backend);
    sk_set_frozen(adb->s, backlog > ADB_MAX_BACKLOG);
}

static bool adb_ldisc(Backend *be, int option)
{
    // Don't allow line discipline options
    return 0;
}

static void adb_provide_ldisc(Backend *be, Ldisc *ldisc)
{
    /* This is a stub. */
}

static int adb_exitcode(Backend *be)
{
    //Adb adb = (Adb) be;
    Adb *adb = container_of(be, Adb, backend);
    if (adb->s != NULL)
        return -1;                     /* still connected */
    else
        /* Exit codes are a meaningless concept in the Adb protocol */
        return 0;
}

/*
 * cfg_info for Adb does nothing at all.
 */
static int adb_cfg_info(Backend *be)
{
    return 0;
}

const struct BackendVtable adb_backend = {
    .init = adb_init,
    .free = adb_free,
    .reconfig = adb_reconfig,
    .send = adb_send,
    .sendbuffer = adb_sendbuffer,
    .size = adb_size,
    .special = adb_special,
    .get_specials = adb_get_specials,
    .connected = adb_connected,
    .exitcode = adb_exitcode,
    .sendok = adb_sendok,
    .ldisc_option_state = adb_ldisc,
    .provide_ldisc = adb_provide_ldisc,
    .unthrottle = adb_unthrottle,
    .cfg_info = adb_cfg_info,
    .id = "adb",
    .displayname = "ADB",
    .protocol = PROT_ADB,
    .default_port = 5037,
};


#endif
