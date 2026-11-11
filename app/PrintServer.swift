import Darwin
import Foundation

/// Raw printer port. One client at a time; a second connection is accepted
/// and closed. Silence ends a job in the app, and the socket stays open so
/// the next byte can start another job.
private final class SocketLoop {
    enum Result {
        case ready(String)
        case inUse
        case failed
    }

    private final class Slot {
        let fd: Int32
        let source: DispatchSourceRead
        var alive = true
        init(fd: Int32, source: DispatchSourceRead) {
            self.fd = fd
            self.source = source
        }
    }

    private let queue = DispatchQueue(label: "gs2print.server")
    private let events: EventBuffer
    private var listeners: [Slot] = []
    private var client: Slot?
    private var idleItem: DispatchWorkItem?
    private var readBuffer = [UInt8](repeating: 0, count: 16384)

    init(events: EventBuffer) {
        self.events = events
    }

    deinit {
        queue.sync {
            teardownLocked()
        }
    }

    /// Drops the listener and the client. The bool is true when a client was
    /// still attached; the caller finishes that job. Close runs in the
    /// source's cancel handler, which is queued ahead of the next start.
    func stopListening() -> Bool {
        queue.sync {
            let had = client != nil
            teardownLocked()
            return had
        }
    }

    func closeClient() -> Bool {
        queue.sync {
            guard client != nil else { return false }
            idleItem?.cancel()
            idleItem = nil
            dropClientLocked()
            return true
        }
    }

    func startListening(port: UInt16, lan: Bool) -> Result {
        queue.sync {
            do {
                let fds = try openFds(port: port, lan: lan)
                for fd in fds {
                    armListener(fd)
                }
                return .ready(Self.addressText(port: port, lan: lan))
            } catch BindError.inUse {
                return .inUse
            } catch {
                return .failed
            }
        }
    }

    private func teardownLocked() {
        idleItem?.cancel()
        idleItem = nil
        if client != nil {
            dropClientLocked()
        }
        for slot in listeners {
            retire(slot)
        }
        listeners.removeAll()
    }

    private func dropClientLocked() {
        idleItem?.cancel()
        idleItem = nil
        guard let client else { return }
        retire(client)
        self.client = nil
    }

    /// The fd stays open until the cancel handler runs. That handler is a
    /// later block on this queue, so the next `startListening` binds after
    /// the port is actually free.
    private func retire(_ slot: Slot) {
        let fd = slot.fd
        slot.alive = false
        slot.source.setEventHandler {}
        slot.source.setCancelHandler {
            close(fd)
        }
        slot.source.cancel()
    }

    private func armListener(_ fd: Int32) {
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        let slot = Slot(fd: fd, source: source)
        source.setEventHandler { [weak self, weak slot] in
            guard let self, let slot, slot.alive else { return }
            self.acceptLocked(slot)
        }
        source.resume()
        listeners.append(slot)
    }

    private func acceptLocked(_ slot: Slot) {
        while slot.alive {
            let fd = accept(slot.fd, nil, nil)
            if fd < 0 {
                let err = errno
                if err == EINTR { continue }
                return
            }
            if client != nil || !slot.alive {
                close(fd)
                continue
            }
            adopt(fd)
        }
    }

