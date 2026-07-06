# 다음 세션에서 이어서 작업하기

> 본 문서는 `physicalAIRos` 프로젝트 작업을 다른 시점/세션에서 이어가기 위한 **체크리스트 + 명령어 모음**입니다. 한국어로 적어두었으니 그대로 따라하면 됩니다.

---

## 0. 프로젝트 한눈에

- **이름**: `physicalAIRos` (GitHub 표시명) / 라이브러리 코드네임은 `axon` (data-centric zero-copy)
- **GitHub**: https://github.com/ohora23/physicalAIRos (private)
- **무엇을 만드는가**: ROS2 + Iceoryx2 위에 **dma-buf FD 사이드채널 + 가속기 import 통합층**을 얹어 센서 → 가속기 → RT 제어 루프 끝단까지 zero-copy를 끊지 않는 SDK. bounded staleness가 7개 항 산식으로 측정·보장.
- **상태 (2026-05-10 기준)**: 설계 단계. 1-2주차 Spike PoC 코드 push 완료. 보드 확보 + 실제 실행은 미수행.

핵심 설계 문서가 모두 `DesignFiles/`에 있고 Claude는 새 세션에서도 이걸 읽으면 컨텍스트가 자동으로 복원됩니다.

---

## 1. 현재 PR 상태

| # | Branch | Base | 내용 | 줄 수 |
|---|---|---|---|---|
| #1 | `feat/design-documentation` | `main` | DesignFiles/* (4 docs) | 1,485 |
| #2 | `feat/api-scaffolding` | `main` | CMakeLists + 6 API headers | 452 |
| #3 | `feat/spike-poc-week-1-2` | **`feat/api-scaffolding`** (stacked) | spike PoC | 643 |

PR #3는 **#2에 stacked** 되어 있으므로 #2가 머지되어야 #3이 main으로 자동 rebase됩니다.

**main에 들어 있는 것**: `README.md`, `LICENSE`, `.gitignore` (그리고 본 `RESUME.md`).

---

## 2. 다음 세션에서 환경 복원하기

### Step 1 — 디렉토리 이동

```bash
cd /home/jkyoo/repositories/physicalAI_ROS
```

> 다른 머신에서 시작한다면 먼저 clone:
> ```bash
> gh repo clone ohora23/physicalAIRos
> cd physicalAIRos
> ```

### Step 2 — 상태 확인

```bash
git status
git branch -a
gh pr list
```

기대 결과 예시:
```
* feat/spike-poc-week-1-2     (또는 main)
  feat/api-scaffolding
  feat/design-documentation
  main
  remotes/origin/...

#1  docs: design documentation set ...   feat/design-documentation
#2  feat: scaffold CMake project ...     feat/api-scaffolding
#3  feat(spike): week 1-2 spike PoC ...  feat/spike-poc-week-1-2
```

### Step 3 — 각 PR 상세 확인

```bash
gh pr view 1
gh pr view 2
gh pr view 3
```

PR이 머지되었거나 닫혔다면 `gh pr list --state all`로 전체 이력.

---

## 3. Claude Code 새 세션 시작

이 디렉토리에서 `claude` 명령(또는 IDE의 Claude Code 패널)을 열고 다음 한 줄을 던집니다:

> **"physicalAIRos 프로젝트 이어서 작업할 거야. README와 DesignFiles/ 읽고, 열린 PR 상태(#1, #2, #3) 확인한 다음 진행 계획 세워줘."**

Claude가 자동으로:
- `README.md` → 프로젝트 비전 / 핵심 명제 파악
- `DesignFiles/data-centric-zero-copy-design-20260510.md` → 합의된 결정사항 (APPROVED v2)
- `DesignFiles/detailed_design_doc.md` → 메커니즘 상세 (14 섹션)
- `DesignFiles/diagrams.md` → 12개 Mermaid 다이어그램
- `gh pr list` → 어디까지 진행됐는지
- 다음 단계 계획 제시

> 한국어로 소통하고 싶다면 첫 메시지에 "한글로 소통" 한 줄 더 적어주세요.

---

## 4. PR 처리 (사용자 결정 단계)

Claude는 머지하지 않습니다 — GitHub에서 본인이 직접:

### 4.1 권장 머지 순서

```
#1 (docs)              → main  : 독립적, 언제든 OK
#2 (api-scaffolding)   → main  : #3의 base이므로 먼저 머지
#3 (spike-poc)         → main  : #2 머지되면 base가 자동으로 main으로 변경됨
```

### 4.2 Squash 머지 권장

각 PR을 `Squash and merge`로 처리하면 main 히스토리가 단정합니다.

### 4.3 리뷰 코멘트 후 변경이 필요하다면

```bash
git checkout feat/api-scaffolding   # 해당 PR branch
# ... 파일 수정 ...
git add <변경된 파일>
git commit -m "fix: address review comment about ..."
git push
```

PR에 자동으로 새 commit이 추가됩니다.

---

## 5. 다음 작업 후보 (Roadmap)

설계 문서 §10.1 (`DesignFiles/detailed_design_doc.md`)의 14주 일정 기준.

