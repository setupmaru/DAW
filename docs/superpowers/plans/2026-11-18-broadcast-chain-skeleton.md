# 방송용 마이크 VST3 체인 호스트 — 뼈대 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 마이크 입력을 VST3 체인에 통과시켜 출력하고, 방송 중 체인 조작에도 클릭이 없는 JUCE 데스크탑 앱 뼈대를 만든다.

**Architecture:** `AudioEngine`이 장치 콜백을 직접 구현하고, 오디오 스레드 쪽 `ChainProcessor`가 슬롯 배열을 wet/dry 크로스페이드로 처리한다. UI 쪽 `PluginChain`은 플러그인 인스턴스를 만들어 락프리 큐로 명령을 보내고, 다 쓴 슬롯을 반납 큐로 돌려받아 해제한다. 화면은 단일 창에 체인 목록, 미터, 오디오 설정 버튼만 둔다.

**Tech Stack:** C++17, JUCE `8.0.17-security` (CMake FetchContent), CMake ≥ 3.22, JUCE `UnitTest` + CTest, GitHub Actions (windows-latest, ubuntu-latest)

**Spec:** `docs/design.md` (저장소 루트 = `DAW/`)

## Global Constraints

- 저장소 루트(`DAW/`)를 기준으로 모든 경로를 적는다.
- JUCE는 `FetchContent`로 `GIT_TAG 8.0.17-security`, `GIT_SHALLOW TRUE`. 저장소에 JUCE를 복사하지 않는다.
- 플러그인 형식은 VST3만: `JUCE_PLUGINHOST_VST3=1`, `JUCE_PLUGINHOST_AU=0`, `JUCE_PLUGINHOST_LADSPA=0`, `JUCE_PLUGINHOST_LV2=0`.
- 의존성 축소: `JUCE_WEB_BROWSER=0`, `JUCE_USE_CURL=0`.
- ASIO: CMake 옵션 `DAW_ENABLE_ASIO` (기본 OFF). ON이면 `JUCE_ASIO=1` + `DAW_ASIO_SDK_DIR` include 경로. 이번 계획에서 ON 빌드는 검증하지 않는다.
- OS 전용 코드 금지(폰트 이름 선택만 `JUCE_WINDOWS`/`JUCE_MAC`/`JUCE_LINUX` 분기 허용).
- **오디오 스레드(`ChainProcessor::process`, `AudioEngine` IO 콜백)에서 할당, 해제, 잠금, 로그, 파일/UI 접근 금지.**
- 상수: `kMaxSlots = 16`, 슬롯 크로스페이드 `0.020` 초, 출력 딥 `0.010` 초, 명령 큐 용량 `64`, 반납 큐 용량 `64`.
- 처리 채널: 선택한 입력 채널 1개 → 스테레오(L=R) 복제 → 체인 → 스테레오 출력.
- UI 문구는 한국어. 폰트: Windows `"Malgun Gothic"`, macOS `"Apple SD Gothic Neo"`, Linux `"Noto Sans CJK KR"`.
- 앱 타깃/제품 이름 `DAW`, 테스트 타깃 `DAWTests`.
- 이 개발 머신은 화면과 VST3가 없다. 로컬 검증은 빌드 + `ctest`까지. 실행·플러그인 검증은 "미검증"으로 보고한다.

## Review Focus

1. **샘플레이트/버퍼 크기 변경**(오디오 설정에서 바꿈) — 기존 체인이 유지되고 플러그인이 새 설정으로 다시 준비되어야 한다. → Task 3 `testReprepareKeepsSlots`
2. **버튼 연타**(바이패스를 매 블록 토글, 큐가 가득 참) — 클릭 없이 마지막 상태로 수렴하고, 큐가 가득 차면 `post`가 `false`를 반환하며 UI 모델은 바뀌지 않아야 한다. → Task 2 `testRapidBypassToggleNoClick`, Task 5 `testPostFailureLeavesModelUnchanged`
3. **입력 채널 1개짜리 장치, 또는 선택 채널이 범위를 벗어남** — 크래시 없이 무음. → Task 4 `testInputChannelOutOfRangeGivesSilence`
4. **스테레오를 지원하지 않는 플러그인, 로드 실패** — 체인에 넣지 않고 한국어 오류 메시지 콜백. 크래시 금지. → Task 5 `testRejectsProcessorWithoutStereoLayout`
5. **체인이 가득 참(17번째 추가)** — 추가 거부, 기존 소리 영향 없음. → Task 3 `testSeventeenthInsertIsRetired`, Task 5 `addProcessor`가 `false` 반환

