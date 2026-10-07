/*
 * server.c - Concurrent Reservation System (โรงภาพยนตร์ 1 โรง 1 รอบฉาย 20 ที่นั่ง)
 *
 * Usage: ./server [--workers N] [--sync on|off]
 *   --workers N   จำนวน Worker thread (ค่าเริ่มต้น 3)
 *   --sync on    ใช้ Mutex ครอบ Critical Section (ค่าเริ่มต้น)
 *   --sync off   ไม่ใช้ Mutex -> เกิด Race Condition ได้ (ใช้ทดลอง)
 *
 * Client --(Request Queue /rsv_req)--> Worker 1..N --(Reply Queue /rsv_reply_<id>)--> Client
 *
 * ใช้ POSIX Message Queue (mq_open / mq_send / mq_receive)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include "common.h"

#define MAX_WORKERS 16

enum { AVAILABLE = 0, RESERVED = 1 };

typedef struct {
    int status;   /* AVAILABLE / RESERVED                                   */
    int owner;    /* client_id ของเจ้าของ (0 = ไม่มี)                        */
    int grants;   /* จำนวนครั้งที่ถูกจองสำเร็จ (ถ้า > 1 แสดงว่าเกิด Race)      */
} Seat;

/* ===== SHARED DATA: ตารางที่ Worker ทุกตัวเข้าถึงพร้อมกัน ===== */
static Seat seats[NUM_SEATS + 1];

/* Mutex ป้องกัน Critical Section (ตารางที่นั่ง) */
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
/* Mutex สำหรับการพิมพ์ Log เท่านั้น (กันบรรทัด Log ปนกัน) */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static int num_workers = 3;
static int sync_on = 1;
static volatile sig_atomic_t running = 1;
static mqd_t req_q = (mqd_t)-1;        /* Request Queue (ทุก Worker ใช้ร่วมกัน) */
static long log_seq = 0;

/* ---------------------------------------------------------------
 * Log: sequence number + เวลา + Worker ID
 * wid = 0 หมายถึงข้อความจาก Server หลัก
 * --------------------------------------------------------------- */
static void slog(int wid, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&log_lock);
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    long seq = ++log_seq;
    if (wid > 0)
        printf("[#%04ld %02d:%02d:%02d.%03ld] [Worker-%d] %s\n", seq, tm.tm_hour,
               tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, wid, msg);
    else
        printf("[#%04ld %02d:%02d:%02d.%03ld] [Server] %s\n", seq, tm.tm_hour,
               tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, msg);
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}

/* ---------------------------------------------------------------
 * Random delay 50-500 ms  (ขยาย Race Window สำหรับการทดลองเท่านั้น
 * ไม่ใช่ส่วนหนึ่งของระบบจริง)
 * --------------------------------------------------------------- */
static void random_delay(unsigned *seed)
{
    int ms = 50 + rand_r(seed) % 451;
    usleep(ms * 1000);
}

/* ---------------------------------------------------------------
 * เข้า/ออก Critical Section
 * sync_on = 1 -> lock/unlock Mutex จริง
 * sync_on = 0 -> แค่ log ว่าเข้า (ไม่ล็อก) เพื่อให้เห็น Race Condition
 * --------------------------------------------------------------- */
static void cs_enter(int wid)
{
    if (sync_on) {
        if (pthread_mutex_trylock(&table_lock) != 0) {
            slog(wid, "waiting for mutex ...");
            pthread_mutex_lock(&table_lock);
        }
        slog(wid, "entering critical section (mutex locked)");
    } else {
        slog(wid, "entering critical section (NO synchronization)");
    }
}

static void cs_leave(int wid)
{
    slog(wid, "leaving critical section");
    if (sync_on)
        pthread_mutex_unlock(&table_lock);
}

static const char *owner_str(int owner, char *buf, size_t n)
{
    if (owner == 0) snprintf(buf, n, "-");
    else            snprintf(buf, n, "Client-%d", owner);
    return buf;
}

/* ===================== คำสั่งต่าง ๆ ===================== */

static void cmd_list(int wid, char *out, size_t n)
{
    cs_enter(wid);
    size_t off = (size_t)snprintf(out, n, "Seat  Status     Owner\n");
    for (int i = 1; i <= NUM_SEATS && off < n; i++) {
        char ob[16];
        off += (size_t)snprintf(out + off, n - off, "%-5d %-10s %s\n", i,
                                seats[i].status == AVAILABLE ? "AVAILABLE" : "RESERVED",
                                owner_str(seats[i].owner, ob, sizeof ob));
    }
    slog(wid, "listed all resources");
    cs_leave(wid);
}

