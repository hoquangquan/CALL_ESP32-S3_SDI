// [HUONG DAN] FIRMWARE new_01: PLC (Modbus master) <-> ESP32-S3 (slave) <-> server (TCP JSON).
// [HUONG DAN] Bản được chú thích: Desktop/new_01/new_01.ino (thư mục riêng bạn đang mở).
// [HUONG DAN] Ánh xạ cảm biến trong bản này là thứ tự tùy chỉnh; xem [07], không dùng nhầm map bản cũ.
// [HUONG DAN] MỤC LỤC: tìm các nhãn [01]...[12] để đến nhanh phần cần chỉnh sửa.
// [HUONG DAN] [01] Thư viện/chân; [02] Cấu trúc cấu hình; [03] Mặc định; [04] Lưu/nạp.
// [HUONG DAN] [05] Modbus; [06] Ethernet/Wi-Fi; [07] TCP và ánh xạ tín hiệu.
// [HUONG DAN] [08] API cấu hình; [09] Giao diện web; [10] Đăng ký API; [11] Khởi động; [12] FreeRTOS.
// [HUONG DAN] Chú thích mô tả hành vi hiện có; xem BAO_CAO_PHAN_TICH_NEW_01.md để biết chi tiết.
// [HUONG DAN] [01] THƯ VIỆN VÀ PHẦN CỨNG: chỉnh GPIO bên dưới theo sơ đồ đấu dây thực tế.
#include <Arduino.h>
#include <WiFi.h>
#include <ETH.h>
#include <SPI.h>
#include <WebServer.h>
#include <LittleFS.h>


#include <ArduinoJson.h>
#include <ModbusRTU.h>

// [HUONG DAN] W5500 nối SPI: CS chọn chip; IRQ báo ngắt; RST reset module.
#define ETH_PHY_TYPE ETH_PHY_W5500
#define ETH_PHY_ADDR -1

#define ETH_PHY_CS   10
#define ETH_PHY_IRQ  21
#define ETH_PHY_RST  14

#define ETH_SPI_SCK  12
#define ETH_SPI_MISO 13
#define ETH_SPI_MOSI 11

SPIClass ethSPI(FSPI);

// [HUONG DAN] UART1 nối module chuyển đổi RS485; DE/RE điều khiển hướng truyền/nhận.
#define RS485_TX       17
#define RS485_RX       8
#define RS485_DE_RE    18

HardwareSerial RS485Serial(1);
ModbusRTU mb;

// [HUONG DAN] Cổng HTTP của trang cấu hình; đây là cổng khác với cổng TCP server phía dưới.
WebServer server(80);

// [HUONG DAN] File nằm trên flash ESP32 (LittleFS), không phải file trên máy tính.
#define CONFIG_FILE "/config.json"

// [HUONG DAN] LED chỉ nhấp nháy theo tác vụ, chưa phản ánh kết nối PLC/server.
#define STATUS_LED 3

uint32_t lastWiFiReconnect = 0;
uint32_t lastETHCheck = 0;

// [HUONG DAN] Đơn vị ms. networkTask dùng khoảng Wi-Fi này để thử kết nối lại.
// [HUONG DAN] ETH_RECONNECT_INTERVAL và lastETHCheck hiện chưa được dùng để kiểm tra lại Ethernet.
#define WIFI_RECONNECT_INTERVAL 5000
#define ETH_RECONNECT_INTERVAL   5000

// [HUONG DAN] [02] CẤU TRÚC CẤU HÌNH: thêm trường mới cần sửa đồng bộ các hàm và web.
// [HUONG DAN] NetworkConfig: DHCP=true dùng IP tự động; DHCP=false dùng IP/mask/gateway/DNS bên dưới.
// [HUONG DAN] lan* dành cho W5500; wifi* dành cho Wi-Fi. Đổi giá trị ban đầu trong setDefaultConfig().
struct NetworkConfig {

  bool lanDHCP;

  IPAddress lanIP;
  IPAddress lanMask;
  IPAddress lanGateway;
  IPAddress lanDNS;

  bool wifiEnabled;
  bool wifiDHCP;

  String wifiSSID;
  String wifiPassword;

  IPAddress wifiIP;
  IPAddress wifiMask;
  IPAddress wifiGateway;
  IPAddress wifiDNS;
};

// [HUONG DAN] TCPConfig: ESP32 chủ động kết nối server; devName được gửi trong khóa JSON "id".
struct TCPConfig {

  bool enabled;

  // [HUONG DAN] Tên định danh của thiết bị gửi server trong khóa "id"; không phải SSID/hostname.
  String devName;

  String serverIP;

  uint16_t port;

  // [HUONG DAN] Tên timeout nhưng hiện dùng làm CHU KỲ GỬI JSON (ms), không phải watchdog phản hồi.
  uint32_t timeout;

  // [HUONG DAN] Khoảng cách tối thiểu giữa các lần thử kết nối TCP (ms).
  uint32_t reconnect;
};

// [HUONG DAN] RTUConfig: phải khớp PLC (ID 1..247, baud, data bits, parity N/E/O, stop bits).
struct RTUConfig {

  uint8_t slaveID;

  uint32_t baud;

  uint8_t dataBits;

  // [HUONG DAN] Parity: 'N' không chẵn lẻ, 'E' chẵn, 'O' lẻ; phải khớp phía PLC.
  char parity;

  uint8_t stopBits;
};

// [HUONG DAN] Các cấu hình và đối tượng mb được nhiều tác vụ dùng chung; sketch chưa có mutex bảo vệ.
NetworkConfig netCfg;
TCPConfig tcpCfg;
RTUConfig rtuCfg;

NetworkClient tcpClient;

bool tcpConnected = false;

uint32_t lastTCPAttempt = 0;
// [HUONG DAN] Mốc hoạt động được ghi nhận nhưng chưa có logic xử lý khi server im lặng quá lâu.
uint32_t lastTCPActivity = 0;

volatile bool ethConnected = false;
volatile bool ethGotIP = false;

TaskHandle_t taskNetwork;
TaskHandle_t taskTCP;
TaskHandle_t taskModbus;
TaskHandle_t taskWeb;
TaskHandle_t taskStatus;

void networkTask(void *pvParameters);
void tcpTask(void *pvParameters);
void modbusTask(void *pvParameters);
void webTask(void *pvParameters);
void statusTask(void *pvParameters);

// [HUONG DAN] Tiện ích chuyển IP <-> chuỗi. stringToIP trả false và giữ IP cũ nếu chuỗi sai.
String ipToString(const IPAddress &ip) {

  return ip.toString();
}

bool stringToIP(
  const String &str,
  IPAddress &ip
) {

  IPAddress tmp;

  if (tmp.fromString(str)) {

    ip = tmp;

    return true;
  }

  return false;
}

// [HUONG DAN] [03] CẤU HÌNH MẶC ĐỊNH: sửa IP, SSID, server, tên thiết bị và serial tại đây.
// [HUONG DAN] LƯU Ý: loadConfig() sẽ ghi đè bằng /config.json đã lưu; sửa mặc định không tự thay file cũ.
void setDefaultConfig() {

  netCfg.lanDHCP = false;

  netCfg.lanIP =
    IPAddress(192, 168, 0, 17);

  netCfg.lanMask =
    IPAddress(255, 255, 255, 0);

  netCfg.lanGateway =
    IPAddress(192, 168, 0, 1);

  netCfg.lanDNS =
    IPAddress(8, 8, 8, 8);

  netCfg.wifiEnabled = true;

  netCfg.wifiDHCP = true;

  netCfg.wifiSSID = "";

  netCfg.wifiPassword = "";

  netCfg.wifiIP =
    IPAddress(192, 168, 0, 18);

  netCfg.wifiMask =
    IPAddress(255, 255, 255, 0);

  netCfg.wifiGateway =
    IPAddress(192, 168, 0, 1);

  netCfg.wifiDNS =
    IPAddress(8, 8, 8, 8);

  // [HUONG DAN] Cấu hình server ứng dụng: dù dùng cổng 502, dữ liệu là JSON/TCP, không phải Modbus TCP.
  tcpCfg.enabled = true;

  tcpCfg.devName = "Call Device";

  tcpCfg.serverIP = "192.168.0.100";

  tcpCfg.port = 502;

  tcpCfg.timeout = 3000;

  tcpCfg.reconnect = 3000;

  // [HUONG DAN] Thông số đường truyền RS485 mặc định: slave 1, 9600 baud, 8N1.
  rtuCfg.slaveID = 1;

  rtuCfg.baud = 9600;

  rtuCfg.dataBits = 8;

  rtuCfg.parity = 'N';

  rtuCfg.stopBits = 1;
}

