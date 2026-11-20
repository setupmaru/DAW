# 다음 단계 후보 목록 (뼈대 작업 리뷰에서 넘어온 항목)

뼈대 구현(2026-11-18~19) 과정의 코드 리뷰에서 "다음 단계로 미룸"으로 분류된 항목입니다. 원문(영어) 그대로 둡니다.

## 2026-11-19 백엔드 후속 수정

- 장치가 없거나 정지했을 때는 `ChainProcessor::setProcessingActive(false)`로 명령과 진행 중인 페이드를 즉시 마무리한다. 플러그인을 실행하지 않으며, 제거한 인스턴스는 `popRetired()`를 통해 메시지 스레드에서 해제해 편집 창의 수명을 보장한다. 정지 중 반납 목록은 별도로 보관하므로 고정 큐 용량을 넘는 연속 편집도 가능하다.
- `AudioEngine`이 시작/정지 상태를 전달한다. `prepare()`와 시작 콜백은 메시지 스레드 전용이다. 드라이버 스레드에서도 오는 `setProcessingActive(false)`(정지/오류)를 `post/popRetired`와 제어용 mutex로 직렬화한다. `process()`와 IO 콜백은 이 mutex를 사용하지 않는다.
- `PluginChain`은 `getProcessingSpec()`으로 엔진의 현재 설정을 읽는다. 준비 도중 장치가 바뀌는 경우도 `post(Insert)`에서 재확인한다. 기존 `setProcessingSpec()`은 엔진 최초 준비 전의 기본값으로 유지해 현재 프론트 호출과 호환된다.
- 회귀 테스트: 장치 없이 256회 바이패스, 정지 중 128회 추가/삭제와 인스턴스 수명, 가득 찬 큐의 지연된 Insert를 포함한 정지/재시작, 드라이버 정지와 메시지 스레드 명령 전송의 직렬화, 준비 도중 장치 설정 변경, 체인 전후 미터 값.
- 검증 명령: `cmake --build build -j2`, `ctest --test-dir build --output-on-failure`, `cmake --build build/release -j2`, `ctest --test-dir build/release --output-on-failure`.
- 검증 결과: Debug/Release 앱과 테스트 빌드 성공, 각각 테스트 46개 항목 통과(`ctest`: 1/1 성공). 실제 오디오 장치와 VST3 플러그인으로 듣는 검증은 미실시.
- 수정 범위는 `src/audio`, `src/plugins/PluginChain.*`, 해당 테스트와 이 기록이다. 프론트 파일은 수정하지 않았다. 실행 중 플러그인 지연 변경 감지와 스캐너 관련 항목은 아래에 남아 있다.

## 2026-11-20 인터페이스 구현 및 리뷰 수정

- 출력 음소거: 원자적 목표 상태를 체인 처리 뒤 10ms 선형 페이드로 적용한다. 시작 전에 지정한 상태는 첫 샘플부터 적용하고, 출력 미터는 실제 출력 채널의 음량을 읽는다. 음소거 중에도 플러그인을 실행한다.
- 체인 저장/복원: `DAWChain version="1"`, 플러그인 설명·바이패스·표준 base64 상태를 저장한다. 전체 XML을 검증한 뒤 기존 항목을 Remove하고 순서대로 비동기 생성한다. 실패 항목과 16개 초과분은 결과에 담으며, 큐 포화는 재시도한다. 복원 중 편집과 이전 비동기 추가의 뒤늦은 삽입을 막는다.
- 리뷰 Important 1/2: `prepare()`에 메시지 스레드 단언을 추가했다. `post(Insert)`의 재준비는 잠금 밖에서 수행한 뒤 설정을 재확인한다. `prepare()` 중 플러그인 알림은 비동기로 미뤄야 한다는 계약을 명시했다.
- 리뷰 Minor 3~5: 장치 오류도 정지 처리를 수행하고 준비 블록 크기는 atomic으로 보관한다. 정지/명령 경합, 비가환 프로세서의 순서, Move 페이드/awaitingDip 중 정지, 재시작 샘플, 정지 중 편집 후 17번째 삽입, 음소거 전환, 상태 왕복·실패·파괴·큐 포화를 회귀 테스트한다.
- 검증: Debug/Release 앱과 테스트 빌드 성공, 각각 59개 항목 통과(`ctest` 1/1), `git diff --check` 통과. 실제 오디오 장치 및 VST3로 듣는 검증은 별도이다. 프론트 파일은 수정하지 않았다.