    private func adopt(_ fd: Int32) {
        var nosig: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, socklen_t(MemoryLayout<Int32>.size))
        let flags = fcntl(fd, F_GETFL)
        if flags >= 0 {
            _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
        }
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        let slot = Slot(fd: fd, source: source)
        client = slot
        source.setEventHandler { [weak self, weak slot] in
            guard let self, let slot, slot.alive else { return }
            self.readLocked(slot)
        }
        source.resume()
        armIdleLocked()
        events.push(.client(true))
    }

    private func readLocked(_ slot: Slot) {
        while slot.alive {
            let n = readBuffer.withUnsafeMutableBytes { raw -> Int in
                guard let base = raw.baseAddress else { return -1 }
                return recv(slot.fd, base, raw.count, 0)
            }
            if n > 0 {
                events.push(.bytes(Data(readBuffer[..<n])))
                armIdleLocked()
                continue
            }
            if n == 0 {
                dropClientLocked()
                events.push(.closed)
                return
            }
            let err = errno
            if err == EINTR { continue }
            if err == EAGAIN || err == EWOULDBLOCK { return }
            dropClientLocked()
            events.push(.closed)
            return
        }
    }

    private func armIdleLocked() {
        idleItem?.cancel()
        let item = DispatchWorkItem { [weak self] in
            guard let self, self.client != nil else { return }
            self.events.push(.idle)
        }
        idleItem = item
        queue.asyncAfter(deadline: .now() + 10, execute: item)
    }

    private enum BindError: Error {
        case inUse
        case failed
    }

    private func openFds(port: UInt16, lan: Bool) throws -> [Int32] {
        let v4 = try listen4(port: port, lan: lan)
        do {
            let v6 = try listen6(port: port, lan: lan)
            return [v4, v6]
        } catch BindError.inUse {
            close(v4)
            throw BindError.inUse
        } catch {
            return [v4]
        }
    }

    private func listen4(port: UInt16, lan: Bool) throws -> Int32 {
        let fd = try makeSocket(AF_INET)
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        if lan {
            addr.sin_addr.s_addr = in_addr_t(INADDR_ANY)
        } else if "127.0.0.1".withCString({ inet_pton(AF_INET, $0, &addr.sin_addr) }) != 1 {
            close(fd)
            throw BindError.failed
        }
        do {
            try bindAndListen(fd, &addr)
            return fd
        } catch {
            close(fd)
            throw error
        }
    }

    private func listen6(port: UInt16, lan: Bool) throws -> Int32 {
        let fd = try makeSocket(AF_INET6)
        var only: Int32 = 1
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &only, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in6()
        addr.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
        addr.sin6_family = sa_family_t(AF_INET6)
        addr.sin6_port = port.bigEndian
        addr.sin6_addr = lan ? in6addr_any : in6addr_loopback
        do {
            try bindAndListen(fd, &addr)
            return fd
        } catch {
            close(fd)
            throw error
        }
    }

    private func makeSocket(_ domain: Int32) throws -> Int32 {
        let fd = socket(domain, SOCK_STREAM, 0)
        if fd < 0 { throw BindError.failed }
        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        var nosig: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, socklen_t(MemoryLayout<Int32>.size))
        let flags = fcntl(fd, F_GETFL)
        if flags >= 0 {
            _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
        }
        _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
        return fd
    }

    private func bindAndListen<T>(_ fd: Int32, _ addr: inout T) throws {
        let length = socklen_t(MemoryLayout<T>.size)
        var err: Int32 = 0
        let rc: Int32 = withUnsafePointer(to: &addr) { raw in
            raw.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                let result = bind(fd, sa, length)
                if result != 0 { err = errno }
                return result
            }
        }
        if rc != 0 {
            throw err == EADDRINUSE ? BindError.inUse : BindError.failed
        }
        if listen(fd, 8) != 0 {
            throw BindError.failed
        }
    }

    private static func addressText(port: UInt16, lan: Bool) -> String {
        if !lan {
            return "127.0.0.1:\(port)"
        }
        var ips: [String] = []
        var seen = Set<String>()
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let first = ifaddr else {
            return "0.0.0.0:\(port)"
        }
        defer { freeifaddrs(ifaddr) }
        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let iface = cursor {
            let info = iface.pointee
            cursor = info.ifa_next
            let flags = Int32(info.ifa_flags)
            if (flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0 { continue }
            guard let sa = info.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            let name = getnameinfo(
                sa, socklen_t(sa.pointee.sa_len),
                &host, socklen_t(host.count),
                nil, 0, NI_NUMERICHOST)
            if name != 0 { continue }
            let ip = String(cString: host)
            if ip.isEmpty || !seen.insert(ip).inserted { continue }
            ips.append(ip)
        }
        ips.sort()
        if ips.isEmpty { return "0.0.0.0:\(port)" }
        return ips.map { "\($0):\(port)" }.joined(separator: "\n")
    }
}

private final class EventBuffer {
    enum Event {
        case bytes(Data)
        case idle
        case client(Bool)
        case closed
        case ready
        case failed(String)
    }

    private let lock = NSLock()
    private var events: [Event] = []
    var onMain: (() -> Void)?

    func push(_ event: Event) {
        lock.lock()
        events.append(event)
        lock.unlock()
        DispatchQueue.main.async { [weak self] in
            self?.onMain?()
        }
    }

    func take() -> [Event] {
        lock.lock()
        let out = events
        events.removeAll()
        lock.unlock()
        return out
    }
}

