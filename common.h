#ifndef COMMON_H
#define COMMON_H

/* ---------- POSIX Message Queue settings ---------- */
#define REQ_QUEUE_NAME   "/cinema_req"          /* Request Queue: ทุก Client ส่งเข้าคิวนี้ ทุก Worker รับจากคิวนี้ */
#define REPLY_QUEUE_FMT  "/cinema_reply_%d"     /* Reply Queue ของ Client แต่ละตัว (Client เป็นคนสร้าง)         */
#define REQ_MAXMSG       10                     /* จำนวนข้อความสูงสุดในคิว (ค่าเริ่มต้นของ Linux/Docker คือ 10)  */
#define REPLY_MAXMSG     4
#define MAX_CLIENT_ID    900

/* ---------- Reservation settings ---------- */
#define NUM_SEATS        20                     /* ที่นั่ง 1-20 */

/* ---------- Message structure (ใช้ทั้ง Request และ Reply) ----------
 * POSIX Message Queue ไม่มี mtype จึงไม่ต้องมีฟิลด์ long นำหน้า
 * และใช้คิวแยกสำหรับ Reply ของ Client แต่ละตัวแทน
 */
typedef struct {
    int  client_id;
    int  seat_id;         /* หมายเลขที่นั่ง (0 ถ้าคำสั่งไม่ใช้) */
    char cmd[16];          /* LIST | STATUS | RESERVE | CANCEL | QUIT */
    char text[1024];       /* ข้อความตอบกลับจาก Server */
} Msg;

#endif