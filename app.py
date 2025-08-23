#IMPORTS
import serial
import threading
import time
import re
import sys
import os
from datetime import datetime

#GLOBAL VARIABLES
SERIAL_PORT = '/dev/tty.usbserial-10'
BAUD_RATE = 115200

serial_lock = threading.Lock()
image_receiving = False
packet_buffer = {}
expected_packets = 0
expected_length = 0
isOpened = False

#SERIAL CONNECTION
def connect_serial(port, baudrate):
    try:
        ser = serial.Serial(port, baudrate, timeout=0.1)
        print(f"[CONN SUCCEED]")
        return ser
    except serial.SerialException as e:
        print(f"[CONN ERROR {e}]")
        sys.exit(1)

#LISTEN TO EVENTS (Specifically IMG-related ones)
def listener(ser):
    global image_receiving, packet_buffer, expected_packets, expected_length, isOpened

    finalMessage = ""
    while True:
        try:
            with serial_lock:
                line = ser.readline()

            if not line:
                continue

            # Caso normal de paquete en texto seguro (hex)
            if image_receiving and line.startswith(b'<<< P:'):
                first_colon = line.find(b':')
                second_colon = line.find(b':', first_colon + 1)
                if second_colon == -1:
                    print("[WARN] Header mal formado")
                    continue

                # Decodificar encabezado
                header_str = line[:second_colon + 1].decode(errors='ignore')
                match = re.search(r"P:(\d+):", header_str)
                if not match:
                    print("[WARN] Header inválido")
                    continue

                pkt_id = int(match.group(1))

                # Leer el payload en formato HEX y convertir a binario
                payload_hex = line[second_colon + 1:].decode(errors='ignore').strip().replace('\r', '').replace('\n', '')
                try:
                    payload = bytes.fromhex(payload_hex)
                    payload = payload[:-1]
                except ValueError:
                    print(f"[ERROR] Payload hex inválido: {payload_hex}")
                    continue

                packet_buffer[pkt_id] = payload
                print(f"[PACKET] Received {pkt_id} ({len(payload)} bytes)")

            else:
                # Procesamiento de texto general
                line_str = line.decode(errors='ignore').strip()

                if "END_I" in line_str and image_receiving:
                    print("[INFO] Image transmission ended.")
                    date = datetime.now()
                    folder_path = f"images/{date.strftime('%Y_%m')}/{date.strftime('%d')}"
                    os.makedirs(folder_path, exist_ok=True)  # <--- CREA LAS CARPETAS

                    file_name = date.strftime("%H_%M_%S") + ".jpg"
                    IMAGE_SAVE_PATH = f"{folder_path}/{file_name}"
                    
                    reassemble_image(packet_buffer, expected_packets, IMAGE_SAVE_PATH)
                    image_receiving = False
                    packet_buffer = {}

                elif line_str.startswith("<<< IMG;"):
                    match = re.match(r"<<< IMG;(\d+);(\d+)", line_str)
                    if match:
                        expected_length = int(match.group(1))
                        expected_packets = int(match.group(2))
                        image_receiving = True
                        print(f"[IMG] {expected_length}B, {expected_packets} packets.")

                elif "<<< BS;OK;FTU:" in line_str:
                    isOpened = True
                    if finalMessage != "":
                        print("[INTERNET]", finalMessage)
                    finalMessage = ""
                    finalMessage += line_str[10::]
                    print(f"[OPEN] {line_str}")
                elif "<<< BS;OK;BYE" in line_str:
                    print(f"[END]")
                    if finalMessage != "":
                        print("[FINAL]", finalMessage)
                        finalMessage = ""
                elif "<<< BS;OK;" in line_str:
                    print(f"[TELEMETRY] {line_str}")
                    finalMessage += line_str[10::]
                else:
                    print(f"[INFO] {line_str}")
        except Exception as e:
            print(f"[ERROR] Listener exception: {e}")

def reassemble_image(received_packets, total_pkts, save_path):
    if len(received_packets) != total_pkts:
        print("[WARN] Missing packets. Image may be incomplete.")
    else:
        print("[INFO] All packets received.")

    img_bytes = b''.join(received_packets.get(i, b'') for i in range(total_pkts))

    with open(save_path, 'wb') as f:
        f.write(img_bytes)
    print(f"[SUCCESS] Image saved as {save_path} ({len(img_bytes)} bytes)")

def automatic_sender(ser):
    global isOpened
    while True:
        if isOpened:
            cmds = ["SIGNAL"]
            for cmd in cmds:
                with serial_lock:
                    ser.write((cmd + '\n').encode('utf-8'))
                    print(f"[SEND] {cmd}")
                time.sleep(1)
            isOpened = False

def input_loop(ser):
    global isOpened
    print("Type commands (e.g., IMG, STATUS). Ctrl+C to exit.")
    try:
        while True:
            cmd = input("> ").strip()
            if cmd:
                with serial_lock:
                    ser.write((cmd + '\n').encode('utf-8'))
    except KeyboardInterrupt:
        print("\n[EXIT] Shutting down.")

def main():
    ser = connect_serial(SERIAL_PORT, BAUD_RATE)
    time.sleep(2)

    threading.Thread(target=listener, args=(ser,), daemon=True).start()
    threading.Thread(target=automatic_sender, args=(ser,), daemon=True).start()

    input_loop(ser)

if __name__ == '__main__':
    main()