@MainActor
final class PrintServer: ObservableObject {
    static let defaultPort = 9100

    @Published private(set) var address = ""
    @Published private(set) var bindError = ""
    @Published private(set) var clientConnected = false

    @Published var listening: Bool {
        didSet {
            guard ready, listening != oldValue else { return }
            UserDefaults.standard.set(listening, forKey: Keys.listen)
            rebind()
        }
    }

    @Published var port: Int {
        didSet {
            guard ready, port != oldValue else { return }
            UserDefaults.standard.set(port, forKey: Keys.port)
            rebind()
        }
    }

    @Published var allowLAN: Bool {
        didSet {
            guard ready, allowLAN != oldValue else { return }
            UserDefaults.standard.set(allowLAN, forKey: Keys.lan)
            rebind()
        }
    }

    var onBytes: ((Data) -> Void)?
    var onIdle: (() -> Void)?
    var onDisconnect: (() -> Void)?
    var onClient: ((Bool) -> Void)?

    private let events = EventBuffer()
    private let loop: SocketLoop
    private var ready = false

    private enum Keys {
        static let listen = "serverListen"
        static let port = "serverPort"
        static let lan = "serverLAN"
    }

    init() {
        loop = SocketLoop(events: events)
        let defaults = UserDefaults.standard
        let stored = defaults.object(forKey: Keys.port) as? Int
        if let stored, (1...65535).contains(stored) {
            port = stored
        } else {
            port = Self.defaultPort
        }
        listening = defaults.bool(forKey: Keys.listen)
        allowLAN = defaults.bool(forKey: Keys.lan)
        events.onMain = { [weak self] in
            self?.drain()
        }
    }

    func restore() {
        ready = true
        rebind()
    }

    /// Stores a port in 1...65535. The same port retries a failed bind when
    /// `rebindIfUnchanged` is set, which is the field's submit action.
    @discardableResult
    func commitPort(_ text: String, rebindIfUnchanged: Bool = true) -> Bool {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        guard let value = Int(trimmed), (1...65535).contains(value) else {
            bindError = "Port must be between 1 and 65535."
            return false
        }
        if value != port {
            port = value
        } else if rebindIfUnchanged, listening {
            rebind()
        }
        return true
    }

    func closeClient() {
        let dropped = loop.closeClient()
        drain()
        if dropped {
            clientConnected = false
            onDisconnect?()
        }
    }

    private func rebind() {
        guard ready else { return }
        let dropped = loop.stopListening()
        drain()
        if dropped {
            clientConnected = false
            onDisconnect?()
        }
        guard listening else {
            address = ""
            bindError = ""
            clientConnected = false
            return
        }
        guard (1...65535).contains(port) else {
            address = ""
            bindError = "Port must be between 1 and 65535."
            clientConnected = false
            return
        }
        switch loop.startListening(port: UInt16(port), lan: allowLAN) {
        case .ready(let text):
            address = text
            bindError = ""
        case .inUse:
            address = ""
            bindError = "Port \(port) is already in use."
            clientConnected = false
        case .failed:
            address = ""
            bindError = "Could not listen on port \(port)."
            clientConnected = false
        }
    }

    private func drain() {
        for event in events.take() {
            switch event {
            case .bytes(let data):
                onBytes?(data)
            case .idle:
                onIdle?()
            case .client(let connected):
                clientConnected = connected
                onClient?(connected)
            case .closed:
                clientConnected = false
                onDisconnect?()
            case .ready, .failed:
                break
            }
        }
    }
}

/// One GS2 ticket. Connects to the `AF_UNIX` path in the ticket and reads
/// printer bytes. GS2 owns the socket file; this side never binds or unlinks it.
final class TicketClient {
    let path: String

    var onBytes: ((Data) -> Void)?
    var onReady: (() -> Void)?
    var onIdle: (() -> Void)?
    var onClosed: (() -> Void)?
    var onFailed: ((String) -> Void)?

    private enum Dial {
        case ok(Int32)
        case tooLong
        case failed(Int32)
    }

    private let queue = DispatchQueue(label: "gs2print.ticket")
    private let idleQueue = DispatchQueue(label: "gs2print.ticket.idle")
    private let stateLock = NSLock()
    private let events = EventBuffer()
    private var fd: Int32 = -1
    private var idleItem: DispatchWorkItem?
    private var stopRequested = false
    private var readBuffer = [UInt8](repeating: 0, count: 16384)

