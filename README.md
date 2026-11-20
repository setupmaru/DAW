# DAW - 방송용 마이크 이펙트

라이브 방송용 마이크 입력에 VST3 이펙트 체인을 걸어 주는 JUCE(C++) 데스크톱 앱입니다.
선택한 입력 채널 1개를 스테레오로 복제한 뒤 VST3 이펙트 체인을 거쳐 스테레오로 출력합니다.
자세한 설계는 `docs/design.md`를 참고하세요.

> **주의: 반드시 헤드폰을 쓰세요.** 앱을 켜면 마이크 소리가 바로 출력 장치로 나갑니다.
> 노트북 스피커처럼 마이크 가까이 있는 스피커로 들으면 하울링(삐- 소리)이 날 수 있습니다.

## 내려받기 (Windows)

[Releases](https://github.com/setupmaru/DAW/releases)에서 `DAW-…-windows-x64.zip`을 받아 압축을 풀고 `DAW.exe`를 실행하세요. 설치는 필요 없습니다.

## 릴리스 만들기

`v0.1.0`처럼 `v`로 시작하는 태그를 푸시하면 `.github/workflows/release.yml`이 Windows에서 빌드·테스트한 뒤 zip을 릴리스에 올립니다.

```sh
git tag v0.1.0
git push origin v0.1.0
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

## ASIO 옵션

`DAW_ENABLE_ASIO`(기본 OFF)를 켜면 `JUCE_ASIO=1`로 빌드합니다. Steinberg ASIO SDK 경로가 필요합니다.

```powershell
cmake -B build -DDAW_ENABLE_ASIO=ON -DDAW_ASIO_SDK_DIR=C:/path/to/asiosdk
```

## 검증 상태

로컬 머신에서의 앱 실행과 VST3 플러그인 동작은 미검증입니다. 현재는 빌드와 `ctest`까지만 확인했습니다.

## 수동 확인

개발 머신에는 화면과 VST3 플러그인이 없어서 아래 항목은 사용자 PC에서 직접 확인해야 합니다.
마이크와 헤드폰을 연결하고, 테스트용 VST3가 필요하면 무료 플러그인(예: Linux는 `lsp-plugins-vst3`)을 설치합니다.

- [ ] **창 표시:** 앱을 실행하면 창이 하나 뜨고, 한글(버튼, 미터 라벨 `입력`/`출력`)이 네모(□) 없이 보인다.
- [ ] **장치 선택:** `오디오 설정`을 누르면 장치 설정 창이 뜬다. 입력 장치, 입력 채널(1개), 출력 장치를 고를 수 있고, 창을 닫아도 앱은 그대로 동작한다.
- [ ] **장치 오류 안내:** 오디오 장치를 열 수 없거나 장치가 없으면 창 맨 위에 한국어 안내가 보이고 앱은 꺼지지 않는다. `오디오 설정`에서 장치를 고르면 안내가 사라진다.
- [ ] **통과음과 미터:** 플러그인이 없을 때 마이크 소리가 헤드폰 양쪽(L=R)으로 그대로 들리고, `입력`/`출력` 미터가 소리에 맞춰 움직인다. 크게 말하면 미터 오른쪽 끝이 빨갛게 켜진다.
- [ ] **입력 채널 변경:** `오디오 설정`에서 입력 채널은 1개만 켤 수 있다. 다른 입력 채널을 켜면 원래 켜져 있던 채널은 자동으로 꺼지고 새 채널의 소리가 들린다. 입력 채널을 모두 끄면 소리가 나지 않는다(앱은 꺼지지 않음).
- [ ] **스캔:** `플러그인 다시 스캔`을 누르면 버튼이 `스캔 중…`으로 바뀌고 눌리지 않는다. 끝나면 원래 문구로 돌아오고 위쪽에 찾은 개수가 잠시 보인다. 앱을 다시 켜도 스캔 결과가 남아 있다.
- [ ] **추가:** `+ 플러그인 추가`를 누르면 스캔된 플러그인 목록이 뜨고, 고르면 목록 끝에 행이 생긴다. 스캔 전에는 "스캔된 플러그인이 없습니다. 먼저 스캔하세요."가 회색으로 보인다.
- [ ] **편집:** `편집`을 누르면 플러그인 이름이 제목인 창이 뜬다. 창을 닫아도 플러그인은 목록에 남아 있고 소리도 계속 처리된다. 편집 화면이 없는 플러그인은 `편집`이 눌리지 않는다.
- [ ] **바이패스:** `켜짐`/`꺼짐`을 눌러 효과를 끄고 켤 때 소리가 바뀌고, 딸깍(클릭) 소리가 나지 않는다.
- [ ] **순서 변경:** `↑`/`↓`로 순서를 바꿀 때 번호가 바뀌고, 딸깍 소리가 나지 않는다.
- [ ] **삭제:** `삭제`를 누르면 행이 바로 사라지고, 열려 있던 그 플러그인의 편집 창도 닫힌다. 딸깍 소리가 나지 않는다.
- [ ] **샘플레이트/버퍼 크기 변경:** 플러그인을 넣은 상태에서 `오디오 설정`의 샘플레이트나 버퍼 크기를 바꿔도 체인 목록이 그대로 남아 있고, 소리도 계속 처리된다.
- [ ] **레이턴시 있는 플러그인:** 레이턴시가 있는 플러그인을 추가하거나 삭제하면 소리가 아주 잠깐 끊긴다(정상 동작).
- [ ] **최대 개수:** 플러그인을 16개 넣은 뒤 17번째를 추가하면 추가되지 않고 안내 메시지가 보인다.
- [ ] **종료:** 편집 창과 `오디오 설정` 창을 연 채로 앱을 닫아도 오류 없이 꺼진다.

## 라이선스

Copyright (C) 2026 setupmaru

이 프로그램은 [GNU Affero General Public License v3.0](LICENSE)(AGPLv3)으로 배포합니다.
자유 소프트웨어로서 AGPLv3 조건에 따라 다시 배포하거나 고칠 수 있으며, **아무런 보증 없이** 제공됩니다.
수정한 버전을 배포하거나 네트워크로 서비스할 때는 그 소스 코드도 AGPLv3로 공개해야 합니다.

이 프로그램은 [JUCE](https://juce.com)를 AGPLv3 조건으로 사용합니다. JUCE는 Raw Material Software Limited의 저작물입니다.