// [HUONG DAN] [04] LƯU/NẠP: saveConfig ghi RAM -> JSON -> LittleFS; loadConfig làm chiều ngược lại.
// [HUONG DAN] Khi thêm cấu hình: sửa struct, setDefaultConfig, saveConfig, loadConfig,
// [HUONG DAN] buildConfig, applyConfig và các trường HTML/JavaScript tương ứng.
bool saveConfig() {

  DynamicJsonDocument doc(4096);

  JsonObject network =
    doc.createNestedObject("network");

  JsonObject lan =
    network.createNestedObject("lan");

  lan["dhcp"] =
    netCfg.lanDHCP;

  lan["ip"] =
    netCfg.lanIP.toString();

  lan["mask"] =
    netCfg.lanMask.toString();

  lan["gateway"] =
    netCfg.lanGateway.toString();

  lan["dns"] =
    netCfg.lanDNS.toString();

  JsonObject wifi =
    network.createNestedObject("wifi");

  wifi["enabled"] =
    netCfg.wifiEnabled;

  wifi["dhcp"] =
    netCfg.wifiDHCP;

  wifi["ssid"] =
    netCfg.wifiSSID;

  wifi["password"] =
    netCfg.wifiPassword;

  wifi["ip"] =
    netCfg.wifiIP.toString();

  wifi["mask"] =
    netCfg.wifiMask.toString();

  wifi["gateway"] =
    netCfg.wifiGateway.toString();

  wifi["dns"] =
    netCfg.wifiDNS.toString();

  JsonObject tcp =
    doc.createNestedObject("tcpClient");

  tcp["enabled"] =
    tcpCfg.enabled;

  tcp["devName"] =
    tcpCfg.devName;

  tcp["serverIP"] =
    tcpCfg.serverIP;

  tcp["port"] =
    tcpCfg.port;

  tcp["timeout"] =
    tcpCfg.timeout;

  tcp["reconnect"] =
    tcpCfg.reconnect;

  JsonObject serial =
    doc.createNestedObject("serial");

  serial["slaveId"] =
    rtuCfg.slaveID;

  serial["baud"] =
    rtuCfg.baud;

  serial["dataBits"] =
    rtuCfg.dataBits;


  if (rtuCfg.parity == 'E') {

    serial["parity"] = "E";

  } else if (rtuCfg.parity == 'O') {

    serial["parity"] = "O";

  } else {

    serial["parity"] = "N";
  }


  serial["stopBits"] =
    rtuCfg.stopBits;

  File file =
    LittleFS.open(
      CONFIG_FILE,
      "w"
    );

  if (!file) {

    Serial.println(
      "ERROR: Cannot open config for writing"
    );

    return false;
  }


  serializeJsonPretty(
    doc,
    file
  );

  file.close();


  Serial.println(
    "Config saved"
  );

  return true;
}

// [HUONG DAN] File thiếu hoặc JSON hỏng: tạo lại mặc định. Trường thiếu dùng giá trị hiện có trong RAM.
// [HUONG DAN] setup() gọi setDefaultConfig trước hàm này để có giá trị dự phòng.
bool loadConfig() {

  if (!LittleFS.exists(CONFIG_FILE)) {

    Serial.println(
      "Config not found"
    );

    setDefaultConfig();

    return saveConfig();
  }


  File file =
    LittleFS.open(
      CONFIG_FILE,
      "r"
    );

  if (!file) {

    Serial.println(
      "ERROR: Cannot open config"
    );

    return false;
  }


  DynamicJsonDocument doc(4096);

  DeserializationError error =
    deserializeJson(
      doc,
      file
    );

  file.close();


  if (error) {

    Serial.print(
      "JSON error: "
    );

    Serial.println(
      error.c_str()
    );

    setDefaultConfig();

    return saveConfig();
  }

  JsonObject network =
    doc["network"];


  if (!network.isNull()) {

    JsonObject lan =
      network["lan"];


    if (!lan.isNull()) {

      netCfg.lanDHCP =
        lan["dhcp"] |
        netCfg.lanDHCP;


      stringToIP(
        lan["ip"] |
        netCfg.lanIP.toString(),

        netCfg.lanIP
      );


      stringToIP(
        lan["mask"] |
        netCfg.lanMask.toString(),

        netCfg.lanMask
      );


      stringToIP(
        lan["gateway"] |
        netCfg.lanGateway.toString(),

        netCfg.lanGateway
      );


      stringToIP(
        lan["dns"] |
        netCfg.lanDNS.toString(),

        netCfg.lanDNS
      );
    }

    JsonObject wifi =
      network["wifi"];


    if (!wifi.isNull()) {

      netCfg.wifiEnabled =
        wifi["enabled"] |
        netCfg.wifiEnabled;


      netCfg.wifiDHCP =
        wifi["dhcp"] |
        netCfg.wifiDHCP;


      netCfg.wifiSSID =
        wifi["ssid"] |
        netCfg.wifiSSID;


      netCfg.wifiPassword =
        wifi["password"] |
        netCfg.wifiPassword;


      stringToIP(
        wifi["ip"] |
        netCfg.wifiIP.toString(),

        netCfg.wifiIP
      );


      stringToIP(
        wifi["mask"] |
        netCfg.wifiMask.toString(),

        netCfg.wifiMask
      );


      stringToIP(
        wifi["gateway"] |
        netCfg.wifiGateway.toString(),

        netCfg.wifiGateway
      );


      stringToIP(
        wifi["dns"] |
        netCfg.wifiDNS.toString(),

        netCfg.wifiDNS
      );
    }
  }

  JsonObject tcp =
    doc["tcpClient"];


  if (!tcp.isNull()) {

    tcpCfg.enabled =
      tcp["enabled"] |
      tcpCfg.enabled;

    tcpCfg.devName =
      tcp["devName"] |
      tcpCfg.devName;

    tcpCfg.serverIP =
      tcp["serverIP"] |
      tcpCfg.serverIP;


    tcpCfg.port =
      tcp["port"] |
      tcpCfg.port;


    tcpCfg.timeout =
      tcp["timeout"] |
      tcpCfg.timeout;


    tcpCfg.reconnect =
      tcp["reconnect"] |
      tcpCfg.reconnect;
  }

  JsonObject serial =
    doc["serial"];


  if (!serial.isNull()) {

    rtuCfg.slaveID =
      serial["slaveId"] |
      rtuCfg.slaveID;


    rtuCfg.baud =
      serial["baud"] |
      rtuCfg.baud;


    rtuCfg.dataBits =
      serial["dataBits"] |
      rtuCfg.dataBits;


    String parity =
      serial["parity"] |
      String(rtuCfg.parity);


    if (parity.length() > 0) {

      rtuCfg.parity =
        parity.charAt(0);
    }


    rtuCfg.stopBits =
      serial["stopBits"] |
      rtuCfg.stopBits;
  }

  if (rtuCfg.slaveID < 1 ||
      rtuCfg.slaveID > 247) {

    rtuCfg.slaveID = 1;
  }


  if (rtuCfg.dataBits != 7 &&
      rtuCfg.dataBits != 8) {

    rtuCfg.dataBits = 8;
  }


  if (rtuCfg.parity != 'N' &&
      rtuCfg.parity != 'E' &&
      rtuCfg.parity != 'O') {

    rtuCfg.parity = 'N';
  }


  if (rtuCfg.stopBits < 1 ||
      rtuCfg.stopBits > 2) {

    rtuCfg.stopBits = 1;
  }


  if (tcpCfg.port == 0) {

    tcpCfg.port = 502;
  }


  if (tcpCfg.timeout == 0) {

    tcpCfg.timeout = 3000;
  }


  if (tcpCfg.reconnect == 0) {

    tcpCfg.reconnect = 3000;
  }


  Serial.println(
    "Config loaded"
  );

  return true;
}