static void cmd_status(int wid, int rid, char *out, size_t n)
{
    cs_enter(wid);
    if (seats[rid].status == AVAILABLE) {
        snprintf(out, n, "Seat %d: AVAILABLE", rid);
        slog(wid, "Seat %d status: AVAILABLE", rid);
    } else {
        int owner = seats[rid].owner;
        snprintf(out, n, "Seat %d: RESERVED by Client-%d", rid, owner);
        slog(wid, "Seat %d status: RESERVED by Client-%d", rid, owner);
    }
    cs_leave(wid);
}

/* ---- RESERVE: ส่วนที่เกิด Race Condition ----
 * ขั้นตอน: check -> (random delay) -> update
 * ถ้าไม่ล็อกทั้งช่วง Worker หลายตัวจะเห็น AVAILABLE พร้อมกัน แล้วจองซ้อนกัน
 */
static void cmd_reserve(int wid, int cid, int rid, unsigned *seed, char *out, size_t n)
{
    cs_enter(wid);

    int st = seats[rid].status;                       /* (1) CHECK  */
    slog(wid, "check Seat %d: %s", rid, st == AVAILABLE ? "AVAILABLE" : "RESERVED");

    if (st == AVAILABLE) {
        random_delay(seed);                           /* ขยาย Race Window */

        seats[rid].status = RESERVED;                 /* (2) UPDATE */
        seats[rid].owner  = cid;
        int g = __sync_add_and_fetch(&seats[rid].grants, 1);

        slog(wid, "Seat %d reserved by Client-%d", rid, cid);
        if (g > 1)
            slog(wid, "*** RACE CONDITION DETECTED: Seat %d was granted %d times ***", rid, g);
        snprintf(out, n, "SUCCESS: Seat %d reserved by Client-%d", rid, cid);
    } else {
        int owner = seats[rid].owner;
        slog(wid, "Seat %d already reserved by Client-%d", rid, owner);
        snprintf(out, n, "FAILED: Seat %d already reserved by Client-%d", rid, owner);
    }

    cs_leave(wid);
}

static void cmd_cancel(int wid, int cid, int rid, char *out, size_t n)
{
    cs_enter(wid);
    if (seats[rid].status == AVAILABLE) {
        slog(wid, "Seat %d is not reserved, nothing to cancel", rid);
        snprintf(out, n, "FAILED: Seat %d is not reserved", rid);
    } else if (seats[rid].owner != cid) {
        int owner = seats[rid].owner;
        slog(wid, "Client-%d cannot cancel Seat %d (owner is Client-%d)", cid, rid, owner);
        snprintf(out, n, "FAILED: Seat %d belongs to Client-%d", rid, owner);
    } else {
        seats[rid].status = AVAILABLE;
        seats[rid].owner  = 0;
        seats[rid].grants = 0;
        slog(wid, "Seat %d cancelled by Client-%d", rid, cid);
        snprintf(out, n, "SUCCESS: Seat %d cancelled", rid);
    }
    cs_leave(wid);
}

/* ---------------------------------------------------------------
 * แยกคำสั่งและตอบกลับ
 * --------------------------------------------------------------- */
static void process(int wid, const Msg *req, Msg *rep, unsigned *seed)
{
    char cmd[16];
    memcpy(cmd, req->cmd, sizeof cmd);
    cmd[sizeof cmd - 1] = '\0';
    int cid = req->client_id;
    int rid = req->seat_id;

    memset(rep, 0, sizeof *rep);
    rep->client_id   = cid;
    rep->seat_id = rid;
    memcpy(rep->cmd, cmd, sizeof rep->cmd);

    int needs_id = !strcmp(cmd, "STATUS") || !strcmp(cmd, "RESERVE") || !strcmp(cmd, "CANCEL");
    if (needs_id)
        slog(wid, "received %s %d from Client-%d", cmd, rid, cid);
    else
        slog(wid, "received %s from Client-%d", cmd, cid);

    if (needs_id && (rid < 1 || rid > NUM_SEATS)) {
        snprintf(rep->text, sizeof rep->text, "ERROR: seat number must be 1-%d", NUM_SEATS);
        slog(wid, "invalid resource id %d", rid);
        return;
    }

    if      (!strcmp(cmd, "LIST"))    cmd_list(wid, rep->text, sizeof rep->text);
    else if (!strcmp(cmd, "STATUS"))  cmd_status(wid, rid, rep->text, sizeof rep->text);
    else if (!strcmp(cmd, "RESERVE")) cmd_reserve(wid, cid, rid, seed, rep->text, sizeof rep->text);
    else if (!strcmp(cmd, "CANCEL"))  cmd_cancel(wid, cid, rid, rep->text, sizeof rep->text);
    else if (!strcmp(cmd, "QUIT")) {
        slog(wid, "Client-%d disconnected", cid);
        snprintf(rep->text, sizeof rep->text, "BYE");
    } else {
        snprintf(rep->text, sizeof rep->text, "ERROR: unknown command '%s'", cmd);
        slog(wid, "unknown command '%s'", cmd);
    }
}

