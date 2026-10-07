/*
 * client.c - Client ของ Concurrent Reservation System (POSIX Message Queue)
 *
 * Usage: ./client <client_id>          (client_id 1-900)
 * คำสั่ง: LIST | STATUS <id> | RESERVE <id> | CANCEL <id> | QUIT
 *
 * ใช้แบบ interactive หรือส่งผ่าน pipe ก็ได้ เช่น
 *     echo "RESERVE 10" | ./client 1
 *
 * Client สร้าง Reply Queue ของตัวเอง (/rsv_reply_<id>) ไว้รับคำตอบ
 * และลบทิ้งเมื่อจบโปรแกรม
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include "common.h"

#define REPLY_TIMEOUT_SEC 30

static volatile sig_atomic_t stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    stop = 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <client_id 1-%d>\n", argv[0], MAX_CLIENT_ID);
        return 1;
    }
    int cid = atoi(argv[1]);
    if (cid < 1 || cid > MAX_CLIENT_ID) {
        fprintf(stderr, "client_id must be 1-%d\n", MAX_CLIENT_ID);
        return 1;
    }

    /* ---- เปิด Request Queue (Server ต้องสร้างไว้แล้ว) ---- */
    mqd_t req_q = mq_open(REQ_QUEUE_NAME, O_WRONLY);
    if (req_q == (mqd_t)-1) {
        perror("mq_open request queue (is the server running?)");
        return 1;
    }

    /* ---- สร้าง Reply Queue ของตัวเอง ---- */
    char reply_name[64];
    snprintf(reply_name, sizeof reply_name, REPLY_QUEUE_FMT, cid);

    struct mq_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.mq_maxmsg  = REPLY_MAXMSG;
    attr.mq_msgsize = sizeof(Msg);
    mq_unlink(reply_name);                               /* ลบของเก่าที่อาจค้าง */
    mqd_t reply_q = mq_open(reply_name, O_CREAT | O_EXCL | O_RDONLY, 0666, &attr);
    if (reply_q == (mqd_t)-1) {
        perror("mq_open reply queue");
        mq_close(req_q);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;                           /* ไม่ใส่ SA_RESTART: Ctrl+C ขัด fgets ได้ */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int tty = isatty(STDIN_FILENO);
    Msg m;

    if (tty) {
        printf("Client-%d connected.\n", cid);
        printf("Commands: LIST | STATUS <id> | RESERVE <id> | CANCEL <id> | QUIT\n");
    }

    char line[128];
    while (!stop) {
        char cmd[16] = "";
        int rid = 0;

        if (tty) {
            printf("client-%d> ", cid);
            fflush(stdout);
        }

        if (!fgets(line, sizeof line, stdin)) {
            strcpy(cmd, "QUIT");                         /* EOF / Ctrl+C = ออกจากโปรแกรม */
        } else {
            int n = sscanf(line, "%15s %d", cmd, &rid);
            if (n < 1) continue;
            for (char *p = cmd; *p; p++)
                *p = (char)toupper((unsigned char)*p);

            int needs_id = !strcmp(cmd, "STATUS") || !strcmp(cmd, "RESERVE") || !strcmp(cmd, "CANCEL");
            if (needs_id && n < 2) {
                printf("usage: %s <resource_id>\n", cmd);
                continue;
            }
        }

        /* ---- ส่ง Request ---- */
        memset(&m, 0, sizeof m);
        m.client_id = cid;
        m.seat_id   = rid;
        snprintf(m.cmd, sizeof m.cmd, "%s", cmd);

        if (mq_send(req_q, (const char *)&m, sizeof m, 0) == -1) {
            perror("mq_send");
            break;
        }

        /* ---- รอ Reply จากคิวของตัวเอง ---- */
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += REPLY_TIMEOUT_SEC;

        ssize_t r;
        do {
            r = mq_timedreceive(reply_q, (char *)&m, sizeof m, NULL, &deadline);
        } while (r < 0 && errno == EINTR && !stop);

        if (r < 0) {
            if (errno == ETIMEDOUT)
                fprintf(stderr, "no reply from server (timeout %d s)\n", REPLY_TIMEOUT_SEC);
            else if (errno != EINTR)
                perror("mq_timedreceive");
            break;
        }

        m.text[sizeof m.text - 1] = '\0';
        if (strchr(m.text, '\n'))
            printf("[Client-%d]\n%s", cid, m.text);       /* LIST: หลายบรรทัด */
        else
            printf("[Client-%d] %s\n", cid, m.text);
        fflush(stdout);

        if (!strcmp(cmd, "QUIT"))
            break;
    }

    /* ---- เก็บกวาด ---- */
    mq_close(req_q);
    mq_close(reply_q);
    mq_unlink(reply_name);
    return 0;
}