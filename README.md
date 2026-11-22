# DAW - 멀티트랙 녹음·편집 프로그램

Pro Tools처럼 여러 오디오 트랙을 녹음하고, 타임라인에서 편집하고, 믹서에서 VST3 플러그인을 걸어 섞은 뒤 WAV로 내보내는 JUCE(C++) 데스크톱 DAW입니다.
자세한 설계와 단축키는 `docs/design.md`를 참고하세요. 이전 버전(v1, 방송용 마이크 이펙트)은 `v1.0.1` 태그에 있습니다.

> **녹음할 때는 헤드폰을 쓰세요.** 녹음 준비(R)한 트랙은 입력 소리를 바로 들려주므로, 스피커로 들으면 하울링이 날 수 있습니다.

## 주요 기능

- **트랙:** 오디오 트랙 최대 64개. 트랙마다 입력(모노 또는 스테레오 쌍), 녹음 준비(R), 입력 모니터(I), 솔로(S), 음소거(M)
- **트랜스포트:** 재생/정지(Space), 녹음(Ctrl+Space), 처음으로(Enter), 선택 구간 반복(Ctrl+L), 메트로놈, 템포
- **녹음:** 준비한 트랙마다 24비트 WAV로 녹음하고, 장치 지연만큼 자동으로 맞춰 놓습니다
- **편집 창:** 마디|박·분:초 눈금자, 파형, 확대/축소(T/R)
  - 도구: 트림(F6) / 선택(F7) / 잡기(F8), 모드: 슬립(F2) / 그리드(F4)
  - 클립 이동(트랙 사이 포함), Alt+끌기 복사, 끝 트림, 페이드(클립 위쪽 모서리의 네모), 분할(Ctrl+E), 복제(Ctrl+D), 삭제
  - 실행 취소/다시 실행(Ctrl+Z / Ctrl+Shift+Z)
- **믹스 창(Ctrl+=):** 트랙마다 VST3 인서트, 입력, 팬, 페이더, 스테레오 미터, 마스터 채널. 재생 중에 플러그인을 넣고 빼도 소리가 튀지 않습니다
- **플러그인 지연 보정:** 지연이 있는 플러그인을 건 트랙도 다른 트랙과 박자가 맞습니다
- **세션:** `.dawsession` 파일과 `Audio Files` 폴더로 저장/열기, 오디오 파일 가져오기(끌어다 놓기 가능)
- **믹스다운 내보내기(Ctrl+Alt+B):** 전체 또는 선택 구간을 WAV로 빠르게(오프라인) 렌더

## 내려받기 (Windows)

