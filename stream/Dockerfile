# Service vision : diffusion webcam + reconnaissance faciale.
#
# opencv-python-HEADLESS, pas opencv-python : la variante complete tire libGL
# et Qt, inutiles dans un conteneur sans ecran, pour des centaines de Mo.
#
# Ni dlib ni PyTorch : la detection (YuNet, 230 ko) et la reconnaissance
# (SFace, 38 Mo) sont deux ONNX fournis avec OpenCV, qui tournent tres bien
# sur le CPU d'un portable.
FROM python:3.12-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends libglib2.0-0 curl \
 && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir \
      "opencv-python-headless>=4.10,<5" \
      "numpy>=1.26,<3" \
      "paho-mqtt>=2.1,<3"

# Modeles telecharges a la CONSTRUCTION : le conteneur n'a besoin d'aucun
# acces reseau sortant a l'execution, ce qui compte puisqu'il tourne sur un
# point d'acces isole.
#
# L'URL passe par media.githubusercontent.com : le depot opencv_zoo utilise
# Git LFS, et raw.githubusercontent.com ne renvoie qu'un pointeur de 130 octets.
ARG ZOO=https://media.githubusercontent.com/media/opencv/opencv_zoo/main/models
RUN mkdir -p /models \
 && curl -fsSL "$ZOO/face_detection_yunet/face_detection_yunet_2023mar.onnx" \
      -o /models/yunet.onnx \
 && curl -fsSL "$ZOO/face_recognition_sface/face_recognition_sface_2021dec.onnx" \
      -o /models/sface.onnx \
 && python3 -c "import os,sys; \
      [sys.exit(f'modele trop petit: {p}') for p in ('/models/yunet.onnx','/models/sface.onnx') \
       if os.path.getsize(p) < 100_000]" \
 && echo "modeles ok: $(du -sh /models | cut -f1)"

WORKDIR /app
COPY store.py vision.py app.py ./

EXPOSE 8080
CMD ["python3", "-u", "app.py"]
