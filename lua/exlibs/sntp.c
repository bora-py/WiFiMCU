/**
 * sntp.c - SNTP (Simple Network Time Protocol) client for WiFiMCU
 * Queries NTP server via UDP (Port 123) and synchronizes internal RTC / timestamps.
 */

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lrotable.h"
#include "user_config.h"

#include "platform.h"
#include "MICOPlatform.h"
#include "MICORTOS.h"
#include "MicoSocket.h"
#include "SocketUtils.h"
#include <string.h>
#include <time.h>

#ifdef USE_SNTP_MODULE

#define NTP_PORT 123
#define NTP_PACKET_SIZE 48
#define NTP_TIMESTAMP_DELTA 2208988800ULL // Seconds between 1900 and 1970

static int sntp_cb_ref = LUA_NOREF;
static lua_State *sntp_L = NULL;
static char ntp_server_host[64] = "pool.ntp.org";
static int sntp_timezone_offset = 3; // Default UTC+3
static uint32_t sntp_last_synced_epoch = 0;
static uint32_t sntp_last_synced_uptime = 0;

typedef struct {
    uint8_t li_vn_mode;
    uint8_t stratum;
    uint8_t poll;
    uint8_t precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_ts_sec;
    uint32_t ref_ts_frac;
    uint32_t orig_ts_sec;
    uint32_t orig_ts_frac;
    uint32_t recv_ts_sec;
    uint32_t recv_ts_frac;
    uint32_t trans_ts_sec;
    uint32_t trans_ts_frac;
} ntp_packet_t;

extern mico_queue_t os_queue;
extern uint32_t mico_get_time(void);

static void sntp_thread(void *arg) {
    (void)arg;
    int fd = -1;
    struct sockaddr_t addr;
    char ip_str[16];
    OSStatus err;
    int timeout_ms = 5000;
    ntp_packet_t packet;
    uint32_t ntp_sec;

    err = gethostbyname(ntp_server_host, (uint8_t *)ip_str, sizeof(ip_str));
    if (err != kNoErr) {
        goto exit_thread;
    }

    fd = socket(AF_INET, SOCK_DGRM, IPPROTO_UDP);
    if (fd < 0) {
        goto exit_thread;
    }

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout_ms, sizeof(timeout_ms));

    memset(&addr, 0, sizeof(addr));
    addr.s_port = NTP_PORT;
    addr.s_ip = inet_addr(ip_str);

    memset(&packet, 0, sizeof(packet));
    packet.li_vn_mode = (0x03 << 6) | (0x04 << 3) | 0x03; // LI=0, VN=4, Mode=3 (Client)

    if (sendto(fd, &packet, sizeof(packet), 0, &addr, sizeof(addr)) > 0) {
        socklen_t addr_len = sizeof(addr);
        if (recvfrom(fd, &packet, sizeof(packet), 0, &addr, &addr_len) >= (int)sizeof(packet)) {
            ntp_sec = (packet.trans_ts_sec >> 24) |
                      ((packet.trans_ts_sec >> 8) & 0x0000FF00) |
                      ((packet.trans_ts_sec << 8) & 0x00FF0000) |
                      (packet.trans_ts_sec << 24);

            if (ntp_sec > NTP_TIMESTAMP_DELTA) {
                uint32_t epoch = (uint32_t)(ntp_sec - NTP_TIMESTAMP_DELTA);
                sntp_last_synced_epoch = epoch;
                sntp_last_synced_uptime = mico_get_time() / 1000;

                // Sync internal hardware RTC if available
                mico_rtc_time_t rtc_time;
                time_t t_epoch = (time_t)epoch;
                struct tm *timeinfo = gmtime(&t_epoch);
                if (timeinfo) {
                    rtc_time.sec = timeinfo->tm_sec;
                    rtc_time.min = timeinfo->tm_min;
                    rtc_time.hr = timeinfo->tm_hour;
                    rtc_time.date = timeinfo->tm_mday;
                    rtc_time.month = timeinfo->tm_mon + 1;
                    rtc_time.year = (timeinfo->tm_year + 1900) % 100;
                    rtc_time.weekday = timeinfo->tm_wday + 1;
                    MicoRtcSetTime(&rtc_time);
                }

                // If callback registered, push event to Lua main queue
                if (sntp_cb_ref != LUA_NOREF && sntp_L != NULL) {
                    queue_msg_t msg;
                    msg.L = sntp_L;
                    msg.source = TMR; // Handled directly in main queue
                    msg.para1 = epoch;
                    msg.para2 = sntp_cb_ref;
                    mico_rtos_push_to_queue(&os_queue, &msg, 0);
                }
            }
        }
    }

