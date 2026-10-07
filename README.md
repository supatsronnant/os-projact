# Cinema Seat Reservation System (CSS223 - OS Project)
 
ระบบจองที่นั่งโรงภาพยนตร์

## สถานการณ์ที่เลือก
**จองที่นั่งโรงภาพยนตร์** — มีที่นั่งทั้งหมด 20 ที่นั่ง (หมายเลข 1–20)
 
## เทคโนโลยีที่ใช้
- ภาษา C
- POSIX Message Queue
- pthread
- pthread_mutex

---
 
## 1. วิธี Build Docker Image
```bash
docker build -t cinema-seat-reserve .
```

## 2. วิธี Run Container
```bash
docker run -dit --name cinema-reservation cinema-seat-reserve
```
 
## 3. วิธีเปิด Server
```bash
docker exec -it cinema-reservation bash
./server --workers 3 --sync on
```
พารามิเตอร์:
- `--workers N` จำนวน Worker thread (ค่าเริ่มต้น 3, สูงสุด 16)
- `--sync on|off` เปิด/ปิด Mutex (ค่าเริ่มต้น `on`)
 
## 4. วิธีเปิด Client หลายตัว
เปิด terminal ใหม่ต่อ client แต่ละคน (อย่างน้อย 5 terminal):
```bash
# Terminal 2
docker exec -it cinema-reservation bash
./client 1
 
# Terminal 3
docker exec -it cinema-reservation bash
./client 2
 
# ... ทำซ้ำจนถึง client 5
```
 
## 5. รูปแบบ Message Queue ที่ใช้
- **Request Queue**: `/cinema_req` (Client → Server, ทุก client ส่งเข้า queue เดียวกัน ทุก Worker รับจากคิวนี้)
- **Reply Queue**: แยกตาม client เช่น `/cinema_reply_<client_id>` (Server → Client เฉพาะราย, Client เป็นคนสร้างเอง)
- ข้อความส่งเป็น **binary struct** ตรงๆ ผ่าน `mq_send`/`mq_receive` ไม่ผ่านการแปลงเป็น string
- POSIX Message Queue ไม่มี `mtype` แบบ System V จึงไม่ต้องมี field `long` นำหน้า struct
**Message structure (ใช้ทั้ง Request และ Reply):**
```c
typedef struct {
    int  client_id;
    int  seat_id;         // หมายเลขที่นั่ง (0 ถ้าคำสั่งไม่ใช้)
    char cmd[16];          // LIST | STATUS | RESERVE | CANCEL | QUIT
    char text[1024];       // ข้อความตอบกลับจาก Server (ใช้เฉพาะตอนเป็น Reply)
} Msg;
```
 
รายละเอียดเต็มอยู่ใน [`src/common.h`](src/common.h)
 
## 6. คำสั่งที่ Client รองรับ
| คำสั่ง | ตัวอย่าง | ความหมาย |
|---|---|---|
| `LIST` | `LIST` | แสดงสถานะที่นั่งทั้ง 20 ที่ |
| `STATUS <id>` | `STATUS 10` | เช็คสถานะที่นั่งหมายเลข 10 |
| `RESERVE <id>` | `RESERVE 10` | จองที่นั่งหมายเลข 10 |
| `CANCEL <id>` | `CANCEL 10` | ยกเลิกการจองที่นั่งหมายเลข 10 |
| `QUIT` | `QUIT` | ออกจากโปรแกรม client |
 
## 7. วิธีทดลอง Race Condition
 
### วิธีอัตโนมัติ (แนะนำ) — ใช้ `experiment.sh`
สคริปต์นี้จะเปิด Server, เปิด Client 5 ตัวยิง `RESERVE 10` พร้อมกัน, สรุปผล, และปิด Server ให้อัตโนมัติ
```bash
docker exec -it cinema-reservation bash
./experiment.sh 1   # Experiment 1: Sequential Baseline (1 worker, sync on)
./experiment.sh 2   # Experiment 2: Concurrent ไม่มี sync -> เกิด Race Condition
./experiment.sh 3   # Experiment 3: Concurrent มี sync -> แก้ปัญหาได้
```
ปรับจำนวน client หรือที่นั่งที่ทดสอบได้ด้วย environment variable:
```bash
CLIENTS=8 SEAT=15 ./experiment.sh 2
```
ผลลัพธ์ (log ของ server และ client แต่ละตัว) จะถูกเก็บไว้ในโฟลเดอร์ `results/`
 
### วิธีมือ (ถ้าต้องการทดสอบเอง)
1. เปิด Server ด้วย `./server --workers 3 --sync off`
2. เปิด client อย่างน้อย 5 ตัว ให้ทุกตัวสั่ง `RESERVE 10` พร้อมกัน (ในเวลาใกล้เคียงกัน)
3. เพราะมี random delay (50-500ms) ระหว่าง check และ update สถานะที่นั่ง
   จะเห็นว่ามีมากกว่า 1 client ได้รับผล `SUCCESS` สำหรับที่นั่งเดียวกัน — นี่คือ Race Condition
4. ดู log ฝั่ง server เพื่อยืนยันว่า worker หลายตัวเข้าไป check พร้อมกันตอนที่นั่งยังเป็น `AVAILABLE`
   (server จะพิมพ์ `*** RACE CONDITION DETECTED ***` เองเมื่อตรวจพบ)
   
## 8. วิธีเปิด/ปิด Synchronization
กำหนดผ่าน **command-line argument ตอนรัน** ไม่ต้อง compile ใหม่:
```bash
./server --workers 3 --sync off   # ปิด Mutex -> ใช้ทดลอง Experiment 2
./server --workers 3 --sync on    # เปิด Mutex -> ใช้ทดลอง Experiment 1 และ 3
```
 
---

## สมาชิกกลุ่ม
| ชื่อ | รหัสนักศึกษา |
|---|---|
| นางสาวชัญญานุช ธนูศร | 68090500404 |
| นางสาวณัฐชญา อัตโยโค | 68090500410 |
| นางสาวณิชกานต์ นวลแก้ว | 68090500434 |
| นายรัชพล จันดาบุตร | 68090500443 |
| นางสาวสุภัสสร นันทานนท์ | 68090500447 |