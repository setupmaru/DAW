# 화면 ↔ 백엔드 인터페이스: 출력 음소거, 체인 저장/불러오기

작성일: 2026-11-20
상태: 백엔드 API 구현 완료. 화면 쪽은 이 문서의 시그니처대로 연결한다.

## 역할 분담 (사용자 결정, 2026-11-20)

- **백엔드 (Codex):** 아래 1, 2절의 함수와 그 테스트. `src/audio/*`, `src/plugins/PluginChain.*`, `tests/*`.
- **화면 (Claude):** 음소거 버튼, 음소거 상태 기억, 프리셋 메뉴(저장·불러오기·삭제), 프리셋 파일 관리, 종료 시 자동 저장과 실행 시 자동 복원. `src/MainComponent.*`, `src/ui/*`.

사용자가 고른 동작:
- 체인 저장: **자동 복원 + 이름 있는 프리셋**
- 실행 시 출력 상태: **마지막 상태 기억** (기억은 화면 쪽 설정 파일이 맡는다)

## 1. 출력 음소거 — `AudioEngine`

```cpp
// 메시지 스레드에서 호출. 오디오 스레드는 원자 변수만 읽는다.
void setOutputMuted (bool muted);
bool isOutputMuted() const;
```

요구 사항:
- **클릭 없음:** 켜고 끌 때 출력 게인을 약 10ms(`ChainProcessor::kOutputDipSeconds`와 같은 값) 선형 페이드. 오디오 스레드에서 할당/잠금/로그 금지.
- **적용 위치:** 체인 처리 뒤, 출력 채널에 쓰기 직전. 체인과 플러그인은 음소거 중에도 계속 처리한다(다시 켤 때 상태가 끊기지 않게).
- **미터:** 입력 미터는 그대로. **출력 미터는 실제로 나가는 소리**(음소거면 0으로 내려감)를 보여준다.
- **기본값:** 생성 직후 `isOutputMuted() == false`. 화면이 `initialise()` 전에 기억된 값을 `setOutputMuted`로 넣는다. `initialise()` 전 호출도 안전해야 하고, 장치가 시작될 때 페이드 없이 그 상태로 바로 시작한다(첫 블록부터 음소거면 0).
- **테스트:** 음소거 전환 시 `maxStep < 0.06`, 음소거 중 출력 전부 0, 출력 미터 0, 입력 미터는 값 유지, 시작 전 설정한 음소거가 첫 블록부터 적용.

## 2. 체인 상태 저장/복원 — `PluginChain`

```cpp
// 메시지 스레드. 현재 모델 순서 그대로 직렬화한다.
std::unique_ptr<juce::XmlElement> createStateXml() const;

struct RestoreResult
{
    int restored = 0;                 // 체인에 들어간 플러그인 수
    juce::StringArray failedNames;    // 만들지 못했거나 스테레오 불가 등으로 빠진 플러그인 이름
};

// 메시지 스레드. 현재 체인을 비우고(클릭 없는 Remove) xml의 플러그인을 xml 순서대로
// 다시 만든다. 비동기로 끝나면 onDone을 메시지 스레드에서 정확히 한 번 호출한다.
// PluginChain이 먼저 파괴되면 onDone은 호출되지 않는다.
void restoreFromXml (const juce::XmlElement& state, std::function<void (RestoreResult)> onDone);

bool isRestoring() const;   // 복원 중이면 true (화면은 이때 추가/삭제/이동 버튼을 막는다)
```

XML 형식:
- 루트 태그 `DAWChain`, 속성 `version="1"`.
- 플러그인마다 `<Plugin bypassed="0|1">` 자식 하나. 그 안에 `PluginDescription::createXml()`의 `<PLUGIN .../>`와 `<State>표준 base64</State>`를 저장한다. 빈 상태는 `<State/>`로 저장한다.
- `addPlugin`은 원래 설명을 보존한다. `addProcessor`로 직접 넣은 프로세서는 `Internal` 형식과 이름을 식별자로 기록한다. 이를 복원하려면 생성자의 선택적 `ProcessorFactory`를 제공한다. 팩토리 완료 콜백은 메시지 스레드에서 한 번 호출해야 하며 동기 완료도 허용한다.
- 복원은 한 번에 하나씩 생성해 XML 순서를 보장한다. 명령 큐가 가득 차면 타이머로 재시도한다. 기존 비동기 추가가 복원 이후 뒤늦게 완료되면 폐기한다.
- 복원 중 `setBypassed`와 중첩 복원도 거절한다. 중첩 복원 요청은 기존 복원을 유지하고 자체 `onDone({0, {}})` 및 `onError`를 비동기로 호출한다.
- 형식을 모르는 버전이거나 망가진 XML이면 체인을 건드리지 않고 `onDone({0, {}})` + `onError(한국어)`.

요구 사항:
- **순서:** 비동기 생성이 어떤 순서로 끝나든 최종 모델 순서 = XML 순서. 빠진 플러그인은 건너뛰고 나머지 순서는 유지.
- **상태:** 각 플러그인에 `setStateInformation`을 적용한 뒤 체인에 넣는다. 바이패스 상태도 복원.
- **16개 한도:** XML에 16개를 넘게 있으면 앞의 16개만, 나머지는 `failedNames`.
- **복원 중 편집:** `isRestoring()` 동안 `addProcessor`/`addPlugin`/`remove`/`move`는 false(또는 무시) + `onError` 없이 조용히 거절해도 된다. 화면이 버튼을 막는다.
- **onChanged:** 복원으로 모델이 바뀔 때마다 호출(편집 창 정리 규칙 유지: 엔트리가 빠지면 프로세서 삭제 전에 onChanged).
- **장치 없음/정지 중:** 기존 정지 모드 규칙대로 동작해야 한다(장치 없이 실행해도 복원 가능).
- **테스트:** VST3 없이 테스트 프로세서로 직렬화→복원 왕복(순서, 바이패스, 상태), 망가진 XML, 16개 초과, 복원 중 파괴(onDone 미호출, 누수 없음), 생성 실패 섞인 경우의 순서. 테스트용으로 프로세서 생성 경로를 주입할 수 있게 해도 된다(예: `PluginDescription` → 프로세서 팩토리).

## 3. 화면 쪽이 할 일 (참고, Codex 작업 아님)

- 설정 파일(`juce::PropertiesFile`, 앱 데이터 폴더 `DAW/settings.xml`)에 `outputMuted` 저장/복원.
- 프리셋 폴더: `userApplicationDataDirectory/DAW/presets/<이름>.xml`. 자동 복원용: `DAW/last-chain.xml`.
- 자동 저장: 체인이 바뀔 때(2초 지연 후) + 앱 종료 시. 실행 시 스캔 캐시를 읽은 뒤 `last-chain.xml`이 있으면 `restoreFromXml`.
- 화면: 아래 막대에 `출력 켜짐 / 음소거 중` 토글, 위 막대에 `프리셋 ▾` 메뉴(현재 체인 저장…, 프리셋 목록, 삭제).