exit_thread:
    if (fd >= 0) SocketClose(&fd);
    mico_rtos_delete_thread(NULL);
}

// sntp.sync( [server], [callback] )
static int lsntp_sync(lua_State *L) {
    int arg_idx = 1;
    if (lua_isstring(L, arg_idx)) {
        size_t len;
        const char *srv = lua_tolstring(L, arg_idx, &len);
        if (srv && len < sizeof(ntp_server_host)) {
            strncpy(ntp_server_host, srv, sizeof(ntp_server_host) - 1);
            ntp_server_host[sizeof(ntp_server_host) - 1] = '\0';
        }
        arg_idx++;
    }

    if (lua_type(L, arg_idx) == LUA_TFUNCTION || lua_type(L, arg_idx) == LUA_TLIGHTFUNCTION) {
        if (sntp_cb_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, sntp_cb_ref);
        }
        lua_pushvalue(L, arg_idx);
        sntp_cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        sntp_L = L;
    }

    mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "sntp", sntp_thread, 2048, NULL);
    return 0;
}

// sntp.time() -> returns current epoch timestamp (or 0 if not synced)
static int lsntp_time(lua_State *L) {
    if (sntp_last_synced_epoch == 0) {
        // Fallback to hardware RTC
        mico_rtc_time_t rtc_time;
        if (MicoRtcGetTime(&rtc_time) == kNoErr) {
            struct tm t;
            t.tm_sec = rtc_time.sec;
            t.tm_min = rtc_time.min;
            t.tm_hour = rtc_time.hr;
            t.tm_mday = rtc_time.date;
            t.tm_mon = rtc_time.month - 1;
            t.tm_year = rtc_time.year + 100; // 2000+
            t.tm_isdst = -1;
            time_t epoch = mktime(&t);
            lua_pushinteger(L, (lua_Integer)epoch);
            return 1;
        }
        lua_pushinteger(L, 0);
        return 1;
    }
    uint32_t now_uptime = mico_get_time() / 1000;
    uint32_t current_epoch = sntp_last_synced_epoch + (now_uptime - sntp_last_synced_uptime);
    lua_pushinteger(L, (lua_Integer)current_epoch);
    return 1;
}

// sntp.setoffset( hours ) -> e.g. 3 for UTC+3
static int lsntp_setoffset(lua_State *L) {
    int offset = luaL_checkinteger(L, 1);
    sntp_timezone_offset = offset;
    return 0;
}

// sntp.getoffset()
static int lsntp_getoffset(lua_State *L) {
    lua_pushinteger(L, sntp_timezone_offset);
    return 1;
}

#define MIN_OPT_LEVEL 2
#include "lrodefs.h"

const LUA_REG_TYPE sntp_map[] = {
    { LSTRKEY("sync"), LFUNCVAL(lsntp_sync) },
    { LSTRKEY("time"), LFUNCVAL(lsntp_time) },
    { LSTRKEY("setoffset"), LFUNCVAL(lsntp_setoffset) },
    { LSTRKEY("getoffset"), LFUNCVAL(lsntp_getoffset) },
    { LNILKEY, LNILVAL }
};

LUALIB_API int luaopen_sntp(lua_State *L) {
#if LUA_OPTIMIZE_MEMORY > 0
    return 0;
#else
    luaL_register(L, "sntp", sntp_map);
    return 1;
#endif
}

#endif /* USE_SNTP_MODULE */
