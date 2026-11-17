# Hướng dẫn kiểm tra và sửa lỗi phản hồi server/AMR

- **Dự án áp dụng:** `C:\Users\ESATECH\Desktop\new_01`.
- **Ngày lập:** 17/11/2026.
- **Mã nguồn:** [new_01.ino](./new_01.ino).
- **Phạm vi:** Chỉ dẫn vị trí kiểm tra, cách sửa và cách kiểm chứng. Các ví dụ trong tài liệu **chưa được áp dụng vào firmware, chưa được biên dịch hoặc thử trên phần cứng**.

## 1. Xác định lỗi nằm ở chặng nào

```text
AMR → server → bản tin TCP → ESP32 phân tích JSON
                            → cập nhật Discrete Input → PLC đọc
```

ESP32 nhận dữ liệu từ server; code hiện không trực tiếp xác nhận kết nối với AMR. Vì vậy cần phân biệt lỗi liên lạc, lỗi định dạng bản tin và một bản tin hợp lệ đang báo AMR có lỗi.

| Biểu hiện | Vị trí cần kiểm tra/chỉnh trong `new_01.ino` |
|---|---|
| Không kết nối được server | `tcpConnect()`, cấu hình IP/cổng và mạng |
| Nhận JSON thiếu, cắt đoạn hoặc mất bản tin | Vùng đọc TCP trong `tcpTaskProcess()` |
| Có dòng `JSON:` nhưng không cập nhật bit | `deserializeJson()` và kiểm tra tên/kiểu dữ liệu |
| JSON đúng nhưng sai bit trạng thái | Các nhánh `doc.containsKey(...)` và `mb.Ists(...)` |
| Serial đúng nhưng PLC đọc sai | `startModbus()`, `getSerialConfig()` và chương trình PLC |
| Mất server nhưng PLC vẫn thấy trạng thái cũ | `tcpDisconnect()` và cơ chế hết hạn dữ liệu trong `tcpTaskProcess()` |
| Không thấy từng trạng thái trên web | `handleStatus()` và JavaScript `updateStatus()` |

Tìm bằng **tên hàm hoặc câu lệnh** vì số dòng sẽ thay đổi sau khi sửa. Khối ánh xạ đang hoạt động bắt đầu bằng `if (doc.containsKey("READY"))` không có dấu `//` ở đầu. Đoạn cũ phía trên có `//` không được thực thi.

## 2. Quan sát trước khi sửa

1. Kết nối ESP32 với máy tính, chọn đúng cổng COM.
2. Mở Serial Monitor với **115200 baud**.
3. Theo dõi bản tin nhận và giá trị Discrete Input sau cập nhật.
4. Đối chiếu cùng thời điểm với log server và giá trị PLC đọc được.

Ví dụ log:

```text
JSON: {"READY":true,"MOVING":true,"ERROR":false}
READY    = 1  -> mb.Ists(0) = 1
ERROR    = 0  -> mb.Ists(1) = 0
MOVING   = 1  -> mb.Ists(6) = 1
```

Trong code hiện tại, dòng `JSON:` được in **trước khi phân tích JSON**. Chỉ thấy dòng này chưa chứng minh bản tin hợp lệ.

## 3. Không kết nối được server

### Vị trí

- `tcpConnect()` — bản hiện tại khoảng dòng 996.
- `startEthernet()`, `startWiFi()`.
- `setDefaultConfig()` và cấu hình đã lưu trong `/config.json`.

### Kiểm tra

- IP/cổng server có đúng với dịch vụ đang lắng nghe không?
- ESP32 có IP, mask và gateway phù hợp không?
- Server có chấp nhận kết nối từ ESP32 không?
- Đã sửa IP mặc định trong code nhưng thiết bị còn dùng cấu hình cũ trên flash không?

Cổng mặc định 502 trong dự án đang truyền **JSON qua TCP**, không phải Modbus TCP.

### Cách sửa