## 리뷰에서 미룬 작은 문제들
- Task 1: minor (deferred): CI cache key uses env JUCE_TAG while CMake hardcodes tag — can drift.
- Task 1: minor (deferred): caching build/_deps also caches juce-build artifacts.
- Task 1: minor (deferred): test runner exits 0 when zero tests run.
- Task 1: minor (deferred): SmokeTest in separate file; .superpowers/ in .gitignore (fine).
- Task 1: minor (deferred): /utf-8 genex matches MSVC only, not clang-cl.
- Task 2: minor (deferred): mono plugin only affects L (T5 enforces stereo layout).
- Task 2: minor (deferred): MIDI buffer fixed 2048 bytes; could realloc on audio thread for MIDI-heavy plugins.
- Task 2: minor (deferred): suspendProcessing()/isSuspended() not honoured.
- Task 2: minor (deferred): test position map static, never cleared.
- [해결: 위 백엔드 후속 수정] Task 3: minor (deferred): prepare() is the queue consumer during re-prepare — T4 must call it only on message thread with no concurrent post().
- Task 3: minor (deferred): re-prepare restarts output hard after total latency (device restart; small).
- Task 3: minor (deferred): process() returns early for <2 channels, freezing fades (AudioEngine always passes 2).
- Task 3: minor (deferred): deferred full-chain Insert blocks later commands until retired drains.
- Task 3: minor (deferred): interleaving test gaps (SetBypass during Move, Move during Remove, Remove while fading in / awaitingDip, overlapping latency changes, unknown ids).
- Task 3: minor (deferred): SetBypass on a removing slot is ignored (T5 must not expect it).
- [해결: 위 백엔드 후속 수정] Task 4: minor (deferred): no test proves chain.process is called / metering order around chain (all tests use empty chain).
- Task 4: minor (deferred): AudioDeviceManager member makes tests log ALSA seq error + JUCE MIDI jassert (Debug).
- Task 4: minor (deferred): out-of-range test checks ch0 only; negative/0-input/unprepared paths untested.
- [해결: 위 백엔드 후속 수정] Task 4: minor (deferred): header thread contract — prepareForTest only while no callback registered; aboutToStart may come from a driver thread on some backends → race with message-thread post() — final review should weigh.
- Task 4: minor (deferred): block size 0 → 1-sample chunks; no ScopedNoDenormals.
- Task 5: minor (deferred): stereo layout no fallback (disable aux buses on retry); instruments get "no stereo" msg.
- Task 5: minor (deferred): cache written only after full scan.
- Task 5: minor (deferred): failed scan files not surfaced.
- Task 5: minor (deferred): onError spam on full queue — UI should rate-limit.
- Task 5: minor (deferred): defensive ghost path index skew (only if model bypassed).
- Task 5: minor (deferred): loadCache during scan guarded only by jassert.
- Task 5: minor (deferred): hanging plugin freezes UI during scan (accepted ruling).
- Task 4: minor (deferred): -Wfloat-equal warnings in tests/AudioEngineTests.cpp (test output not pristine).
- [해결: 위 백엔드 후속 수정] Task 6: minor (deferred): plugin added during async spec-sync gap prepared at old SR — fix: PluginChain reads ChainProcessor prepared SR/block getters.
- Task 6: minor (deferred): notice strip appearing/disappearing shifts rows (mis-click risk).
- Task 6: minor (deferred): specific engine error replaced by generic hint after a timed notice.
- Task 6: minor (deferred): selector allows multiple inputs; engine uses first — set maxAudioInputChannels=1 (matches global constraint "선택한 입력 채널 1개").
- Task 6: minor (deferred): "Not modal" comment wrong for launchAsync; bypass button tooltip; redundant repaint.

