"""
Construit la base de visages autorisés.

Structure :
vision/known_faces/alice.jpg
vision/known_faces/bob.jpg

Puis :
python vision/build_face_db.py
"""

import pickle
from pathlib import Path

import face_recognition

ROOT = Path(__file__).resolve().parent
KNOWN_DIR = ROOT / "known_faces"
OUTPUT = ROOT / "data" / "encodings.pkl"

IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png"}


def main():
    KNOWN_DIR.mkdir(parents=True, exist_ok=True)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)

    encodings = []
    names = []

    for image_path in sorted(KNOWN_DIR.iterdir()):
        if image_path.suffix.lower() not in IMAGE_EXTENSIONS:
            continue

        name = image_path.stem

        image = face_recognition.load_image_file(str(image_path))
        locations = face_recognition.face_locations(image, model="hog")
        face_encodings = face_recognition.face_encodings(
            image,
            known_face_locations=locations,
        )

        if len(face_encodings) != 1:
            print(
                f"[FACE DB] {image_path.name}: "
                f"{len(face_encodings)} visage(s), attendu exactement 1."
            )
            continue

        encodings.append(face_encodings[0])
        names.append(name)
        print(f"[FACE DB] Ajout : {name}")

    with OUTPUT.open("wb") as f:
        pickle.dump(
            {
                "encodings": encodings,
                "names": names,
            },
            f,
        )

    print(f"[FACE DB] Base créée : {OUTPUT}")
    print(f"[FACE DB] Personnes : {len(names)}")


if __name__ == "__main__":
    main()
