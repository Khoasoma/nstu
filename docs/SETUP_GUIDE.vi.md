# Hướng dẫn thiết lập NSTU

[README tiếng Việt](../README.vi.md) | [English](../README.md)

Tài liệu này phân biệt ba thành phần thường bị nhầm lẫn:

1. Installer NSTU, nơi cài binary và tự gọi script lifecycle.
2. `nstu-setup.exe`, bootstrapper quản trị tương tác.
3. PowerShell script triển khai, được đóng gói thành file và có thể chạy thủ
   công để enrollment hoặc kiểm tra.

## Nội dung từng gói tải về

| Gói tải về | Có `nstu-setup.exe` | Deployment script | Tác vụ tự động |
| --- | --- | --- | --- |
| Full server installer | Có, trong `setup\\` | Có, trong `docs\\deployment\\` | Kiểm tra server và cấu hình data root được bảo vệ |
| Full client installer | Không | Có; helper lifecycle trong `client\\`, script dùng chung trong `docs\\deployment\\` | Đăng ký `nstu-service` và yêu cầu restart |
| File riêng `nstu-server.exe` | Không | Không | Không làm gì; phải chạy thủ công |
| Các EXE client riêng | Không | Không | Không làm gì; dùng full client installer để đăng ký service |

Script có sẵn sau khi cài package đầy đủ. Release asset chỉ có EXE không phải
package đầy đủ và không thể dùng làm nguồn cho các lệnh trong phần enrollment.
Có thể dùng source checkout thay thế; script nằm trong `packaging\\`.

## Giao diện `nstu-setup.exe`

`nstu-setup.exe` là tool quản trị chạy thủ công với quyền Administrator. Tool
không chạy như service, không tự khởi động client service và không thay thế
NSIS installer. Không có tham số, tool mở màn hình chọn vai trò:

```powershell
& "$env:ProgramFiles\\NSTU\\setup\\nstu-setup.exe"
```

Chọn `Client`, `Server` hoặc `Both` để audit điều kiện. `Both` chỉ gộp các
panel kiểm tra, không cài đồng thời hai vai trò trên cùng máy. Có thể truyền
lựa chọn khi chạy có kiểm soát:

```powershell
& "$env:ProgramFiles\\NSTU\\setup\\nstu-setup.exe" --target=server
& "$env:ProgramFiles\\NSTU\\setup\\nstu-setup.exe" --target=client
& "$env:ProgramFiles\\NSTU\\setup\\nstu-setup.exe" --target=both
```

`--graphics-debug` yêu cầu lớp debug Direct3D 11 và ghi lại việc lớp này có
sẵn hay không. Có thể kết hợp với target:

```powershell
& "$env:ProgramFiles\\NSTU\\setup\\nstu-setup.exe" `
  --target=server --graphics-debug
```

Cửa sổ setup cung cấp:

- Panel điều kiện theo vai trò. Kiểm tra server hiển thị màn hình và encoder
  H.264 phần cứng; kiểm tra client hiển thị ngữ cảnh network/display session.
- Tên adapter mạng, trạng thái hoạt động và link speed.
- Audit policy cho Task Manager, Command Prompt, Control Panel và khả năng
  hiển thị ổ C:.
- Panel allowlist website IPv4 opt-in. Bấm Apply sẽ thay đổi WFP policy và chỉ
  nên thực hiện khi Administrator chủ động muốn áp policy đó.
- Popup `Diagnostics` với tên/hãng adapter DXGI, D3D feature level, trạng thái
  Desktop Duplication, chế độ hardware/WARP và HRESULT gần đây có giới hạn.

Setup không tự âm thầm áp lockdown hoặc WFP. Các thao tác này cần bấm nút rõ
ràng. Tool cũng không cấu hình autologon, cài Deep Freeze hoặc tự enrollment
client.

## Quan hệ giữa installer và script

Installer tự gọi các script lifecycle:

- Server package chạy `test-system-setup.ps1` và `configure-data-root.ps1`
  trong lúc cài đặt, sau khi role-conflict check xác nhận chưa có NSTU client.
- Client package chạy `client\\install-client-service.ps1`; helper này gọi
  role-conflict check tương tự, gọi `test-system-setup.ps1` và
  `configure-data-root.ps1`, đăng ký service với `start= auto` và yêu cầu
  restart.
- Uninstaller client chạy `client\\uninstall-client-service.ps1`, buộc dừng
  process agent/service của NSTU, dừng/xóa service và yêu cầu restart. File
  package còn bị khóa sẽ được đăng ký với Windows
  `MoveFileEx(..., MOVEFILE_DELAY_UNTIL_REBOOT)` để xóa ở lần boot tiếp theo.
  Script dừng an toàn nếu phát hiện Deep Freeze đang active.

Server uninstaller chạy `docs\\deployment\\uninstall-server.ps1`, buộc dừng
`nstu-server.exe` đã cài, xóa file server/setup, lên lịch xóa file bị khóa ở
lần boot tiếp theo và đặt cờ restart cho NSIS.

Enrollment secret là thao tác quản trị riêng. Sau khi cài server, chạy
`new-enrollment-secret.ps1` từ `docs\\deployment\\` trên server, sau đó dùng
`client\\nstu-provision.exe` trên từng client. Xem
[phần enrollment phòng máy](../README.vi.md#kết-nối-một-phòng-máy) để biết lệnh
đầy đủ.

## Cờ build và cờ runtime

`NSTU_BUILD_SETUP=ON` là tùy chọn CMake lúc configure để build executable
`nstu-setup`. Đây không phải tham số dòng lệnh của executable đã build. Hiện
không có cờ runtime `--setup`; cờ runtime là `--target=client|server|both` và
`--graphics-debug`.

Để build tool từ source:

```powershell
cmake -S . -B build-setup -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DNSTU_BUILD_SETUP=ON `
  -DNSTU_BUILD_CLIENT=OFF -DNSTU_BUILD_SERVER=OFF `
  -DNSTU_BUILD_VIDEO=OFF -DNSTU_BUILD_TESTS=OFF
cmake --build build-setup --target nstu-setup
```