    init(path: String) {
        self.path = path
        events.onMain = { [weak self] in
            self?.drain()
        }
    }

    deinit {
        idleItem?.cancel()
        let current = takeFd()
        if current >= 0 {
            shutdown(current, SHUT_RDWR)
            close(current)
        }
    }

    func start() {
        queue.async { [weak self] in
            self?.connectAndRead()
        }
    }

    /// Unblocks the read. The caller finishes the job; no event is posted.
    func stop() {
        stateLock.lock()
        stopRequested = true
        let current = fd
        stateLock.unlock()
        idleItem?.cancel()
        if current >= 0 {
            shutdown(current, SHUT_RDWR)
        }
    }

    private func connectAndRead() {
        stateLock.lock()
        let stopped = stopRequested
        stateLock.unlock()
        if stopped { return }
        switch dial() {
        case .ok(let opened):
            stateLock.lock()
            let stopped = stopRequested
            if !stopped {
                fd = opened
            }
            stateLock.unlock()
            if stopped {
                close(opened)
                return
            }
            events.push(.ready)
            armIdle()
            readLoop(opened)
        case .tooLong:
            events.push(.failed("Socket path is too long."))
        case .failed(let err):
            events.push(.failed(explain(err)))
        }
    }

    private func dial() -> Dial {
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let cap = MemoryLayout.size(ofValue: addr.sun_path)
        let encoded = Array(path.utf8CString)
        if encoded.isEmpty || encoded.count > cap {
            return .tooLong
        }
        withUnsafeMutableBytes(of: &addr.sun_path) { raw in
            for index in 0..<encoded.count {
                raw[index] = UInt8(bitPattern: encoded[index])
            }
        }
        addr.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)

        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        if fd < 0 { return .failed(errno) }
        var nosig: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, socklen_t(MemoryLayout<Int32>.size))
        _ = fcntl(fd, F_SETFD, FD_CLOEXEC)

        var err: Int32 = 0
        let rc: Int32 = withUnsafePointer(to: &addr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                let result = connect(fd, sa, socklen_t(MemoryLayout<sockaddr_un>.size))
                if result != 0 { err = errno }
                return result
            }
        }
        if rc != 0 {
            close(fd)
            return .failed(err)
        }
        return .ok(fd)
    }

    private func explain(_ err: Int32) -> String {
        "Could not connect to \(path): \(String(cString: strerror(err)))"
    }

    /// Blocking read. A dispatch source on this socket can miss the peer's
    /// close, so the job would stay open and never write its PDF.
    private func readLoop(_ opened: Int32) {
        while true {
            stateLock.lock()
            let stopped = stopRequested
            stateLock.unlock()
            if stopped { break }
            let n = readBuffer.withUnsafeMutableBytes { raw -> Int in
                guard let base = raw.baseAddress else { return -1 }
                return recv(opened, base, raw.count, 0)
            }
            if n > 0 {
                events.push(.bytes(Data(readBuffer[..<n])))
                armIdle()
                continue
            }
            if n < 0 && errno == EINTR { continue }
            break
        }
        idleItem?.cancel()
        let mine = takeFd()
        if mine >= 0 {
            close(mine)
        }
        stateLock.lock()
        let stopped = stopRequested
        stateLock.unlock()
        if !stopped {
            events.push(.closed)
        }
    }

    private func armIdle() {
        idleItem?.cancel()
        let item = DispatchWorkItem { [weak self] in
            self?.cutOff()
        }
        idleItem = item
        idleQueue.asyncAfter(deadline: .now() + 10, execute: item)
    }

    /// Wakes the blocking read. The read loop reports the close, which ends
    /// the job the same way a hangup from GS2 does.
    private func cutOff() {
        stateLock.lock()
        let current = fd
        stateLock.unlock()
        if current >= 0 {
            shutdown(current, SHUT_RDWR)
        }
    }

    private func takeFd() -> Int32 {
        stateLock.lock()
        let current = fd
        fd = -1
        stateLock.unlock()
        return current
    }

    private func drain() {
        for event in events.take() {
            switch event {
            case .bytes(let data):
                onBytes?(data)
            case .ready:
                onReady?()
            case .idle:
                onIdle?()
            case .closed:
                onClosed?()
            case .failed(let message):
                onFailed?(message)
            case .client:
                break
            }
        }
    }
}
