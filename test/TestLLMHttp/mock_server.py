"""測試用的 OpenAI 相容伺服器（只給 TestLLMHttp 用）。

POST /v1/chat/completions
  一般：回傳固定的候選詞；stream=true 時以 SSE 分段送出（中間夾保持連線的註解行）
POST /slow     一直送空白保持連線，不結束（測總時間上限）
POST /stall    送一個事件後就不再送任何東西（測串流的閒置逾時）
POST /error    401 與錯誤訊息
GET  /count    到目前為止建立過幾條連線（測連線重用）
"""
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

connections = 0
lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'  # 讓用戶端可以重用連線

    def setup(self):
        global connections
        super().setup()
        with lock:
            connections += 1

    def log_message(self, *args):
        pass

    def send_json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/count':
            self.send_json(200, {'connections': connections})
        else:
            self.send_json(404, {'error': {'message': 'not found'}})

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        request = json.loads(self.rfile.read(length) or b'{}')
        if self.path == '/error':
            self.send_json(401, {'error': {'message': 'bad key: ' + self.headers.get('Authorization', '')}})
            return
        if self.path in ('/slow', '/stall'):
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.end_headers()
            try:
                if self.path == '/stall':
                    self.wfile.write(b'data: {"x":1}\n\n')
                    self.wfile.flush()
                    time.sleep(30)
                else:
                    for _ in range(300):
                        self.wfile.write(b' ')
                        self.wfile.flush()
                        time.sleep(0.1)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass
            self.close_connection = True
            return
        if request.get('stream'):
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.end_headers()
            try:
                for i, piece in enumerate(['甲', '乙', '丙']):
                    self.wfile.write(b': keep-alive\n\n')
                    event = {'choices': [{'delta': {'content': piece}}], 'i': i}
                    self.wfile.write(('data: ' + json.dumps(event, ensure_ascii=False) + '\n\n').encode('utf-8'))
                    self.wfile.flush()
                    time.sleep(0.05)
                self.wfile.write(b'data: [DONE]\n\n')
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass
            self.close_connection = True
            return
        content = '候選一 候選二，候選三、4. 候選四'
        self.send_json(200, {'choices': [{'message': {'role': 'assistant', 'content': content}}],
                             'echo_model': request.get('model')})


class Server(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        if not isinstance(sys.exc_info()[1], ConnectionError):  # 用戶端關閉連線是正常的
            super().handle_error(request, client_address)


if __name__ == '__main__':
    server = Server(('127.0.0.1', int(sys.argv[1])), Handler)
    print('ready', flush=True)
    server.serve_forever()
