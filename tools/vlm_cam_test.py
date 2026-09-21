"""
CoreS3 の内蔵カメラから WebSocket 経由で生画像を取得し、
同一NW上のローカル VLM (qwen3.5-9b-vlm) に周囲の状況認識と自律走行アクション判定を行わせる検証スクリプト
"""

import base64
import json
import socket
import struct
import sys
import time
import urllib.request
import io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

CORES3_IP = '192.168.11.16'
VLM_URL = 'http://127.0.0.1:1234/v1/chat/completions'
MODEL_NAME = 'qwen3.5-9b-vlm'

def capture_cores3_frame(ip, port=80):
    print(f"Connecting to CoreS3 WebSocket at ws://{ip}:{port}/ws ...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect((ip, port))

    # WebSocket Handshake
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
        print('WebSocket Handshake failed!')
        s.close()
        return None

    # Send StartCameraStream (0x05) with 0-length payload
    mask_key = b'\x12\x34\x56\x78'
    raw_payload = b'\x05\x00\x00\x00\x00'
    masked_payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(raw_payload))
    frame = bytes([0x82, 0x85]) + mask_key + masked_payload
    s.sendall(frame)

    # Receive JPEG frame
    s.settimeout(10.0)
    buf = b''
    start_time = time.time()
    while time.time() - start_time < 10:
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
    return None

def analyze_scene_with_vlm(jpeg_bytes):
    print(f"Sending image ({len(jpeg_bytes)} bytes) to VLM ({MODEL_NAME}) ...")
    img_b64 = base64.b64encode(jpeg_bytes).decode('utf-8')

    prompt = """このカメラ画像から自律移動ロボットの回避走行判断を行い、以下のJSONフォーマットのみを出力してください：
```json
{
  "scene_description": "周囲の簡潔な状況説明",
  "obstacle_detected": true,
  "recommended_action": "forward" または "spin_left" または "spin_right" または "stop",
  "reason": "判断理由"
}
```
"""
    payload = {
        'model': MODEL_NAME,
        'messages': [
            {
                'role': 'system',
                'content': 'You are the visual navigation AI of an autonomous robot. Output JSON only.'
            },
            {
                'role': 'user',
                'content': [
                    {'type': 'text', 'text': prompt},
                    {'type': 'image_url', 'image_url': {'url': f'data:image/jpeg;base64,{img_b64}'}}
                ]
            }
        ],
        'max_tokens': 250,
        'temperature': 0.1
    }

    req = urllib.request.Request(
        VLM_URL,
        data=json.dumps(payload).encode('utf-8'),
        headers={'Content-Type': 'application/json'}
    )
    with urllib.request.urlopen(req, timeout=45) as response:
        result = json.loads(response.read().decode('utf-8'))
        return result['choices'][0]['message']['content']

if __name__ == '__main__':
    jpeg = capture_cores3_frame(CORES3_IP)
    if not jpeg:
        print("Failed to capture frame from CoreS3.")
        sys.exit(1)

    print(f"Captured live frame: {len(jpeg)} bytes. Analyzing...")
    vlm_result = analyze_scene_with_vlm(jpeg)
    print("\n=== VLM 推論・自律回避判断結果 ===")
    print(vlm_result)
