# Project NSTU

[English](README.md) | [Tiếng Việt](README.vi.md) | [Hướng dẫn phát triển](docs/DEVELOPMENT.md)

[![Windows CI](https://github.com/Khoasoma/nstu/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/Khoasoma/nstu/actions/workflows/windows.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

NSTU là dự án quản lý lớp học và phòng máy Windows miễn phí, mã nguồn mở. Dự
án hướng đến một máy giáo viên quản lý tập trung, client nhẹ trên máy học sinh,
lệnh điều khiển được xác thực, chat, snapshot màn hình tiết kiệm băng thông và
broadcast màn hình giáo viên trong mạng nội bộ.

![Dashboard NSTU Server ở chế độ sáng tiếng Anh và chế độ tối tiếng Việt](docs/assets/server-dashboard-preview.png)

*Dashboard server: chế độ sáng tiếng Anh và chế độ tối tiếng Việt.*

> **Trạng thái phát triển:** NSTU hiện là engineering MVP, chưa phải bản
> production. Persisted enrollment, dispatcher nhiều client, authenticated
> control routing, JPEG snapshot định kỳ, annotation overlay, teacher-screen
> snapshot broadcast, packetization và device recovery đã có implementation/test;
> continuous H.264 decode end-to-end và validation matrix trên phòng máy thật
> chưa hoàn thành.
> Không nên dùng bản nightly hiện tại như một biện pháp bảo mật trong trường học
> thật.

## Câu chuyện bắt đầu

NSTU bắt nguồn từ sự tiếc nuối khi chứng kiến các trường học phải dựa vào phần
mềm quản lý phòng máy không có bản quyền. Phần mềm đến từ nguồn không tin cậy có
thể bị sửa đổi, bị khai thác, hoặc âm thầm biến máy tính trong phòng học thành
hạ tầng đào tiền mã hóa cho kẻ tấn công. Đồng thời, khi Việt Nam ngày càng siết
chặt vấn đề bản quyền, chi phí phần mềm có thể khiến một số trường có ngân sách
hạn chế khó trang bị và quản lý phòng tin học đúng pháp luật.

Vì vậy, chúng tôi xây dựng NSTU như một lựa chọn thực tế: miễn phí, có thể kiểm
tra mã nguồn, dễ cài đặt, và được thiết kế cho cả an toàn lẫn phần cứng cấu hình
thấp. NSTU dùng MIT License. Trường học, giáo viên, doanh nghiệp và cá nhân có
thể sử dụng, nghiên cứu, sửa đổi và phân phối lại, kể cả cho mục đích thương mại,
miễn là giữ lại thông báo bản quyền và nội dung giấy phép.

Mã nguồn mở không tự động đồng nghĩa với an toàn. Vì vậy NSTU công khai threat
model và các hạng mục production chưa hoàn thành tại
[SECURITY.md](docs/SECURITY.md) và [ROADMAP.md](docs/ROADMAP.md).

## Năng lực hướng đến

- Quản lý từ 50 máy Windows trở lên bằng một máy giáo viên.
- Theo dõi phòng máy bằng JPEG snapshot có giới hạn, chụp mỗi 5-10 giây để
  tránh tải video liên tục trên switch.
- Lock/unlock client, vẽ lên màn hình một máy học sinh bằng click-through
  overlay và broadcast snapshot màn hình giáo viên.
- Giữ nền tảng H.264 multicast/unicast cho chế độ broadcast liên tục tùy chọn
  trong tương lai, không dùng làm đường monitoring mặc định.
- Hiển thị screen wall responsive của các snapshot mới nhất, cùng telemetry,
  điều khiển và chat tập trung cho một máy.
- Chạy Windows Service nhỏ và Win32 tray/chat agent native trên client.
- Xác thực lệnh điều khiển và video mà không cần JSON, XML, Electron hay driver
  kernel tùy biến.
- Giữ secret, dump, capture và runtime state ở local, không đưa lên Git.

## Cấu hình hệ thống

Đây là mục tiêu cấu hình của dự án, chưa phải kết quả đã được xác nhận bằng soak
test trên phòng máy 50 client thật.

| Vai trò | Cấu hình mục tiêu | Mạng |
| --- | --- | --- |
| Server | Intel Core i5-6400, RAM 8 GB, trống 512 MB, Windows 10/11 x64 | Khuyến nghị Gigabit Ethernet có dây |
| Client | Intel Core i5-6400, RAM 8 GB, trống 512 MB, Windows 10/11 x64 | Khuyến nghị Ethernet có dây |
| Router/switch | Hỗ trợ UDP multicast, IGMPv2 hoặc IGMPv3, IGMP snooping và IGMP querier | Một LAN/VLAN được kiểm soát cho lần triển khai đầu |

Với server quản lý từ 50 máy trở lên, RAM 16 GB và SSD là lựa chọn thận trọng
cho đến khi mục tiêu 8 GB vượt qua kiểm thử phần cứng dài hạn. Intel HD Graphics
530 là baseline cho hardware acceleration, không phải cam kết hoạt động với mọi
phiên bản driver.

## Benchmark tài nguyên cục bộ

Các số liệu dưới đây chỉ là phép đo tham khảo trên máy phát triển, không thay
thế benchmark production với 50 client. Phép đo được thực hiện ngày 1 tháng 9
năm 2026 từ commit `f183e42`, dùng
`build-verify/server/nstu-server.exe`, registry client trống và không có traffic
snapshot hoặc broadcast.

Máy thử nghiệm: AMD Ryzen 7 5700X (8 core/16 logical processor), RAM 15,9 GiB,
ổ SSD, NVIDIA GeForce RTX 2060 SUPER và Windows 11 Pro build 26200.

| Tình huống | CPU trên tổng năng lực máy | Working set | Private memory | Phương pháp |
| --- | ---: | ---: | ---: | --- |
| Dashboard hiển thị, idle | trung bình 0,725%, p95 2,336% | trung bình 53,54 MiB, tối đa 54,50 MiB | trung bình 117,95 MiB, tối đa 118,83 MiB | 120 mẫu, mỗi mẫu cách 1 giây, sau warm-up 10 giây |
| Đóng cửa sổ xuống tray, idle | trung bình 0,023%, tối đa 0,289% | trung bình 52,48 MiB, tối đa 53,00 MiB | trung bình 120,60 MiB, tối đa 121,06 MiB | 60 mẫu, mỗi mẫu cách 1 giây, sau warm-up 5 giây |

Lần đo phòng trống không tạo snapshot payload của ứng dụng, vì vậy đây không
phải benchmark băng thông trực tiếp. Bảng dưới là mô hình payload JPEG trường
hợp xấu nhất từ giới hạn 60 KiB/frame đã được triển khai. Mô hình giả định mọi
frame đều đạt giới hạn và chưa tính overhead của authenticated command, TCP/IP,
Ethernet, retransmission và control traffic khác.

| Snapshot traffic | Chu kỳ 5 giây | Chu kỳ 7 giây | Chu kỳ 10 giây |
| --- | ---: | ---: | ---: |
| Một client, một chiều | 0,098 Mbps | 0,070 Mbps | 0,049 Mbps |
| Theo dõi phòng 50 client, chiều vào server | 4,92 Mbps | 3,51 Mbps | 2,46 Mbps |
| Broadcast màn hình giáo viên tới 50 client qua TCP riêng cho từng client hiện tại | 4,92 Mbps | 3,51 Mbps | 2,46 Mbps |
| Theo dõi và broadcast giáo viên cùng lúc | 9,83 Mbps | 7,02 Mbps | 4,92 Mbps |

Các giá trị này là trần payload của chế độ snapshot định kỳ hiện tại, không
phải throughput đo trực tiếp trên switch. JPEG thực tế có thể nhỏ hơn, trong khi
wire overhead làm mỗi lần truyền lớn hơn một chút. Đường continuous H.264 trong
tương lai cần benchmark rate control và multicast/unicast riêng.

Mười lần khởi chạy trong cùng một phiên Windows đạt trạng thái cửa sổ phản hồi
với median 191,8 ms và trung bình 215,4 ms. Giá trị thấp nhất là 187,4 ms, cao
nhất là 435,1 ms, và lần chạy đầu tiên đo được là 435,1 ms. Đây là độ trễ mở ứng
dụng khi Windows đã chạy, không phải thời gian từ lúc reboot Windows đến khi
manager sẵn sàng.

Để tham khảo, số liệu do chủ dự án cung cấp cho các phần mềm quản lý lớp học
khác trên cùng cấu hình Ryzen 7 5700X là CPU trung bình 11-12%, peak có thể đạt
20%, RAM khoảng 820 MB-1,1 GB với peak 1,3 GB, 30-40 Mbps khi streaming và
khoảng 32 giây cho quá trình boot và mở manager. Các số liệu này không được tái
kiểm chứng độc lập trong lần đo hiện tại; khác biệt về workload và phương pháp
đo vẫn khiến đây chưa phải phép so sánh hoàn toàn cùng điều kiện. Các hệ thống
tham khảo cũng được mô tả là học sinh có thể gỡ cài đặt. Khả năng gỡ NSTU không
được kiểm thử trong benchmark tài nguyên này; nội dung đó vẫn thuộc validation
matrix về policy quản trị, reboot và triển khai Windows.

### Kết quả bổ sung được báo cáo trên i5-6400

Chủ dự án cũng báo cáo một lần chạy NSTU gần đúng trên hệ thống Intel Core
i5-6400 mục tiêu. Kết quả không kèm raw sample, thời lượng, số client hoặc mô tả
workload, vì vậy đây là số liệu sơ bộ chứ chưa phải kết luận chính thức.

| Chỉ số | NSTU trên i5-6400 | So sánh và mức giảm |
| --- | ---: | --- |
| CPU | 4-12% | Chưa có baseline CPU của phần mềm khác trên cùng máy i5. Nếu chỉ dùng mức trung bình 11-12% trên Ryzen trước đó làm tham khảo, mức thấp 4% của NSTU thấp hơn 7-8 điểm phần trăm, tương đương 63,6-66,7%; mức cao 12% trùng với baseline, nên chưa thể khẳng định CPU luôn giảm. |
| RAM | Xấp xỉ tương tự lần chạy NSTU trên Ryzen | Kết quả giữa các máy tương tự nhau, nhưng chưa ghi nhận số RAM i5 đủ chính xác để kết luận. |
| GPU tích hợp | 11% | Phần mềm khác dùng 34% iGPU. NSTU thấp hơn 23 điểm phần trăm, tương đương giảm 67,6%, và dùng khoảng 32,4% mức tải GPU của phần mềm so sánh. |

### Số liệu client được báo cáo tại Trường THCS Vũng Tàu

Giáo viên tại Trường THCS Vũng Tàu đã thực hiện và cho phép ghi nhận các kiểm
tra phía client dưới đây trên ba thiết bị cùng cấu hình Intel Core i5-6400.
Đây là số liệu trung bình gần đúng từ ba lần thử, chưa phải benchmark
production chính thức; workload, cảnh chụp, driver và network có thể làm kết
quả thay đổi.

| Trạng thái client | RAM gần đúng | CPU | GPU / chi tiết capture |
| --- | ---: | ---: | --- |
| Chụp snapshot và service đang chạy | 44 MB | không quá 7% | 11% iGPU / 6% CPU trong mẫu GPU/CPU tương ứng |
| Xem stream từ server có overlay (1080p/60 fps) | 96 MB | 11% | 27% iGPU / 19% CPU |
| Vẽ trực tiếp trên máy học sinh | 72 MB | chưa ghi nhận | Workload vẽ overlay |
| Stream màn hình học sinh về server (720p/30 fps) | 65 MB | chưa ghi nhận | 25% iGPU / 16% CPU |

Kết quả phía client bổ sung cho các phép đo server ở trên. Không nên hiểu đây là
cam kết mọi máy i5-6400 hoặc mọi workload trong phòng học sẽ có đúng các giá trị
này.

### Quan sát về gỡ cài đặt với Deep Freeze

Kiểm tra gỡ client NSTU tại Vũng Tàu cho thấy cần thực hiện đúng quy trình quản
trị Deep Freeze. Phải boot máy ở trạng thái Thawed, mở console Deep Freeze theo
cách thông thường, tắt protection và restart Windows trước khi gỡ hoàn toàn
client; sau đó uninstaller vẫn yêu cầu thêm một lần restart để dừng service và
xóa file. Nếu không mở Deep Freeze và tắt protection, việc gỡ gần như bị chặn
hoàn toàn. Đây là báo cáo quan sát thực địa, không phải khẳng định mọi edition
Deep Freeze đều hoạt động giống nhau.

### Trường phối hợp và địa điểm kiểm thử

Các trường dưới đây đang phối hợp với NSTU với vai trò đối tác dự án hoặc đang
được đánh giá để phối hợp. Kết quả kiểm thử bổ sung vẫn đang được thu thập.

#### Đối tác đã xác minh (hiện tại)

<table>
  <tr>
    <td align="center" valign="top" width="180">
      <img src="docs/assets/partners/vung-tau-junior-high.png" alt="Logo Trường THCS Vũng Tàu" width="112"><br>
      <sub><b>Trường THCS Vũng Tàu</b><br>Đã hoàn thành kiểm thử; giáo viên thực hiện và cho phép</sub>
    </td>
  </tr>
</table>

#### Đối tác đang chờ xác minh

<table>
  <tr>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/vo-truong-toan-junior-high.png" alt="Logo Trường THCS Võ Trường Toản" width="112"><br><sub><b>Trường THCS Võ Trường Toản</b><br>Đã đồng ý phối hợp; chờ kiểm thử</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/dinh-tien-hoang-high-school.png" alt="Logo Trường THPT Đinh Tiên Hoàng" width="112"><br><sub><b>Trường THPT Đinh Tiên Hoàng</b><br>Đã đồng ý phối hợp; chờ kiểm thử</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/le-quy-don-gifted-high-school.png" alt="Logo Trường THPT Chuyên Lê Quý Đôn" width="112"><br><sub><b>Trường THPT Chuyên Lê Quý Đôn</b><br>Đã đồng ý phối hợp; chờ kiểm thử</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/ptnk-vnu-hcm.png" alt="Logo PTNK ĐHQG-HCM" width="112"><br><sub><b>PTNK, ĐHQG-HCM</b><br>Đã đồng ý phối hợp; chờ kiểm thử</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/ben-cat-high-school.png" alt="Logo Trường THPT Bến Cát" width="112"><br><sub><b>Trường THPT Bến Cát</b><br>Đang xem xét; chưa xác nhận đối tác</sub></td>
  </tr>
</table>

Các file logo trong `docs/assets/partners/` là bản đã làm sạch nền từ các hình
ảnh được cung cấp cho báo cáo này.

Dùng `tools/production/collect-benchmarks.ps1` cho các lần đo dài hơn. Vẫn cần
kết quả ở chu kỳ snapshot 5, 7 và 10 giây, với client thật, network traffic,
raw CSV và workload được mô tả rõ trên phần cứng i5-6400 mục tiêu trước khi đưa
ra tuyên bố tài nguyên production.

## Sơ đồ mạng khuyến nghị

```text
Máy giáo viên (NSTU Server)
          |
     Gigabit Ethernet
          |
Managed switch/router có IGMP snooping + một IGMP querier
     |            |             |
 Client 01     Client 02      Client 50+
```

Trước khi triển khai production:

1. Đặt server và client trong cùng VLAN hoặc subnet tin cậy ở lần triển khai
   đầu tiên.
2. Bật IGMP snooping trên managed switch và đảm bảo chỉ một router hoặc switch
   Layer 3 làm IGMP querier cho VLAN đó.
3. Không expose trực tiếp control/video traffic của NSTU ra Internet.
4. Ưu tiên Ethernet có dây. Nếu thử nghiệm bằng Wi-Fi, tắt AP client isolation
   và kiểm tra multicast không bị ép xuống legacy data rate.
5. Không nên dùng unmanaged switch cho phòng máy lớn. Nếu không có IGMP
   snooping, multicast có thể bị flood đến mọi port. Nếu multicast bị chặn,
   fallback unicast dự kiến sẽ làm băng thông server và switch tăng theo từng
   client.
6. Luôn bật Windows Firewall. TCP `47001` là control port mặc định và UDP `47000`
   dành cho video transport. Chỉ mở rule cho VLAN phòng học và executable cần
   thiết; không expose rule rộng ra Internet.

Multicast qua nhiều VLAN cần multicast routing được cấu hình có chủ đích. Không
nên bật tính năng này chỉ để discovery hoạt động.

## Cài bản thử nghiệm hiện tại

Nightly installer được phát hành tại
[trang Releases](https://github.com/Khoasoma/nstu/releases). Đây là artifact
development chưa được ký số nên Microsoft Defender SmartScreen có thể cảnh báo.
Hãy xác minh nguồn release và SHA-256 trước khi chạy:

```powershell
Get-FileHash .\nstu-server-*.exe -Algorithm SHA256
Get-FileHash .\nstu-client-*.exe -Algorithm SHA256
```

### Server

1. Tải `nstu-server-<version>.exe` từ pre-release mới nhất.
2. Chạy installer và chấp nhận UAC nếu Windows yêu cầu.
3. Khởi động:

   ```powershell
   & "$env:ProgramFiles\NSTU\server\nstu-server.exe"
   ```

Dashboard không tự chèn dữ liệu demo. Registry client khởi động ở trạng thái
trống và chỉ hiển thị record do runtime registry cung cấp. Giáo viên có thể
chuyển giữa `Room screens`, hiển thị toàn bộ client phù hợp trong screen wall
responsive, và `Selected client`, tập trung telemetry, điều khiển snapshot,
annotation, lock/unlock và chat cho một máy. Khoảng chụp điều chỉnh từ 5-10
giây. Mỗi JPEG bị giới hạn ở 60 KiB và queue chỉ giữ snapshot mới nhất để tránh
tích tụ frame cũ. Broadcast màn hình giáo viên dùng cùng đường snapshot có giới
hạn. Dashboard có nút chuyển English/Tiếng Việt và chế độ sáng/tối. Khi minimize
hoặc đóng cửa sổ, server tiếp tục chạy trong notification area của Windows; dùng
menu tray để mở lại hoặc thoát. Continuous H.264/UDP preview vẫn là hạng mục tùy
chọn trong tương lai.

Cũng có thể chọn sẵn Tiếng Việt và dark mode khi khởi động:

```powershell
& "$env:ProgramFiles\NSTU\server\nstu-server.exe" --language=vi --dark
```

### Client

1. Tải và chạy `nstu-client-<version>.exe` bằng quyền Administrator.
2. Installer tự đăng ký `nstu-service` để khởi động cùng Windows và cấu hình
   service recovery. Service không được khởi động ngay bên trong phiên cài đặt.
3. Restart Windows khi installer yêu cầu. Ở lần boot tiếp theo, service tự chạy
   và khởi động đúng một instance `nstu-agent.exe` trong user session đang active
   khi đăng nhập hoặc mở khóa.
4. Sau khi restart, Administrator có thể kiểm tra service bằng:

   ```powershell
   Get-Service nstu-service
   ```

Để gỡ client, dùng **Installed apps** của Windows hoặc NSTU uninstaller. Trình
gỡ cài đặt sẽ dừng agent, xóa service, xóa file package và yêu cầu restart thêm
một lần. Gỡ service thủ công không phải quy trình triển khai được hỗ trợ.

## Kết nối một phòng máy

Quy trình enrollment engineering hiện dùng command line. Trên server, chạy
`configure-data-root.ps1` và `new-enrollment-secret.ps1` bằng quyền Administrator,
sau đó restart `nstu-server.exe`. Trên từng client, dùng identity 128-bit riêng:

```powershell
$clientId = [guid]::NewGuid().ToString("N")
& "$env:ProgramFiles\NSTU\client\nstu-provision.exe" `
  192.168.10.10 47001 $clientId 1 "D:\SecureTransfer\nstu-enrollment.bin"
```

Tool xác thực enrollment transcript, derive PSK mà không truyền PSK trên mạng,
và lưu cấu hình client bằng machine-scope DPAPI. Xóa mọi bản copy enrollment
secret sau khi enroll, rồi restart service hoặc Windows.

```text
Cài client
  -> cấp danh tính riêng và enrollment credential được bảo vệ
  -> xác thực với server qua TCP
  -> đăng ký thiết bị và nhận room policy
  -> nhận cấu hình video group đã được xác thực
  -> join multicast, đo packet loss và fallback unicast có giới hạn khi cần
```

Connection preamble chỉ giúp loại nhanh peer sai rõ ràng. Danh tính máy chỉ
được chấp nhận sau khi cryptographic handshake thành công. Enrollment UI cho
giáo viên và workflow multicast/group-key cuối cùng vẫn là release work.

## Triển khai cùng Deep Freeze

- Cài binary vào vị trí Windows được bảo vệ thông thường.
- Dành riêng một thawed location có ACL chặt cho identity đã enroll, key material
  được bảo vệ, cấu hình, audit log và update state.
- Cấu hình vị trí đó trước enrollment bằng `configure-data-root.ps1 -DataRoot
  "D:\NSTUData"`.
- Tuyệt đối không đưa PSK, certificate, dump, screen capture hay runtime secret
  vào repository.
- Không đóng băng image production trước khi đã kiểm thử persistence của
  enrollment, service recovery, upgrade và rollback.
- Boot máy ở trạng thái Thawed và tắt bảo vệ Deep Freeze trước khi cài, nâng cấp
  hoặc gỡ NSTU. Client uninstaller kiểm tra các service `DFServ`/`DeepFrz` đã
  biết và dừng thao tác khi bảo vệ vẫn active; cần xác minh cơ chế kiểm tra thận
  trọng này với đúng edition Deep Freeze được trường sử dụng.

Named pipe và memory-mapped file chỉ giảm I/O tạm thời, không thay thế persistent
protected storage. Deep Freeze sẽ hủy mọi state nằm ngoài thawed space sau reboot.

## Lưu ý bảo mật

- Control session hiện tại dùng mutual HMAC authentication và replay protection.
- JPEG snapshot và video datagram được xác thực, nhưng nội dung màn hình chưa
  được mã hóa. Không thử nghiệm màn hình nhạy cảm trên LAN không tin cậy.
- Automation Authenticode đã có, nhưng artifact nightly vẫn chưa ký; production
  release phải chạy workflow với certificate thật.
- Review/fuzz độc lập, validation từng edition Deep Freeze, ký bằng certificate
  thật và hardware/network matrix vẫn là production blocker.
- CI xanh chỉ chứng minh build và automated test đạt, không chứng minh an toàn
  hoặc độ bền trên phần cứng trường học thật.

## Đóng góp và phát triển

Hướng dẫn build, cấu trúc repository, tùy chọn CMake, test và quy trình fork/PR
nằm trong [development guide](docs/DEVELOPMENT.md). Dependency mới phải tương
thích license permissive; dự án không chấp nhận GPL dependency.

## Người đóng góp

- **Bùi Hồng Hải Đăng (`yanij`)**: đóng góp ý tưởng, hỗ trợ thiết bị test và
  tham gia xây dựng Project NSTU.
- **Lê Anh Tuấn (`ssdarealest`)**: chịu trách nhiệm quản lý dự án, hỗ trợ pháp
  lý, xây dựng ý tưởng, quản lý tiến trình và phát triển dự án.

## Bản quyền

Project NSTU được phát hành theo [MIT License](LICENSE). Bạn có thể sử dụng, sao
chép, sửa đổi, công bố, phân phối, cấp phép lại và bán bản sao nếu tuân thủ yêu
cầu giữ thông báo license. Thông báo dependency nằm tại
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
