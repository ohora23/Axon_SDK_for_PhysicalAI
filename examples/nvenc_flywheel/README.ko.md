# NVENC 데이터 플라이휠 데모 — 공유 GPU 버퍼에서 바로 녹화

[English](README.md) | **한국어**

*데이터 플라이휠*은 로봇의 실시간 센서 스트림을 녹화해, 다음 모델을 학습시킬
데이터셋을 쌓는 순환 고리입니다. 여기서 비싼 부분은 **녹화**입니다. 단순하게 하면
매 프레임을 GPU에서 CPU로 복사해 인코더에 넣습니다 — 프레임마다 호스트를 한 바퀴
돕니다.

이 데모는 axon 방식으로 합니다. 프레임은 공유 **CUDA** 버퍼에 머물고, 녹화기는 각
프레임을 **하드웨어 영상 인코더(NVENC)에 곧장** 넘깁니다 — 호스트 복사도, GPU로의
재업로드도 없습니다. CPU를 거치는 것은 훨씬 작은 **압축 비트스트림**뿐이고, 그것도
파일로 가는 길에만 지나갑니다.

```
 frame_producer.py                          nvenc_recorder.py
 ┌────────────────────────┐                 ┌──────────────────────────────┐
 │ GPU에서 RGBA 렌더        │                 │ torch.as_tensor(view)        │
 │ axon 풀 버퍼(device)에   │ axon 디스크립터  │  (복사 없는 CAI 별칭)         │
 │ 기록                    │ ──────────────► │ NVENC.Encode(frame)          │
 │ publish_device()        │  (픽셀은 GPU에   │  GPU 버퍼 → 인코더,           │
 │                         │   그대로 머묾)   │  호스트 복사 없이 → .h264     │
 └────────────────────────┘                 └──────────────────────────────┘
```

## 무엇을 증명하나

- 녹화기는 `latest_view()`를 `__cuda_array_interface__`로 torch CUDA 텐서로 감싸
  그대로 `NVENC.Encode(...)`에 넘깁니다. 인코더가 **생산자의 공유 버퍼에서** 직접
  픽셀을 읽습니다 — 중간 호스트 복사도, GPU로 되돌리는 `cudaMemcpy`도 없습니다.
- 기록한 `.h264`를 다시 디코딩해 프레임 수가 인코딩한 수와 같은지 확인합니다 —
  실제로 유효한 영상이 나왔음을 증명합니다.
- axon은 **latest-value-wins**라, 녹화기가 뒤처지면 생산자를 막는 대신 프레임을
  건너뜁니다. 데모는 몇 개를 건너뛰었는지 보고합니다(따라잡으면 `0`). 모든 프레임이
  필요하면 생산자 주기나 풀 깊이를 키우세요
  ([`docs/usage.md`](../../docs/usage.md)의 사이징 설명 참고).

## 요구 사항

- axon Python 모듈을 CUDA로 빌드:
  `cmake -S . -B build-cuda -DAXON_BUILD_PYTHON=ON -DAXON_WITH_CUDA=ON && cmake --build build-cuda -j`
- `torch`(GPU에 맞는 CUDA 빌드)와
  [`PyNvVideoCodec`](https://pypi.org/project/PyNvVideoCodec/)이 있는 파이썬 환경
  (`pip install PyNvVideoCodec`). 이 장비에서는 `/home/jkyoo/.venvs/axon-vla`.
- NVENC이 있는 NVIDIA GPU(대부분의 GeForce/RTX 및 데이터센터 카드).

## 실행

```bash
examples/nvenc_flywheel/run_demo.sh
```

환경 변수 오버라이드:

```bash
VLA_W=1280 VLA_H=720 VLA_FRAMES=120 \
VLA_OUT=/tmp/clip.h264 \
VLA_PYTHON=/path/to/venv/bin/python \
AXON_SO_DIR=/path/to/build-cuda/python \
examples/nvenc_flywheel/run_demo.sh
```

기대 출력(끝부분):

```
[recorder] encoded 60 frames (0 skipped by latest-value-wins) -> 14949 bytes at /tmp/axon_flywheel.h264
[recorder] verify OK: /tmp/axon_flywheel.h264 decodes to 60 frames
[recorder] SUCCESS: pixels went axon GPU buffer -> NVENC with no host copy.
```

출력은 원시 H.264 엘리멘터리 스트림입니다. 재생하려면 컨테이너로 감싸세요.
예: `ffmpeg -i /tmp/axon_flywheel.h264 -c copy clip.mp4`.

## 파일

- `frame_producer.py` — GPU에서 RGBA 프레임을 합성해 axon `Accelerator` 풀에 발행.
- `nvenc_recorder.py` — 각 프레임을 복사 없이 읽어 NVENC으로 `.h264`에 인코딩한 뒤
  디코드-검증.
- `run_demo.sh` — 두 프로세스를 띄우고 결과를 보고.