### 🔴 즉시 (오늘/이번 주)

- [ ] PR #1, #2, #3 GitHub UI에서 머지
- [ ] (선택) RESUME.md를 git에 commit할지 결정 — 아래 §7 참조

### 🟡 외부 작업 (하드웨어 의존)

- [ ] **타깃 보드 확보**: AMD AI Series (XDNA) 또는 NVIDIA Jetson Orin
- [ ] **USB UVC 카메라** 준비 (간단한 logitech 캠 정도면 충분)
- [ ] Spike PoC 실제 실행:
  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAXON_BUILD_EXAMPLES=ON
  cmake --build build -j

  # Terminal A
  ./build/examples/spike_poc/axon_spike_producer /dev/video0

  # Terminal B
  ./build/examples/spike_poc/axon_spike_consumer
  ```
- [ ] 결과로 첫 타깃 플랫폼 확정 (`DesignFiles/diagrams.md` §12 결정 트리)

### 🟢 코드 작업 (Claude와 함께 진행 가능)

- [ ] **3-4주차**: TensorDescriptor + sidecar 핸드셰이크 정식 구현 (`src/` 추가)
- [ ] **5-6주차**: 가속기 import 백엔드 (선택된 플랫폼 1개)
- [ ] **7-8주차**: RT consumer 헬퍼 (mlockall, seqlock, fallback)
- [ ] **9-10주차**: closed-loop 통합 + cyclictest 24h + eBPF 검증
- [ ] **11-12주차**: ROS2 baseline 비교 벤치마크
- [ ] **13-14주차**: README 영상 추가 + 공개

각 단계별 다음 PR을 만들 때 새 branch:
```bash
git checkout main
git pull --ff-only origin main
git checkout -b feat/week-3-4-formal-impl
# ... 작업 ...
git push -u origin feat/week-3-4-formal-impl
gh pr create --title "..." --body "..."
```

---

## 6. 자주 쓰는 명령어 모음

### Git / GitHub
```bash
# 상태 확인
git status
git log --oneline -10
gh pr list
gh pr view <번호>

# main 최신화
git checkout main
git pull --ff-only origin main

# 새 branch
git checkout -b feat/<설명>
git add <파일>
git commit -m "..."
git push -u origin feat/<설명>
gh pr create --title "..." --body "..."

# 기존 branch 작업 이어가기
git checkout feat/<branch>
git pull --rebase origin feat/<branch>

# stacked PR (다른 branch 위에)
git checkout feat/api-scaffolding
git checkout -b feat/<새-기능>
# ... 작업 ...
gh pr create --base feat/api-scaffolding ...
```

### 빌드 / 실행
```bash
# 헤더만 (#2 머지된 후)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# spike PoC 포함 (#3 머지된 후)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAXON_BUILD_EXAMPLES=ON
cmake --build build -j
```

### Mermaid 다이어그램 SVG로 추출 (필요 시)
```bash
npm i -g @mermaid-js/mermaid-cli
mmdc -i DesignFiles/diagrams.md -o diagrams.svg
```

---

## 7. 트러블슈팅 / FAQ

**Q. `gh` 인증이 풀렸어요.**
```bash
gh auth status
# 풀려 있으면:
gh auth login
```

**Q. git config가 다른 머신에서 비어있어요.**
```bash
git config user.name "ohora23"
git config user.email "ohora23@gmail.com"
```
> `--global`을 붙이면 모든 repo에 적용됩니다.

**Q. PR을 잘못 만들었어요. 닫고 다시.**
```bash
gh pr close <번호>
git push origin --delete feat/<branch>
git branch -D feat/<branch>
```

**Q. 기존 PR에 commit을 추가하고 싶어요.**
```bash
git checkout feat/<branch>
# 수정 후
git add <파일>
git commit -m "..."
git push
```

**Q. main에 잘못 commit했어요.**
- main에 push했다면 force-push는 위험. 새 PR로 revert 또는 fix.
- 아직 push 전이라면 `git reset --soft HEAD~1`로 되돌리고 새 branch에서.

**Q. 이 RESUME.md 자체를 git에 포함할까요?**
- 본인용 노트면 `.gitignore`에 `RESUME.md` 추가:
  ```bash
  echo "RESUME.md" >> .gitignore
  git add .gitignore && git commit -m "chore: ignore RESUME.md (personal note)"
  git push
  ```
- 팀원과 공유할 가이드면 main에 commit:
  ```bash
  git checkout main
  git add RESUME.md
  git commit -m "docs: add resume guide for resuming work in future sessions"
  git push
  ```

---

## 8. TL;DR (3줄 요약)

1. `cd /home/jkyoo/repositories/physicalAI_ROS && gh pr list`로 PR 상태 확인
2. GitHub UI에서 #1·#2·#3 머지 (순서: #1 자유 → #2 → #3)
3. Claude한테 "physicalAIRos 이어서, [다음 작업]" 한 줄 던지면 자동으로 컨텍스트 복원

모든 결정사항이 `DesignFiles/`에 영구 보존되어 있어 시간이 지나도 작업을 이어갈 수 있습니다.