---

## 파일 구조

```
DAW/
├── .github/workflows/build.yml
├── .gitignore
├── CMakeLists.txt
├── README.md
├── docs/design.md, docs/superpowers/plans/…
├── src/
│   ├── Main.cpp
│   ├── MainComponent.h/.cpp
│   ├── audio/  AudioEngine.h/.cpp, ChainProcessor.h/.cpp, ChainSlot.h, LockFreeQueue.h, LevelMeterSource.h
│   ├── plugins/ PluginChain.h/.cpp, PluginScanner.h/.cpp, PluginEditorWindow.h/.cpp
│   └── ui/     ChainListComponent.h/.cpp, LevelMeter.h/.cpp, KoreanLookAndFeel.h
└── tests/
    ├── TestMain.cpp
    ├── TestProcessors.h          # 테스트용 AudioProcessor (반전, 게인, 고정 지연, 모노 전용)
    ├── ChainProcessorTests.cpp
    ├── AudioEngineTests.cpp
    └── PluginChainTests.cpp
```

CMake: 코어 소스(`src/audio/*.cpp`, `src/plugins/PluginChain.cpp`)를 변수 `DAW_CORE_SOURCES`로 묶어 `DAW`와 `DAWTests` 양쪽에서 컴파일한다(JUCE 모듈 중복 링크를 피하려고 정적 라이브러리를 만들지 않는다).

---

### Task 0: 개발 머신 준비 (사용자 수행)

`sudo`가 비대화식으로 안 되므로 사용자가 터미널에서 직접 실행한다.

- [ ] **Step 1:** 사용자에게 아래 명령 실행을 요청하고 완료를 기다린다.

```bash
sudo apt install -y libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libglu1-mesa-dev mesa-common-dev
```

- [ ] **Step 2:** 확인: `pkg-config --exists alsa freetype2 x11 && echo ok` → `ok`

---

### Task 1: 프로젝트 뼈대, 빌드, 테스트 러너, CI

**Files:**
- Create: `.gitignore`, `CMakeLists.txt`, `src/Main.cpp`, `src/MainComponent.h/.cpp`(빈 창), `tests/TestMain.cpp`, `.github/workflows/build.yml`, `README.md`

**Interfaces:**
- Produces: CMake 타깃 `DAW`(juce_add_gui_app), `DAWTests`(juce_add_console_app, `add_test(NAME DAWTests COMMAND DAWTests)`), 변수 `DAW_CORE_SOURCES`, `DAW_TEST_SOURCES`. 이후 Task는 여기에 파일만 추가한다.
- `tests/TestMain.cpp`: `juce::ScopedJuceInitialiser_GUI` 후 `juce::UnitTestRunner`로 전체 실행, 실패가 하나라도 있으면 종료 코드 1.

