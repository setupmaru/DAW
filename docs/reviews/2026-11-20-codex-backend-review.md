# Codex 백엔드 변경 리뷰 (2026-11-20)

대상: 커밋되지 않은 Codex 변경. 범위는 `src/audio/*`, `src/plugins/PluginChain.*`, `tests/*`, `docs/next-steps.md`이고 11개 파일, +405/−30줄이다.
판정: **수정 후 승인 (Approve with fixes)**. Critical은 없다.
검증:
- Debug와 Release 모두 앱과 테스트가 빌드된다.
- `ctest` 1/1 통과, 테스트 실행 파일 종료 코드 0.
- 리뷰는 코드 읽기로만 했다.

## Important

### 1. mutex가 필요한 이유가 잘못 적혀 있고, 그 설명대로 되면 교착(deadlock)이 생긴다

해당 위치: `ChainProcessor.cpp:63`(`prepare`가 `controlMutex`를 잡음), `ChainProcessor.h:48-50`, `AudioEngine.h:13-16`, `docs/next-steps.md` 항목.

- 주석은 `prepare`가 드라이버 스레드에서 올 수 있다고 한다. 하지만 JUCE의 VST3 `prepareToPlay`는 `JUCE_ASSERT_MESSAGE_THREAD` 뒤에 `MessageManagerLock`을 잡는다(`juce_VST3PluginFormatImpl.h:2295-2300`).
- 그래서 드라이버 스레드에서 `aboutToStart`가 오면 다음 순서로 교착이 생긴다.
  1. 드라이버 스레드가 `audioCallbackLock`을 잡는다.
  2. 같은 스레드가 `controlMutex`를 잡는다.
  3. VST3 `prepareToPlay`가 `MessageManagerLock`을 기다린다.
  4. 그동안 메시지 스레드는 `post()`, `popRetired`(30Hz) 또는 `getProcessingSpec`에서 `controlMutex`를 기다리고 있다.
- JUCE 8.0.17에서는 `aboutToStart`가 모든 백엔드에서 메시지 스레드로 온다. 그래서 지금은 실제로 일어나지 않는다.
- mutex 자체는 필요하다. JACK `shutdownCallback`이 JACK 스레드에서 `audioDeviceStopped`를 부르고, 그 경로가 이제 `settleStoppedChanges`를 실행한다.
- **수정할 것:**
  - mutex는 유지한다.
  - `prepare()`는 메시지 스레드 전용이라고 명시하고 `JUCE_ASSERT_MESSAGE_THREAD`(또는 `jassert`)를 넣는다.
  - 드라이버 스레드에서 올 수 있는 것은 `setProcessingActive(false)`뿐이라고 주석과 문서를 고친다.
  - `testDevicePrepareExcludesPosts`는 `std::thread`에서 `prepare`를 부르는데, 실제 플러그인으로는 지원되지 않는 경로다. 다른 스레드에서 부르는 `setProcessingActive(false)`와 `post()`가 직렬화되는지 확인하는 테스트로 바꾸거나 없앤다.

### 2. 플러그인의 `prepareToPlay`를 `std::mutex`를 잡은 채로 부른다 — 재진입하면 자기 자신과 교착

해당 위치: `ChainProcessor.cpp:111-120`(`post(Insert)` 안의 재준비), `prepare`.

- VST3 `prepareToPlay` → `setLatencySamples` → `audioProcessorChanged`가 동기로 불린다.
- 다음 단계 계획에 "실행 중 지연 변경 감지(audioProcessorChanged 리스너 + 재준비)"가 있다. 그 리스너가 `post`/`prepare`/`getProcessingSpec`/`popRetired`를 동기로 부르면, 이미 잡고 있는 mutex를 다시 잡게 된다. 이는 UB이고 사실상 멈춘다.
- **수정할 것 (둘 중 하나):**
  - ChainProcessor를 플러그인 콜백 안에서 부르지 말라고 문서화하고, 리스너는 `AsyncUpdater`로 미루게 한다.
  - 또는 `post()`에서 잠금을 잡은 채로 prepare하지 않는다. 잠금 안에서 spec을 읽고 → 풀고 → prepare → 다시 잡고 → 재확인하는 루프로 바꾼다.

## Minor

3. `audioDeviceError`가 와도 정지(stop)가 따라오지 않거나, `aboutToStart` 뒤 `start()`가 실패하면 IO 없이 `processingActive == true`가 남는다. 그러면 예전 "64번 편집 후 큐 가득 참" 증상이 다시 생길 수 있다. → `audioDeviceError`에서 `setProcessingActive(false)`를 부른다.
4. `AudioEngine::preparedBlockSize`가 평범한 `int`인데, 이제 `audioDeviceStopped`(JACK 스레드일 수 있음)에서 쓰인다. JUCE의 `audioCallbackLock` 덕분에 지금은 경합이 없지만 깨지기 쉽다. → 비용 없이 `std::atomic<int>`로 바꿀 수 있다.
5. 테스트 공백:
   - `testStopSettlesDeferredChanges`는 교환 가능한(gain/invert) 프로세서만 써서 순서를 증명하지 못한다.
   - 다음 경우가 테스트되지 않는다.
     - Move 페이드 도중 정지
     - `awaitingDip` 상태의 지연 슬롯이 있을 때 정지
     - 페이드 도중 정지 후 재시작할 때 `maxStep < 0.06`
     - 정지 중 편집을 여러 번 한 뒤 17번째 Insert
6. `popRetired`가 `stoppedRetired`를 LIFO로, `retired`보다 먼저 돌려준다. 이것은 무해하다. 벡터 용량이 최대 폭주 크기만큼 남지만 소멸자에서 해제된다.

## 확인 결과 문제없음

- **정지 모드 정리 루프:** 끝난다. 정리 후 실제 순서 = 모델 순서. 재시작할 때 페이드 상태가 남지 않는다.
- **데이터 경합:** `processingActive`, `stoppedRetired`, 준비된 spec은 모두 `controlMutex` 아래에서만 접근한다. `process()`는 읽지 않는다.
- **`post(Insert)` spec 재확인:** 잠금 안에서 해서 확인 시점과 사용 시점 사이에 틈이 없다.
- **오디오 스레드:** 잠금, 할당, 로그가 추가되지 않았다.
- **종료:** `shutdown()`의 이중 `audioDeviceStopped`는 여러 번 불려도 안전하다.
- **범위:** 프론트 파일은 건드리지 않았다.

## 화면 쪽 영향

- 필수 변경은 없다.
- `MainComponent::syncWithDevice`의 `chain.setProcessingSpec(...)` 호출은 이제 중복이다(제거해도 안전).
- 장치가 없을 때 편집이 바로 적용되므로, 장치 없음 상태에서 "잠시 후 다시 시도하세요" 오류는 더 이상 나오지 않는다.
- 앞으로 지연 감지 리스너를 만들 때 플러그인 콜백 안에서 ChainProcessor를 동기로 부르지 말 것(Important 2).
