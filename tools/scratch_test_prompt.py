import base64
import json
import urllib.request
import sys
import io
import socket
import struct
import time

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

CORES3_IP = '192.168.11.19'

def capture_frame(ip, port=80):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3.0)
        s.connect((ip, port))
        handshake = (
            f'GET /ws HTTP/1.1\r\n'
            f'Host: {ip}:{port}\r\n'
            f'Upgrade: websocket\r\n'
            f'Connection: Upgrade\r\n'
            f'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n'
            f'Sec-WebSocket-Version: 13\r\n\r\n'
        )
        s.sendall(handshake.encode())
        resp = b''
        while b'\r\n\r\n' not in resp:
            chunk = s.recv(1024)
            if not chunk: break
            resp += chunk
        if b'101' not in resp:
            print('Handshake failed')
            s.close()
            return None
        mask_key = b'\x12\x34\x56\x78'
        raw_payload = b'\x05\x00\x00\x00\x00'
        masked_payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(raw_payload))
        frame = bytes([0x82, 0x85]) + mask_key + masked_payload
        s.sendall(frame)
        s.settimeout(5.0)
        buf = b''
        start_time = time.time()
        while time.time() - start_time < 5:
            chunk = s.recv(4096)
            if not chunk: break
            buf += chunk
            idx = 0
            while idx < len(buf) - 2:
                if buf[idx] & 0x0F in (0, 2):
                    ws_len = buf[idx+1] & 0x7F
                    header_size = 2
                    if ws_len == 126:
                        if idx + 4 > len(buf): break
                        ws_len = struct.unpack('!H', buf[idx+2:idx+4])[0]
                        header_size = 4
                    elif ws_len == 127:
                        if idx + 10 > len(buf): break
                        ws_len = struct.unpack('!Q', buf[idx+2:idx+10])[0]
                        header_size = 10
                    if idx + header_size + ws_len <= len(buf):
                        ws_payload = buf[idx+header_size : idx+header_size+ws_len]
                        if len(ws_payload) >= 5 and ws_payload[0] == 0x02:
                            jpeg_len = struct.unpack('!I', ws_payload[1:5])[0]
                            jpeg_data = ws_payload[5:5+jpeg_len]
                            s.close()
                            return jpeg_data
                        idx += header_size + ws_len
                    else:
                        break
                else:
                    idx += 1
        s.close()
    except Exception as e:
        print(f"Capture error: {e}")
    return None

jpeg = capture_frame(CORES3_IP)
if not jpeg:
    print("Could not capture frame from CoreS3.")
    sys.exit(0)

print(f"Captured {len(jpeg)} bytes JPEG.")
with open("c:/Users/funky/.gemini/antigravity-ide/brain/c20f578c-a8c9-47f1-b398-d5ddbc21ff2a/scratch/live_view.jpg", "wb") as f:
    f.write(jpeg)

b64 = base64.b64encode(jpeg).decode("utf-8")
prompt = (
    "自律移動ロボットの回避判断です。画像の前方（向き: front_check）の通行可能性を判断し、"
    "次のJSON形式のみで出力してください: "
    "{\"passable\": trueまたはfalse, \"score\": 0〜100の安全度, \"reason\": \"理由\"}"
)

payload = {
    "model": "qwen3.5-9b-vlm",
    "messages": [
        {
            "role": "user",
            "content": [
                {"type": "text", "text": prompt},
                {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{b64}"}}
            ]
        }
    ],
    "max_tokens": 120,
    "temperature": 0.1
}

req = urllib.request.Request(
    "http://192.168.11.6:1234/v1/chat/completions",
    data=json.dumps(payload).encode("utf-8"),
    headers={"Content-Type": "application/json"}
)

try:
    with urllib.request.urlopen(req, timeout=30) as r:
        res = json.loads(r.read().decode("utf-8"))
        print("=== LM Studio VLM Response ===")
        print(res["choices"][0]["message"]["content"])
except Exception as e:
    print(f"VLM Error: {e}")