## 최종 리뷰에서 다음 단계로 분류된 항목
- [해결: 위 백엔드 후속 수정] With no audio device open, the command queue is never drained; after 64 edits every click shows "잠시 후 다시 시도하세요".
- Plugins that change latency at runtime are not tracked (dry path misaligns, total latency stale) — needs audioProcessorChanged listener + re-prepare. 리스너는 `AsyncUpdater` 등으로 메시지 스레드에 미뤄야 한다. 플러그인 콜백에서 `ChainProcessor`를 동기 호출하면 `prepare()`의 비재귀 mutex와 교착할 수 있다.
- Mic goes straight to output at launch (README warns to use headphones); monitoring policy is an open question (design.md §7).
- JUCE silently falls back to default devices when the interface is unplugged — show a notice when the device name changes.
- "다시 스캔" never removes uninstalled plugins; saveCache ignores write failure.

## 내린 결정 (되돌릴 수 있음)
- Ruling: git repo already initialised on `main` by controller; T1 skips `git init` — needed for SDD workspace — cost if wrong: none.
- Ruling: LockFreeQueue must hold exactly `Capacity` items (allocate Capacity+1 in AbstractFifo) — T5 test relies on 64 — cost: off-by-one in test.
- Ruling: PluginScanner.cpp and PluginEditorWindow.cpp are app-only sources (not DAW_CORE_SOURCES) — scanner has no unit test — cost: none.
- 2026-11-19: controller session was interrupted during Task 1; implementer resumed. Build thrashed (7 GB RAM, -j8). Ruling: all builds use `-j2` on this machine — JUCE TUs ~1 GB each — cost if wrong: slower builds only.
- Ruling: add MSVC `/utf-8` in daw_configure_target now (reviewer minor #5) — Tasks 5–6 add Korean literals and Windows CI must compile them correctly — cost if wrong: one compile flag.
- Ruling: T3 makes click detection phase-independent and proves by mutation that a hard switch fails by a wide margin — current margin 0.071 vs 0.06 is too thin to trust — cost: test-only change.
- Ruling: Insert honours cmd.bypassed (sets slot->bypassed) — avoids silent mismatch for PluginChain in T5 — cost: one line.
- Ruling: silent hold of (affected latency) during latency-change dip accepted — spec requires no click; outputting an empty delay line would be a dropout click anyway — cost if wrong: longer gap (~latency) on air when adding/removing high-latency plugins; revisit in next phase.
- Ruling (Important 2, plan-mandated): live limit kMaxSlots counts only slots not pending Remove; physical slot array gets headroom 2*kMaxSlots; if physically full the Insert stays deferred (retry next block), never retired while model shows room — spec: model and audio must agree; retiring a slot the model still lists is a UAF — cost: 16 extra pointers.
- Ruling: T5 PluginChain must also defensively drop any model entry whose slot comes back via popRetired before the model removed it (onError) — cost: few lines in T5.
- Ruling (Important 1, plan-mandated): scan incrementally on the message thread via juce::Timer (one file per tick; JUCE PluginListComponent default) instead of a background thread — spec intent "UI가 멈추지 않도록" kept (UI hitches only while one slow plugin is scanned), race eliminated; spec literal "백그라운드" relaxed — cost if wrong: brief UI hitch per slow plugin during rescans; audio thread unaffected.
- Ruling: final fix wave = Critical + 2 Important + runner fails on zero tests + ScopedNoDenormals + -Wfloat-equal test warnings + README (headphones warning, exe path, Windows prereqs, checklist additions) + .gitignore (.vs/, out/, CMakeUserPresets.json) + launchAsync comment + filter instruments from add menu; all other minors next phase — cheap, user-facing on air or required for pristine Release CI — cost: none significant.
- Ruling (plan defect): verify both Debug and Release ctest from now on — Release+LTO exposed a test-ordering bug Debug hid.
