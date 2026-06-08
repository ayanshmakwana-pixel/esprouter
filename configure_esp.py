import serial, time, sys, random

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
ser = serial.Serial(port, 115200, timeout=3)
time.sleep(3)
ser.reset_input_buffer()
ser.write(b"\r\n")
time.sleep(2)

# Generate random STA MAC
mac_bytes = [0x02] + [random.randint(0x00, 0xFF) for _ in range(5)]
mac_str = " ".join(f"{b:02X}" for b in mac_bytes)

# Generate random AP MAC (different from STA)
ap_mac_bytes = [0x02] + [random.randint(0x00, 0xFF) for _ in range(5)]
ap_mac_str = " ".join(f"{b:02X}" for b in ap_mac_bytes)

print(f"STA MAC: {mac_bytes[0]:02X}:{mac_bytes[1]:02X}:{mac_bytes[2]:02X}:{mac_bytes[3]:02X}:{mac_bytes[4]:02X}:{mac_bytes[5]:02X}")
print(f"AP MAC:  {ap_mac_bytes[0]:02X}:{ap_mac_bytes[1]:02X}:{ap_mac_bytes[2]:02X}:{ap_mac_bytes[3]:02X}:{ap_mac_bytes[4]:02X}:{ap_mac_bytes[5]:02X}")

cmds = [
    f"set_sta_mac {mac_str}",
    f"set_ap_mac {ap_mac_str}",
    "set_hostname windows-pc",
    "set ap_dns 8.8.8.8",
    "set sta_ttl 64",
    "save",
    "reboot"
]

for cmd in cmds:
    ser.write((cmd + "\r\n").encode())
    print(f">>> {cmd}")
    time.sleep(1.5)
    while ser.in_waiting:
        line = ser.readline().decode(errors="ignore").strip()
        if line:
            print(f"  {line}")

time.sleep(3)
while ser.in_waiting:
    print(ser.readline().decode(errors="ignore").strip())
ser.close()
print("Done! ESP32 will now show as 'windows-pc' on network.")