Ưu tiên sửa IP/cổng qua trang web cấu hình rồi reboot. Sửa riêng giá trị trong `setDefaultConfig()` không ghi đè `/config.json` đã tồn tại.

Để bổ sung thông tin chẩn đoán, trong `tcpConnect()` thêm log quanh lệnh `tcpClient.connect(...)`. Giữ lại các điều kiện bật/tắt và giới hạn khoảng thử kết nối của hàm:

```cpp
Serial.printf("[TCP] Connecting to %s:%u\n",
              tcpCfg.serverIP.c_str(), tcpCfg.port);

if (tcpClient.connect(tcpCfg.serverIP.c_str(), tcpCfg.port)) {
    tcpConnected = true;
    lastTCPActivity = millis();
    Serial.println("[TCP] Connected");
} else {
    tcpConnected = false;
    tcpClient.stop();
    Serial.println("[TCP] Connect failed");
}
```

Nếu IP/cổng được đổi khi socket cũ còn mở, code hiện chưa chủ động kết nối lại theo địa chỉ mới. Reboot là cách áp dụng cấu hình rõ ràng trong bản hiện tại.

## 4. JSON nhận được nhưng không cập nhật trạng thái

### Vị trí

Trong `tcpTaskProcess()`, tìm:

```cpp
DeserializationError error = deserializeJson(doc, rxData);
```

Hiện tại khi lỗi, chương trình chỉ `continue`, không in lý do.

### Cách sửa bước đầu: bổ sung log lỗi

Thay đúng khối `if (error)` ngay sau câu lệnh trên bằng:

```cpp
if (error) {
    Serial.printf("[JSON] Parse failed: %s; length=%u\n",
                  error.c_str(),
                  static_cast<unsigned int>(rxData.length()));
    continue;
}
```

Không thay nhầm các khối `if (error)` khác trong hàm đọc cấu hình hoặc API web.

### Kiểm tra nội dung bản tin

Ví dụ bản tin hợp lệ:

```json
{"READY":true,"ERROR":false,"MOVING":true}
```

Khi truyền TCP, cần thêm ký tự xuống dòng thực tế `\n` sau JSON, không phải gửi hai ký tự dấu gạch chéo và chữ `n`.

- Khóa phải đúng tên và chữ hoa/thường, ví dụ `MOVING`, không phải `Moving`.
- `ULOADING` là tên đang dùng trong code; server gửi `UNLOADING` sẽ không khớp.
- Dùng boolean JSON `true`/`false`; thống nhất rõ nếu server muốn dùng `0`/`1`.
- Không gửi chuỗi `"true"`/`"false"` nếu chưa có quy tắc chuyển đổi ở ESP32.
- Phản hồi cần phù hợp cấu trúc JSON cấp ngoài cùng mà code đang đọc. Nếu server bọc trạng thái vào `{"data":{...}}`, phải sửa đường dẫn truy cập các trường tương ứng.

### Cách sửa bước tiếp theo: kiểm tra kiểu trước khi gán

Ví dụ thay riêng nhánh `MOVING` bằng:

```cpp
if (doc.containsKey("MOVING")) {
    if (doc["MOVING"].is<bool>()) {
        bool moving = doc["MOVING"].as<bool>();
        mb.Ists(6, moving);
        Serial.printf("MOVING = %d -> mb.Ists(6) = %d\n",
                      moving, mb.Ists(6));
    } else {
        Serial.println("[JSON] MOVING must be boolean");
    }
}
```

Nhánh có kiểu sai giữ giá trị cũ và in lỗi; chưa phải quy tắc xử lý an toàn cho toàn hệ thống. Nếu yêu cầu bản tin phải đầy đủ và cập nhật đồng bộ, hãy xác thực toàn bộ các trường trước khi ghi bất kỳ bit nào.

## 5. Bản tin bị chia nhỏ hoặc thiếu ký tự kết thúc

### Nguyên nhân cần xét

TCP là luồng byte. Một lần server gửi có thể được ESP32 đọc qua nhiều lần, hoặc nhiều bản tin đến sát nhau. Code hiện dùng:

