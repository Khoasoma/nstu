# Project NSTU

NSTU là dự án mã nguồn mở dành cho quản lý phòng máy và classroom lab trên
Windows. Mục tiêu là cung cấp server quản trị tập trung, client nhẹ chạy nền,
và kênh truyền màn hình độ trễ thấp từ một máy giáo viên tới nhiều máy học
sinh.

Repository: <https://github.com/Khoasoma/nstu>

NSTU hiện là engineering MVP dùng để phát triển và kiểm thử kiến trúc, chưa
phải bản triển khai production hoàn chỉnh cho trường học. Các giới hạn thực tế
được ghi trong [ROADMAP.md](docs/ROADMAP.md).

## Tính năng hiện tại

- Protocol nhị phân versioned, little-endian, có giới hạn kích thước và parser
  TCP incremental.
- TCP control channel với connection preamble, mutual HMAC handshake, replay
  protection, sequence guard và rate limiter cho kết nối chưa xác thực.
- `KeyStore` hỗ trợ enrollment, rotation, revocation, tombstone `key_id` và
  zeroization; DPAPI dùng để bảo vệ secret ở local machine.
- UDP multicast cho broadcast và policy fallback sang unicast.
- Packet-loss tracker có reorder window, duplicate/late/wrong-stream/jump
  handling. Xem [PACKET_LOSS.md](docs/PACKET_LOSS.md).
- Bounded video frame reassembly, deadline, duplicate/conflict detection và
  memory-pressure accounting. Xem [VIDEO_REASSEMBLY.md](docs/VIDEO_REASSEMBLY.md).
- DXGI Desktop Duplication, D3D11 BGRA-to-NV12 và Media Foundation H.264 MFT.
- Windows Service, active-session agent, secure named pipe, tray icon và
  fullscreen overlay.
- Server UI dùng Dear ImGui + D3D11 và client registry thread-safe.
- CMake modular, build được bằng MSVC hoặc MinGW-w64, có Windows CI.

## Kiến trúc thư mục

```text
nstu/
|-- common/                  Protocol, auth, TCP/UDP, IOCP, key, rate limit
|   |-- include/nstu/        Public C++ headers
|   `-- src/                 Implementation dùng chung
|-- video/                   DXGI, D3D11 converter, Media Foundation encoder
|-- client/
|   |-- service/             Windows Service entry point
|   |-- agent/               Interactive-session tray/overlay entry point
|   `-- src/                 Named pipe và session integration
|-- server/
|   |-- app/                 Server UI entry point
|   |-- include/nstu/        Client state registry headers
|   `-- src/                 Client registry implementation
|-- tests/                   Protocol, auth, packet-loss, reassembly, IPC tests
|-- docs/                    Architecture, security, packet-loss, roadmap
|-- cmake/                   Compiler warning policy
|-- CMakeLists.txt           Root build configuration
|-- CMakePresets.json        Windows Debug/Release presets
|-- LICENSE                  MIT license
`-- THIRD_PARTY_NOTICES.md   Dependency and license notices
```

## Yêu cầu cài đặt

- Windows 10 hoặc Windows 11 x64.
- Visual Studio 2022 với Desktop development with C++ và Windows SDK, hoặc
  MinGW-w64 hỗ trợ C++20.
- CMake 3.25 trở lên, Git.
- Ninja nếu dùng CMake presets.

Các API video và service là Windows-only. Build đầy đủ và integration tests cần
Windows.

## Clone và build

```powershell
git clone https://github.com/Khoasoma/nstu.git
cd nstu
```

### MSVC / Visual Studio

```powershell
cmake -S . -B build-msvc -A x64 -DNSTU_ENABLE_WERROR=ON
cmake --build build-msvc --config Release --parallel
ctest --test-dir build-msvc -C Release --output-on-failure
```

### MinGW-w64

```powershell
cmake -S . -B build-mingw -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DNSTU_ENABLE_WERROR=ON
cmake --build build-mingw
ctest --test-dir build-mingw --output-on-failure
```

### CMake presets

Preset yêu cầu Ninja và một toolchain C++20 phù hợp:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Release dùng `windows-release` thay cho `windows-debug`.

## Tùy chọn CMake

- `NSTU_BUILD_CLIENT=ON|OFF`: build service và agent client.
- `NSTU_BUILD_SERVER=ON|OFF`: build server UI.
- `NSTU_BUILD_VIDEO=ON|OFF`: build DXGI/Media Foundation trên Windows.
- `NSTU_BUILD_TESTS=ON|OFF`: build test suite.
- `NSTU_SERVER_USE_IMGUI=ON|OFF`: bật/tắt server UI Dear ImGui.
- `NSTU_ENABLE_WERROR=ON|OFF`: coi warning là error.

Ví dụ chỉ build core protocol/test:

```powershell
cmake -S . -B build-core -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DNSTU_BUILD_VIDEO=OFF `
  -DNSTU_BUILD_CLIENT=OFF -DNSTU_BUILD_SERVER=OFF
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

## Artifact sau khi build