// [HUONG DAN] [05] MODBUS RTU: chuyển cấu hình serial sang hằng SERIAL_xxx cho UART.
// [HUONG DAN] Nhánh 7 data bits hiện luôn dùng 1 stop bit, kể cả khi cấu hình chọn 2.
uint32_t getSerialConfig() {

  if (rtuCfg.dataBits == 7) {

    if (rtuCfg.parity == 'E')
      return SERIAL_7E1;

    if (rtuCfg.parity == 'O')
      return SERIAL_7O1;

    return SERIAL_7N1;
  }

  if (rtuCfg.parity == 'E') {

    if (rtuCfg.stopBits == 2)
      return SERIAL_8E2;

    return SERIAL_8E1;
  }

  if (rtuCfg.parity == 'O') {

    if (rtuCfg.stopBits == 2)
      return SERIAL_8O2;

    return SERIAL_8O1;
  }

  if (rtuCfg.stopBits == 2)
    return SERIAL_8N2;


  return SERIAL_8N1;
}

// [HUONG DAN] Khởi tạo UART1 và Modbus SLAVE. PLC phải là MASTER để chủ động đọc/ghi.
// [HUONG DAN] Thay cấu hình RS485 trên web cần reboot: luồng lưu hiện chưa gọi lại hàm này.
void startModbus()
{
  RS485Serial.end();

  delay(20);

  RS485Serial.begin(
    rtuCfg.baud,
    getSerialConfig(),
    RS485_RX,
    RS485_TX
  );

  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW);

  mb.begin(&RS485Serial, RS485_DE_RE);
  mb.slave(rtuCfg.slaveID);

  // [HUONG DAN] Địa chỉ bắt đầu từ 0: Coil 0..49 (PLC ghi); Discrete Input 0..9 (PLC đọc).
  // [HUONG DAN] Đang dùng Coil 0..23 và Ists 0..8; tăng dung lượng nếu bổ sung địa chỉ ngoài vùng.
  mb.addCoil(0, false, 50);
  mb.addIsts(0, false, 10);
}

// [HUONG DAN] [06] MẠNG: callback cập nhật cờ Ethernet và in log khi link/IP thay đổi.
void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {

  switch (event) {

    case ARDUINO_EVENT_ETH_START:

      Serial.println(
        "ETH START"
      );

      ETH.setHostname(
        "ESP32-S3-Gateway"
      );

      break;


    case ARDUINO_EVENT_ETH_CONNECTED:

      Serial.println(
        "ETH CONNECTED"
      );

      ethConnected = true;

      break;


    case ARDUINO_EVENT_ETH_GOT_IP:

      Serial.println(
        "ETH GOT IP"
      );


      Serial.print(
        "ETH IP: "
      );

      Serial.println(
        ETH.localIP()
      );


      Serial.print(
        "ETH GW: "
      );

      Serial.println(
        ETH.gatewayIP()
      );


      Serial.print(
        "ETH MASK: "
      );

      Serial.println(
        ETH.subnetMask()
      );


      ethConnected = true;

      ethGotIP = true;

      break;


    case ARDUINO_EVENT_ETH_LOST_IP:

      Serial.println(
        "ETH LOST IP"
      );

      ethGotIP = false;

      break;


    case ARDUINO_EVENT_ETH_DISCONNECTED:

      Serial.println(
        "ETH DISCONNECTED"
      );

      ethConnected = false;

      ethGotIP = false;

      break;

    case ARDUINO_EVENT_ETH_STOP:

      Serial.println(
        "ETH STOP"
      );

      ethConnected = false;

      ethGotIP = false;

      break;


    default:

      break;
  }
}

// [HUONG DAN] Khởi động SPI/W5500; chỉ gọi ETH.config khi chọn LAN IP tĩnh.
// [HUONG DAN] Có cả LAN và Wi-Fi nhưng chưa có logic riêng chọn ưu tiên/chuyển mạng dự phòng.
void startEthernet() {

  Serial.println();
  Serial.println(
    "Starting W5500..."
  );

  ethSPI.begin(
    ETH_SPI_SCK,
    ETH_SPI_MISO,
    ETH_SPI_MOSI
  );

  bool result =
    ETH.begin(
      ETH_PHY_TYPE,
      ETH_PHY_ADDR,
      ETH_PHY_CS,
      ETH_PHY_IRQ,
      ETH_PHY_RST,
      ethSPI
    );


  if (!result) {

    Serial.println(
      "ERROR: ETH.begin() failed"
    );

    return;
  }

  if (!netCfg.lanDHCP) {

    Serial.println(
      "ETH static IP"
    );


    bool configOK =
      ETH.config(
        netCfg.lanIP,
        netCfg.lanGateway,
        netCfg.lanMask,
        netCfg.lanDNS
      );


    if (!configOK) {

      Serial.println(
        "ETH.config failed"
      );
    }
  }


  Serial.println(
    "W5500 initialization done"
  );
}

// [HUONG DAN] Wi-Fi ở chế độ station (kết nối router); không tạo access point cấu hình.
// [HUONG DAN] wifiEnabled=false hoặc SSID trống thì không thực hiện kết nối.
void startWiFi() {

  if (!netCfg.wifiEnabled) {

    Serial.println(
      "WiFi disabled"
    );

    WiFi.mode(
      WIFI_OFF
    );

    return;
  }


  if (netCfg.wifiSSID.length() == 0) {

    Serial.println(
      "WiFi SSID empty"
    );

    return;
  }


  Serial.println();
  Serial.println(
    "Starting WiFi..."
  );


  WiFi.mode(
    WIFI_STA
  );

  if (!netCfg.wifiDHCP) {

    Serial.println(
      "WiFi static IP"
    );


    bool configOK =
      WiFi.config(
        netCfg.wifiIP,
        netCfg.wifiGateway,
        netCfg.wifiMask,
        netCfg.wifiDNS
      );


    if (!configOK) {

      Serial.println(
        "WiFi.config failed"
      );
    }
  }


  Serial.print(
    "Connecting WiFi: "
  );

  Serial.println(
    netCfg.wifiSSID
  );


  WiFi.begin(
    netCfg.wifiSSID.c_str(),
    netCfg.wifiPassword.c_str()
  );
}

// [HUONG DAN] [07] TCP JSON: quản lý kết nối và chuyển đổi tín hiệu PLC <-> server.
// [HUONG DAN] Ngắt TCP hiện chưa xóa Ists, vì vậy PLC có thể vẫn đọc trạng thái AMR cũ.
void tcpDisconnect() {

  if (tcpClient.connected()) {

    tcpClient.stop();
  }


  tcpConnected = false;
}

// [HUONG DAN] Thử kết nối theo reconnect (ms); socket còn kết nối thì giữ nguyên.
// [HUONG DAN] Đổi IP/cổng qua web chưa đóng socket cũ; reboot để áp dụng chắc chắn.
void tcpConnect() {
  if (!tcpCfg.enabled)
    return;
  if (tcpConnected && tcpClient.connected()) {
    return;
  }
  if (tcpConnected && !tcpClient.connected()) {
    tcpDisconnect();
  }
  uint32_t now = millis();
  if (now - lastTCPAttempt < tcpCfg.reconnect) {
    return;
  }
  lastTCPAttempt = now;
  // [HUONG DAN] Timeout ĐỌC stream 100 ms; không phải chu kỳ gửi hoặc timeout kết nối.
  tcpClient.setTimeout(100);
  if (tcpClient.connect(tcpCfg.serverIP.c_str(), tcpCfg.port)) {
    tcpConnected = true;
    lastTCPActivity = millis();
  } else {
    tcpConnected = false;
    tcpClient.stop();
  }
}

