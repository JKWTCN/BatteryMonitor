// Razer Basilisk X HyperSpeed(2.4G Dongle)电量插件。
// 协议从 src/providers/hid/RazerHidProvider.cpp 移植:Razer 私有协议走
// HID feature report(90 字节 payload + 1 字节 Report ID)。
// 插件开发文档:docs/js-plugin.md(中文)、docs/js-plugin-en.md(English)。
//
// 使用:构建时会复制到 exe 同级 plugins/razer/ 目录,重启程序后自动加载。

export function Name()      { return "Razer Basilisk X HyperSpeed (JS)"; }
export function VendorId()  { return 0x1532; }
export function ProductId() { return [0x0083]; }

// —— 协议常量(与原生实现一致)——
const REPORT_SIZE = 90;            // feature 报告数据长度
const REPORT_WITH_ID = 91;         // 含 Report ID
const STATUS_OK = 0x02;            // 响应 status 字节
const CLASS_BATTERY = 0x07;        // 命令类:电池
const CMD_GET_BATTERY = 0x80;      // 查电量
const DATA_SIZE = 0x02;            // 参数长度
const TX_ID = 0xFF;                // Basilisk X 的 transactionId
const WAIT_MS = 31;                // 发包后等待设备处理
const MAX_RETRIES = 5;

// CRC:payload[2..87] 逐字节异或。
function crc(payload) {
    let c = 0;
    for (let i = 2; i < 88 && i < payload.length; i++) {
        c ^= payload[i];
    }
    return c;
}

function makeRequest(cmdClass, cmd, dataSize) {
    const p = new Array(REPORT_SIZE).fill(0);
    p[0] = 0x00;        // status:新命令
    p[1] = TX_ID;       // transactionId
    p[4] = 0x00;        // 协议类型
    p[5] = dataSize;
    p[6] = cmdClass;
    p[7] = cmd;
    p[88] = crc(p);
    return p;
}

// 发一包 feature 请求并读响应;响应校验失败 / 收发异常返回 null
// (纯输入接口上 sendFeature 会抛异常,捕获后静默跳过该接口)。
function send(cmdClass, cmd, dataSize) {
    const req = makeRequest(cmdClass, cmd, dataSize);
    const out = [0x00].concat(req);   // 前缀 Report ID(单一 feature 报告,填 0)

    for (let retry = 0; retry < MAX_RETRIES; retry++) {
        try {
            device.sendFeature(out, REPORT_WITH_ID);
            if (WAIT_MS > 0) {
                device.pause(WAIT_MS);
            }
            const r = device.getFeature(REPORT_WITH_ID);
            if (!r) {
                continue;
            }
            // 兼容带 / 不带 Report ID 前缀两种返回,取 90 字节 payload。
            let p;
            if (r.length >= REPORT_WITH_ID) {
                p = r.subarray(1, 1 + REPORT_SIZE);
            } else if (r.length >= REPORT_SIZE) {
                p = r.subarray(0, REPORT_SIZE);
            } else {
                continue;
            }
            // 校验:status 成功 + 命令类 / 命令号回显一致。
            if (p[0] !== STATUS_OK || p[6] !== req[6] || p[7] !== req[7]) {
                continue;
            }
            return p;
        } catch (e) {
            return null;   // 该接口不支持 feature 报告等,交由宿主试下一接口
        }
    }
    return null;
}

export function GetBattery() {
    const resp = send(CLASS_BATTERY, CMD_GET_BATTERY, DATA_SIZE);
    if (!resp) {
        return null;
    }
    // 电量原始字节 0-255 → 百分比(四舍五入)。Basilisk X 不支持充电状态查询
    // (原生表 chargingUnsupported),不返回 charging。
    const percentage = Math.round((resp[9] * 100 + 127) / 255);
    return { percentage: percentage };
}