```cpp
String rxData = tcpClient.readStringUntil('\n');
```

Timeout đọc hiện đặt 100 ms. Nếu JSON chưa đến đủ trước khi hết thời gian chờ, chuỗi có thể bị đọc thiếu và bị bỏ qua do lỗi phân tích.

Tăng timeout chỉ thay đổi thời gian chờ, không thay thế cơ chế ghép bản tin. Cần thống nhất mỗi bản tin nằm trên một dòng và kết thúc bằng LF.

### Cách sửa: giữ bộ đệm qua nhiều vòng xử lý

Ví dụ dưới đây áp dụng cho giao thức JSON một dòng kết thúc bằng LF. Giới hạn 1024 byte là lựa chọn minh họa; cần đối chiếu kích thước bản tin thực tế.

**Bước 1:** Thêm các biến toàn cục và hàm sau trước `tcpDisconnect()`:

```cpp
String tcpRxBuffer;
bool tcpRxDiscarding = false;
const size_t MAX_TCP_RX_LINE = 1024;

bool readTcpJsonLine(String &line) {
    uint16_t byteBudget = 256;

    while (byteBudget > 0 && tcpClient.available()) {
        --byteBudget;
        char ch = static_cast<char>(tcpClient.read());

        if (ch == '\n') {
            if (tcpRxDiscarding) {
                tcpRxDiscarding = false;
                tcpRxBuffer = "";
                continue;
            }

            line = tcpRxBuffer;
            tcpRxBuffer = "";
            line.trim();
            if (line.length() > 0) return true;
            continue;
        }

        if (ch == '\r' || tcpRxDiscarding) continue;

        if (tcpRxBuffer.length() >= MAX_TCP_RX_LINE) {
            tcpRxBuffer = "";
            tcpRxDiscarding = true;
            Serial.println("[TCP] Line too long; discard until LF");
            continue;
        }

        tcpRxBuffer += ch;
    }

    return false;
}
```

**Bước 2:** Trong `tcpTaskProcess()`, thay phần mở đầu vòng nhận:

```cpp
while (tcpClient.available()) {
    String rxData = tcpClient.readStringUntil('\n');
```

bằng:

```cpp
String rxData;
for (uint8_t handled = 0;
     handled < 4 && readTcpJsonLine(rxData);
     ++handled) {
```

Giữ phần log, phân tích JSON, cập nhật trạng thái và dấu đóng vòng nhận phía sau. Không thêm một bộ đọc TCP khác chạy song song. Giới hạn số bản tin và byte giúp phần nhận không chiếm toàn bộ thời gian của tác vụ TCP khi dữ liệu đến liên tục.

**Bước 3:** Trong `tcpDisconnect()`, thêm trước khi hàm kết thúc:

```cpp
tcpRxBuffer = "";
tcpRxDiscarding = false;
```

Thêm cùng hai lệnh vào nhánh kết nối thành công của `tcpConnect()` để phiên mới không nhận nhầm phần dư của phiên cũ.

**Bước 4:** Kiểm tra dung lượng `DynamicJsonDocument doc(1024)`. Giới hạn độ dài chuỗi nhận và dung lượng bộ nhớ JSON là hai khái niệm khác nhau; không thể kết luận mọi chuỗi dài dưới 1024 byte đều phân tích được với document 1024 byte. Dùng log lỗi để xác định vấn đề trước khi điều chỉnh dung lượng.

### Kiểm chứng

- Gửi nửa bản tin, chờ lâu hơn 100 ms, gửi phần còn lại và LF: chỉ xử lý khi đủ dòng.
- Gửi hai JSON, mỗi JSON có LF, trong cùng một lần gửi: xử lý được cả hai.
- Gửi dòng quá dài: bỏ dòng đó rồi xử lý được dòng hợp lệ tiếp theo.
- Ngắt TCP giữa một bản tin rồi kết nối lại: không ghép dữ liệu của hai phiên.

## 6. JSON đúng nhưng sai bit trạng thái