// [HUONG DAN] Mỗi vòng: kiểm tra kết nối -> nhận JSON server -> gửi trạng thái PLC khi đến hạn.
void tcpTaskProcess()
{

  if (!tcpCfg.enabled)
  {
    if (tcpConnected)
    {
      tcpDisconnect();
    }

    return;
  }

  if (!tcpConnected)
  {
    tcpConnect();
    return;
  }

  if (!tcpClient.connected())
  {
    tcpDisconnect();
    return;
  }

  while (tcpClient.available())
  {
    // [HUONG DAN] Giao thức: mỗi bản tin là một JSON trên một dòng, kết thúc bằng ký tự \n.
    // [HUONG DAN] Chưa có bộ đệm ghép qua nhiều lần đọc; bản tin đến chậm có thể bị đọc thiếu và bỏ qua.
    String rxData = tcpClient.readStringUntil('\n');

    Serial.print("JSON: ");
    Serial.println(rxData);

    rxData.trim();

    if (rxData.length() == 0)
      continue;

    lastTCPActivity = millis();

    DynamicJsonDocument doc(1024);

    DeserializationError error = deserializeJson(doc, rxData);

    if (error)
    {
      continue;
    }

    //    if (doc.containsKey("READY"))
    //    {
    //      bool ready = doc["READY"];
    //
    //      mb.Ists(0,  ready);
    //    }
    //    if (doc.containsKey("ERROR"))
    //    {
    //      bool err = doc["ERROR"];
    //
    //      mb.Ists(1,  err);
    //    }
    //    if (doc.containsKey("CART_R"))
    //    {
    //      bool cart_r = doc["CART_R"];
    //
    //      mb.Ists(2,  cart_r);
    //    }
    //    if (doc.containsKey("CART_L"))
    //    {
    //      bool cart_l = doc["CART_L"];
    //
    //      mb.Ists(3,  cart_l);
    //    }
    //    if (doc.containsKey("ULOADING"))
    //    {
    //      bool unload = doc["ULOADING"];
    //
    //      mb.Ists(4,  unload);
    //    }
    //    if (doc.containsKey("DONE"))
    //    {
    //      bool done = doc["DONE"];
    //
    //      mb.Ists(5,  done);
    //    }
    //    if (doc.containsKey("MOVING"))
    //    {
    //      bool moving = doc["MOVING"];
    //
    //      mb.Ists(6,  moving);
    //    }
    //    if (doc.containsKey("BATTERY"))
    //    {
    //      bool battery = doc["BATTERY"];
    //
    //      mb.Ists(7,  battery);
    //    }
    //    if (doc.containsKey("EMG"))
    //    {
    //      bool emg = doc["EMG"];
    //
    //      mb.Ists(8,  emg);
    //    }
    // [HUONG DAN] ÁNH XẠ NHẬN server -> PLC (Ists, địa chỉ bắt đầu từ 0):
    // [HUONG DAN] 0 READY; 1 ERROR; 2 CART_R; 3 CART_L; 4 ULOADING;
    // [HUONG DAN] 5 DONE; 6 MOVING; 7 BATTERY; 8 EMG.
    // [HUONG DAN] Giữ đúng tên "ULOADING" theo giao thức hiện tại. Thiếu khóa thì giữ bit cũ.
    // [HUONG DAN] Đoạn comment cũ phía trên chỉ để tham khảo; các nhánh bên dưới mới được thực thi.
    if (doc.containsKey("READY"))
    {
      bool ready = doc["READY"];
      mb.Ists(0, ready);

      Serial.printf("READY    = %d  -> mb.Ists(0) = %d\n", ready, mb.Ists(0));
    }

    if (doc.containsKey("ERROR"))
    {
      bool err = doc["ERROR"];
      mb.Ists(1, err);

      Serial.printf("ERROR    = %d  -> mb.Ists(1) = %d\n", err, mb.Ists(1));
    }

    if (doc.containsKey("CART_R"))
    {
      bool cart_r = doc["CART_R"];
      mb.Ists(2, cart_r);

      Serial.printf("CART_R   = %d  -> mb.Ists(2) = %d\n", cart_r, mb.Ists(2));
    }

    if (doc.containsKey("CART_L"))
    {
      bool cart_l = doc["CART_L"];
      mb.Ists(3, cart_l);

      Serial.printf("CART_L   = %d  -> mb.Ists(3) = %d\n", cart_l, mb.Ists(3));
    }

    if (doc.containsKey("ULOADING"))
    {
      bool unload = doc["ULOADING"];
      mb.Ists(4, unload);

      Serial.printf("ULOADING = %d  -> mb.Ists(4) = %d\n", unload, mb.Ists(4));
    }

    if (doc.containsKey("DONE"))
    {
      bool done = doc["DONE"];
      mb.Ists(5, done);

      Serial.printf("DONE     = %d  -> mb.Ists(5) = %d\n", done, mb.Ists(5));
    }

    if (doc.containsKey("MOVING"))
    {
      bool moving = doc["MOVING"];
      mb.Ists(6, moving);

      Serial.printf("MOVING   = %d  -> mb.Ists(6) = %d\n", moving, mb.Ists(6));
    }

    if (doc.containsKey("BATTERY"))
    {
      bool battery = doc["BATTERY"];
      mb.Ists(7, battery);

      Serial.printf("BATTERY  = %d  -> mb.Ists(7) = %d\n", battery, mb.Ists(7));
    }

    if (doc.containsKey("EMG"))
    {
      bool emg = doc["EMG"];
      mb.Ists(8, emg);

      Serial.printf("EMG      = %d  -> mb.Ists(8) = %d\n", emg, mb.Ists(8));
    }

  }

  static uint32_t lastSend = 0;

  uint32_t now = millis();

  // [HUONG DAN] Chu kỳ gửi lấy từ timeout; tối thiểu 10 ms. Mặc định 3000 ms.
  uint32_t interval = tcpCfg.timeout;

  if (interval < 10)  interval = 10;

  if (now - lastSend >= interval)
  {
    lastSend = now;
    DynamicJsonDocument doc(2048);
    // [HUONG DAN] ÁNH XẠ GỬI PLC -> server: đổi địa chỉ Coil/khóa JSON tại khối này nếu PLC thay map.
    // [HUONG DAN] Coil 0 READY máy; 1 CALL; 2 EMG máy; 3/4 khay phải/trái sẵn sàng; 5 LOAD TRAY.
    // [HUONG DAN] "CART_R  READY" và "CART_L  READY" có HAI dấu cách trước READY; server phải khớp.
    doc["id"]   = tcpCfg.devName;
    doc["READY"]         = mb.Coil(0);
    doc["CALL"]          = mb.Coil(1);
    doc["EMG"]           = mb.Coil(2);
    doc["CART_R  READY"] = mb.Coil(3);
    doc["CART_L  READY"] = mb.Coil(4);
    doc["LOAD TRAY"]     = mb.Coil(5);
    //JsonArray Rbuffer = doc.createNestedArray("SS CART_R");
    //JsonArray Lbuffer = doc.createNestedArray("SS CART_L");
    //for (int coil = 6; coil <= 14; coil++)
    //for (int coil =15
    //{
      //Rbuffer.add(mb.Coil(coil) ? 1 : 0);
      //Lbuffer.add(mb.Coil(coil * 2) ? 1 : 0);
    //}

    JsonArray Rbuffer = doc.createNestedArray("SS CART_R");
    JsonArray Lbuffer = doc.createNestedArray("SS CART_L");

    // [HUONG DAN] SỬA ĐỊA CHỈ CẢM BIẾN TẠI ĐÂY: vùng phải 6..14, trái 15..23; mỗi bên 9 vị trí.
    // [HUONG DAN] Hai dòng //const bên dưới là map cũ đã vô hiệu hóa. Hai const đang hoạt động là map tùy chỉnh.
    // [HUONG DAN] SS CART_R, index 0..8: Coil 8, 11, 14, 7, 10, 13, 6, 9, 12.
    // [HUONG DAN] SS CART_L, index 0..8: Coil 17, 20, 23, 16, 19, 22, 15, 18, 21.
    // [HUONG DAN] JSON giữ đúng thứ tự mảng này, KHÔNG đọc theo địa chỉ tăng dần.
    // [HUONG DAN] Chưa có sơ đồ PLC để suy ra vị trí vật lý hàng/cột; đối chiếu PLC khi thay thứ tự.
    // [HUONG DAN] Đây là bit PLC ghi qua Modbus, không phải số chân GPIO đọc cảm biến.
    // [HUONG DAN] Nếu đổi số vị trí, sửa cả 2 mảng, giới hạn vòng for, dung lượng JSON và phía server.
    //const int R_Coil[] = {6, 7, 8, 9, 10, 11, 12, 13, 14};
    //const int L_Coil[] = {15, 16, 17, 18, 19, 20, 21, 22, 23};
    const int R_Coil[] = {8, 11, 14, 7, 10, 13, 6, 9, 12};
    const int L_Coil[] = {17, 20, 23, 16, 19, 22, 15, 18, 21};

    for (int i = 0; i < 9; i++)
   {
    // [HUONG DAN] i là vị trí phần tử JSON; R_Coil[i]/L_Coil[i] mới là địa chỉ Modbus. Giá trị gửi là số 1/0.
    Rbuffer.add(mb.Coil(R_Coil[i]) ? 1 : 0);
    Lbuffer.add(mb.Coil(L_Coil[i]) ? 1 : 0);
   }

    String json;

    serializeJson(
      doc,
      json
    );

    // [HUONG DAN] Luôn thêm xuống dòng sau JSON để phía server xác định ranh giới bản tin.
    tcpClient.print(json);
    tcpClient.print('\n');
  }
}


