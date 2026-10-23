<!-- 이 파일(README.ko.md, 한국어)은 README.md(영어, 원본)의 번역본입니다. 어느 한쪽을 바꾸면 같은 커밋에서 두 파일을 함께 갱신하세요. -->

<p align="center">
  <img src="images/Axon_Drawing.png" alt="axon — 로봇 안의 데이터 백본: 복잡하고 흩어진 로봇 데이터를 신뢰할 수 있는 지능으로" width="880">
</p>

# Axon — Physical AI를 위한 데이터 중심 Zero-Copy

[English](README.md) | **한국어**

> **한 줄 요약**: ROS2 위에 또 하나의 미들웨어를 얹는 게 아니다. **센서 → 가속기 → RT 제어 루프** 경로를 **끊기지 않는 end-to-end zero-copy**로 확장하고, **측정·보장되는 bounded staleness(신선도 한계)** 를 제공한다.

[![Status: Pre-alpha](https://img.shields.io/badge/status-pre--alpha-orange)]()
[![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue)](LICENSE)
[![Platform: Linux + PREEMPT_RT](https://img.shields.io/badge/platform-Linux%20%2B%20PREEMPT__RT-success)]()

> **상태**: 동작하는 라이브러리 + 실측 결과. 코어는 빌드·테스트 완료(9/9), 벤치마크·하드웨어 검증·ROS1/ROS2 연동·VLM 핸드오프 데모·프로세스 간 GPU accelerator pool 이 모두 이 개발 PC(RTX 5080)에서 동작한다. 처음이라면 **[docs/usage.md](docs/usage.md)**, [측정 결과](#측정-결과), [docs/hardware-verification.md](docs/hardware-verification.md) 를 보라.

---

## 개요

**무엇인가.** `axon`은 텐서 **페이로드를 공유 dma-buf(또는 GPU 할당)에 그대로 두고**, 모든 프로세스·가속기가 그것을 *자리에서* 참조하게 한다. 큐로는 고정 크기 디스크립터(≤256 B)만 흐르고, 버퍼의 FD는 `SCM_RIGHTS` 사이드카로 **최초 1회만** 전달된다. 한 줄로: **페이로드는 옮기지 말고, 자리에 두고 참조만 넘긴다.**

**왜인가.** 로봇 위의 Physical AI는 `센서 → 가속기 → RT 제어` 경로에서 **고대역폭 · 초저지연 · RT 결정성** 을 *동시에* 요구한다. 그런데 표준 미들웨어(ROS2 + DDS)는 프레임마다 텐서를 여러 번 복사한다. 그래서 페이로드가 클수록(4K 카메라·포인트클라우드·이미지 임베딩) 지연과 CPU가 **크기에 비례**해 커진다 — 바로 로봇이 가장 많은 바이트를 옮기는 지점에서. 게다가 staleness가 부하에 따라 흔들려 **안전 분석에 쓸 수 없다.** 메시지 레벨 zero-copy(Iceoryx2)는 메시지를 RAM까지 복사 없이 나르지만, **그것을 가속기에 올리는 순간 다시 복사**가 생기고 RT 신선도 보장은 여전히 비어 있다. `axon`은 이 두 빈틈을 메운다.

그림 한 장이면 충분하다: **초록(페이로드)은 한 자리에 머물고, 파랑(작은 메타데이터)만 움직인다.**

```mermaid
flowchart LR
    S["📷 센서<br/>카메라 · LiDAR"] --> POOL
    subgraph BIG[" axon: 페이로드는 한 자리에 머문다 (복사 0) "]
        POOL[("🔗 공유 dma-buf / GPU 풀<br/>원본 프레임 · 임베딩 · 출력 텐서")]
    end
    POOL -->|zero-copy import| INF["🧠 가속기 추론<br/>GPU / NPU"]
    INF -->|"출력도 같은 평면"| POOL
    POOL -.->|zero-copy view| RT["⏱️ RT 제어 루프<br/>1kHz · seqlock"]
    RT --> ACT["🦿 액추에이터"]

    S -->|"작은 디스크립터<br/>(≤256B) + FD 1회"| Q{{"메타데이터 큐<br/>seqlock / Iceoryx2"}}
    INF -->|"작은 디스크립터"| Q
    Q -.->|"신선도 · seqno"| RT

    classDef g fill:#c8e6c9,stroke:#2e7d32,stroke-width:2px
    classDef b fill:#bbdefb,stroke:#1565c0,stroke-width:2px
    class POOL,INF,RT,S g
    class Q b
```

초록 = 자리에서 참조되고 절대 복사되지 않는 페이로드. 파랑 = 큐를 넘는 유일한 것. (단계별 구체적 파이프라인은 [데이터 흐름 한눈에](#데이터-흐름-한눈에)에 있고, 이것이 대체하는 복사 중심 ROS2/DDS 경로는 그 그림이 제거하는 4번 복사 사슬이다.)

**무엇이 좋아지나** — 전송 자체가 **페이로드 크기에 O(1)** 이다. 디스크립터만 오가므로 페이로드는 생산자가 쓴 자리에 한 번 쓰이고, GPU나 RT 루프로 가는 동안 다시 복사되지 않는다. 이 개발 PC 실측:

### 핵심 결과 (실측, RTX 5080 / Ryzen 9800X3D vs ROS 2 Jazzy / Fast DDS)

| 항목 | 결과 |
|---|---|
| 프로세스 간 GPU → GPU, 4 MiB (ROS 2 loaned message + data-sharing 대비, C++) | 지연 **19× 낮음** (26 µs vs 485 µs) |
| GPU 메모리를 공유하는 ROS 2 전송 대비 GPU → GPU (공식 `rosidl::Buffer` CUDA 백엔드, ros2_cuda_ipc) | CUDA MPS를 켜면, 프레임에 커널을 실행하는 소비자 기준 1 MiB에서 **약 2× 낮음**(≈23 vs 44~49 µs, 4 MiB에서는 1.6×), 결과를 복사해 가는 소비자 기준 1.5~2×. MPS가 없으면 복사해 가는 소비자에서만 약 5× 낮다(≈25 vs ≈125 µs). 커널을 실행하는 소비자는 프로세스 간 GPU 컨텍스트 전환 비용(약 100 µs)을 axon에서도 낸다(129 vs 122~124 µs) |
| 프로세스 간 axon vs **한 프로세스 안의** ROS 2 (intra-process `unique_ptr` / type adapter) | **느리지 않다**: axon 소비자가 spin하고 ROS 2가 `spin_some` 루프로 받을 때 4~5.5 µs 빠르고(예: GPU 24 vs 28 µs), 두 소비자가 모두 blocking일 때(깊은 C-state 끔) 1.3~2 µs 빠르다. 노드를 한 프로세스로 모아도 axon보다 빨라지지 않는다 |
| 구독자 1 → 8개, GPU 1 MiB (이벤트 방식 소비자, 깊은 C-state 끔) | **25 → 38 µs** vs 154 → 450 µs. 생산자와 구독자 전체 CPU 0.021 vs 0.26 cores |
| 다른 공유 메모리 전송 대비 host → host (iceoryx2 0.10, Cyclone DDS + iceoryx, zenoh SHM), 64 B~64 MiB | 4 MiB까지는 **iceoryx2와 비슷하다**(1 MiB: 15 vs 17 µs, Cyclone loan 31, zenoh 125). 기본값인 버퍼 4개짜리 ring에서는 **16 MiB부터 더 느리다**(클럭을 고정한 측정에서 373 vs 219 µs). ring이 깊을수록 DRAM 트래픽이 늘기 때문이며, 버퍼 1개면 iceoryx2와 같다. host 경로는 axon이 이기는 곳이 아니다 |
| host → host, 1 MiB, ROS 2 loaned message + data-sharing 대비 (#43) | **2.4× 낮음** (15 µs vs 36 µs), 4 MiB에서는 1.4× |
| 다중스트림 로봇 워크로드 @ 297 MB/s (같은 비교, 이벤트 방식 소비자) | 카메라 p50 **29 vs 44 µs**, **CPU 절반** (0.019 vs 0.038 cores), 캐시미스 2.8× 적음, 모든 프레임 전달 |
| RT 루프 page fault | **0 / frame** |
| GPU 프로세스 간 공유 | **zero-copy**, 200/200 프레임, 1.68 GB |
| VLM 인코더→LLM 핸드오프 (video 34 MB) | host 왕복 대비 **36× 빠름** |
| Jetson(AGX Xavier)에서 실제 카메라 → CUDA | **복사 0**: 카메라가 쓴 버퍼를 다른 프로세스의 CUDA가 그대로 읽음 |

자세한 내용은 [측정 결과](#측정-결과), 신선도 보장은 [목표 지표와 실측](#목표-지표와-실측)에 있다.

---

## 왜 axon인가 — 대안들과 무엇이 다른가

프로세스 간 GPU zero-copy 공유는 **새로운 기술이 아닙니다.** 표준 부품(dma-buf,
CUDA VMM, `__cuda_array_interface__`/DLPack) 위에 서 있고, 성숙한 시스템들이 이미
이걸 합니다. axon의 승부수는 그 시스템들이 차지하지 않은 **특정한 패키징**입니다:

- 🔌 **프레임워크·벤더 중립.** GStreamer·ROS·고정 오퍼레이터 그래프에 묶이지
  않습니다. 그냥 `device_ptr` / CUDA-array-interface를 내주므로, **같은 전송**이
  PyTorch 모델도 하드웨어 NVENC 인코더도 먹입니다 —
  [`examples/vla_demo/`](examples/vla_demo/)(DeiT-tiny → GPT-2, 양쪽 동일 GPU 포인터),
  [`examples/nvenc_flywheel/`](examples/nvenc_flywheel/)(공유 GPU 버퍼에서 바로 녹화).
- ⏱️ **실시간 latest-value-wins.** seqlock 평면 + **측정·공식으로 상한이 보장되는
  staleness** + 백프레셔 없음 — *가장 신선한* 샘플을 원하는 제어 루프용. 대부분의
  대안은 무손실 큐를 최적화합니다.
- 🪶 **얇고 감사 가능** — 오후 한나절이면 읽는 코어, 수년짜리 프레임워크가 아님.

**비교** (가장 가까운 것들 — 전체 분석은 **[docs/positioning.md](docs/positioning.md)**):

| 솔루션 | 프로세스 간 GPU zero-copy | 결합도 | 전달 모델 |
|---|---|---|---|
| **axon** | ✅ (CUDA VMM, GPU→GPU 약 25 µs) | **없음** — `device_ptr` / CAI | **latest-value-wins, 상한 staleness** |
| ROS 2 `rosidl::Buffer` CUDA 백엔드 (Lyrical) | ✅ (GPU→GPU 약 124 µs, MPS에서 약 42 µs) | ROS 2 Lyrical 이상 | ROS pub/sub |
| ros2_cuda_ipc | ✅ (GPU→GPU 약 125 µs, MPS에서 약 37 µs) | ROS 2 | ROS pub/sub |
| NVIDIA Isaac ROS / NITROS | ✅ | ROS 2 + NVIDIA | ROS pub/sub |
| NVIDIA DeepStream | ✅ | GStreamer + NVIDIA | 파이프라인 / 큐 |
| Iceoryx2 / eCAL | ❌ 호스트 SHM만 | 없음 | 큐 |
| ROS 2 + DDS | ❌ 호스트 SHM만 | DDS / ROS | 큐 / history |

**정직한 범위**: axon은 pre-alpha이고 단일 호스트 전용입니다. NVIDIA 프레임워크들은
프로덕션급이고 노드를 넘으며 훨씬 많은 기능을 갖췄습니다. axon은 그 폭을 내주는 대신
중립성·RT 최신값 의미론·작은 코어를 택합니다. **단일 호스트에서 프레임워크 중립적인
zero-copy 전송을 RT 소비자에게** 주고 싶으면 axon을, **벤더가 밀어주는 올인원
파이프라인**을 원하면 Isaac ROS / DeepStream / Holoscan을 고르세요. 자세한 결정
가이드: **[docs/positioning.md](docs/positioning.md)**.

---

## 핵심 아이디어 (60초)

ROS2 + Iceoryx2 연동은 **메시지 미들웨어 레벨**에서 zero-copy를 제공한다. 메시지는 복사 없이 RAM까지 도달할 수 있지만, 그것을 가속기(NPU/GPU)에 올리려면 대개 또 한 번의 복사가 필요하다.

`axon`은 그 위에 **빠져 있던 두 평면**을 더한다:

1. **FD 평면** — dma-buf FD를 `SCM_RIGHTS` / `pidfd_getfd(2)` 사이드카로 직접 전달한다. V4L2 캡처가 export한 dma-buf를 가속기 드라이버가 import — **host 메모리 복사 없음.**
2. **시간 평면** — 추론은 비-RT 워커에서 돌고, RT 제어 루프는 seqlock으로 결과를 zero-copy view로 읽는다. staleness 한계는 **명시적 산식(7개 항)** 으로 계산되어 안전 분석에 바로 쓸 수 있다.

---

## 데이터 흐름 한눈에

```mermaid
flowchart LR
    SENSOR["📷 센서"] --> CAPTURE["V4L2 캡처<br/>VIDIOC_EXPBUF"]
    CAPTURE --> DMABUF["dma-buf"]
    DMABUF --> NPU["NPU/GPU import<br/>+ 추론"]
    NPU --> OUT["출력 dma-buf"]
    OUT --> META["Iceoryx2:<br/>TensorDescriptor (메타만)"]
    META --> RT["RT seqlock 읽기<br/>(1kHz)"]
    OUT -.->|"zero-copy view"| RT
    RT --> CTL["제어 루프"]
    CTL --> ACT["액추에이터"]

    classDef zerocopy fill:#c8e6c9,stroke:#2e7d32,stroke-width:2px
    classDef meta fill:#bbdefb,stroke:#1565c0,stroke-width:2px
    class DMABUF,OUT,RT zerocopy
    class META meta
```

초록 = zero-copy 영역. 파랑 = 메타데이터 메시지. 12개 다이어그램이 더 있다: [`DesignFiles/diagrams.md`](DesignFiles/diagrams.md).

---

## 목표 지표와 실측

워크스테이션(RTX 5080 / Ryzen 9800X3D, 비-RT 커널)에서 동일 워크로드로 ROS2 Jazzy / Fast-RTPS와 비교 측정. 전체 내용: [docs/hardware-verification.md](docs/hardware-verification.md).

| 지표 | 목표 | 실측 (axon vs ROS2) |
|---|---|---|
| ROS 2 zero-copy 경로 대비 지연 (C++, loaned message + data-sharing) | 낮게 | **host 1 MiB: 15 vs 36 µs (2.4×), GPU→GPU 4 MiB: 26 vs 485 µs (19×), 다중스트림 카메라 p50 29 vs 44 µs** ✅ |
| 297 MB/s에서 CPU (다중스트림, C++, ROS 2 zero-copy 대비) | 낮게 | **이벤트 방식 0.019 vs 0.038 cores (1 kHz 루프는 0.015)**, 둘 다 0.1 core 미만 ✅ |
| 부하 시 프레임 전달 (C++) | 드롭 없음 | **모든 전송에서 모든 카메라 스트림 100 %** (ROS 2는 프레임보다 큰 Fast DDS SHM 세그먼트가 필요하며, 벤치마크는 없으면 실행을 거부한다) ✅ |
| 메모리 비용 (같은 워크로드, C++, ROS 2 zero-copy 대비) | 페이로드 복사 0 | **캐시미스 이벤트 방식 2.8×, 1 kHz 루프 4.2× 적음; 프레임당 전송 syscall ~0** ✅ |
| RT 중 page fault | **0** | **0 / frame** (프레임 20배에도 평평) ✅ |
| GPU zero-copy import | 실제 하드웨어 | **RTX 5080: 200/200 프레임, 복사 0, 1.68 GB** ✅ |
| 센서 → 가속기, 복사 없음 | 실제 보드 + 실제 센서 | **AGX Xavier + UVC 웹캠: host == CUDA view 30/30 프레임, staleness ~0.5 ms** ✅ |
| Staleness 한계 | 명시적 7개 항의 합 | 결정적 산식 (§5) ✅ |
| 1kHz RT 최악 지터 | < 100µs | Jetson 기본 커널 1.26 ms(기준선), PREEMPT_RT 측정 대기 🟡 |

산식:
```
worst_case_staleness ≤ 
    T_cap + T_fence_p + T_inf + T_pub
  + T_sc + T_rt_seq + T_view
```
[정의](DesignFiles/detailed_design_doc.md#5-bounded-staleness-formula) | [시각화](DesignFiles/diagrams.md#9-bounded-staleness--visualized)

---

## 측정 결과

핵심 명제는 전송이 **페이로드 크기에 O(1)** 이라는 것이다: 고정 크기 디스크립터만 메타데이터 평면을 넘고 텐서는 공유 dma-buf에 남으므로, 페이로드는 한 번 쓰이고 도중에 복사되지 않는다.

**다중스트림 로봇 워크로드, C++** ([`benchmarks/ros2_cpp`](benchmarks/ros2_cpp/README.md#multi-stream-cpu-latency-delivery-and-cache-misses)): 카메라 4대와 LiDAR, IMU, 오도메트리를 297 MB/s까지 늘렸다. ROS 2는 이벤트 방식 executor로 받고, axon은 1 kHz 루프로 받거나 `TensorSubscriber::wait_any`에서 기다린다(이벤트 방식, [docs/usage.md](docs/usage.md#event-driven-consumer-wake-on-publish)).

| 297 MB/s에서 (3회 중앙값) | rclcpp `UInt8MultiArray` | ROS 2 zero-copy | axon, 1 kHz 루프 | axon, `wait_any` |
|---|---|---|---|---|
| CPU, 생산자 + 소비자 | 0.077 cores | 0.038 cores | **0.015 cores** | 0.019 cores |
| 카메라 지연 p50 / p99 | 103 / 334 µs | 44 / 235 µs | 557 / 1017 µs | **29 / 124 µs** |
| 초당 캐시미스 / 명령어 | 15.6 M / 0.219 B | 3.66 M / 0.127 B | **0.87 M / 0.024 B** | 1.31 M / 0.040 B |
| 카메라 스트림 전달률 | 100 % | 100 % | 100 % | 100 % |

C++에서는 모든 전송이 가볍기 때문에, CPU 차이는 실제로 있지만 절대값은 작다. 이벤트 방식의 axon은 ROS 2 zero-copy 경로의 절반 CPU로 가장 낮은 지연을 낸다. 1 kHz 루프는 CPU를 더 적게 쓰지만 프레임이 반 tick을 기다린다. 아래 rclpy 측정(0.30 vs 0.98 cores)의 차이는 거의 전부 Python 비용이다:

![CPU vs bandwidth](docs/assets/02_cpu_sweep.png)

**ROS 2 자체의 zero-copy 경로와 C++로 비교** ([`benchmarks/ros2_cpp`](benchmarks/ros2_cpp/README.md)). 위아래 그래프는 rclpy(Python)와 비교한 것이고, 그 차이의 대부분은 Python에서 나온다. rclcpp로 다시 쟀고, 비교 대상은 loaned message + Fast DDS data-sharing이다(p50, µs, 3회 중앙값):

| 페이로드 | host → host: ROS 2 zero-copy / axon | GPU → GPU: ROS 2 zero-copy / axon |
|---|---|---|
| 147 KiB | 25 / **4** | 50 / **24** |
| 1 MiB | 36 / **15** | 171 / **25** |
| 4 MiB | 80 / **59** | 485 / **26** |

host에서는 ROS 2의 zero-copy 경로가 axon에 가깝다. 둘 다 페이로드를 공유 메모리에 한 번 쓰고, axon이 아끼는 것은 메시지마다 붙는 약 20 µs의 고정 비용이다. GPU에서는 ROS 2 메시지가 host 메모리에 있어야 하므로 페이로드가 전송 앞뒤로 D2H와 H2D를 거친다. axon은 device 버퍼 자체를 넘기므로 크기와 상관없이 24~26 µs를 유지한다. 기본 설정의 Fast DDS는 이 크기에서 zero-copy 경로를 쓰지 **않으며**, 그 디렉터리의 XML 프로파일이 필요하다.

**단일스트림 지연, rclpy 대비** ([`benchmarks/`](benchmarks/)):

![Single-stream latency](docs/assets/01_latency_single.png)

**복사는 공짜가 아니다** — 동일 전달 바이트, 커널 측정 ([docs/hardware-verification.md](docs/hardware-verification.md)):

![Memory cost](docs/assets/04_memory.png)

ROS2(rclpy)는 같은 데이터를 옮기는 데 **캐시미스 8.6배**, **명령어 5배** 를 쓴다. C++로 ROS 2 zero-copy 경로와 같은 측정 구간에서 비교하면 그 차이는 4.2배와 5.3배다(axon의 1 kHz 루프 기준).

**로봇 보드 없이도 실제 GPU zero-copy.** axon의 FD 사이드카가 살아 있는 RTX 5080 GPU 메모리 핸들을 프로세스 간에 나른다: 200/200 프레임 on-GPU 검증, **host 페이로드 복사 0**, 1.68 GB zero-copy 이동. 여기에 RT page fault **프레임당 0** 측정. [docs/hardware-verification.md](docs/hardware-verification.md) 및 [docs/adr/](docs/adr/)의 아키텍처 결정 참조.

**실제 Jetson과 실제 카메라에서.** AGX Xavier(JetPack 5.1.6)와 UVC 웹캠으로, 카메라가 axon `NvBufSurface` pool에 직접 캡처하고 두 번째 프로세스가 같은 프레임을 host view **와** CUDA device 포인터로 받는다 — 경로 어디에도 복사가 없다.

| 검증 항목 (AGX Xavier, 640×480 YUYV) | 결과 |
|---|---|
| 카메라가 pool에 직접 기록 (`V4L2_MEMORY_DMABUF`) | ✅ 프로브 3/3회 |
| 다른 프로세스: CUDA view == host view | ✅ **30/30** 프레임(프로브), **4/4** 검사(데모) |
| 소비자의 GPU 쓰기가 생산자에게 보임 | ✅ 같은 물리 메모리 |
| staleness, 카메라 프레임 → 소비자 | **~0.5 ms** (1 ms 폴링 대기 포함) |
| 보드에서 라이브러리 테스트 | ctest **9/9**, `test_nvbuf_pool` 10/10회 반복 |

재현: [`examples/jetson_camera/`](examples/jetson_camera/README.md), 단계별 근거: [docs/hardware-verification.md §6](docs/hardware-verification.md#6-jetson--camera--cuda-zero-copy-via-nvbufsurface-agx-xavier-).

> 수치는 머신 종속적 — `benchmarks/mock/mock_compare.py`와 `instrumentation/` 스위트로 재현 가능(모두 리소스 제한되어 PC를 얼리지 않음).

---

## 30초 데모 (작업 중)

```
[ TODO: 30초 비디오 — closed-loop 미니 데모 ]
[ V4L2 카메라 → NPU 추론 → 1kHz RT 제어 루프 ]
[ 위 지표들의 라이브 오버레이 ]
```

Closed-loop 비디오 데모는 실 센서 + 로봇 보드 대기 중이나, 그것이 실행하는 GPU/RT 데이터 경로는 이미 이 개발 PC에서 측정되었다([측정 결과](#측정-결과) 참조).

---

## 빠른 빌드

전체 라이브러리, 예제, Python 바인딩, 벤치마크, 하드웨어 계측 스위트가 오늘 빌드된다:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAXON_BUILD_PYTHON=ON
cmake --build build -j
(cd build && ctest --output-on-failure)      # 9/9, 경고 0

# axon vs ROS2 벤치마크 (ROS2 선택)
python3 benchmarks/mock/mock_compare.py --scales 1,2,3,4 --seconds 5
# 실 하드웨어 검증 (GPU / page-fault / syscall / cache)
instrumentation/gpu/build.sh && instrumentation/run_bounded.sh ./build/gpu_sidecar_demo 8 200
```

처음이라면 **[docs/usage.md](docs/usage.md)** 부터 — 생산자/소비자, RT 루프, Python, GPU 예제가 각각 검증된 데모에 매핑된 복붙 코드로 있다.

선택 기능 플래그 (모두 기본 off — 코어는 의존성 0 유지):
- `-DAXON_WITH_ICEORYX2=ON` — Iceoryx2 메타데이터 백엔드 ([docs/metadata-backends.md](docs/metadata-backends.md)).
- `-DAXON_WITH_CUDA=ON` — 프로세스 간 GPU zero-copy용 `PoolBackend::Accelerator` CUDA VMM device pool ([docs/usage.md §4](docs/usage.md#4-cross-process-gpu-zero-copy-accelerator-pool-r6)).
- `-DAXON_ENABLE_JETSON=ON` — `PoolBackend::NvBufSurface` Jetson pool: 카메라 → host view + CUDA view, 복사 없음 ([docs/usage.md §5](docs/usage.md#5-jetson-camera--cuda-zero-copy-nvbufsurface-pool)).

### 사전 요구사항

- Linux + PREEMPT_RT 패치 커널 (RT 검증에만 필요. 스파이크 PoC 단독은 스톡 커널에서 동작.)
- V4L2 호환 카메라 (USB UVC 카메라면 됨)
- 가속기 보드 1종:
  - **AMD AI Series** (XDNA NPU) — XDNA 드라이버, ROCm 6.x+
  - **NVIDIA Jetson** — JetPack 5.x+ (검증: AGX Xavier, JetPack 5.1.6 / CUDA 11.4)
- gcc 11+, cmake 3.22+

### 스파이크 PoC 빌드 (week 1-2 검증)

```bash
# 생산자와 소비자 모두 빌드
cmake -S examples/spike_poc -B build/spike -DCMAKE_BUILD_TYPE=Release
cmake --build build/spike -j

# 실행 (한 터미널)
./build/spike/axon_spike_producer /dev/video0

# 다른 터미널
./build/spike/axon_spike_consumer
```

PoC가 검증하는 것:
- ✅ V4L2 캡처 → `VIDIOC_EXPBUF`가 dma-buf FD를 export
- ✅ `SCM_RIGHTS`가 dma-buf FD를 프로세스 간 전달
- ✅ 받은 FD를 zero-copy host view로 mmap (eBPF 검증)
- ✅ 가속기 import — NVIDIA Jetson은 NvBufSurface로 완료 ([examples/jetson_camera](examples/jetson_camera/README.md)), AMD XDNA는 아직

[스파이크 가이드](examples/spike_poc/README.md) | [스파이크 결정 트리](DesignFiles/diagrams.md#12-week-1-2-spike-poc-decision-tree)

---

## API 한눈에

라이브러리는 구현·테스트 완료 — 스켈레톤이 아니다. 실행 가능한 전체 예제(생산자/소비자, RT 루프, Python, GPU)는 **[docs/usage.md](docs/usage.md)** 에 있고, 여기선 형태만 본다.

```cpp
#include <axon/publisher.h>
#include <axon/subscriber.h>
#include <axon/pool.h>

// 생산자 측 (비-RT)
auto pool = axon::TensorPool::create({
    .n_buffers   = 32,
    .buffer_size = 4 * 1024 * 1024,
    .backend     = axon::PoolBackend::Custom,   // Custom/UDMABUF/V4L2/Accelerator
    .v4l2_device = nullptr,
});
auto pub = axon::TensorPublisher::create("camera/inference_out", *pool);
pub->handshake_pool();  // SCM_RIGHTS 일괄 전달

while (running) {
    axon::AcquiredDescriptor a = pub->acquire_descriptor();
    // ... 프레임을 a.host_view에 바로 쓰고, a.desc를 채운다 ...
    pub->publish(std::move(a));
}

// 소비자 측 (RT 1kHz 루프)
auto sub = axon::TensorSubscriber::create("camera/inference_out");
sub->wait_handshake();
sub->set_fallback_policy(axon::FallbackPolicy::LastKnownGood);

axon::rt_setup_memory_and_sched();  // mlockall + prefault + SCHED_FIFO

while (rt_tick()) {
    auto view = sub->latest_view(/*max_retry=*/8);
    if (view) {
        process(view->data, view->shape);
        log_staleness(view->staleness_ns);
    }
    // fallback은 sub 내부에서 적용됨
}
```

[사용 & 예제](docs/usage.md) | [전체 API 헤더](include/axon/) | [`TensorDescriptor` 정의](DesignFiles/detailed_design_doc.md#112-tensordescriptor-definition-iceoryx2-payload)

---

## FAQ

**Q. ROS2 + Iceoryx2 연동이 이미 있는데, 왜 또?**
A. `rmw_iceoryx_cpp`와 Iceoryx2의 ROS2 연동은 **메시지 미들웨어 레벨**의 zero-copy를 준다. axon은 그 위에 **dma-buf FD 사이드카 + 가속기 import 통합층**을 더해, 센서에서 가속기를 지나 RT 제어 루프까지 zero-copy가 끊기지 않게 한다. [차별점 상세](DesignFiles/data-centric-zero-copy-design-20260510.md#premises-agreed)

**Q. dma-buf FD를 Iceoryx2 SHM으로 보낼 수 있나?**
A. 아니다. 정수 FD를 공유 메모리에 써도 다른 프로세스에선 의미가 없다 — FD 테이블은 프로세스별이다. 프로세스 간 FD 전달은 `SCM_RIGHTS` 또는 `pidfd_getfd(2)`가 필요하다. [사이드카 핸드셰이크 시퀀스](DesignFiles/diagrams.md#3-fd-handshake-sequence)

**Q. RT 루프가 NPU 추론을 직접 호출하나?**
A. 아니다. NPU 추론 지연은 P99.99까지 롱테일이고 발열 스로틀링의 영향을 받아 결정적으로 한계 지을 수 없다. axon은 추론을 비-RT 워커에서 돌리고, RT 루프는 **측정·보장된 staleness 한계를 갖는 가장 최근 추론 결과**만 seqlock zero-copy view로 읽는다. [RT 패턴](DesignFiles/detailed_design_doc.md#33-rt-consumer-pattern-seqlock)

**Q. 첫 빌드 타깃 플랫폼은?**
A. week 1-2 스파이크 PoC에서 결정. 후보: **AMD AI Series (XDNA)** 와 **NVIDIA Jetson Orin**. Apple Silicon은 macOS에 V4L2가 없고 Asahi Linux에 ANE 드라이버가 없어 **첫 빌드 범위 밖**. [결정 트리](DesignFiles/diagrams.md#12-week-1-2-spike-poc-decision-tree)

**Q. 멀티 호스트(분산) 지원?**
A. 첫 빌드는 단일 호스트 다중 프로세스만. 멀티 호스트는 Phase 4(zenoh 연동 또는 Iceoryx2 분산 모드). [진화 경로](DesignFiles/diagrams.md#11-evolution-path--phase-1--phase-4)

**Q. ROS2 사용자가 채택할 수 있나?**
A. 그렇다 — Phase 3에서 ROS2 RMW 백엔드(Approach C)를 제공. Phase 1 코어는 ROS2 의존성이 없는 최소 라이브러리라, ROS2 사용자는 얇은 래퍼로 채택할 수 있다.

**Q. 라이선스?**
A. Apache 2.0 (특허 그랜트 포함 — 로봇 산업 채택에 더 친화적).

---

## 벤치마크 재현 환경

현재 README 그래프는 워크스테이션에서 측정(수치는 머신 종속적):

```
- Host: AMD Ryzen 7 9800X3D (16 threads), NVIDIA RTX 5080, 80 GB RAM
- Kernel: Linux 6.17 (non-RT) — 1kHz 지터 목표는 PREEMPT_RT 커널 대기
- Middleware: ROS2 Jazzy / Fast-RTPS (default DDS), Iceoryx2 v0.9.2
- Payloads: synthetic tensors; serving-robot multi-stream profile (benchmarks/mock)
- Runner: all under instrumentation/run_bounded.sh (리소스 제한)
```

전체 방법 + 원 수치: [docs/hardware-verification.md](docs/hardware-verification.md).

---

## 설계 문서 트리

| 문서 | 역할 |
|---|---|
| [`docs/usage.md`](docs/usage.md) | **사용 & 예제** — C++/Python/GPU 복붙 스니펫, 각각 검증된 데모에 매핑 |
| [`DesignFiles/data-centric-zero-copy-design-20260510.md`](DesignFiles/data-centric-zero-copy-design-20260510.md) | 방향, 차별점, 리스크/완화 (APPROVED v2) |
| [`DesignFiles/detailed_design_doc.md`](DesignFiles/detailed_design_doc.md) | 메커니즘 상세 (14 섹션, ~700줄) |
| [`DesignFiles/diagrams.md`](DesignFiles/diagrams.md) | 12개 Mermaid 다이어그램 |
| [`docs/adr/`](docs/adr/) | 아키텍처 결정 기록 (근거 + 실측 증거) |
| [`docs/hardware-verification.md`](docs/hardware-verification.md) | 실측 결과 (GPU zero-copy, page-fault, syscall, cache) |
| [`docs/metadata-backends.md`](docs/metadata-backends.md) | Seqlock vs Iceoryx2 백엔드 |
| `examples/spike_poc/README.md` | Week 1-2 스파이크 PoC 가이드 |

---

## 로드맵

설계 → 동작하는 라이브러리 + 실측 결과. 완료(`main` 머지):

- [x] 설계 문서 v2, 메커니즘 상세(~700줄), 12개 Mermaid 다이어그램
- [x] 스파이크 PoC — V4L2 `VIDIOC_EXPBUF` → `SCM_RIGHTS` → mmap zero-copy view
- [x] 코어 라이브러리 — 사이드카(FD 평면) + seqlock 메타데이터 + dma-buf 풀 + RT 헬퍼 + pub/sub
- [x] Python 바인딩 (zero-copy NumPy)
- [x] Iceoryx2 lock-free SHM 메타데이터 백엔드 (`AXON_WITH_ICEORYX2`)
- [x] ROS2 단일스트림 + 다중스트림(MockSystem) 벤치마크
- [x] RTX 5080 하드웨어 검증 — GPU zero-copy, page-fault, syscall, perf/cache
- [x] ROS1 통합 — 디스크립터 토픽 offload(M1) **+ 드롭인 `axon` `image_transport` 플러그인(M2)**, ROS 비의존 공용 `axon_bridge` 위에 (Docker 검증: 232/232 프레임·복사 0)
- [x] **ROS2 통합** — *동일한* `axon_bridge` 위의 디스크립터 토픽 offload, 네이티브 ROS2 Jazzy(`integrations/ros2_offload/`): 페이로드는 dma-buf에 머물고 디스크립터만 DDS로, 메시지별 상한 staleness(평균 ~160 µs). 90/90 프레임·복사 0
- [x] Depth wire v2 (row_pitch / depth_scale / intrinsics) + 검증
- [x] VLM 인코더→LLM 핸드오프 벤치마크 (최대 36×)
- [x] **R6 accelerator pool** — `PoolBackend::Accelerator` CUDA VMM device zero-copy (`AXON_WITH_CUDA`)
- [x] **R2 sync-fence** — 생산자→소비자 순서 보장을 위해 `sync_file` fence를 `latest_view`에 노출; 비-RT `drain_fences()`가 RT 읽기를 syscall-free로 유지(fence 도착 전 프레임은 스킵)
- [x] **방향 A — vision→LLM zero-copy** — 프레임워크 텐서 ↔ axon GPU 버퍼를 `__cuda_array_interface__`로 생산자(`device_array`)·소비자(`latest_view`) 양쪽에서 연결 + 소비자 측 CUDA VMM import. 동작하는 2-프로세스 데모(`examples/vla_demo/`: DeiT-tiny → axon → GPT-2 prefill, 양쪽 동일 GPU 포인터, 호스트 복사 없음)
- [x] **Jetson NvBufSurface 백엔드** — `PoolBackend::NvBufSurface` (`AXON_ENABLE_JETSON`): UVC 카메라가 pool에 직접 캡처하고, 다른 프로세스가 같은 프레임을 host view **와** CUDA 포인터로 복사 없이 받음. 실제 웹캠과 AGX Xavier에서 검증(`examples/jetson_camera/`, host==device 4/4, staleness ~0.5 ms)
- [x] **방향 D — NVENC 데이터 플라이휠** — 공유 GPU 버퍼에서 곧장 디스크로 녹화: 녹화기가 각 `latest_view()`를 호스트 복사 없이 하드웨어 NVENC에 전달(`examples/nvenc_flywheel/`, 디코드로 검증된 H.264)
- [x] **ROS 2 zero-copy 경로와의 공정한 C++ 비교** — rclcpp, loaned message + Fast DDS data-sharing, host와 GPU(`benchmarks/ros2_cpp/`): host 1 MiB 15 vs 36 µs, GPU→GPU 4 MiB 26 vs 485 µs, 297 MB/s 다중스트림 카메라 p50 29 vs 44 µs (0.019 vs 0.038 cores)
- [x] **이벤트 방식 소비자** — `TensorSubscriber::wait_for_update` / `wait_any`가 메타데이터 슬롯의 futex에서 기다린다(스트림 여러 개는 `futex_waitv`). 생산자는 누가 기다릴 때만 `FUTEX_WAKE`를 호출하므로 RT 생산자의 `publish()`에는 syscall이 없다
- [x] **조용한 드롭 제거** — 맞지 않는 디스크립터는 `publish()`가 `EMSGSIZE`/`EINVAL`과 함께 -1을 돌려주고 seqno를 쓰지 않는다. 핸드셰이크에 실을 수 없는 import 정보는 `TensorPublisher::create()`가 거부한다(`E2BIG`). Python 바인딩은 예외를 던진다

다음:

- [ ] PREEMPT_RT 커널에서 `cyclictest` 1 kHz 지터 (타깃 보드 필요)
- [ ] AMD XDNA 백엔드 + 실 로봇 통합

---

## 기여

동작하는 라이브러리, 빠르게 움직이는 pre-alpha. 이슈·토론·PR 환영. 코어는 필수 의존성이 0이고 스톡 Linux 박스에서 빌드·테스트(9/9) 통과한다 — [docs/usage.md](docs/usage.md)로 시작하라.

> **문서는 이중 언어다.** `README.md`(영어)가 원본이고 `README.ko.md`(한국어)가 이를 미러링한다. 어느 쪽을 바꾸든 **같은 커밋에서 둘 다** 수정하라.

## 라이선스

Apache 2.0 — [LICENSE](LICENSE) 참조