### Ánh xạ cần đối chiếu

| Trường JSON nhận | Discrete Input (`mb.Ists`) | Ý nghĩa khi true theo quy ước hiện tại |
|---|---:|---|
| `READY` | 0 | AMR sẵn sàng |
| `ERROR` | 1 | AMR có lỗi |
| `CART_R` | 2 | Lấy hàng bên phải |
| `CART_L` | 3 | Lấy hàng bên trái |
| `ULOADING` | 4 | Trả hàng vào máy |
| `DONE` | 5 | Hoàn thành trả hàng |
| `MOVING` | 6 | Đang di chuyển |
| `BATTERY` | 7 | Đủ pin |
| `EMG` | 8 | Dừng khẩn cấp |

### Cách sửa

Nếu PLC được thống nhất đọc `MOVING` ở địa chỉ khác, thay địa chỉ trong **nhánh đang hoạt động**:

```cpp
mb.Ists(6, moving);
```

Sau đó sửa đồng bộ phần log, tài liệu và chương trình PLC. Nếu dùng địa chỉ ngoài vùng 0–9, phải tăng số lượng Discrete Input được tạo trong `startModbus()`.

Không sửa mảng `R_Coil`/`L_Coil` để chữa lỗi này: hai mảng đó thuộc chiều **PLC → server**, không phải chiều phản hồi server → PLC.

Nếu hai bên dùng ý nghĩa ngược nhau, thống nhất định nghĩa trước khi quyết định đảo bit. Ví dụ `BATTERY=true` hiện là đủ pin, còn tên `LOW BATTERY` lại gợi ý pin yếu. Không tự đổi thành `!battery` chỉ vì tên hiển thị khác.

### Kiểm chứng

Server bật riêng từng trường một, PLC chỉ được thấy đúng một Discrete Input tương ứng thay đổi. Sau đó thử cả hai giá trị true và false của từng trường.

## 7. Serial đúng nhưng PLC đọc sai hoặc không đọc được

### Vị trí

- `startModbus()` — khởi tạo UART1 và Modbus slave.
- `getSerialConfig()` — chuyển data bits/parity/stop bits sang cấu hình UART.
- Cấu hình RS485 trên web và chương trình PLC.

### Cách xử lý

1. Xác nhận PLC là master và ESP32 là slave.
2. Đối chiếu slave ID, baud, data bits, parity và stop bits ở hai phía.
3. Xác nhận PLC đọc **Discrete Input**, không đọc Coil cho các trạng thái AMR.
4. Đối chiếu địa chỉ bắt đầu từ 0 theo code với cách nhập địa chỉ trong phần mềm PLC.
5. Kiểm tra đấu dây và chân điều khiển DE/RE theo sơ đồ thực tế.
6. Sau khi đổi cấu hình RS485 trên web, reboot vì bản hiện tại chưa khởi tạo lại UART/Modbus ngay lúc lưu.

Ví dụ `MOVING` phải đọc từ Discrete Input 6. Serial có `mb.Ists(6)=1` chỉ chứng minh bộ nhớ ESP32 đã cập nhật, chưa chứng minh PLC đã đọc thành công.

Nếu dùng 7 data bits và 2 stop bits, cần bổ sung nhánh tương ứng trong `getSerialConfig()` vì code hiện luôn chọn 1 stop bit cho nhánh 7 data bits. Chọn hằng serial theo môi trường ESP32 thực tế và biên dịch kiểm tra trước khi nạp.

## 8. Mất server nhưng trạng thái AMR vẫn giữ nguyên

### Nguyên nhân hiện tại

- `tcpDisconnect()` chưa thay đổi các Discrete Input trạng thái.
- Trường không xuất hiện trong JSON mới giữ giá trị trước đó.
- `lastTCPActivity` được cập nhật trước khi xác thực JSON và chưa được dùng làm watchdog.
- `tcpCfg.timeout` đang là **chu kỳ gửi JSON**, không phải thời gian hết hạn phản hồi.