// ============================================================
// Build Config JSON
// ============================================================

// [HUONG DAN] [08] API CẤU HÌNH: buildConfig tạo JSON trả cho web từ các cấu hình trong RAM.
void buildConfig(
  DynamicJsonDocument &doc
) {

  // ==========================================================
  // NETWORK
  // ==========================================================

  JsonObject network =
    doc.createNestedObject(
      "network"
    );


  // ----------------------------------------------------------
  // LAN
  // ----------------------------------------------------------

  JsonObject lan =
    network.createNestedObject(
      "lan"
    );


  lan["dhcp"] =
    netCfg.lanDHCP;

  lan["ip"] =
    netCfg.lanIP.toString();

  lan["mask"] =
    netCfg.lanMask.toString();

  lan["gateway"] =
    netCfg.lanGateway.toString();

  lan["dns"] =
    netCfg.lanDNS.toString();


  // ----------------------------------------------------------
  // WiFi
  // ----------------------------------------------------------

  JsonObject wifi =
    network.createNestedObject(
      "wifi"
    );


  wifi["enabled"] =
    netCfg.wifiEnabled;

  wifi["dhcp"] =
    netCfg.wifiDHCP;

  wifi["ssid"] =
    netCfg.wifiSSID;

  wifi["password"] =
    netCfg.wifiPassword;

  wifi["ip"] =
    netCfg.wifiIP.toString();

  wifi["mask"] =
    netCfg.wifiMask.toString();

  wifi["gateway"] =
    netCfg.wifiGateway.toString();

  wifi["dns"] =
    netCfg.wifiDNS.toString();


  // ==========================================================
  // TCP
  // ==========================================================

  JsonObject tcp =
    doc.createNestedObject(
      "tcpClient"
    );


  tcp["enabled"] =
    tcpCfg.enabled;

  tcp["devName"] =
    tcpCfg.devName;

  tcp["serverIP"] =
    tcpCfg.serverIP;

  tcp["port"] =
    tcpCfg.port;

  tcp["timeout"] =
    tcpCfg.timeout;

  tcp["reconnect"] =
    tcpCfg.reconnect;


  // ==========================================================
  // SERIAL
  // ==========================================================

  JsonObject serial =
    doc.createNestedObject(
      "serial"
    );


  serial["slaveId"] =
    rtuCfg.slaveID;

  serial["baud"] =
    rtuCfg.baud;

  serial["dataBits"] =
    rtuCfg.dataBits;


  if (rtuCfg.parity == 'E') {

    serial["parity"] = "E";

  } else if (rtuCfg.parity == 'O') {

    serial["parity"] = "O";

  } else {

    serial["parity"] = "N";
  }


  serial["stopBits"] =
    rtuCfg.stopBits;
}


// ============================================================
// API GET CONFIG
// ============================================================

// [HUONG DAN] GET /api/config: trả toàn bộ cấu hình để JavaScript điền vào form.
void handleGetConfig() {

  DynamicJsonDocument doc(4096);


  buildConfig(
    doc
  );


  String output;


  serializeJson(
    doc,
    output
  );


  server.send(
    200,
    "application/json",
    output
  );
}


// ============================================================
// API GET STATUS
// ============================================================

// [HUONG DAN] GET /api/status: trạng thái LAN/Wi-Fi/TCP và thông số serial.
// [HUONG DAN] Chưa có chỉ báo PLC giao tiếp thành công hay bảng giá trị Coil/Ists trên trang này.
void handleStatus() {

  DynamicJsonDocument doc(4096);

  // =====================================================
  // Ethernet LAN
  // =====================================================
  JsonObject eth = doc.createNestedObject("ethernet");

  eth["connected"] = ethConnected;
  eth["mac"] = ETH.macAddress();

  eth["ip"] = ETH.localIP().toString();
  eth["mask"] = ETH.subnetMask().toString();
  eth["gateway"] = ETH.gatewayIP().toString();
  eth["dns"] = ETH.dnsIP().toString();

  eth["dhcp"] = netCfg.lanDHCP;


  // =====================================================
  // WiFi
  // =====================================================
  JsonObject wifi = doc.createNestedObject("wifi");

  bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  wifi["enabled"] = netCfg.wifiEnabled;
  wifi["connected"] = wifiConnected;

  wifi["mac"] = WiFi.macAddress();

  wifi["ssid"] = WiFi.SSID();

  wifi["ip"] = WiFi.localIP().toString();
  wifi["mask"] = WiFi.subnetMask().toString();
  wifi["gateway"] = WiFi.gatewayIP().toString();
  wifi["dns"] = WiFi.dnsIP().toString();

  wifi["dhcp"] = netCfg.wifiDHCP;

  wifi["rssi"] = wifiConnected ? WiFi.RSSI() : -127;


  // =====================================================
  // TCP Client
  // =====================================================
  JsonObject tcp = doc.createNestedObject("tcp");

  tcp["enabled"] = tcpCfg.enabled;
  tcp["connected"] = tcpConnected;
  tcp["server"] = tcpCfg.serverIP;
  tcp["port"] = tcpCfg.port;


  // =====================================================
  // RS485 / Modbus RTU
  // =====================================================
  JsonObject serial = doc.createNestedObject("serial");

  serial["slaveId"] = rtuCfg.slaveID;
  serial["baud"] = rtuCfg.baud;
  serial["dataBits"] = rtuCfg.dataBits;
  serial["parity"] = rtuCfg.parity;
  serial["stopBits"] = rtuCfg.stopBits;


  // =====================================================
  // Send JSON
  // =====================================================
  String output;

  serializeJson(doc, output);

  server.send(
    200,
    "application/json",
    output
  );
}


// ============================================================
// Apply Config
// ============================================================