- [ ] **Step 1:** `git init` (브랜치 `main`). `.gitignore`: `build/`, `.vscode/`, `.idea/`, `*.user`.
- [ ] **Step 2:** `CMakeLists.txt` 작성. Global Constraints의 정의, `juce::juce_recommended_config_flags`, `juce_recommended_lto_flags`, `juce_recommended_warning_flags` 링크. 모듈: `juce_audio_utils`, `juce_audio_processors`, `juce_audio_devices`, `juce_dsp`, `juce_gui_extra`. `enable_testing()`.
- [ ] **Step 3:** `src/Main.cpp`: `DAWApplication : juce::JUCEApplication`, 창 제목 `"DAW - 방송용 마이크 이펙트"`, 크기 640×480, 크기 조절 가능. `MainComponent`는 빈 컴포넌트.
- [ ] **Step 4:** `tests/TestMain.cpp` + 실패하는 자리표시 테스트 하나(`SmokeTest`: `expect(false)`).
- [ ] **Step 5:** 빌드 후 테스트 실패 확인.
  Run: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j8 && ctest --test-dir build --output-on-failure`
  Expected: 빌드 성공, `DAWTests` FAIL.
- [ ] **Step 6:** 자리표시 테스트를 `expect(true)`로 바꾸고 다시 실행 → PASS.
- [ ] **Step 7:** `.github/workflows/build.yml`: `push`/`pull_request`, matrix `windows-latest`, `ubuntu-latest`. ubuntu는 Task 0 패키지 apt 설치. 단계: configure(`-DCMAKE_BUILD_TYPE=Release`) → `cmake --build build --config Release --parallel` → `ctest --test-dir build -C Release --output-on-failure`. FetchContent 캐시(`build/_deps`)는 `actions/cache`로 JUCE 태그를 키로.
- [ ] **Step 8:** `README.md`(한국어): 무엇인지 한 단락, Linux 의존 패키지 명령, Linux/Windows 빌드·테스트 명령, ASIO 옵션 설명, "로컬 머신에서 실행은 미검증" 메모.
- [ ] **Step 9:** 커밋 `chore: JUCE 앱 뼈대, 테스트 러너, CI 구성`

---

### Task 2: ChainProcessor — 통과, 추가, 바이패스 크로스페이드

**Files:**
- Create: `src/audio/LockFreeQueue.h`, `src/audio/ChainSlot.h`, `src/audio/ChainProcessor.h/.cpp`, `tests/TestProcessors.h`, `tests/ChainProcessorTests.cpp`

**Interfaces:**
- Produces:
  ```cpp
  template <typename T, int Capacity> class LockFreeQueue {   // SPSC, juce::AbstractFifo 기반
  public: bool push (const T&); bool pop (T&); };

  struct ChainSlot {                       // 메시지 스레드에서 생성, 오디오 스레드가 소유하는 동안 그쪽만 접근
      int id = 0;
      std::unique_ptr<juce::AudioProcessor> processor;
      juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wet;   // 0 = dry, 1 = wet
      juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay;
      int latencySamples = 0;
      bool bypassed = false;
      void prepare (double sampleRate, int maxBlockSize);   // processor->prepareToPlay, dryDelay 준비, 버퍼 확보
  };

  enum class ChainCommandType { Insert, Remove, Move, SetBypass };
  struct ChainCommand { ChainCommandType type; int slotId = 0; int index = 0; bool bypassed = false; ChainSlot* slot = nullptr; };

  class ChainProcessor {
  public:
      static constexpr int kMaxSlots = 16;
      static constexpr double kSlotFadeSeconds = 0.020, kOutputDipSeconds = 0.010;
      void prepare (double sampleRate, int maxBlockSize);   // 오디오 정지 상태에서만. 보유 슬롯 전부 재준비
      void process (juce::AudioBuffer<float>& stereo);      // 오디오 스레드, 2채널
      bool post (const ChainCommand&);                      // 메시지 스레드. 큐가 가득 차면 false
      ChainSlot* popRetired();                              // 메시지 스레드. 없으면 nullptr. 호출자가 delete
      int getTotalLatencySamples() const;                   // std::atomic<int>
      int getNumActiveSlots() const;                        // std::atomic<int>
  };
  ```
- `tests/TestProcessors.h`: `TestProcessorBase : juce::AudioProcessor`(스테레오 in/out, 에디터 없음), 파생 `InvertProcessor`, `GainProcessor(float)`, `FixedDelayProcessor(int samples)`(`setLatencySamples` 호출), `MonoOnlyProcessor`(`isBusesLayoutSupported`가 모노만 true). 헬퍼: `makeSlot(int id, std::unique_ptr<AudioProcessor>, double sr, int block)`, `runSine(ChainProcessor&, int numBlocks, float amp=0.5f, double freq=440, double sr=48000, int block=256) -> std::vector<float>`(왼쪽 채널 출력 누적), `maxStep(const std::vector<float>&) -> float`(인접 샘플 차 최댓값).

**클릭 기준:** 48 kHz, 440 Hz, 진폭 0.5 사인의 정상 최대 인접 차 ≈ 0.0288. 모든 테스트에서 `maxStep(out) < 0.06`.

- [ ] **Step 1: 실패 테스트 작성** (`ChainProcessorTests`, 48000 Hz / 블록 256)
  - `testEmptyChainPassesThrough`: 슬롯 없음 → 출력 == 입력(샘플 단위 정확히 일치).
  - `testInsertFadesInWithoutClick`: 10블록 후 `InvertProcessor` Insert, 이후 20블록 → `maxStep < 0.06`, 마지막 블록에서 `out ≈ -in`(오차 1e-5), `getNumActiveSlots()==1`.
  - `testBypassToggleWithoutClick`: Invert 삽입·안정 후 SetBypass(true) → `maxStep < 0.06`, 안정 후 `out ≈ in`; 다시 false → `out ≈ -in`.
  - `testRapidBypassToggleNoClick`: 매 블록 SetBypass 토글 50회 → `maxStep < 0.06`, 마지막 상태(짝수 번 → 원래)로 수렴.
- [ ] **Step 2:** 실행 → 컴파일/링크 실패(FAIL) 확인. `cmake --build build -j8 && ctest --test-dir build --output-on-failure`
- [ ] **Step 3: 구현** — `process` 순서: ① 큐의 명령 적용(Insert는 `wet`을 0에서 시작, 목표 `bypassed ? 0 : 1`) ② 슬롯마다: 입력을 미리 확보한 scratch에 복사 → 플러그인 처리 → scratch를 `dryDelay`로 `latencySamples`만큼 지연 → `out = wet·processed + (1−wet)·delayedDry`(샘플마다 `wet.getNextValue()`). 바이패스 중에도 플러그인은 처리한다. `LockFreeQueue`, `ChainSlot::prepare` 포함.
- [ ] **Step 4:** 실행 → 위 4개 PASS.
- [ ] **Step 5:** 커밋 `feat: 클릭 없는 체인 처리기(추가, 바이패스)`

---

### Task 3: ChainProcessor — 삭제, 순서 변경, 지연 변화 딥, 재준비, 한도

**Files:**
- Modify: `src/audio/ChainProcessor.h/.cpp`
- Test: `tests/ChainProcessorTests.cpp`

**Interfaces:**
- Consumes: Task 2 전부.
- Produces: 동작만 추가(시그니처 변경 없음).

**상태 규칙 (오디오 스레드):**
- Remove: 대상 `wet` 목표 0 → 0 도달 후 배열에서 빼서 반납 큐에 push.
- Move(slotId → index): 대상 `wet` 목표 0 → 0 도달 후 위치 이동 → 원래 목표(바이패스면 0, 아니면 1)로 복귀.
- 적용하면 `getTotalLatencySamples()`가 바뀌는 구조 변경(Insert/Remove 중 지연 있는 슬롯)은 출력 게인을 `kOutputDipSeconds` 동안 0으로 내리고, 0에서 적용한 뒤 다시 1로 올린다.
- 슬롯이 `kMaxSlots`개일 때의 Insert는 새 슬롯을 처리하지 않고 바로 반납 큐로 보낸다.
- 반납 큐가 가득 차면 다음 블록에 다시 시도한다(슬롯을 잃지 않는다).

- [ ] **Step 1: 실패 테스트 작성**
  - `testRemoveWithoutClickAndRetires`: Invert 삽입·안정 → Remove → `maxStep < 0.06`, 안정 후 `out ≈ in`, `popRetired()`가 그 슬롯(id 일치) 반환, 다음 호출 `nullptr`.
  - `testMoveWithoutClick`: `GainProcessor(0.5)` id1, `InvertProcessor` id2 삽입 → Move(id2 → 0) → `maxStep < 0.06`, 안정 후 `out ≈ -0.5·in`.
  - `testLatencyChangeUsesDipWithoutClick`: `FixedDelayProcessor(480)` 삽입 → `maxStep < 0.06`, `getTotalLatencySamples()==480`, 안정 후 `out[n] ≈ in[n-480]`; Remove 후 `maxStep < 0.06`, 지연 0.
  - `testSeventeenthInsertIsRetired`: 16개 `GainProcessor(1.0)` 삽입 후 17번째 → `getNumActiveSlots()==16`, `popRetired()`가 17번째 id 반환.
  - `testReprepareKeepsSlots`: Invert 삽입·안정 → `prepare(44100, 512)` → 44.1k/512로 `runSine` → `getNumActiveSlots()==1`, `out ≈ -in`.
- [ ] **Step 2:** 실행 → 새 테스트 FAIL 확인.
- [ ] **Step 3:** 위 상태 규칙 구현.
- [ ] **Step 4:** 실행 → `ChainProcessorTests` 전체 PASS.
- [ ] **Step 5:** 커밋 `feat: 체인 삭제/순서 변경/지연 변화 시 클릭 방지`

---

### Task 4: AudioEngine — 장치, 모노→스테레오, 미터

**Files:**
- Create: `src/audio/AudioEngine.h/.cpp`, `src/audio/LevelMeterSource.h`, `tests/AudioEngineTests.cpp`

**Interfaces:**
- Consumes: `ChainProcessor`.
- Produces:
  ```cpp
  class LevelMeterSource {             // 오디오 스레드가 쓰고 UI가 읽음
  public: void pushPeak (float);        // 현재 값보다 크면 갱신(std::atomic<float>)
          float readAndReset();         // UI 타이머용: 값을 읽고 0으로
  };
  class AudioEngine : public juce::AudioIODeviceCallback {
  public:
      juce::String initialise();        // 기본 장치 열기(입력 1, 출력 2). 실패 시 한국어 오류 문자열, 성공 시 빈 문자열
      void shutdown();
      juce::AudioDeviceManager& getDeviceManager();
      ChainProcessor& getChain();
      void prepareForTest (double sampleRate, int blockSize);   // aboutToStart와 같은 준비(장치 없이)
      void setInputChannel (int index); // 기본 0, std::atomic<int>
      LevelMeterSource& getInputMeter();
      LevelMeterSource& getOutputMeter();
      // AudioIODeviceCallback: audioDeviceIOCallbackWithContext / audioDeviceAboutToStart / audioDeviceStopped
  };
  ```
- `audioDeviceAboutToStart`: 스테레오 작업 버퍼 확보, `chain.prepare(sr, blockSize)`.
- IO 콜백: 선택 입력 채널을 작업 버퍼 L/R에 복사(채널이 없거나 범위 밖이면 0으로 채움) → 입력 피크 → `chain.process` → 출력 피크 → 출력 채널 0,1에 복사, 나머지 출력 채널은 0.

- [ ] **Step 1: 실패 테스트 작성** (장치 없이 콜백을 직접 호출. `audioDeviceAboutToStart`에는 `nullptr` 대신 테스트용 최소 `juce::AudioIODevice` 스텁을 넘기거나, 엔진에 `prepareForTest(double sr, int block)`를 둔다 — 후자를 택하고 `aboutToStart`도 이를 호출)
  - `testMonoInputCopiedToBothOutputs`: 입력 2채널(채널1에 사인, 채널0 무음), `setInputChannel(1)`, 출력 2채널 → 출력 L == R == 입력 채널1.
  - `testInputChannelOutOfRangeGivesSilence`: 입력 1채널, `setInputChannel(3)` → 출력 전부 0, 크래시 없음.
  - `testMetersReportPeaks`: 진폭 0.5 사인 → `getInputMeter().readAndReset()`≈0.5(±0.01), 두 번째 호출 0.
- [ ] **Step 2:** 실행 → FAIL.
- [ ] **Step 3:** 구현 (`initialise`는 `deviceManager.initialiseWithDefaultDevices(1, 2)` + `addAudioCallback(this)`).
- [ ] **Step 4:** 실행 → PASS.
- [ ] **Step 5:** 커밋 `feat: 오디오 엔진(장치, 모노→스테레오, 미터)`

---

### Task 5: PluginChain(UI 쪽 모델)과 PluginScanner

**Files:**
- Create: `src/plugins/PluginChain.h/.cpp`, `src/plugins/PluginScanner.h/.cpp`, `tests/PluginChainTests.cpp`

**Interfaces:**
- Consumes: `ChainProcessor`, `ChainSlot`, `ChainCommand`.
- Produces:
  ```cpp
  struct ChainEntry { int id; juce::String name; bool bypassed; juce::AudioProcessor* processor; };  // processor는 표시/에디터용 비소유 포인터

  class PluginChain : private juce::Timer {           // 메시지 스레드 전용
  public:
      PluginChain (ChainProcessor&, juce::AudioPluginFormatManager&);
      void setProcessingSpec (double sampleRate, int maxBlockSize);
      bool addProcessor (std::unique_ptr<juce::AudioProcessor>, const juce::String& name);   // 스테레오 불가/가득 참/큐 실패 시 false
      void addPlugin (const juce::PluginDescription&);   // createPluginInstanceAsync → addProcessor
      bool remove (int id);  bool move (int id, int newIndex);  bool setBypassed (int id, bool);
      const std::vector<ChainEntry>& getEntries() const;
      std::function<void()> onChanged;                       // 모델 변경 시
      std::function<void (const juce::String&)> onError;    // 한국어 메시지
      void pumpRetired();                                    // 반납 큐 비우기(타이머가 호출, 테스트도 직접 호출)
  };

  class PluginScanner {
  public:
      PluginScanner (juce::AudioPluginFormatManager&, juce::File cacheFile);
      void loadCache();                                      // 없으면 빈 목록
      void startScan (std::function<void()> onFinished);    // 백그라운드 스레드에서 VST3 기본 경로 스캔 → 캐시 저장 → 메시지 스레드에서 콜백
      bool isScanning() const;
      juce::KnownPluginList& getKnownPlugins();
      static juce::File defaultCacheFile();                  // userApplicationDataDirectory/DAW/plugins.xml
  };
  ```
- `addProcessor`: 스테레오 in/out 레이아웃 설정(`setBusesLayout`) 실패 시 `onError("이 플러그인은 스테레오를 지원하지 않아 추가할 수 없습니다: " + name)` 후 false. 성공 시 `ChainSlot` 생성·`prepare` → `post(Insert)`; `post` 실패 시 슬롯 삭제하고 false, 모델 불변.
- 타이머(30 Hz)로 `popRetired()`를 비워 delete. `remove`는 모델에서 즉시 빼고, 에디터 창 닫기 신호는 UI가 `onChanged`에서 처리.
- `setProcessingSpec`는 `AudioEngine`의 `aboutToStart` 값과 같게 `MainComponent`가 맞춘다.

- [ ] **Step 1: 실패 테스트 작성** (`ChainProcessor`를 직접 만들어 `prepare(48000,256)`, 오디오 콜백 없이 `process`를 수동 호출해 명령 소비)
  - `testAddRemoveMoveUpdatesModel`: Invert, Gain 추가 → `getEntries()` 이름 순서 일치 → `move` → 순서 반영 → `remove` → 1개.
  - `testRejectsProcessorWithoutStereoLayout`: `MonoOnlyProcessor` → `addProcessor`가 false, `onError` 1회 호출, 엔트리 0.
  - `testPostFailureLeavesModelUnchanged`: `process`를 호출하지 않은 채 바이패스를 큐 용량(64)보다 많이 토글 → 실패한 호출은 false, 그때 모델의 `bypassed`는 직전 성공 값과 같음.
  - `testRetiredSlotsAreDeleted`: remove 후 `process` 여러 번 + 타이머 콜백 강제 호출(테스트용 `pumpRetired()` 공개 메서드) → 반납 큐 비어 있음, 삭제된 프로세서 소멸자 호출 확인(테스트 프로세서의 static 카운터).
- [ ] **Step 2:** 실행 → FAIL.
- [ ] **Step 3:** 구현. `PluginScanner`는 `juce::PluginDirectoryScanner` + `VST3PluginFormat::getDefaultLocationsToSearch()`, dead-mans-pedal 파일은 캐시 옆 `scan-pedal.txt`. 스캐너는 단위 테스트 없음(VST3 필요).
- [ ] **Step 4:** 실행 → PASS.
- [ ] **Step 5:** 커밋 `feat: 플러그인 체인 모델과 VST3 스캐너`

---

### Task 6: 화면 — 체인 목록, 미터, 편집 창, 오디오 설정, 한국어 폰트

**Files:**
- Create: `src/ui/ChainListComponent.h/.cpp`, `src/ui/LevelMeter.h/.cpp`, `src/ui/KoreanLookAndFeel.h`, `src/plugins/PluginEditorWindow.h/.cpp`
- Modify: `src/MainComponent.h/.cpp`, `src/Main.cpp`

**Interfaces:**
- Consumes: `AudioEngine`, `PluginChain`, `PluginScanner`, `LevelMeterSource`.
- `MainComponent`가 `AudioEngine`, `AudioPluginFormatManager`(VST3만 `addFormat`), `PluginScanner`, `PluginChain`을 소유. 시작 시 `engine.initialise()` 오류면 화면 상단에 한국어 안내 문구 표시(앱은 계속 열림), `scanner.loadCache()`.
- 레이아웃: design.md 5.3 그림대로. 버튼 문구: `오디오 설정`, `+ 플러그인 추가`, `플러그인 다시 스캔`(스캔 중엔 `스캔 중…` 비활성), 행: `켜짐/꺼짐` 토글, `편집`, `↑`, `↓`, `삭제`. 미터 라벨 `입력`, `출력`.
- `오디오 설정`: `AudioDeviceSelectorComponent`(입력 0–N, 출력 2) 를 `DialogWindow::LaunchOptions`로. 입력 채널 선택 결과를 `engine.setInputChannel`에 반영(장치 변경 리스너에서 활성 입력 채널 중 첫 번째).
- `+ 플러그인 추가`: `KnownPluginList`를 `PopupMenu`로(목록이 비면 "스캔된 플러그인이 없습니다. 먼저 스캔하세요." 비활성 항목).
- `PluginEditorWindow : juce::DocumentWindow`: 제목 = 플러그인 이름, 닫기 버튼은 창만 숨김/삭제. `MainComponent`가 id→창 맵으로 관리하고 `onChanged`에서 사라진 id의 창 삭제.
- `LevelMeter`: 30 Hz 타이머로 `readAndReset()`, 감쇠 표시, 0 dBFS 근처 빨강.
- `KoreanLookAndFeel`: Global Constraints의 OS별 폰트를 `setDefaultSansSerifTypefaceName`.

- [ ] **Step 1:** 구현.
- [ ] **Step 2:** 빌드 + 기존 테스트 회귀 없음: `cmake --build build -j8 && ctest --test-dir build --output-on-failure` → 전부 PASS, `build/DAW_artefacts/…/DAW` 실행 파일 존재.
- [ ] **Step 3:** 실행 검증은 이 머신에서 불가 → 사용자 확인 체크리스트를 `README.md`의 "수동 확인" 절에 추가(창 표시, 장치 선택, 통과음+미터, 스캔, 추가/편집/바이패스/순서/삭제 중 클릭 없음).
- [ ] **Step 4:** 커밋 `feat: 기본 화면(체인 목록, 미터, 편집 창, 오디오 설정)`

---

### Task 7: 마무리 검증

- [ ] **Step 1:** 깨끗한 빌드: `rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8 && ctest --test-dir build --output-on-failure` → PASS.
- [ ] **Step 2:** 오디오 스레드 규칙 점검: `ChainProcessor::process`, `AudioEngine` IO 콜백 경로에 `new`, `delete`, `std::vector::push_back/resize`, `juce::String`, `DBG`, 잠금이 없는지 grep + 리뷰.
- [ ] **Step 3:** GitHub 저장소 생성·push는 사용자 확인 후. 확인 전에는 "CI는 구성됨, 원격 실행은 미검증"으로 보고.