### Cách sửa theo hướng dữ liệu có hiệu lực

Cần tách trạng thái **socket đang kết nối** khỏi trạng thái **dữ liệu AMR còn mới và hợp lệ**.

Các bước triển khai:

1. Thêm mốc `lastValidAmrMs` và cờ `haveFullAmrSnapshot`.
2. Chỉ cập nhật mốc sau khi JSON hợp lệ và đạt yêu cầu về các trường/kiểu dữ liệu.
3. Trong `tcpTaskProcess()`, sau phần nhận và trước phần gửi, tính dữ liệu có quá hạn không.
4. Khi mất TCP, đánh dấu dữ liệu không còn hiệu lực ngay trong `tcpDisconnect()`.
5. Thông báo trạng thái hiệu lực cho PLC theo một địa chỉ đã được thống nhất.

Ví dụ biến dùng bởi tác vụ TCP:

```cpp
uint32_t lastValidAmrMs = 0;
bool haveFullAmrSnapshot = false;
const uint32_t AMR_RESPONSE_TIMEOUT_MS = 10000; // Ví dụ, cần thống nhất
```

Nếu giao thức quy định gửi đủ 9 trạng thái trong mỗi bản tin, xác thực toàn bộ 9 trường là boolean trước khi cập nhật các bit. Sau khi bản tin đạt yêu cầu và được áp dụng:

```cpp
lastValidAmrMs = millis();
haveFullAmrSnapshot = true;
```

Tại vị trí kiểm tra hết hạn trong `tcpTaskProcess()`:

```cpp
bool amrDataValid = tcpConnected &&
    haveFullAmrSnapshot &&
    static_cast<uint32_t>(millis() - lastValidAmrMs)
        <= AMR_RESPONSE_TIMEOUT_MS;
```

Trong `tcpDisconnect()`:

```cpp
haveFullAmrSnapshot = false;
```

Nếu server gửi từng phần, không thể coi một bản tin chỉ có `READY` là đã làm mới cả 9 trạng thái. Cần chọn rõ một trong hai hướng: yêu cầu server gửi bản tin đầy đủ định kỳ, hoặc quản lý thời điểm cập nhật riêng cho từng tín hiệu.

### Ví dụ cấp thêm bit liên lạc hợp lệ cho PLC

Discrete Input 9 hiện đã được tạo nhưng chưa dùng trong logic hiện tại. **Chỉ sau khi thống nhất với phía PLC**, có thể dành địa chỉ này cho `AMR_DATA_VALID`:

```cpp
// Sau khi tính amrDataValid trong tác vụ TCP:
mb.Ists(9, amrDataValid);

// Trong tcpDisconnect(), sau khi haveFullAmrSnapshot = false:
mb.Ists(9, false);
```

PLC cần có logic xử lý khi bit này bằng false. Không tự xóa toàn bộ các bit bằng false như một cách xử lý chung: ví dụ điều đó có thể làm mất ý nghĩa đang báo lỗi hoặc dừng khẩn cấp. Giá trị và hành vi dự phòng phải do thiết kế hệ thống quy định.

Mốc 10 giây chỉ là ví dụ. Chọn theo chu kỳ phản hồi của server và độ trễ vận hành cho phép. Các biến trên nếu cần đọc từ webTask phải được đồng bộ giữa các tác vụ; không coi việc thêm `volatile` là thay thế cho đồng bộ.

## 9. Server gửi ERROR=true thì sửa ở đâu?

Ví dụ bản tin:

```json
{"ERROR":true}
```

Đây có thể là bản tin hợp lệ đang báo **AMR có lỗi**, không phải lỗi phân tích JSON hoặc kết nối TCP.

Trong ESP32, nhánh hiện tại chỉ thực hiện:

```cpp
bool err = doc["ERROR"];
mb.Ists(1, err);
```

Cần đối chiếu log server và trạng thái AMR để xác định vì sao giá trị được gửi là true. Nếu ESP32 chuyển đúng giá trị thì nguyên nhân không nằm ở nhánh ánh xạ này. Không đổi cố định `err` thành false để làm mất thông báo lỗi.