- `nstu-server.exe`: server administration UI.
- `nstu-service.exe`: Windows Service process.
- `nstu-agent.exe`: interactive session agent.
- `nstu_video_probe.exe`: kiểm tra khả năng video/MFT trên máy hiện tại.
- Các executable trong `tests/`: test protocol, auth, networking và video.

## Đóng gói installer

Trên Windows, CMake tạo hai target CPack/NSIS độc lập:

```powershell
cmake --build build-msvc --config Release --target nstu-package-server
cmake --build build-msvc --config Release --target nstu-package-client
```

Kết quả là `nstu-server-0.1.0.exe` và `nstu-client-0.1.0.exe` trong build
directory. Cần cài [NSIS](https://nsis.sourceforge.io/) và đặt `makensis.exe`
trong `PATH`. Nếu môi trường build không có NSIS, có thể tạo archive để kiểm
tra layout bằng CPack:

```powershell
cpack --config build-msvc/CPackConfig.cmake -G ZIP `
  -DCPACK_COMPONENTS_ALL="server;docs" `
  -DCPACK_PACKAGE_FILE_NAME=nstu-server-0.1.0
cpack --config build-msvc/CPackConfig.cmake -G ZIP `
  -DCPACK_COMPONENTS_ALL="client;docs" `
  -DCPACK_PACKAGE_FILE_NAME=nstu-client-0.1.0
```

Client package có `install-client-service.ps1` và
`uninstall-client-service.ps1`. Các script yêu cầu PowerShell chạy với quyền
Administrator; installer không tự tạo service ngoài ý muốn.

MVP chưa tự động signing, cấp key, cấu hình multicast hoặc service recovery.
Các bước đó phải được thực hiện trong deployment workflow riêng.

## Secret, memory và Deep Freeze

Không commit PSK, DPAPI entropy, certificate, dump, memory snapshot, capture,
log hoặc file runtime. Các thư mục local được ignore gồm `local/`, `memory/`,
`dumps/`, `logs/`, `runtime/` và các build tree.

Secret runtime nên được lưu bằng DPAPI machine scope hoặc certificate store.
Named pipe và memory-mapped/local runtime data chỉ giảm disk I/O; chúng không
thay thế ACL, authentication, key rotation hoặc policy của Deep Freeze.

## Bảo mật và protocol

Connection preamble là admission filter nhanh cho magic, version, role,
`client_id` và `key_id`; nó không chứng minh danh tính máy con. Danh tính chỉ
được chấp nhận sau HMAC handshake thành công.

Luồng video xác thực HMAC trước packet-loss accounting và frame reassembly.
HMAC không mã hóa nội dung màn hình; nếu LAN không được tin cậy cần AES-GCM
hoặc cơ chế confidentiality tương đương.

Chi tiết xem [ARCHITECTURE.md](docs/ARCHITECTURE.md),
[SECURITY.md](docs/SECURITY.md), [PACKET_LOSS.md](docs/PACKET_LOSS.md),
[VIDEO_REASSEMBLY.md](docs/VIDEO_REASSEMBLY.md) và [ROADMAP.md](docs/ROADMAP.md).

## Fork và phát triển

1. Mở repository trên GitHub và chọn **Fork**.
2. Clone fork, sau đó cấu hình repository gốc làm `upstream`:

   ```powershell
   git clone https://github.com/<your-account>/nstu.git
   cd nstu
   git remote add upstream https://github.com/Khoasoma/nstu.git
   ```

3. Tạo branch cho thay đổi:

   ```powershell
   git fetch upstream
   git switch -c feature/<short-name> upstream/main
   ```

4. Build với `NSTU_ENABLE_WERROR=ON`, chạy test và kiểm tra diff:

   ```powershell
   cmake --build build-msvc --config Release --parallel
   ctest --test-dir build-msvc -C Release --output-on-failure
   git diff --check
   ```

5. Commit nhỏ, có mục đích rõ ràng, rồi push lên fork:

   ```powershell
   git add <files>
   git commit -m "Describe the change"
   git push -u origin feature/<short-name>
   ```

6. Mở Pull Request từ fork tới `Khoasoma/nstu:main`. PR phải nêu test đã chạy,
   thay đổi protocol/ABI nếu có, rủi ro Windows compatibility và ảnh hưởng
   tới memory/runtime artifact.

Giữ thay đổi tập trung theo module và cập nhật roadmap khi hoàn tất milestone.

## License và dependency

NSTU phát hành theo MIT License. Dependency runtime phải tương thích MIT,
Apache-2.0 hoặc license permissive tương đương; không thêm GPL dependency.
Dear ImGui được fetch ở thời điểm configure và dùng MIT License. Windows SDK,
Winsock, DXGI, D3D11 và Media Foundation là thành phần của Windows SDK, không
được redistribute như source dependency. Xem [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Trạng thái production

Green CI chỉ chứng minh build và local correctness. Trước khi triển khai thật
cần hoàn tất IOCP `AcceptEx`/`WSARecv`/`WSASend` dispatcher, persisted keyring và
enrollment transport, packetizer/jitter/NACK/keyframe scheduling, service-agent
routing, installer/signing, device-loss recovery, 50-client soak test, switch
multicast matrix và Intel driver matrix.