// [HUONG DAN] JSON từ web -> cập nhật RAM; hàm này chưa khởi động lại mạng/UART hoặc ngắt socket TCP.
bool applyConfig(
  DynamicJsonDocument &doc
) {

  // ==========================================================
  // NETWORK
  // ==========================================================

  JsonObject network =
    doc["network"];


  if (!network.isNull()) {


    // --------------------------------------------------------
    // LAN
    // --------------------------------------------------------

    JsonObject lan =
      network["lan"];


    if (!lan.isNull()) {

      netCfg.lanDHCP =
        lan["dhcp"] |
        netCfg.lanDHCP;


      String value;


      value =
        lan["ip"] |
        netCfg.lanIP.toString();

      stringToIP(
        value,
        netCfg.lanIP
      );


      value =
        lan["mask"] |
        netCfg.lanMask.toString();

      stringToIP(
        value,
        netCfg.lanMask
      );


      value =
        lan["gateway"] |
        netCfg.lanGateway.toString();

      stringToIP(
        value,
        netCfg.lanGateway
      );


      value =
        lan["dns"] |
        netCfg.lanDNS.toString();

      stringToIP(
        value,
        netCfg.lanDNS
      );
    }


    // --------------------------------------------------------
    // WiFi
    // --------------------------------------------------------

    JsonObject wifi =
      network["wifi"];


    if (!wifi.isNull()) {

      netCfg.wifiEnabled =
        wifi["enabled"] |
        netCfg.wifiEnabled;


      netCfg.wifiDHCP =
        wifi["dhcp"] |
        netCfg.wifiDHCP;


      netCfg.wifiSSID =
        wifi["ssid"] |
        netCfg.wifiSSID;


      netCfg.wifiPassword =
        wifi["password"] |
        netCfg.wifiPassword;


      String value;


      value =
        wifi["ip"] |
        netCfg.wifiIP.toString();

      stringToIP(
        value,
        netCfg.wifiIP
      );


      value =
        wifi["mask"] |
        netCfg.wifiMask.toString();

      stringToIP(
        value,
        netCfg.wifiMask
      );


      value =
        wifi["gateway"] |
        netCfg.wifiGateway.toString();

      stringToIP(
        value,
        netCfg.wifiGateway
      );


      value =
        wifi["dns"] |
        netCfg.wifiDNS.toString();

      stringToIP(
        value,
        netCfg.wifiDNS
      );
    }
  }


  // ==========================================================
  // TCP
  // ==========================================================

  JsonObject tcp =
    doc["tcpClient"];


  if (!tcp.isNull()) {

    tcpCfg.enabled =
      tcp["enabled"] |
      tcpCfg.enabled;

    tcpCfg.devName =
      tcp["devName"] |
      tcpCfg.devName;

    tcpCfg.serverIP =
      tcp["serverIP"] |
      tcpCfg.serverIP;


    tcpCfg.port =
      tcp["port"] |
      tcpCfg.port;


    tcpCfg.timeout =
      tcp["timeout"] |
      tcpCfg.timeout;


    tcpCfg.reconnect =
      tcp["reconnect"] |
      tcpCfg.reconnect;


    if (tcpCfg.port == 0) {

      tcpCfg.port = 502;
    }


    if (tcpCfg.timeout == 0) {

      tcpCfg.timeout = 3000;
    }


    if (tcpCfg.reconnect == 0) {

      tcpCfg.reconnect = 3000;
    }
  }


  // ==========================================================
  // SERIAL
  // ==========================================================

  JsonObject serial =
    doc["serial"];


  if (!serial.isNull()) {


    rtuCfg.slaveID =
      serial["slaveId"] |
      rtuCfg.slaveID;


    rtuCfg.baud =
      serial["baud"] |
      rtuCfg.baud;


    rtuCfg.dataBits =
      serial["dataBits"] |
      rtuCfg.dataBits;


    String p =
      serial["parity"] |
      String(rtuCfg.parity);


    if (p.length() > 0) {

      char c =
        p.charAt(0);


      if (c == 'N' ||
          c == 'E' ||
          c == 'O') {

        rtuCfg.parity = c;
      }
    }


    rtuCfg.stopBits =
      serial["stopBits"] |
      rtuCfg.stopBits;


    // --------------------------------------------------------
    // Validate
    // --------------------------------------------------------

    if (rtuCfg.slaveID < 1 ||
        rtuCfg.slaveID > 247) {

      rtuCfg.slaveID = 1;
    }


    if (rtuCfg.dataBits != 7 &&
        rtuCfg.dataBits != 8) {

      rtuCfg.dataBits = 8;
    }


    if (rtuCfg.stopBits < 1 ||
        rtuCfg.stopBits > 2) {

      rtuCfg.stopBits = 1;
    }
  }


  return true;
}


// ============================================================
// API POST CONFIG
// ============================================================

// [HUONG DAN] POST /api/config: kiểm tra JSON -> applyConfig -> saveConfig -> trả kết quả.
// [HUONG DAN] Chỉ lưu cấu hình, không tự reboot; dùng nút REBOOT sau khi thay mạng hoặc RS485.
void handleSetConfig() {

  if (!server.hasArg("plain")) {

    server.send(
      400,
      "application/json",
      "{\"error\":\"No JSON body\"}"
    );

    return;
  }


  DynamicJsonDocument doc(4096);


  DeserializationError error =
    deserializeJson(
      doc,
      server.arg("plain")
    );


  if (error) {

    server.send(
      400,
      "application/json",
      "{\"error\":\"Invalid JSON\"}"
    );

    return;
  }


  if (!applyConfig(doc)) {

    server.send(
      400,
      "application/json",
      "{\"error\":\"Invalid configuration\"}"
    );

    return;
  }


  // ----------------------------------------------------------
  // Save
  // ----------------------------------------------------------

  if (!saveConfig()) {

    server.send(
      500,
      "application/json",
      "{\"error\":\"Save failed\"}"
    );

    return;
  }

  DynamicJsonDocument response(1024);


  response["ok"] = true;


  response["message"] =
    "Configuration saved. Reboot required for network changes.";


  String output;


  serializeJson(
    response,
    output
  );


  server.send(
    200,
    "application/json",
    output
  );
}


// ============================================================
// API REBOOT
// ============================================================

// [HUONG DAN] POST /api/reboot: trả phản hồi trước, đợi 500 ms rồi khởi động lại chip.
void handleReboot() {

  server.send(
    200,
    "application/json",
    "{\"ok\":true,\"message\":\"Rebooting...\"}"
  );


  delay(500);


  ESP.restart();
}


// ============================================================
// HTML
// ============================================================

// [HUONG DAN] [09] GIAO DIỆN WEB: nội dung từ R"rawliteral( đến )rawliteral" là chuỗi HTML/CSS/JS.
// [HUONG DAN] Muốn đổi màu/kích thước: tìm <style>; đổi nhãn/ô nhập: tìm <body>; xử lý nút: tìm <script>.
// [HUONG DAN] Sửa giao diện tại đây; dùng comment đúng ngôn ngữ bên trong chuỗi (HTML/CSS/JavaScript).
// [HUONG DAN] Giữ đồng bộ id của ô nhập với getElementById trong JavaScript.
const char INDEX_HTML[] PROGMEM = R"rawliteral(

<!DOCTYPE html>

<html>

<head>

<meta charset="UTF-8">

<meta name="viewport"
      content="width=device-width,initial-scale=1">

<title>Nắng April</title>

<style>

body {
    font-family: Arial, sans-serif;
    background:#f2f2f2;
    margin:0;
    padding:20px;
}

.container {
    max-width:900px;
    margin:auto;
}

.card {
    background:white;
    padding:20px;
    margin-bottom:15px;
    border-radius:8px;
    box-shadow:0 2px 6px #bbb;
}

h1 {
    margin-top:0;
}

h2 {
    border-bottom:1px solid #ddd;
    padding-bottom:8px;
}

.row {
    display:grid;
    grid-template-columns:180px 1fr;
    margin:8px 0;
    align-items:center;
}

input, select {
    padding:8px;
    width:100%;
    box-sizing:border-box;
}

button {
    padding:12px 25px;
    border:0;
    border-radius:5px;
    cursor:pointer;
    margin-right:10px;
}

.save {
    background:#1976d2;
    color:white;
}

.reboot {
    background:#d32f2f;
    color:white;
}

.status {
    font-family:monospace;
    white-space:pre-wrap;
    background:#111;
    color:#00ff00;
    padding:15px;
    border-radius:5px;
}

</style>

</head>

<body>

<div class="container">


<!-- ===================================================== -->
<!-- STATUS -->
<!-- ===================================================== -->

<div class="card">

<h1>ESA TECH</h1>

<div class="status"
     id="status">
Loading...
</div>

</div>


<!-- ===================================================== -->
<!-- LAN -->
<!-- ===================================================== -->

<div class="card">

<h2>Ethernet LAN</h2>


<div class="row">

<label>DHCP</label>

<input type="checkbox"
       id="lanDHCP">

</div>


<div class="row">

<label>IP Address</label>

<input id="lanIP">

</div>


<div class="row">

<label>Subnet Mask</label>

<input id="lanMask">

</div>


<div class="row">

<label>Gateway</label>

<input id="lanGateway">

</div>


<div class="row">

<label>DNS</label>

<input id="lanDNS">

</div>

</div>


<!-- ===================================================== -->
<!-- WIFI -->
<!-- ===================================================== -->

<div class="card">

<h2>WiFi</h2>


<div class="row">

<label>Enable</label>

<input type="checkbox"
       id="wifiEnabled">

</div>


<div class="row">

<label>DHCP</label>

<input type="checkbox"
       id="wifiDHCP">

</div>


<div class="row">

<label>SSID</label>

<input id="wifiSSID">

</div>


<div class="row">

<label>Password</label>

<input type="password"
       id="wifiPassword">

</div>


<div class="row">

<label>IP Address</label>

<input id="wifiIP">

</div>


<div class="row">

<label>Subnet Mask</label>

<input id="wifiMask">

</div>


<div class="row">

<label>Gateway</label>

<input id="wifiGateway">

</div>


<div class="row">

<label>DNS</label>

<input id="wifiDNS">

</div>

</div>


<!-- ===================================================== -->
<!-- TCP -->
<!-- ===================================================== -->

<div class="card">

<h2>TCP Client</h2>


<div class="row">

<label>Enable</label>

<input type="checkbox"
       id="tcpEnabled">

</div>

