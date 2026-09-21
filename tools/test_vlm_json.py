import base64
import json
import urllib.request
import io
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

with open('cores3_cam_test.jpg', 'rb') as f:
    img_b64 = base64.b64encode(f.read()).decode('utf-8')

url = 'http://127.0.0.1:1234/v1/chat/completions'
headers = {'Content-Type': 'application/json'}
prompt = """このカメラ画像からロボットの自律回避走行の判断を行い、以下のJSONフォーマットのみを出力してください（マークダウンのコードブロックで囲んでください）：
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
    'model': 'qwen3.5-9b-vlm',
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

req = urllib.request.Request(url, data=json.dumps(payload).encode('utf-8'), headers=headers)
try:
    with urllib.request.urlopen(req, timeout=30) as response:
        result = json.loads(response.read().decode('utf-8'))
        print('=== JSON Action Output ===')
        print(result['choices'][0]['message']['content'])
except Exception as e:
    print('Error:', e)