/* ---------------------------------------------------------------
 * ส่ง Reply กลับไปที่คิวของ Client (/rsv_reply_<client_id>)
 * --------------------------------------------------------------- */
static void send_reply(int wid, const Msg *rep)
{
    char name[64];
    snprintf(name, sizeof name, REPLY_QUEUE_FMT, rep->client_id);

    mqd_t q = mq_open(name, O_WRONLY);
    if (q == (mqd_t)-1) {
        slog(wid, "cannot reply to Client-%d (%s): %m", rep->client_id, name);
        return;
    }
    if (mq_send(q, (const char *)rep, sizeof *rep, 0) == -1)
        slog(wid, "mq_send reply to Client-%d failed: %m", rep->client_id);
    mq_close(q);
}

/* ---------------------------------------------------------------
 * Worker thread: mq_timedreceive -> process -> send_reply
 * ทุก Worker รับจาก Request Queue เดียวกัน (Message หนึ่งชิ้นได้ Worker แค่ตัวเดียว)
 * ใช้ timeout 0.5 วินาทีเพื่อให้ Worker กลับมาเช็ก running ได้ (POSIX mq ไม่มี
 * วิธีปลุก Thread ที่รออยู่เมื่อลบคิว)
 * --------------------------------------------------------------- */
static void *worker(void *arg)
{
    int wid = (int)(intptr_t)arg;
    unsigned seed = (unsigned)time(NULL) + (unsigned)wid * 7919u;
    Msg req, rep;

    slog(wid, "started");
    while (running) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 500 * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        ssize_t n = mq_timedreceive(req_q, (char *)&req, sizeof req, NULL, &deadline);
        if (n < 0) {
            if (errno == ETIMEDOUT || errno == EINTR) continue;   /* ไม่มีข้อความ / ถูก signal ขัด */
            slog(wid, "mq_timedreceive error: %m");
            break;
        }

        if (req.client_id < 1 || req.client_id > MAX_CLIENT_ID) {
            slog(wid, "dropped request with invalid client id %d", req.client_id);
            continue;
        }

        process(wid, &req, &rep, &seed);
        send_reply(wid, &rep);
    }
    slog(wid, "stopped");
    return NULL;
}

static void on_signal(int sig)
{
    (void)sig;
    running = 0;                              /* Worker จะเห็นค่านี้หลัง timeout แล้วจบเอง */
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [--workers N (1-%d)] [--sync on|off]\n", prog, MAX_WORKERS);
    exit(1);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--workers") && i + 1 < argc) {
            num_workers = atoi(argv[++i]);
            if (num_workers < 1 || num_workers > MAX_WORKERS) usage(argv[0]);
        } else if (!strcmp(argv[i], "--sync") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "on"))       sync_on = 1;
            else if (!strcmp(argv[i], "off")) sync_on = 0;
            else usage(argv[0]);
        } else {
            usage(argv[0]);
        }
    }

    /* ลบคิวเก่าที่ค้างอยู่ (ถ้ามี) แล้วสร้างใหม่ */
    struct mq_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.mq_maxmsg  = REQ_MAXMSG;
    attr.mq_msgsize = sizeof(Msg);
    mq_unlink(REQ_QUEUE_NAME);
    req_q = mq_open(REQ_QUEUE_NAME, O_CREAT | O_EXCL | O_RDONLY, 0666, &attr);
    if (req_q == (mqd_t)-1) {
        perror("mq_open");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    slog(0, "started: workers=%d, sync=%s, request queue=%s, resources=%d",
         num_workers, sync_on ? "ON (mutex)" : "OFF (race possible)", REQ_QUEUE_NAME, NUM_SEATS);

    pthread_t tid[MAX_WORKERS];
    for (int i = 0; i < num_workers; i++)
        pthread_create(&tid[i], NULL, worker, (void *)(intptr_t)(i + 1));
    for (int i = 0; i < num_workers; i++)
        pthread_join(tid[i], NULL);

    mq_close(req_q);
    mq_unlink(REQ_QUEUE_NAME);
    slog(0, "stopped, message queue removed");
    return 0;
}