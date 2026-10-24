# 다음 세션에서 이어서 작업하기

> 본 문서는 Axon 작업을 다른 시점/세션에서 이어가기 위한 **현황 + 체크리스트 + 명령어 모음**입니다.
> 마지막 갱신: 2026-10-24 (PR #52까지 머지 — `CpuLatencyHold` API)

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
| 공정 벤치 | C++ rclcpp로 ROS 2 zero-copy(loaned + Fast DDS data-sharing)와 비교 (`benchmarks/ros2_cpp/`): host 1 MiB 15 vs 36 µs, GPU→GPU 4 MiB 26 vs 485 µs, 다중스트림(297 MB/s) 카메라 p50 29 vs 44 µs·CPU 0.019 vs 0.038 cores. 기존 수치는 rclpy 대비였음 | #43, #44, #46 |
| 경쟁 비교 | GPU→GPU를 ROS 2 공식 `rosidl::Buffer` CUDA 백엔드(Lyrical, Docker)와 ros2_cuda_ipc와 직접 비교: 둘 다 약 124 µs, axon 약 25 µs. 차이는 DDS descriptor(24~32 µs)와 소비자 측 CUDA IPC event 대기(61~72 µs). axon은 생산자가 host-sync하는 대가 | #47 |
| 추가 비교 | 같은 프로세스 ROS 2(intra-process)보다 프로세스 간 axon이 느리지 않음(spin 4~5.5 µs, 둘 다 blocking 1.3~2 µs 빠름). 생산자 host-sync는 파이프라인으로 짜면 처리량 손실 없음. 구독자 1→8(GPU 1 MiB) axon 25→38 µs vs ROS 2 154→450 µs. **CUDA MPS를 켜면 GPU 공유 전송과의 차이가 5×에서 1.5×로 줄어든다**(rosidl::Buffer 42, ros2_cuda_ipc 37, axon 25 µs). 약 60 µs는 프로세스 간 GPU 컨텍스트 전환으로 추정. 측정은 `run_gated.sh`(다른 GPU·CPU 사용 시 단위 재측정), 깊은 C-state는 PM QoS 0 | #48 |
| 남은 비교 | 소비자가 커널을 실행하면 MPS 없이는 axon 이점 없음(129 vs 122~124 µs), MPS에서는 1.6~2.2×(23 vs 44~49 µs). host 경로는 iceoryx2 0.10과 4 MiB까지 동등, 16 MiB부터는 axon이 느림(ring 깊이). Cyclone+iceoryx loan 31 µs, zenoh SHM 125 µs(직렬화). GPU 16/64 MiB axon 36/184 µs vs ROS 2 1.8/7.0 ms. `run_gated.sh gpu/sizes/rmw` | #49 |
| RT API | `axon::CpuLatencyHold`(PM QoS 전체 / per-CPU sysfs, RAII), `RtSetupConfig::cpu_latency_us`, `rt_core_siblings`, Python `cpu_latency_hold`. 권장: 생산자 CPU에 1 µs(C1) → 30 Hz 16 MiB 4.9~5.0 GHz, 241 µs(제한 없음 3.1~3.5 GHz, 397~424 µs). 권한은 `tools/udev/99-axon-cpu-latency.rules`(전용 그룹 axon-rt) | #52 |
| 클럭 | 30 Hz 생산자가 idle 코어에서 깨면 2.8~3.3 GHz로 써서 지연 2배(바쁜 코어·PM QoS 0이면 5.2 GHz) → 낮은 rate host 셀은 `PIN`+`PMQOS=1`로 잴 것. 고정 재측정: 16 MiB 1/2/4버퍼 220/230/373 µs vs iceoryx2 219. #50의 "64 MiB 7%" 주장은 재현 안 됨(철회, 원인 미확립) | #51 |
| ring 깊이 | 16 MiB host에서 버퍼 4개면 프레임의 21%를 DRAM에서 채움(1~2개는 ~0%), 384 vs 227/250 µs. TLB·page fault는 동일 → 캐시 용량. 64 MiB에서 1버퍼 axon이 iceoryx2보다 7% 느린 이유는 미해명. `run_ring_perf.sh`, usage.md ring 가이드 갱신 | #50 |
| 이벤트 대기 | `TensorSubscriber::wait_for_update` / `wait_any`(futex, `futex_waitv`), 크기 한도 위반 시 조용한 드롭 제거(`publish()` -1/EMSGSIZE, `create()` E2BIG, Python 예외) | #46 |

---

## 2. 남은 작업 (Next)

- [ ] **PREEMPT_RT 커널에서 `cyclictest` 1 kHz jitter 측정** — **진행 중, 보드 다운**: 기준선(stock 커널) 최악 1,138 µs(무부하)/1,263 µs(부하). 직접 빌드한 `5.10.216-rt108-tegra`로 재부팅한 뒤 hermes-head가 응답하지 않음. 콘솔에서 부팅 메뉴 `primary kernel` 선택 → `/boot/extlinux/extlinux.conf`의 `DEFAULT`를 `primary`로 복구 → `journalctl -b -1`로 원인 확인. 빌드·설치·측정 스크립트: `instrumentation/jetson/rt/` (#42), 결과 문서 `docs/hardware-verification.md` §7. 빌드 산출물은 호스트 `~/z_Setup_NvidiaXavier/rt-kernel/`
- [ ] **ROS Discourse 글** — 사용자가 게시하지 않기로 함(2026-10-19). 초안 `.omc/discourse_draft.md`는 #48의 MPS 결과(5× → 1.5×)를 반영하지 않았음
- [ ] **남은 비교** — 카메라 캡처→추론 end-to-end(보드 필요), 부하 시 꼬리 지연. NITROS는 Isaac ROS 5.0에서 `rosidl::Buffer` CUDA 백엔드로 대체됨(#47에서 측정한 경로). 같은 클럭에서도 남던 세션 간 ~35% 편차는 L3 경쟁(생산자가 DRAM에서 다시 채우는 양)과 일치함(#53, 당시 세션의 DRAM fill은 기록 없음). 1 µs hold의 p90 ~420 µs는 프레임 26~30%가 절반 클럭(2.61 GHz)으로 도는 꼬리 — POLL(0 µs) 또는 프레임 2 ms 전 pre-wake spin으로 제거(#54). SDK API `rt_sleep_until_prewarmed(deadline_ns, spin_us)` 추가(#55, API 경유 p90 220~221 µs). 커널 7.0.0-38(EPP balance_performance)에서도 같은 결과(#56)
- [ ] **개발 PC 주의** — 9800X3D 물리 코어 2~4 불량 의심, `cpu-offline-cores.service`로 CPU 2·3·4·10·11·12를 끈 상태. 벤치마크에 코어 상태를 기록할 것. CPU RMA 예정(Obsidian `Tasks/Hardware/CPU RMA 자료 …`)
- [ ] **Jetson에서 `wait_any` 확인** — r35 커널 5.10에는 `futex_waitv`가 없어 100 µs 폴링으로 동작. 보드 복구 후 테스트
- [ ] **AMD XDNA 백엔드** (`AXON_ENABLE_AMD_XDNA`) — 하드웨어 없음 (이 PC 9800X3D엔 NPU 없음)
- [ ] **실제 로봇 통합** — 실 센서(UVC 웹캠)는 Jetson에서 검증 완료

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
