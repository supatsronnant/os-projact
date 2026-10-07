FROM gcc:latest

WORKDIR /app

# ซอร์สโค้ดอยู่ในโฟลเดอร์ src/ และสคริปต์ทดลองอยู่ที่ root ของโปรเจกต์
COPY src/ ./src/
COPY experiment.sh ./

# แปลงบรรทัดจบแบบ Windows (CRLF) เป็น Linux (LF) กัน experiment.sh พัง
# แล้วให้สิทธิ์รันไฟล์
RUN sed -i 's/\r$//' experiment.sh \
 && chmod +x experiment.sh

# คอมไพล์: -pthread สำหรับ Thread, -lrt สำหรับ POSIX Message Queue
# ผลลัพธ์คือ /app/server และ /app/client
RUN gcc -Wall -Wextra -O2 -pthread src/server.c -o server -lrt \
 && gcc -Wall -Wextra -O2 src/client.c -o client -lrt

CMD ["bash"]