<div class="row">
  <label>Device Name</label>
  <input
    type="text"
    id="devName"
    placeholder="Call Device">
</div>

<div class="row">

<label>Server IP</label>

<input id="serverIP">

</div>


<div class="row">

<label>Port</label>

<input type="number"
       id="serverPort">

</div>


<div class="row">

<label>KeepAlive (ms)</label>

<input type="number"
       id="timeout">

</div>


<div class="row">

<label>Reconnect (ms)</label>

<input type="number"
       id="reconnect">

</div>

</div>


<!-- ===================================================== -->
<!-- MODBUS -->
<!-- ===================================================== -->

<div class="card">

<h2>Modbus RTU Slave</h2>


<div class="row">

<label>Slave ID</label>

<input type="number"
       min="1"
       max="247"
       id="slaveID">

</div>


<div class="row">

<label>Baudrate</label>

<select id="baud">

<option value="2400">2400</option>
<option value="4800">4800</option>
<option value="9600">9600</option>
<option value="19200">19200</option>
<option value="38400">38400</option>
<option value="57600">57600</option>
<option value="115200">115200</option>
<option value="230400">230400</option>

</select>

</div>


<div class="row">

<label>Data Bits</label>

<select id="dataBits">

<option value="7">7</option>
<option value="8">8</option>

</select>

</div>


<div class="row">

<label>Parity</label>

<select id="parity">

<option value="N">None</option>
<option value="E">Even</option>
<option value="O">Odd</option>

</select>

</div>


<div class="row">

<label>Stop Bits</label>

<select id="stopBits">

<option value="1">1</option>
<option value="2">2</option>

</select>

</div>

</div>


<!-- ===================================================== -->
<!-- BUTTON -->
<!-- ===================================================== -->

<div class="card">

<button class="save"
        onclick="saveConfig()">

SAVE & APPLY

</button>


<button class="reboot"
        onclick="rebootDevice()">

REBOOT

</button>

</div>


</div>


<script>

// [HUONG DAN] JavaScript chạy trên trình duyệt: lấy /api/config rồi điền dữ liệu vào form.
async function loadConfig() {

    try {

        const r =
            await fetch("/api/config");


        const c =
            await r.json();

        document.getElementById(
            "lanDHCP"
        ).checked =
            c.network.lan.dhcp;


        document.getElementById(
            "lanIP"
        ).value =
            c.network.lan.ip;


        document.getElementById(
            "lanMask"
        ).value =
            c.network.lan.mask;


        document.getElementById(
            "lanGateway"
        ).value =
            c.network.lan.gateway;


        document.getElementById(
            "lanDNS"
        ).value =
            c.network.lan.dns;

        document.getElementById(
            "wifiEnabled"
        ).checked =
            c.network.wifi.enabled;


        document.getElementById(
            "wifiDHCP"
        ).checked =
            c.network.wifi.dhcp;


        document.getElementById(
            "wifiSSID"
        ).value =
            c.network.wifi.ssid;


        document.getElementById(
            "wifiPassword"
        ).value =
            c.network.wifi.password;


        document.getElementById(
            "wifiIP"
        ).value =
            c.network.wifi.ip;


        document.getElementById(
            "wifiMask"
        ).value =
            c.network.wifi.mask;


        document.getElementById(
            "wifiGateway"
        ).value =
            c.network.wifi.gateway;


        document.getElementById(
            "wifiDNS"
        ).value =
            c.network.wifi.dns;

        document.getElementById(
            "tcpEnabled"
        ).checked =
            c.tcpClient.enabled;

        document.getElementById(
            "devName"
        ).value =
            c.tcpClient.devName;
    
        document.getElementById(
            "serverIP"
        ).value =
            c.tcpClient.serverIP;


        document.getElementById(
            "serverPort"
        ).value =
            c.tcpClient.port;


        document.getElementById(
            "timeout"
        ).value =
            c.tcpClient.timeout;


        document.getElementById(
            "reconnect"
        ).value =
            c.tcpClient.reconnect;

        document.getElementById(
            "slaveID"
        ).value =
            c.serial.slaveId;


        document.getElementById(
            "baud"
        ).value =
            c.serial.baud;


        document.getElementById(
            "dataBits"
        ).value =
            c.serial.dataBits;


        document.getElementById(
            "parity"
        ).value =
            c.serial.parity;


        document.getElementById(
            "stopBits"
        ).value =
            c.serial.stopBits;

    }
    catch(e) {

        console.log(e);
    }
}

// [HUONG DAN] Thu thập form thành JSON rồi POST /api/config; thêm trường cần sửa cả C++ phía trên.
async function saveConfig() {

    const c = {

        network: {


            lan: {

                dhcp:
                    document.getElementById(
                        "lanDHCP"
                    ).checked,


                ip:
                    document.getElementById(
                        "lanIP"
                    ).value,


                mask:
                    document.getElementById(
                        "lanMask"
                    ).value,


                gateway:
                    document.getElementById(
                        "lanGateway"
                    ).value,


                dns:
                    document.getElementById(
                        "lanDNS"
                    ).value
            },


            wifi: {

                enabled:
                    document.getElementById(
                        "wifiEnabled"
                    ).checked,


                dhcp:
                    document.getElementById(
                        "wifiDHCP"
                    ).checked,


                ssid:
                    document.getElementById(
                        "wifiSSID"
                    ).value,


                password:
                    document.getElementById(
                        "wifiPassword"
                    ).value,


                ip:
                    document.getElementById(
                        "wifiIP"
                    ).value,


                mask:
                    document.getElementById(
                        "wifiMask"
                    ).value,


                gateway:
                    document.getElementById(
                        "wifiGateway"
                    ).value,


                dns:
                    document.getElementById(
                        "wifiDNS"
                    ).value
            }
        },

        tcpClient: {


            enabled:
                document.getElementById(
                    "tcpEnabled"
                ).checked,

            devName:
                document.getElementById(
                    "devName"
                ).value.trim(),
                
            serverIP:
                document.getElementById(
                    "serverIP"
                ).value,


            port:
                Number(
                    document.getElementById(
                        "serverPort"
                    ).value
                ),


            timeout:
                Number(
                    document.getElementById(
                        "timeout"
                    ).value
                ),


            reconnect:
                Number(
                    document.getElementById(
                        "reconnect"
                    ).value
                )
        },

        serial: {


            slaveId:
                Number(
                    document.getElementById(
                        "slaveID"
                    ).value
                ),


            baud:
                Number(
                    document.getElementById(
                        "baud"
                    ).value
                ),


            dataBits:
                Number(
                    document.getElementById(
                        "dataBits"
                    ).value
                ),


            parity:
                document.getElementById(
                    "parity"
                ).value,

            stopBits:
                Number(
                    document.getElementById(
                        "stopBits"
                    ).value
                )
        }
    };

    try {

        const r =
            await fetch(
                "/api/config",
                {

                    method:
                        "POST",

                    headers: {

                        "Content-Type":
                            "application/json"
                    },

                    body:
                        JSON.stringify(c)
                }
            );

        const result =
            await r.json();

        alert(
            result.message ||
            result.error ||
            "Done"
        );

        updateStatus();

    }
    catch(e) {

        alert(
            "Save failed"
        );
    }
}

