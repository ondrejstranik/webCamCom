import os
import webbrowser
from http.server import SimpleHTTPRequestHandler, HTTPServer
import threading
import time

# --- Serve files from the same folder as this script ---
script_dir = os.path.dirname(os.path.abspath(__file__))
os.chdir(script_dir)

# --- CORS-enabled request handler ---
class CORSRequestHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        return super().end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.end_headers()

# --- Start server ---
PORT = 8080
server = HTTPServer(("0.0.0.0", PORT), CORSRequestHandler)

print("Serving directory:", script_dir)
print(f"Starting server on http://localhost:{PORT}/index.html")

# --- Open browser automatically after server starts ---
def open_browser():
    time.sleep(0.5)  # small delay so server starts before opening browser
    webbrowser.open(f"http://localhost:{PORT}/index.html")

threading.Thread(target=open_browser).start()

# --- Run server ---
server.serve_forever()
