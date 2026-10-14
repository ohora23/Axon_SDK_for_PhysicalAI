# 다음 세션에서 이어서 작업하기

> 본 문서는 Axon 작업을 다른 시점/세션에서 이어가기 위한 **현황 + 체크리스트 + 명령어 모음**입니다.
> 마지막 갱신: 2026-10-15 (main = `035d8c6`, PR #39까지 머지 — Jetson 백엔드 완료, hermes-head를 Axon 전용 보드로 전환)

---

## 0. 프로젝트 한눈에

- **이름**: Axon (구 코드네임 `dczc`, #18에서 rename)
- **GitHub**: https://github.com/ohora23/Axon_SDK_for_PhysicalAI
- **로컬 경로**: `/home/jkyoo/orca/Axon_SDK_for_PhysicalAI`
- **무엇을 만드는가**: dma-buf FD 사이드채널 + 가속기 import 통합층으로 센서 → 가속기 → RT 제어 루프 끝단까지 zero-copy를 끊지 않는 SDK. ROS1/ROS2/Iceoryx2 위에 얹힌다. bounded staleness를 측정·보장.
- **상태**: pre-alpha 동작 라이브러리. core 빌드/테스트 green (x86 8/8, Jetson 9/9). Jetson NvBufSurface 백엔드까지 main에 머지 완료.

---

## 1. 완료된 작업 (main 머지 완료)

| 영역 | 내용 | PR |
|---|---|---|
| 설계 | 설계 문서 v2, 메커니즘 상세, Mermaid 12개, ADR 4건 (`docs/adr/`) | #1, #14 |
| Spike | V4L2 `VIDIOC_EXPBUF` → `SCM_RIGHTS` → mmap zero-copy view | #4 |
| Core | sidecar(FD plane) + seqlock 메타데이터 + dma-buf pool + RT 헬퍼 + pub/sub | #5 |
| 데모 | closed-loop 데모 + staleness/seqlock 측정 harness | #6 |
| Python | pybind11 바인딩, zero-copy NumPy view | #7 |
| 메타데이터 | pluggable 백엔드 + Iceoryx2 SHM 백엔드 (`AXON_WITH_ICEORYX2`) | #9 |
| 벤치마크 | ROS2 baseline, MockSystem 멀티스트림, VLM encoder→LLM 핸드오프 (최대 36×) | #8, #10, #17 |
| HW 검증 | RTX 5080: GPU zero-copy, page-fault, syscall, perf/cache (`docs/hardware-verification.md`) | #11–#13 |
| Depth | wire v2 (row_pitch / depth_scale / intrinsics) + 검증 (R1, R4) | #16 |
| R6 | CUDA VMM 가속기 pool, 프로세스 간 device zero-copy (`AXON_WITH_CUDA`) | #19 |
| ROS1 | descriptor-topic offload (M1) + drop-in `axon` image_transport 플러그인 (M2), 공용 `axon_bridge` | #15, #24, #25 |
| R2 / R3 | `latest_view`에 sync_file fence 노출 / R3는 latest-value-wins로 확정 + n_buffers 산정 문서화 | #28, #29 |
| 방향 A | `__cuda_array_interface__` (producer/consumer), consumer 측 device import, VLA 데모 (DeiT→GPT-2) | #30–#32 |
| 방향 D | NVENC data flywheel: 공유 GPU 버퍼에서 바로 H.264 녹화 | #33 |
| 문서 | usage 가이드, 한국어 README, 포지셔닝/경쟁 분석 (`docs/positioning.md`) | #21, #22, #34 |
| ROS2 | 같은 `axon_bridge` 위 descriptor-topic offload, Jazzy (평균 staleness ~160 µs, 90/90 frames) | #35 |
| Jetson | `PoolBackend::NvBufSurface`: UVC 카메라 → pool 직접 캡처 → 다른 프로세스가 host view + CUDA 포인터로 수신, 복사 0. wire v4. AGX Xavier 실기 검증 | #36–#38 |

---

## 2. 남은 작업 (Next)

- [ ] **PREEMPT_RT 커널에서 `cyclictest` 1 kHz jitter 측정** — 타깃 보드 필요
- [ ] **AMD XDNA 백엔드** (`AXON_ENABLE_AMD_XDNA`) — 하드웨어 없음 (이 PC 9800X3D엔 NPU 없음)
- [ ] **실제 로봇 통합** — 실 센서(UVC 웹캠)는 Jetson에서 검증 완료
- [ ] (기존 버그) `SidecarClient::connect`가 재핸드셰이크 때 이전 소켓을 닫지 않음

Jetson 보드 = hermes-head (AGX Xavier, JetPack 5.1.6). 2026-10-15부터 **Axon 전용**이다: Hermes LLM 서비스는 껐고(파일·모델은 보존), 백업과 복구법은 호스트 PC의 `~/z_Setup_NvidiaXavier/backup_head_2026-10-14/README.md`에 있다. sudo는 비밀번호가 필요하므로 사용자가 직접 실행한다. 접속은 `~/z_Setup_NvidiaXavier/hermes_ssh.sh head '<cmd>'`, 작업 트리 `~/axon-dev/{src,build}`, cmake는 `~/axon-dev/cmake/bin` (시스템 cmake 3.16은 너무 낮음). 웹캠 `/dev/video0`.

---

## 3. 코드 지도

| 경로 | 내용 |
|---|---|
| `include/`, `src/` | core 라이브러리 |
| `tests/` | 단위/통합 테스트 (ctest) |
| `python/` | pybind11 바인딩 + 테스트 |
| `examples/` | `spike_poc`, `demo`, `depth_filter`, `accelerator_pool`, `vla_demo`, `nvenc_flywheel` |
| `integrations/ros1_offload/` | ROS1 (Docker) — `run.sh`, `demo.sh`, `demo_transport.sh` |
| `integrations/ros2_offload/` | ROS2 Jazzy — `run_demo.sh` |
| `examples/jetson_camera/` | Jetson 카메라 → CUDA zero-copy 데모 |
| `instrumentation/jetson/` | NvBufSurface 실기 프로브 (`nvbuf_probe`) |
| `benchmarks/` | axon vs ROS2 비교 스크립트, MockSystem |
| `instrumentation/` | eBPF / GPU / perf 측정, `run_bounded.sh` |
| `DesignFiles/` | 원본 설계 문서 (정본) |
| `docs/` | usage, ADR, HW 검증, 메타데이터 백엔드, 포지셔닝 |

---

## 4. 빌드 / 실행

```bash
cd /home/jkyoo/orca/Axon_SDK_for_PhysicalAI

# core + tests (9/9)
cmake -S . -B build && cmake --build build -j
(cd build && ctest --output-on-failure)

# 선택 옵션
#   -DAXON_BUILD_PYTHON=ON     pybind11 바인딩
#   -DAXON_WITH_CUDA=ON        CUDA VMM 가속기 pool (R6, 방향 A/D)
#   -DAXON_WITH_ICEORYX2=ON    Iceoryx2 메타데이터 백엔드
#   -DAXON_ENABLE_JETSON=ON    Jetson NvBufSurface pool (보드에서만, CUDA와 함께 쓰면 device view)
```

예제·통합별 실행법은 각 디렉터리의 `README.md`와 `docs/usage.md`를 본다.

---

## 5. Claude Code 새 세션에서

이 디렉터리에서 Claude Code를 열고:

> **"Axon 이어서 작업할 거야. RESUME.md랑 README Roadmap 읽고 [다음 작업] 진행해줘."**

규칙:
- 문서는 이중 언어. `README.md`(정본)와 `README.ko.md`를 **같은 커밋에서** 함께 고친다.
- 새 작업은 `main`에서 `feat/<설명>` 브랜치를 따서 PR로 올린다 (squash merge).

---

## 6. 자주 쓰는 명령어

```bash
git status && git log --oneline -10
gh pr list --state all | head
git checkout main && git pull --ff-only origin main
git checkout -b feat/<설명>
git push -u origin feat/<설명> && gh pr create --title "..." --body "..."
```
