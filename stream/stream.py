"""
Diffuseur MJPEG de la webcam, pour le panneau "Webcam" du dashboard.

Pourquoi ce fichier existe : nginx proxifie /video/ vers le port 8080 de la
machine hote, mais vision.py affiche les images dans une fenetre OpenCV locale
(cv2.imshow) et n'expose rien en HTTP. Le panneau du dashboard ne pouvait donc
jamais rien afficher. Ce module comble ce trou, sans dependre de YOLO ni de
face_recognition : il ne faut qu'opencv, deja installe.

    python3 stream.py                 # port 8080, camera 0

Utilise par le dashboard via https://<serveur>/video/stream
"""
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import cv2

PORT = int(os.getenv("STREAM_PORT", "8080"))
CAMERA = int(os.getenv("CAMERA_INDEX", "0"))
WIDTH = int(os.getenv("FRAME_WIDTH", "640"))
HEIGHT = int(os.getenv("FRAME_HEIGHT", "480"))
QUALITY = int(os.getenv("STREAM_QUALITY", "70"))


class Camera:
    """Une seule capture, partagee par tous les clients.

    Ouvrir /dev/video0 deux fois echoue sur la plupart des webcams, donc un
    thread unique lit en continu et les clients se servent de la derniere image.
    """

    def __init__(self):
        self.frame = None
        self.lock = threading.Lock()
        self.running = True
        self.cap = cv2.VideoCapture(CAMERA)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, WIDTH)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, HEIGHT)
        if not self.cap.isOpened():
            raise RuntimeError(
                f"Camera {CAMERA} introuvable. Branchee ? "
                f"Essayer CAMERA_INDEX=1, ou verifier avec: ls /dev/video*"
            )
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while self.running:
            ok, frame = self.cap.read()
            if not ok:
                time.sleep(0.1)
                continue
            ok, jpg = cv2.imencode(".jpg", frame,
                                   [int(cv2.IMWRITE_JPEG_QUALITY), QUALITY])
            if ok:
                with self.lock:
                    self.frame = jpg.tobytes()
            time.sleep(1 / 25)

    def latest(self):
        with self.lock:
            return self.frame


camera = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/stream", "/"):
            self.send_response(200)
            self.send_header("Content-Type",
                             "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                while True:
                    frame = camera.latest()
                    if frame is None:
                        time.sleep(0.05)
                        continue
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n")
                    self.wfile.write(f"Content-Length: {len(frame)}\r\n\r\n".encode())
                    self.wfile.write(frame)
                    self.wfile.write(b"\r\n")
                    time.sleep(1 / 20)
            except (BrokenPipeError, ConnectionResetError):
                pass          # l'onglet a ete ferme, c'est normal
        elif path == "/snapshot":
            frame = camera.latest()
            if frame is None:
                self.send_error(503, "pas encore d'image")
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(frame)))
            self.end_headers()
            self.wfile.write(frame)
        else:
            self.send_error(404)


if __name__ == "__main__":
    camera = Camera()
    print(f"[stream] camera {CAMERA} ouverte, diffusion sur http://0.0.0.0:{PORT}/stream")
    print(f"[stream] visible dans le dashboard via /video/stream")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