[Releases](https://github.com/setupmaru/DAW/releases)에서 받습니다.

- **설치 파일:** `DAW-Setup-v….exe`를 실행해 설치합니다(시작 메뉴 바로가기, 제거 프로그램 포함).
- **설치 없이:** `DAW-v…-windows-x64.zip`을 받아 압축을 풀고 `DAW.exe`를 실행합니다.

설치 파일은 `installer/DAW.iss`(Inno Setup 6)와 `installer/build-installer.ps1`로 만들고, CI의 Windows 빌드마다 함께 만들어 실행 페이지의 아티팩트로 올립니다.

## 릴리스 만들기

`v1.0.0`처럼 `v`로 시작하는 태그를 푸시하면 `.github/workflows/release.yml`이 Windows에서 빌드·테스트한 뒤 설치 파일과 zip을 릴리스에 올립니다.

```sh
git tag v1.0.0
git push origin v1.0.0
```

## Windows 준비물

- Visual Studio 2022 ("C++를 사용한 데스크톱 개발" 워크로드 포함)
- CMake 3.22 이상
- Git

## Linux 의존 패키지

(build-essential, cmake, pkg-config는 별도로 설치되어 있어야 합니다.)

```sh
sudo apt-get install -y libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libglu1-mesa-dev mesa-common-dev
```

## 빌드와 테스트

JUCE는 CMake `FetchContent`로 내려받으므로 첫 빌드는 오래 걸립니다. JUCE 소스 한 개를 컴파일하는 데 메모리가 약 1GB 필요하므로, RAM이 작은 머신(8GB 이하)에서는 `-j2`처럼 병렬도를 낮추세요(`cmake --build build -j2`).

Linux:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Windows (Visual Studio 생성기):

```powershell
cmake -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

## 실행 파일 위치

빌드가 끝나면 앱은 `build/DAW_artefacts/<Config>/DAW`(Windows는 `DAW.exe`)에 생깁니다.
`<Config>`는 빌드 구성 이름입니다(예: `Release`, `Debug`).

## ASIO

Windows 빌드에는 ASIO가 기본으로 들어갑니다(`DAW_ENABLE_ASIO`, Windows에서 기본 ON).
JUCE에 포함된 ASIO SDK 헤더를 쓰므로 따로 받을 것은 없습니다.

- **쓰는 법:** [오디오 설정] → 장치 종류에서 `ASIO`를 고르고, 오디오 인터페이스의 ASIO 드라이버를 고릅니다. 지연이 가장 짧습니다. `ASIO` 항목은 ASIO 드라이버가 설치돼 있을 때만 보입니다(오디오 인터페이스 제조사 드라이버를 설치하세요).
- **기억:** 고른 장치(종류, 장치, 샘플레이트, 버퍼)는 다음 실행 때도 그대로 열립니다. 그 장치를 열 수 없으면(인터페이스가 빠짐 등) Windows 오디오의 기본 장치로 열고 위쪽에 알려 줍니다. 저장해 둔 선택은 그대로 남아, 인터페이스를 다시 꽂고 실행하면 다시 그 장치로 열립니다.
- **주의:** 대부분의 ASIO 드라이버는 한 번에 한 프로그램만 쓸 수 있습니다. ASIO가 열려 있는 동안 다른 프로그램의 소리가 끊기거나, 다른 프로그램이 이미 쓰고 있으면 ASIO가 안 열릴 수 있습니다. 그럴 때는 다른 프로그램을 닫거나 [오디오 설정]에서 Windows 오디오를 고르세요.
- **다른 SDK 사용:** 다른 ASIO SDK를 쓰려면 경로를 지정합니다.

```powershell
cmake -B build -DDAW_ASIO_SDK_DIR=C:/path/to/asiosdk
```

## 검증 상태

자동 테스트(`ctest`)로 재생 엔진, 녹음, 지연 보정, 세션 저장/열기, 믹스다운, 타임라인 마우스 편집을 확인합니다.
화면은 Linux 가상 화면(Xvfb)에서 그려 확인했습니다. 실제 오디오 인터페이스와 VST3 플러그인으로 듣는 확인은 아래 목록으로 사용자 PC에서 합니다.

```sh
# 개발용: 창을 그려 PNG로 저장하고 끝냅니다 (--mix: 믹스 창)
./build/DAW_artefacts/Release/DAW a.wav b.wav --screenshot edit.png
```

## 수동 확인

- [ ] **창과 한글:** 메뉴, 버튼, 상태 줄의 한글이 네모(□) 없이 보인다.
- [ ] **장치:** [설정] → [오디오 설정]에서 장치를 고르면 상태 줄에 장치 이름, 샘플레이트, 버퍼, 지연이 보인다.
- [ ] **가져오기와 재생:** WAV를 편집 창에 끌어다 놓으면 클립과 파형이 생기고, Space로 재생/정지된다. 재생 위치 선이 움직이고 화면이 따라간다.
- [ ] **녹음:** 트랙의 [R]을 켜고 입력을 고른 뒤 Ctrl+Space로 녹음, Space로 정지하면 새 클립이 생긴다. 다른 트랙을 재생하며 녹음한 소리가 박자에 맞게 놓인다.
- [ ] **편집:** 클립 이동·트랙 사이 이동·Alt 복사·트림·페이드·분할·삭제가 되고, Ctrl+Z로 되돌릴 수 있다.
- [ ] **믹서:** 믹스 창(Ctrl+=)에서 인서트 칸을 눌러 VST3를 넣고, 두 번 눌러 플러그인 창을 연다. 페이더·팬·S/M이 소리에 반영되고 클릭 소리가 나지 않는다.
- [ ] **반복 재생:** 선택 도구로 구간을 고르고 Ctrl+L을 켜면 그 구간을 반복한다.
- [ ] **저장/열기:** 저장한 세션을 닫고 다시 열면 트랙, 클립, 믹서, 플러그인 상태가 그대로다.
- [ ] **내보내기:** Ctrl+Alt+B로 내보낸 WAV가 들은 소리와 같다.
- [ ] **ASIO:** Windows에서 ASIO 장치로 재생·녹음이 된다.

## 라이선스

Copyright (C) 2026 setupmaru

이 프로그램은 [GNU Affero General Public License v3.0](LICENSE)(AGPLv3)으로 배포합니다.
자유 소프트웨어로서 AGPLv3 조건에 따라 다시 배포하거나 고칠 수 있으며, **아무런 보증 없이** 제공됩니다.
수정한 버전을 배포하거나 네트워크로 서비스할 때는 그 소스 코드도 AGPLv3로 공개해야 합니다.

이 프로그램은 [JUCE](https://juce.com)를 AGPLv3 조건으로 사용합니다. JUCE는 Raw Material Software Limited의 저작물입니다.

Windows 빌드는 JUCE에 포함된 Steinberg ASIO SDK 헤더를 사용합니다. 이 헤더는 Steinberg ASIO 라이선스 또는 GPLv3의 이중 라이선스이며, 이 프로젝트는 GPLv3 조건으로 사용합니다(릴리스 zip의 `THIRD-PARTY-ASIO-LICENSE.txt`).
ASIO is a trademark and software of Steinberg Media Technologies GmbH.
