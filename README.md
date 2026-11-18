# DAW - 방송용 마이크 이펙트

라이브 방송용 마이크 입력에 VST3 이펙트 체인을 걸어 주는 JUCE(C++) 데스크톱 앱입니다.
선택한 입력 채널 1개를 스테레오로 복제한 뒤 VST3 이펙트 체인을 거쳐 스테레오로 출력합니다.
자세한 설계는 `docs/design.md`를 참고하세요.

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

## ASIO 옵션

`DAW_ENABLE_ASIO`(기본 OFF)를 켜면 `JUCE_ASIO=1`로 빌드합니다. Steinberg ASIO SDK 경로가 필요합니다.

```powershell
cmake -B build -DDAW_ENABLE_ASIO=ON -DDAW_ASIO_SDK_DIR=C:/path/to/asiosdk
```

## 검증 상태

로컬 머신에서의 앱 실행과 VST3 플러그인 동작은 미검증입니다. 현재는 빌드와 `ctest`까지만 확인했습니다.
