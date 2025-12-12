## Build & Flash Guide

### Prerequisites
- Python 3 (Tkinter 포함), `pip install pyserial`
- 빌드 위치: `firmware/` 디렉터리
- 기본 플래시 용량: 2MB Pico (다르면 `PICO_FLASH_SIZE_BYTES` 수정)

### 1) Bootloader 빌드 (오프셋 0)
```sh
cmake -S . -B build_boot \
  -DBUILD_BOOTLOADER=ON -DBUILD_FIRMWARE=OFF \
  -DPICO_FLASH_SIZE_BYTES=2097152 -DPICO_FLASH_TARGET_OFFSET=0
cmake --build build_boot --target bootloader
```

### 2) Firmware 빌드 (오프셋 0x20000)
```sh
cmake -S . -B build_fw \
  -DBUILD_BOOTLOADER=OFF -DBUILD_FIRMWARE=ON \
  -DPICO_FLASH_SIZE_BYTES=2097152 -DPICO_FLASH_TARGET_OFFSET=0x20000
cmake --build build_fw --target firmware
```

### 3) 부트로더 UF2 플래시
- 보드를 BOOTSEL 모드로 연결 후 `build_boot/bootloader.uf2`를 RPI-RP2 드라이브에 복사.

### 4) GUI 실행
```sh
python3 ../tools/bootloader_gui.py
```

### 5) 펌웨어 플래시 절차
- 포트가 안 보이면 Refresh 후 선택.
- 펌웨어 파일은 `build_fw/firmware.bin` 선택.
- Flash 버튼 클릭 → 필요 시 자동으로 BOOTLOADER 전환 후 다운로드 진행.

---

## Flash Layout (2MB 기준)

```
Flash offset (XIP addr)                Size/설명
0x000000 (0x10000000)  boot2 (256B)
0x000100 (0x10000100)  ┐
...                    ├ bootloader (128KB, 끝 = 0x01FFFF)
0x01FFFF               ┘
0x020000 (0x10020000)  ┐
...                    │ Main slot payload (0x0EFF00 bytes)
0x10EFF00 (0x1010EF00) │ Main slot header (마지막 256B)
0x10EFFFF              ┘
0x110000 (0x10110000)  ┐
...                    │ Staging slot payload (0x0EFF00 bytes)
0x1FEFF00 (0x101FEF00) │ Staging slot header (마지막 256B)
0x1FEFFFF              ┘
```
- 슬롯 크기: `(2MB - 0x20000) / 2 = 0x0F0000`
- 헤더: 각 슬롯 끝의 마지막 256B(boot2 크기와 동일한 페이지)에 저장.
- 펌웨어 벡터 테이블은 main payload 시작+0x100 (boot2 뒤) = 0x10020100.

## Bootloader 동작 로직
- USB CDC로 `MAGIC`을 주기적으로 송신, 핸드셰이크 후 아래 명령 처리:
  - `WRITE <off> <len>`: 256B 페이지 단위로 램에 기록.
  - `DONE <len> <crc>`: 스테이징 삭제 → 램에 기록된 내용을 스테이징에 기록 → 스테이징 헤더 기록 → CRC 확인 → 리셋.
- 부팅 타임아웃(2초) 후 자동 행동:
  1. 스테이징 유효/CRC OK면 메인으로 복사 후 스테이징 삭제, 리셋.
  2. 메인이 유효하면 바로 점프.
  3. 둘 다 없으면 리셋 재시도.
- 점프 시 USB 블록을 리셋해 메인 펌웨어가 깨끗하게 재인식되도록 처리.
