# axon ROS2 오프로드

[English](README.md) | **한국어**

텐서 **페이로드는 공유 dma-buf에 두고**, ROS2 토픽에는 고정 크기 **디스크립터**만
흘려보내는 ROS2(Jazzy) 통합입니다. 페이로드는 axon `SCM_RIGHTS` 사이드카로 한 번만
전달되고, DDS 계층은 메타데이터만 나릅니다 — 그래서 `ros2 topic`·`ros2 bag`·
메시지별 워치독이 그대로 동작하는 동안, 텐서는 **복사 없이** 프로세스를 건넙니다.

풀 + mmap + 사이드카 부트스트랩은 ROS1 wrapper가 쓰는 것과 **동일한 ROS-무관
`axon_bridge.h`** 입니다 — 여기 노드들은 그 위의 rclcpp 접착제일 뿐입니다.

```
 producer_node (rclcpp)                     consumer_node (rclcpp)
 ┌───────────────────────┐                  ┌────────────────────────────┐
 │ axon dma-buf 풀에       │  TensorDescriptor│ 공유 dma-buf에서 페이로드    │
 │ seqno 기록             │   (DDS 토픽)      │ 읽기 (복사 없음)             │
 │ 디스크립터 발행         │ ───────────────► │ 무결성 + staleness 확인      │
 └───────────────────────┘   (페이로드는      └────────────────────────────┘
      FD는 사이드카로 1회      dma-buf에 머묾)
```

## 왜 (순수 DDS / rmw_iceoryx 대비)

**호스트** 공유메모리는 ROS2에 이미 `rmw_iceoryx`가 있습니다. 여기서 axon의 각도는:

- **메시지마다 상한 있는 staleness** — `now − producer_publish_ts`를 프레임마다 실어
  측정합니다(이 장비에서 평균 ~160 µs, 최대 ~330 µs). 순수 DDS가 주지 못하는 RT
  신선도 보장이고, 안전 로직에 바로 쓸 수 있습니다.
- **GPU 경로의 토대** — 같은 디스크립터-오프로드가 Accelerator(CUDA VMM) 풀로
  확장되어, ROS2 소비자가 호스트 복사 없이 GPU 버퍼를 받습니다(다음 단계, 로드맵 참고).

QoS는 양쪽 모두 **keep-last(1), best-effort** 로, axon의 **latest-value-wins** 전달을
의도적으로 반영합니다. 소비자는 항상 가장 신선한 디스크립터만 처리하고, 이미
덮어써진 풀 슬롯을 읽게 만드는 오래된 백로그를 쌓지 않습니다.

## 요구 사항

- ROS2(**Jazzy**에서 검증) — 네이티브, **Docker 불필요**.
- axon 코어 정적 라이브러리. 러너가 대신 빌드하거나:
  `cmake -S . -B build && cmake --build build -j`

## 실행

```bash
integrations/ros2_offload/run_demo.sh          # 코어 + 패키지 빌드 후 데모 실행
FRAMES=200 integrations/ros2_offload/run_demo.sh
```

기대 출력(끝부분):

```
────── axon ROS2 offload — consumer summary ──────
  frames read:      90
  payload errors:   0   (must be 0 — cross-process zero-copy)
  staleness:        mean=160us  max=332us
  payload path:     shared dma-buf (never serialized through DDS)
──────────────────────────────────────────────────
```

또는 수동으로(터미널 2개, `ros2_ws`에서 `source install/setup.bash` 후):

```bash
ros2 run axon_ros2 producer_node
ros2 run axon_ros2 consumer_node --ros-args -p frames:=90
```

## 구성

- `ros2_ws/src/axon_ros2/include/axon_ros2/axon_bridge.h` — 재사용 ROS-무관 브리지
  (ROS1 헤더의 그대로 복사본).
- `msg/TensorDescriptor.msg` — 메타데이터 전용 메시지(페이로드 없음).
- `src/producer_node.cpp`, `src/consumer_node.cpp` — rclcpp 노드.
- `run_demo.sh` — 빌드 + 실행 + 판정.

## 사이드카 버퍼 한계

풀 기본값은 **32** 버퍼입니다 — 사이드카는 모든 풀 FD를 하나의 `SCM_RIGHTS`
메시지로 보내며 최대 32개로 제한됩니다. `n_buffers ≤ 32`를 지키고, 소비자가 읽는
도중 슬롯이 lap되지 않을 만큼 깊게 잡으세요
([`docs/usage.md`](../../docs/usage.md)의 사이징 설명 참고).
