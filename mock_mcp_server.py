import socket, json, threading

RESPONSES = {
    "disk": {"command_used": "df -h", "result": "Filesystem  Size  Used Avail\n/dev/sda1   50G   20G   30G"},
    "memory": {"command_used": "free -h", "result": "              total  used  free\nMem:          16G    4G   12G"},
    "cpu": {"command_used": "top -bn1 | head -5", "result": "top - 17:00:01 up 2 days\nLoad: 0.10 0.12 0.15"},
    "uptime": {"command_used": "uptime", "result": " 17:00:01 up 2 days, 3:22,  1 user"},
}

def handle(conn):
    with conn:
        data = b""
        while not data.endswith(b"\n"):
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
        try:
            req = json.loads(data.decode())
            query = req.get("params", {}).get("query", "").lower()
            print(f"[mock-mcp] query: {query}")
            matched = next((v for k, v in RESPONSES.items() if k in query), None)
            if matched:
                resp = {"status": "ok", **matched}
            else:
                resp = {"status": "error", "message": f"no command for: {query}"}
        except Exception as e:
            resp = {"status": "error", "message": str(e)}
        conn.sendall((json.dumps(resp) + "\n").encode())

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 9000))
    s.listen(10)
    print("[mock-mcp] Listening on 127.0.0.1:9000")
    while True:
        conn, _ = s.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()
