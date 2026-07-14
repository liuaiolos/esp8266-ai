import Foundation

/// USB serial fallback for networks where the clock cannot reach the bridge.
/// Frames use the same JSON payloads as the HTTP endpoints:
/// bridge -> device: #HELLO, #STATUS {json}, #NET {json}
/// device -> bridge: #DEVICE {json}
final class SerialLink {
    private let service: StatusService
    private let netMonitor: NetSpeedMonitor
    private var fd: Int32 = -1
    private var portPath = ""
    private var linked = false
    private var openedAt = Date.distantPast
    private var lastHelloAt = Date.distantPast
    private var lastStatusAt = Date.distantPast
    private var lastNetAt = Date.distantPast
    private var rxBuf = Data()
    private var timer: Timer?

    init(service: StatusService, netMonitor: NetSpeedMonitor) {
        self.service = service
        self.netMonitor = netMonitor
    }

    func start() {
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            self?.tick()
        }
    }

    private func tick() {
        if fd < 0 { scanAndOpen(); return }
        readPending()
        let now = Date()
        if !linked {
            if now.timeIntervalSince(openedAt) > 30 { closePort(); return }
            if now.timeIntervalSince(lastHelloAt) > 3 {
                lastHelloAt = now
                send(Data("#HELLO\n".utf8))
            }
            return
        }
        if now.timeIntervalSince(lastStatusAt) > 5 {
            lastStatusAt = now
            send(frame("#STATUS ", service.snapshot().jsonData()))
        }
        if now.timeIntervalSince(lastNetAt) > 2 {
            lastNetAt = now
            send(frame("#NET ", netMonitor.jsonData()))
        }
    }

    private func frame(_ prefix: String, _ json: Data) -> Data {
        var frame = Data(prefix.utf8)
        frame.append(json)
        frame.append(0x0A)
        return frame
    }

    private func scanAndOpen() {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: "/dev")) ?? []
        for name in names.filter({ $0.hasPrefix("cu.usbserial") || $0.hasPrefix("cu.wchusbserial") }).sorted()
        where openPort("/dev/" + name) { return }
    }

    private func openPort(_ path: String) -> Bool {
        let candidate = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard candidate >= 0 else { return false }
        var tio = termios()
        tcgetattr(candidate, &tio)
        cfmakeraw(&tio)
        cfsetspeed(&tio, speed_t(B115200))
        tio.c_cflag |= tcflag_t(CLOCAL | CREAD)
        tio.c_cflag &= ~tcflag_t(HUPCL)
        tcsetattr(candidate, TCSANOW, &tio)
        // Deassert DTR/RTS so the CH340 auto-reset circuit leaves the ESP32 running.
        var bits: Int32 = 0x006
        _ = ioctl(candidate, 0x8004_746B, &bits)
        fd = candidate
        portPath = path
        linked = false
        openedAt = Date()
        lastHelloAt = .distantPast
        rxBuf.removeAll()
        FileHandle.standardError.write(Data("[serial] trying \(path)\n".utf8))
        return true
    }

    private func closePort() {
        if fd >= 0 { close(fd) }
        if linked || fd >= 0 { FileHandle.standardError.write(Data("[serial] closed \(portPath)\n".utf8)) }
        fd = -1
        portPath = ""
        linked = false
    }

    private func send(_ data: Data) {
        guard fd >= 0 else { return }
        let n = data.withUnsafeBytes { write(fd, $0.baseAddress, data.count) }
        if n < 0 && (errno == ENXIO || errno == EIO || errno == EBADF || errno == ENODEV) { closePort() }
    }

    private func readPending() {
        var buf = [UInt8](repeating: 0, count: 4096)
        while true {
            let n = read(fd, &buf, buf.count)
            if n > 0 {
                rxBuf.append(contentsOf: buf[0..<n])
                if rxBuf.count > 16_384 { rxBuf.removeAll() }
                continue
            }
            if n == 0 || (n < 0 && errno != EAGAIN) { closePort() }
            break
        }
        while let newline = rxBuf.firstIndex(of: 0x0A) {
            let lineData = rxBuf.prefix(upTo: newline)
            rxBuf.removeSubrange(...newline)
            guard let line = String(data: lineData, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines) else { continue }
            if line.hasPrefix("#DEVICE"), !linked {
                linked = true
                lastStatusAt = .distantPast
                lastNetAt = .distantPast
                FileHandle.standardError.write(Data("[serial] linked \(portPath): \(line)\n".utf8))
            }
        }
    }
}
