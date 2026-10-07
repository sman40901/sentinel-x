import pickle
import time
from pathlib import Path
from typing import Callable, Optional

import cv2
import face_recognition
from ultralytics import YOLO

from config import CONFIG


class VisionAnalyzer:
    PERSON_CLASS_ID = 0

    def __init__(
        self,
        on_event: Optional[Callable[[dict], None]] = None,
    ):
        self.on_event = on_event

        self.model = YOLO(CONFIG.yolo_model)

        self.face_encodings = []
        self.face_names = []
        self._load_face_db()

        self.last_intrusion = 0.0
        self.frame_counter = 0

    def _load_face_db(self):
        path = Path("vision/data/encodings.pkl")

        if not path.exists():
            print(
                "[VISION] Base de visages absente. "
                "La reconnaissance fonctionnera comme 'inconnu'."
            )
            return

        with path.open("rb") as f:
            data = pickle.load(f)

        self.face_encodings = data.get("encodings", [])
        self.face_names = data.get("names", [])

        print(
            f"[VISION] {len(self.face_names)} personne(s) autorisée(s) chargée(s)."
        )

    def _recognize_faces(self, frame):
        # face_recognition utilise RGB.
        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)

        locations = face_recognition.face_locations(rgb, model="hog")
        encodings = face_recognition.face_encodings(rgb, locations)

        results = []

        for encoding, location in zip(encodings, locations):
            name = "unknown"

            if self.face_encodings:
                matches = face_recognition.compare_faces(
                    self.face_encodings,
                    encoding,
                    tolerance=CONFIG.face_tolerance,
                )

                distances = face_recognition.face_distance(
                    self.face_encodings,
                    encoding,
                )

                if len(distances) > 0:
                    best_index = distances.argmin()

                    if matches[best_index]:
                        name = self.face_names[best_index]

            results.append(
                {
                    "name": name,
                    "location": location,
                }
            )

        return results

    def process_frame(self, frame):
        self.frame_counter += 1

        results = self.model.predict(
            source=frame,
            conf=CONFIG.yolo_confidence,
            verbose=False,
            imgsz=640,
        )

        person_detected = False

        for result in results:
            if result.boxes is None:
                continue

            for box in result.boxes:
                class_id = int(box.cls[0])
                confidence = float(box.conf[0])

                if class_id == self.PERSON_CLASS_ID:
                    person_detected = True

                    x1, y1, x2, y2 = map(
                        int,
                        box.xyxy[0].tolist(),
                    )

                    cv2.rectangle(
                        frame,
                        (x1, y1),
                        (x2, y2),
                        (0, 255, 0),
                        2,
                    )

                    cv2.putText(
                        frame,
                        f"Person {confidence:.2f}",
                        (x1, max(20, y1 - 10)),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        0.6,
                        (0, 255, 0),
                        2,
                    )

        # On ne lance la reconnaissance faciale que si YOLO voit une personne,
        # et seulement une frame sur N pour limiter la charge CPU.
        if (
            person_detected
            and self.frame_counter % CONFIG.face_check_every_n_frames == 0
        ):
            faces = self._recognize_faces(frame)

            for face in faces:
                name = face["name"]
                top, right, bottom, left = face["location"]

                if name == "unknown":
                    label = "INTRUS / INCONNU"
                    level = "critique"
                    event_type = "unknown_person"

                    now = time.time()

                    if (
                        now - self.last_intrusion
                        >= CONFIG.intrusion_cooldown_seconds
                    ):
                        self.last_intrusion = now

                        self._emit_event(
                            {
                                "event_type": event_type,
                                "niveau": level,
                                "score": 1.0,
                                "person": name,
                                "message": "Personne non autorisée détectée",
                            }
                        )
                else:
                    label = f"Autorise: {name}"
                    self._emit_event(
                        {
                            "event_type": "known_person",
                            "niveau": "info",
                            "score": 0.0,
                            "person": name,
                            "message": f"Personne autorisée reconnue : {name}",
                        }
                    )

                cv2.rectangle(
                    frame,
                    (left, top),
                    (right, bottom),
                    (0, 0, 255) if name == "unknown" else (255, 255, 0),
                    2,
                )

                cv2.putText(
                    frame,
                    label,
                    (left, max(20, top - 10)),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.6,
                    (0, 0, 255) if name == "unknown" else (255, 255, 0),
                    2,
                )

        return frame

    def _emit_event(self, event: dict):
        if self.on_event:
            self.on_event(event)

    def run(self):
        camera = cv2.VideoCapture(CONFIG.camera_index)

        if not camera.isOpened():
            raise RuntimeError(
                f"Impossible d'ouvrir la webcam index {CONFIG.camera_index}"
            )

        camera.set(cv2.CAP_PROP_FRAME_WIDTH, CONFIG.frame_width)
        camera.set(cv2.CAP_PROP_FRAME_HEIGHT, CONFIG.frame_height)

        print("[VISION] Webcam démarrée. Appuyer sur Q pour quitter.")

        try:
            while True:
                ok, frame = camera.read()

                if not ok:
                    print("[VISION] Lecture webcam impossible.")
                    break

                frame = self.process_frame(frame)

                cv2.imshow("Sentinel-X - Vision IA", frame)

                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break

        finally:
            camera.release()
            cv2.destroyAllWindows()