// [HUONG DAN] Lấy /api/status và dựng chuỗi hiển thị; thêm trạng thái cần sửa cả handleStatus C++.
async function updateStatus() {

    try {

        const r =
            await fetch(
                "/api/status"
            );


        const s =
            await r.json();

        let text = "";

        text +=
            "================ LAN ================\n";

        text +=
            "Status  : " +
            (
                s.ethernet.connected
                ?
                "CONNECTED"
                :
                "DISCONNECTED"
            );

        text +=
            "\nMAC     : " +
            s.ethernet.mac;

        text +=
            "\nIP      : " +
            s.ethernet.ip;

        text +=
            "\nMASK    : " +
            s.ethernet.mask;

        text +=
            "\nGATEWAY : " +
            s.ethernet.gateway;

        text +=
            "\nDNS     : " +
            s.ethernet.dns;

        text +=
            "\nDHCP    : " +
            (
                s.ethernet.dhcp
                ?
                "ON"
                :
                "OFF"
            );

        text +=
            "\n\n================ WIFI ================\n";

        text +=
            "Status  : " +
            (
                s.wifi.connected
                ?
                "CONNECTED"
                :
                "DISCONNECTED"
            );

        text +=
            "\nEnabled : " +
            (
                s.wifi.enabled
                ?
                "YES"
                :
                "NO"
            );

        text +=
            "\nMAC     : " +
            s.wifi.mac;

        text +=
            "\nSSID    : " +
            s.wifi.ssid;

        text +=
            "\nIP      : " +
            s.wifi.ip;

        text +=
            "\nMASK    : " +
            s.wifi.mask;

        text +=
            "\nGATEWAY : " +
            s.wifi.gateway;

        text +=
            "\nDNS     : " +
            s.wifi.dns;

        text +=
            "\nDHCP    : " +
            (
                s.wifi.dhcp
                ?
                "ON"
                :
                "OFF"
            );

        text +=
            "\nRSSI    : " +
            s.wifi.rssi +
            " dBm";

        text +=
            "\n\n================ TCP ================\n";

        text +=
            "Status  : " +
            (
                s.tcp.connected
                ?
                "CONNECTED"
                :
                "DISCONNECTED"
            );

        text +=
            "\nEnabled : " +
            (
                s.tcp.enabled
                ?
                "YES"
                :
                "NO"
            );

        text +=
            "\nServer  : " +
            s.tcp.server;

        text +=
            "\nPort    : " +
            s.tcp.port;

        text +=
            "\n\n================ RS485 ================\n";

        text +=
            "Mode    : SLAVE";

        text +=
            "\nSlave ID: " +
            s.serial.slaveId;

        text +=
            "\nBaud    : " +
            s.serial.baud;

        text +=
            "\nFormat  : " +
            s.serial.dataBits +
            String.fromCharCode(s.serial.parity) +
            s.serial.stopBits;

        document.getElementById(
            "status"
        ).innerText =
            text;


    }
    catch(e) {

        document.getElementById(
            "status"
        ).innerText =
            "Connection lost";
    }
}

// [HUONG DAN] Hỏi xác nhận trên trình duyệt rồi POST /api/reboot.
async function rebootDevice() {


    if (!confirm(
        "Reboot ESP32-S3?"
    )) {

        return;
    }


    try {

        await fetch(
            "/api/reboot",
            {
                method:
                    "POST"
            }
        );

    }
    catch(e) {
    }


    alert(
        "Device rebooting..."
    );
}


// ===========================================================
// Start
// ===========================================================

loadConfig();

updateStatus();


// [HUONG DAN] Chu kỳ làm mới trạng thái trên trình duyệt: 2000 ms; không phải chu kỳ gửi TCP.
setInterval(
    updateStatus,
    2000
);


</script>

</body>

</html>

)rawliteral";


// ============================================================
// Web Root
// ============================================================

// [HUONG DAN] GET /: gửi chuỗi giao diện nhúng trong flash cho trình duyệt.
void handleRoot() {

  server.send_P(
      200,
      "text/html",
      INDEX_HTML
  );
}


// ============================================================
// Web Server
// ============================================================

// [HUONG DAN] [10] ĐĂNG KÝ API: muốn thêm API thì tạo hàm xử lý và đăng ký server.on tại đây.
void startWebServer() {


  server.on(
      "/",
      HTTP_GET,
      handleRoot
  );


  server.on(
      "/api/config",
      HTTP_GET,
      handleGetConfig
  );


  server.on(
      "/api/config",
      HTTP_POST,
      handleSetConfig
  );


  server.on(
      "/api/status",
      HTTP_GET,
      handleStatus
  );


  server.on(
      "/api/reboot",
      HTTP_POST,
      handleReboot
  );


  server.onNotFound(
      []() {

        server.send(
            404,
            "text/plain",
            "Not Found"
        );

      }
  );


  server.begin();


  Serial.println(
      "HTTP server started on port 80"
  );
}

// [HUONG DAN] [11] KHỞI ĐỘNG: Serial -> flash/cấu hình -> mạng -> web -> tạo 5 tác vụ.
// [HUONG DAN] Modbus được startModbus() khởi tạo bên trong modbusTask, không gọi trực tiếp tại setup.
void setup() {
  
  Serial.begin(115200);
  delay(100);
  // [HUONG DAN] true cho phép format LittleFS nếu mount thất bại; chưa kiểm tra giá trị trả về ở đây.
  LittleFS.begin(true);
  setDefaultConfig();
  loadConfig();
  Network.onEvent(onNetworkEvent);
  startEthernet();
  startWiFi();
  startWebServer();

  // [HUONG DAN] Tham số: hàm, tên, stack, đối số, priority, handle, core. Sửa ưu tiên/core tại đây.
  // [HUONG DAN] Core 0: mạng + TCP. Core 1: Modbus + web + LED; Modbus có priority cao nhất (5).
  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 4096, NULL, 2, &taskNetwork, 0);

  xTaskCreatePinnedToCore(tcpTask, "TCPTask", 6144, NULL, 3, &taskTCP, 0);

  xTaskCreatePinnedToCore(modbusTask, "ModbusTask", 4096, NULL, 5, &taskModbus, 1);

  xTaskCreatePinnedToCore(webTask, "WebTask", 6144, NULL, 2, &taskWeb, 1);

  xTaskCreatePinnedToCore(statusTask, "StatusTask", 4096, NULL, 1, &taskStatus, 1);
}

// [HUONG DAN] loop chỉ nhường CPU; công việc chính đã chuyển vào các tác vụ FreeRTOS.
void loop() {
  
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// [HUONG DAN] [12] TÁC VỤ FREERTOS: mỗi tác vụ chạy vòng lặp riêng; luôn giữ thời gian nhường CPU.
// [HUONG DAN] Modbus: khởi tạo một lần, liên tục phục vụ yêu cầu PLC, chờ 1 ms sau mỗi vòng.
void modbusTask(void *pvParameters)
{
    startModbus();
    
    for (;;)
    {
        mb.task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// [HUONG DAN] Web: xử lý HTTP và các callback cấu hình, chờ 2 ms sau mỗi vòng.
void webTask(void *pvParameters)
{
    for (;;)
    {
        server.handleClient();

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// [HUONG DAN] TCP: gọi bộ xử lý sau mỗi vòng và chờ 5 ms; chu kỳ gửi thực tế do tcpCfg.timeout quyết định.
void tcpTask(void *pvParameters)
{
    for (;;)
    {
        tcpTaskProcess();

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// [HUONG DAN] Mạng: kiểm tra Wi-Fi mỗi vòng (chờ 500 ms), thử lại theo WIFI_RECONNECT_INTERVAL.
// [HUONG DAN] lastWiFiReconnect cục bộ bên dưới đang được dùng; biến toàn cục cùng tên không được dùng ở đây.
void networkTask(void *pvParameters)
{
    uint32_t lastWiFiReconnect = 0;

    for (;;)
    {
        uint32_t now = millis();
        if (netCfg.wifiEnabled &&
            netCfg.wifiSSID.length() > 0)
        {
            if (WiFi.status() != WL_CONNECTED)
            {
                if (now - lastWiFiReconnect >=
                    WIFI_RECONNECT_INTERVAL)
                {
                    lastWiFiReconnect = now;

                    Serial.println(
                        "WiFi disconnected"
                    );

                    Serial.println(
                        "WiFi reconnecting..."
                    );

                    WiFi.disconnect(false);

                    delay(100);

                    if (!netCfg.wifiDHCP)
                    {
                        WiFi.config(
                            netCfg.wifiIP,
                            netCfg.wifiGateway,
                            netCfg.wifiMask,
                            netCfg.wifiDNS
                        );
                    }

                    WiFi.begin(
                        netCfg.wifiSSID.c_str(),
                        netCfg.wifiPassword.c_str()
                    );
                }
            }
        }

        vTaskDelay(
            pdMS_TO_TICKS(500)
        );
    }
}

// [HUONG DAN] LED: đảo mức mỗi 250 ms (chu kỳ sáng/tắt khoảng 500 ms), không kiểm tra tình trạng kết nối.
void statusTask(void *pvParameters)
{
    pinMode(STATUS_LED, OUTPUT);

    digitalWrite(
        STATUS_LED,
        LOW
    );

    for (;;)
    {
        digitalWrite(
            STATUS_LED,
            !digitalRead(STATUS_LED)
        );

        vTaskDelay(
            pdMS_TO_TICKS(250)
        );
    }
}