## 10. Bổ sung trạng thái AMR lên trang web

Trang web hiện chỉ hiển thị kết nối LAN/Wi-Fi/TCP và cấu hình serial. Muốn thấy các bit AMR cần sửa cả API C++ và JavaScript.

### Bước 1: mở rộng handleStatus()

Trong `handleStatus()`, thêm trước đoạn `serializeJson(doc, output)`:

```cpp
JsonObject amr = doc.createNestedObject("amr");
amr["READY"]    = mb.Ists(0);
amr["ERROR"]    = mb.Ists(1);
amr["CART_R"]   = mb.Ists(2);
amr["CART_L"]   = mb.Ists(3);
amr["ULOADING"] = mb.Ists(4);
amr["DONE"]     = mb.Ists(5);
amr["MOVING"]   = mb.Ists(6);
amr["BATTERY"]  = mb.Ists(7);
amr["EMG"]      = mb.Ists(8);
```

Đây là ví dụ các trường cần xuất. Sketch hiện dùng chung `mb` ở nhiều tác vụ; khi triển khai cần bảo vệ truy cập theo cùng một cơ chế đồng bộ ở các nơi dùng `mb`, hoặc tạo bản chụp trạng thái đã đồng bộ cho web đọc. Không chỉ khóa riêng đoạn web rồi coi các truy cập khác đã được bảo vệ.

### Bước 2: mở rộng JavaScript updateStatus()

Trong `INDEX_HTML`, tìm `async function updateStatus()`. Thêm phần sau khi đã tạo biến `text` và trước câu lệnh đưa `text` vào ô trạng thái:

```javascript
if (s.amr) {
    text += "\n\n================ AMR ================\n";
    const keys = [
        "READY", "ERROR", "CART_R", "CART_L", "ULOADING",
        "DONE", "MOVING", "BATTERY", "EMG"
    ];

    for (const key of keys) {
        text += "\n" + key + " : " + (s.amr[key] ? "1" : "0");
    }
}
```

JavaScript chạy trong trình duyệt, còn `handleStatus()` chạy trên ESP32. Cần cập nhật cả hai phía. Các giá trị hiển thị là trạng thái đã lưu; muốn biết có còn mới không phải bổ sung chỉ báo hiệu lực theo mục 8.

### Kiểm chứng

1. Mở `http://<IP-ESP32>/api/status`, kiểm tra có đối tượng `amr`.
2. Mở trang chính, kiểm tra các giá trị được hiển thị.
3. Server thay đổi từng bit, đối chiếu Serial, web và PLC.
4. Ngắt server, kiểm tra giao diện phân biệt dữ liệu cũ với dữ liệu còn hiệu lực nếu đã triển khai chỉ báo này.

## 11. Trình tự áp dụng các thay đổi

1. Giữ lại bản `.ino` đang chạy ổn và ghi lại cấu hình, phiên bản board package/thư viện.
2. Xác định biểu hiện lỗi bằng log trước khi chọn mục sửa.
3. Bổ sung log lỗi JSON và kết nối TCP trước.
4. Nếu có bằng chứng nhận thiếu bản tin, triển khai bộ đệm nhận.
5. Đối chiếu và sửa ánh xạ nếu sai trường/địa chỉ/ý nghĩa.
6. Thống nhất quy tắc hết hạn dữ liệu với PLC/server trước khi thêm watchdog và bit hiệu lực.
7. Biên dịch sau từng thay đổi, sau đó kiểm tra đúng trường hợp lỗi đã phát hiện.
8. Khi sửa cấu hình mạng hoặc serial, reboot theo cơ chế hiện tại.

Các ví dụ ở từng mục là thay đổi có điều kiện, không cần áp dụng tất cả cùng lúc. Sau khi sửa làm thay đổi giao thức hoặc địa chỉ, cập nhật cả tài liệu giao tiếp và chương trình PLC/server liên quan.
