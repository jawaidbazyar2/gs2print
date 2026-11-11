import AppKit
import SwiftUI

@main
struct GS2PrintApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @StateObject private var appModel = AppModel()

    var body: some Scene {
        Window("gs2print", id: "home") {
            HomeRoot(appModel: appModel)
        }
        .restorationBehavior(.disabled)
        .commands { openCommands }

        WindowGroup(id: "ticket", for: UUID.self) { $id in
            TicketRoot(appModel: appModel, id: id)
        }
        .handlesExternalEvents(matching: [])
        .restorationBehavior(.disabled)
        .defaultLaunchBehavior(.suppressed)
        .commands { openCommands }

        Settings {
            SettingsView()
        }
    }

    @CommandsBuilder
    private var openCommands: some Commands {
        CommandGroup(replacing: .newItem) {
            Button("Open…") {
                (appModel.focused ?? appModel.primary).openPanel()
            }
            .keyboardShortcut("o", modifiers: .command)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    static var pending: [URL] = []

    func application(_ application: NSApplication, open urls: [URL]) {
        if let model = AppBridge.model {
            for url in urls {
                model.handleFile(url)
            }
        } else {
            Self.pending.append(contentsOf: urls)
        }
    }

    func application(_ sender: NSApplication, openFile filename: String) -> Bool {
        application(sender, open: [URL(fileURLWithPath: filename)])
        return true
    }
}

private enum AppBridge {
    static weak var model: AppModel?
}

@MainActor
final class AppModel: ObservableObject {
    let server = PrintServer()
    let primary: JobModel
    @Published private(set) var extras: [UUID: JobModel] = [:]

    weak var focused: JobModel?
    private var tcpSession: JobModel?
    private var openExtra: ((UUID) -> Void)?
    private var pendingExtras: [UUID] = []
    private var seen: [String: Date] = [:]

    init() {
        let session = JobModel()
        primary = session
        AppBridge.model = self
        session.closeTCP = { [weak self] in
            self?.server.closeClient()
        }
        server.onClient = { [weak self] connected in
            self?.tcpAttached(connected)
        }
        server.onBytes = { [weak self] data in
            self?.tcpBytes(data)
        }
        server.onIdle = { [weak self] in
            self?.tcpSession?.finishNetworkJob()
        }
        server.onDisconnect = { [weak self] in
            guard let self else { return }
            let session = self.tcpSession
            self.tcpSession = nil
            session?.ownsTCP = false
            session?.finishNetworkJob()
        }
        server.restore()
        let queued = AppDelegate.pending
        AppDelegate.pending.removeAll()
        for url in queued {
            handleFile(url)
        }
    }

    func model(for id: UUID) -> JobModel? {
        extras[id]
    }

    func setOpener(_ open: @escaping (UUID) -> Void) {
        openExtra = open
        let pending = pendingExtras
        pendingExtras.removeAll()
        for id in pending {
            open(id)
        }
    }

    func closed(_ session: JobModel) {
        let wasTCP = session.ownsTCP
        session.shutdownConnection()
        if wasTCP {
            tcpSession = nil
        }
        if focused === session {
            focused = nil
        }
        guard session !== primary else { return }
        extras = extras.filter { $0.value !== session }
    }

    /// Finder and File → Open both land here. A second delivery of the same
    /// path in the same moment is ignored, and so is a ticket whose socket
    /// already has a live client.
    func handleFile(_ url: URL) {
        let path = url.resolvingSymlinksInPath().standardizedFileURL.path
        let now = Date()
        seen = seen.filter { now.timeIntervalSince($0.value) < 2 }
        if let previous = seen[path], now.timeIntervalSince(previous) < 0.5 {
            return
        }
        seen[path] = now

        let data: Data
        do {
            data = try Data(contentsOf: url)
        } catch {
            let session = focused ?? primary
            session.show(error.localizedDescription)
            session.raiseWindow()
            return
        }

        switch classify(data) {
        case .live(let socket):
            if isLive(socket) { return }
            let session = claimIdle()
            session.openTicket(path: socket)
        case .rejected(let message):
            let session = claimIdle()
            session.show(message)
            session.raiseWindow()
        case .dump:
            let session = focused ?? primary
            session.openDump(url)
            session.raiseWindow()
        }
    }

    private func tcpAttached(_ connected: Bool) {
        guard connected, tcpSession == nil else { return }
        let session = claimIdle()
        session.ownsTCP = true
        tcpSession = session
    }

    private func tcpBytes(_ data: Data) {
        let session: JobModel
        if let tcpSession {
            session = tcpSession
        } else {
            session = claimIdle()
            session.ownsTCP = true
            tcpSession = session
        }
        session.receive(data)
    }

    private func claimIdle() -> JobModel {
        if primary.isIdle { return primary }
        if let existing = extras.values.first(where: { $0.isIdle }) {
            return existing
        }
        let id = UUID()
        let session = makeSession()
        var next = extras
        next[id] = session
        extras = next
        if let openExtra {
            openExtra(id)
        } else {
            pendingExtras.append(id)
        }
        return session
    }

    private func makeSession() -> JobModel {
        let session = JobModel()
        session.closeTCP = { [weak self] in
            self?.server.closeClient()
        }
        return session
    }

    private func isLive(_ path: String) -> Bool {
        if primary.ticketPath == path && primary.ticket != nil { return true }
        return extras.values.contains { $0.ticketPath == path && $0.ticket != nil }
    }

    private enum TicketKind {
        case live(String)
        case dump
        case rejected(String)
    }

    private func classify(_ data: Data) -> TicketKind {
        let socket = headerField(data, key: "socket", cap: 4096)
        if socket.code < 0 {
            return .rejected("Socket path is too long.")
        }
        if socket.code == 0 || socket.value.isEmpty {
            return .dump
        }
        let printer = headerField(data, key: "printer", cap: 256)
        if printer.code != 1 || printer.value != "imagewriter-ii" {
            return .rejected("Unsupported printer.")
        }
        let socketPath = URL(fileURLWithPath: socket.value).resolvingSymlinksInPath().path
        return .live(socketPath)
    }

    private func headerField(_ data: Data, key: String, cap: Int) -> (code: Int32, value: String) {
        var buf = [CChar](repeating: 0, count: max(cap, 1))
        let code = data.withUnsafeBytes { raw -> Int32 in
            guard let base = raw.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return 0 }
            return key.withCString { keyPtr in
                gs2_ticket_field(base, raw.count, keyPtr, &buf, buf.count)
            }
        }
        if code != 1 { return (code, "") }
        return (code, String(cString: buf))
    }
}

@MainActor
final class JobModel: ObservableObject {
    @Published var image: CGImage?
    @Published var pageIndex = 0
    @Published var pageCount = 0
    @Published var message = "Open a printer dump."
    /// True while a network job is open. The status line shows a spinner for that span.
    @Published private(set) var printing = false
    @Published var pdfURL: URL?
    @Published var error = ""
    @Published fileprivate(set) var ownsTCP = false

    var closeTCP: (() -> Void)?
    private(set) var ticket: TicketClient?
    private(set) var ticketPath: String?

    var isIdle: Bool {
        ticket == nil && !ownsTCP && !networkJobOpen && !printing
            && pdfURL == nil && image == nil && pageCount == 0 && error.isEmpty
    }

    private var job: OpaquePointer?
    private var networkJobOpen = false
    private var sawBytes = false
    private var followTail = true
    private var previewGeneration = 0
    private var previewInFlight = false
    private var previewDirty = false
    private var previewScheduled = false
    private var previewDue = Date.distantPast
    private weak var window: NSWindow?

    func openPanel() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.title = "Open printer dump"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        openDump(url)
    }

    func closeActiveJob() {
        if ticket != nil || ownsTCP {
            relinquishStream()
        }
    }

    func canCloseJob(serverConnected: Bool) -> Bool {
        ticket != nil || (ownsTCP && serverConnected)
    }

    func show(_ text: String) {
        error = text
    }

    func step(_ delta: Int) {
        let next = pageIndex + delta
        guard next >= 0, next < pageCount else { return }
        pageIndex = next
        followTail = next == pageCount - 1
        requestPreview()
    }

    func openSavedPDF() {
        guard let pdfURL else { return }
        if !NSWorkspace.shared.open(pdfURL) {
            error = "Could not open \(pdfURL.lastPathComponent)."
        }
    }

    func attach(_ window: NSWindow) {
        self.window = window
    }

    func openDump(_ url: URL) {
        relinquishStream()
        error = ""
        pdfURL = nil
        followTail = false
        do {
            let data = try Data(contentsOf: url)
            let payload = data.withUnsafeBytes { raw -> Data in
                guard let base = raw.baseAddress?.assumingMemoryBound(to: UInt8.self) else {
                    return data
                }
                let offset = gs2_dump_payload_offset(base, raw.count)
                return data.subdata(in: offset..<data.count)
            }
            if let old = job {
                gs2_job_free(old)
                job = nil
            }
            guard let created = gs2_job_new() else {
                error = "Could not start a print job."
                return
            }
            gs2_job_set_auto_lf(created, PrinterSettings.autoLineFeed ? 1 : 0)
            payload.withUnsafeBytes { raw in
                if let base = raw.baseAddress?.assumingMemoryBound(to: UInt8.self), raw.count > 0 {
                    gs2_job_write(created, base, raw.count)
                }
            }
            gs2_job_finish(created)
            job = created
            pageCount = Int(gs2_job_page_count(created))
            pageIndex = 0
            previewGeneration += 1
            message = url.lastPathComponent
            requestPreview()

            let pdf = Self.pdfURL(for: url)
            let ok = pdf.path.withCString { gs2_job_save_pdf(created, $0) }
            if ok == 0 {
                pdfURL = pdf
            } else {
                pdfURL = nil
                error = "Could not write \(pdf.lastPathComponent)."
            }
        } catch {
            self.error = error.localizedDescription
        }
    }

    func openTicket(path: String) {
        relinquishStream()
        error = ""
        pdfURL = nil
        image = nil
        pageIndex = 0
        pageCount = 0
        message = "Connecting"
        printing = true
        ticketPath = path
        let client = TicketClient(path: path)
        ticket = client
        client.onBytes = { [weak self, weak client] data in
            guard let self, self.ticket === client else { return }
            self.receive(data)
        }
        client.onReady = { [weak self, weak client] in
            guard let self, self.ticket === client else { return }
            self.message = "Printing"
            self.printing = true
            self.raiseWindow()
        }
        client.onIdle = { [weak self, weak client] in
            guard let self, self.ticket === client else { return }
            self.relinquishStream()
        }
        client.onClosed = { [weak self, weak client] in
            guard let self, self.ticket === client else { return }
            self.relinquishStream()
        }
        client.onFailed = { [weak self, weak client] text in
            guard let self, self.ticket === client else { return }
            self.ticket = nil
            self.ticketPath = nil
            self.printing = false
            self.error = text
            self.message = "Open a printer dump."
        }
        raiseWindow()
        client.start()
    }

    func shutdownConnection() {
        relinquishStream()
    }

    func receive(_ data: Data) {
        guard !data.isEmpty else { return }
        if !networkJobOpen {
            beginNetworkJob()
        }
        guard networkJobOpen, let job else { return }
        sawBytes = true
        data.withUnsafeBytes { raw in
            if let base = raw.baseAddress?.assumingMemoryBound(to: UInt8.self), raw.count > 0 {
                gs2_job_write(job, base, raw.count)
            }
        }
        noteInk()
    }

    func finishNetworkJob() {
        guard networkJobOpen else { return }
        networkJobOpen = false
        printing = false
        previewScheduled = false
        guard sawBytes, let job else { return }
        gs2_job_finish(job)
        let count = Int(gs2_job_page_count(job))
        pageCount = count
        guard count > 0 else {
            image = nil
            return
        }
        if followTail || pageIndex >= count {
            pageIndex = count - 1
        }
        previewGeneration += 1
        let pdf = Self.documentsPDF()
        let saved: Int32 = pdf.withUnsafeFileSystemRepresentation { ptr in
            guard let ptr else { return -1 }
            return gs2_job_save_pdf(job, ptr)
        }
        requestPreview()
        if saved == 0 {
            pdfURL = pdf
            error = ""
            message = pdf.lastPathComponent
        } else {
            pdfURL = nil
            error = "Could not write \(pdf.lastPathComponent)."
        }
    }

    /// The window for this job comes forward. A ticket does this as soon as
    /// the file is accepted, before any printer bytes arrive.
    func raiseWindow() {
        raise(attempt: 0)
    }

    private func relinquishStream() {
        if let client = ticket {
            ticket = nil
            ticketPath = nil
            client.stop()
            if networkJobOpen {
                finishNetworkJob()
            } else {
                printing = false
                if pdfURL == nil && image == nil {
                    message = "Open a printer dump."
                }
            }
            return
        }
        if ownsTCP {
            ownsTCP = false
            closeTCP?()
            return
        }
        if networkJobOpen {
            finishNetworkJob()
        }
    }

    private func raise(attempt: Int) {
        if NSApp.isHidden {
            NSApp.unhide(nil)
        }
        NSApp.activate(ignoringOtherApps: true)
        if let window {
            if window.isMiniaturized {
                window.deminiaturize(nil)
            }
            window.makeKeyAndOrderFront(nil)
            window.orderFrontRegardless()
            if attempt == 0 {
                DispatchQueue.main.async { [weak self] in
                    self?.raise(attempt: 1)
                }
            }
            return
        }
        if attempt < 8 {
            DispatchQueue.main.async { [weak self] in
                self?.raise(attempt: attempt + 1)
            }
        }
    }

    private func beginNetworkJob() {
        previewGeneration += 1
        pdfURL = nil
        if let old = job {
            gs2_job_free(old)
            job = nil
        }
        guard let created = gs2_job_new() else {
            error = "Could not start a print job."
            networkJobOpen = false
            return
        }
        gs2_job_set_auto_lf(created, PrinterSettings.autoLineFeed ? 1 : 0)
        job = created
        networkJobOpen = true
        sawBytes = false
        followTail = true
        pageIndex = 0
        pageCount = 1
        error = ""
        message = "Printing"
        printing = true
        raiseWindow()
    }

    private func noteInk() {
        let now = Date()
        if now >= previewDue {
            previewDue = now.addingTimeInterval(0.1)
            refreshLivePreview()
            return
        }
        guard !previewScheduled else { return }
        previewScheduled = true
        let delay = max(previewDue.timeIntervalSince(now), 0)
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self else { return }
            self.previewScheduled = false
            guard self.networkJobOpen else { return }
            self.previewDue = Date().addingTimeInterval(0.1)
            self.refreshLivePreview()
        }
    }

    private func refreshLivePreview() {
        guard networkJobOpen, let job else { return }
        let count = Int(gs2_job_page_count(job))
        guard count > 0 else { return }
        pageCount = count
        if followTail || pageIndex >= count {
            pageIndex = count - 1
        }
        requestPreview()
    }

    /// Copying the page bitmap on the main thread keeps the pointer valid.
    /// Expanding it to the preview image runs off the main thread, one scan
    /// at a time, so a live job does not stall the socket.
    private func requestPreview() {
        previewDirty = true
        pumpPreview()
    }

    private func pumpPreview() {
        guard previewDirty, !previewInFlight, let job else { return }
        var width: Int32 = 0
        var height: Int32 = 0
        var stride: Int32 = 0
        guard let bits = gs2_job_page_bits(job, Int32(pageIndex), &width, &height, &stride) else { return }
        guard width > 0, height > 0, stride > 0 else { return }
        let copied = Data(bytes: bits, count: Int(height) * Int(stride))
        let w = Int(width)
        let h = Int(height)
        let s = Int(stride)
        let gen = previewGeneration
        previewDirty = false
        previewInFlight = true
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let image = pageImage(copied, width: w, height: h, stride: s)
            DispatchQueue.main.async {
                guard let self else { return }
                self.previewInFlight = false
                if gen == self.previewGeneration {
                    self.image = image
                }
                if self.previewDirty {
                    self.pumpPreview()
                }
            }
        }
    }

    private static let pdfStamp: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        return formatter
    }()

    private static func documentsPDF() -> URL {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Documents")
        let stamp = pdfStamp.string(from: Date())
        var url = docs.appendingPathComponent("GS2-Print-\(stamp).pdf")
        var n = 2
        while FileManager.default.fileExists(atPath: url.path) {
            url = docs.appendingPathComponent("GS2-Print-\(stamp)-\(n).pdf")
            n += 1
        }
        return url
    }

    private static func pdfURL(for source: URL) -> URL {
        let name = source.lastPathComponent
        let pdfName: String
        if let dot = name.lastIndex(of: "."), name.distance(from: dot, to: name.endIndex) - 1 <= 8 {
            pdfName = String(name[..<dot]) + ".pdf"
        } else {
            pdfName = name + ".pdf"
        }
        return source.deletingLastPathComponent().appendingPathComponent(pdfName)
    }
}

/// Full 720 dpi page, one gray byte per printer pixel. The view scales this
/// to the window. A baked 8× reduction was 90 dpi, and nearest-neighbor
/// then drew those pixels as blocks on a retina display.
private func pageImage(_ bits: Data, width: Int, height: Int, stride: Int) -> CGImage? {
    guard width > 0, height > 0, bits.count >= height * stride else { return nil }
    var pixels = [UInt8](repeating: 255, count: width * height)
    bits.withUnsafeBytes { raw in
        guard let base = raw.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
        pixels.withUnsafeMutableBytes { dst in
            guard let out = dst.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
            for y in 0..<height {
                let row = base + y * stride
                let dest = out + y * width
                let bytes = min(stride, (width + 7) / 8)
                for byte in 0..<bytes {
                    let value = row[byte]
                    if value == 0 { continue }
                    let x0 = byte * 8
                    let n = min(8, width - x0)
                    var mask: UInt8 = 0x80
                    for bit in 0..<n {
                        if value & mask != 0 { dest[x0 + bit] = 0 }
                        mask >>= 1
                    }
                }
            }
        }
    }
    let data = Data(pixels) as CFData
    guard let provider = CGDataProvider(data: data) else { return nil }
    return CGImage(
        width: width,
        height: height,
        bitsPerComponent: 8,
        bitsPerPixel: 8,
        bytesPerRow: width,
        space: CGColorSpaceCreateDeviceGray(),
        bitmapInfo: CGBitmapInfo(rawValue: 0),
        provider: provider,
        decode: nil,
        shouldInterpolate: true,
        intent: .defaultIntent
    )
}

private enum PrinterSettings {
    static let autoLineFeedKey = "imagewriter.autoLineFeed"

    static var autoLineFeed: Bool {
        UserDefaults.standard.bool(forKey: autoLineFeedKey)
    }
}

private struct SettingsView: View {
    @AppStorage(PrinterSettings.autoLineFeedKey) private var autoLineFeed = false

    var body: some View {
        Form {
            Section("ImageWriter") {
                Toggle("SW1-8  Line feed after carriage return", isOn: $autoLineFeed)
            }
        }
        .formStyle(.grouped)
        .frame(width: 420)
        .padding()
    }
}

private enum PreviewScale: Equatable {
    case fitWidth
    case fitHeight
    case fixed(CGFloat)
}

private let previewZoomStep: CGFloat = 1.25
private let previewMinLongSide: CGFloat = 80
private let previewMaxLongSide: CGFloat = 4000

struct ContentView: View {
    @ObservedObject var model: JobModel
    @ObservedObject var server: PrintServer
    var onKey: () -> Void = {}
    var onClose: () -> Void = {}

    @Environment(\.displayScale) private var displayScale
    @Environment(\.controlActiveState) private var controlActive
    @State private var portText: String
    @State private var scaleMode: PreviewScale = .fitHeight
    @State private var viewport = CGSize.zero

    init(model: JobModel, server: PrintServer, onKey: @escaping () -> Void = {}, onClose: @escaping () -> Void = {}) {
        self.model = model
        self.server = server
        self.onKey = onKey
        self.onClose = onClose
        _portText = State(initialValue: String(server.port))
    }

    var body: some View {
        VStack(spacing: 12) {
            if let image = model.image {
                jobStatus
                    .font(.headline)
                pagePreview(image)
                HStack {
                    Button("Previous") { model.step(-1) }
                        .disabled(model.pageIndex == 0)
                    Text("Page \(model.pageIndex + 1) of \(model.pageCount)")
                        .frame(minWidth: 140)
                    Button("Next") { model.step(1) }
                        .disabled(model.pageIndex + 1 >= model.pageCount)
                    Spacer(minLength: 12)
                    Button("−") { zoom(1 / previewZoomStep, image: image) }
                        .disabled(!canZoom(1 / previewZoomStep, image: image))
                    Button("+") { zoom(previewZoomStep, image: image) }
                        .disabled(!canZoom(previewZoomStep, image: image))
                    Button("Fit Width") { scaleMode = .fitWidth }
                    Button("Fit Height") { scaleMode = .fitHeight }
                }
            } else {
                Spacer()
                jobStatus
                    .foregroundStyle(.secondary)
                Spacer()
            }
            if let pdfURL = model.pdfURL {
                HStack(spacing: 4) {
                    Text("Wrote")
                        .font(.caption)
                    Button {
                        model.openSavedPDF()
                    } label: {
                        Text(pdfURL.lastPathComponent)
                            .font(.caption)
                            .underline()
                    }
                    .buttonStyle(.borderless)
                    .foregroundStyle(Color.accentColor)
                    .pointerStyle(.link)
                }
            }
            if !model.error.isEmpty {
                Text(model.error)
                    .font(.caption)
                    .foregroundStyle(.red)
            }
            Divider()
            serverBar
        }
        .padding()
        .frame(minWidth: 720, minHeight: 640)
        .background {
            WindowLifetime(onWindow: { model.attach($0) }, onClose: onClose)
        }
        .onAppear {
            if controlActive == .key { onKey() }
        }
        .onChange(of: controlActive) { _, state in
            if state == .key { onKey() }
        }
        .onChange(of: server.port) { _, port in
            portText = String(port)
        }
    }

    /// "Printing" while bytes are still arriving, with the spinner lit
    /// for that same span. A finished job is the filename alone.
    private var jobStatus: some View {
        HStack(spacing: 6) {
            if model.printing {
                Image(systemName: "progress.indicator")
                    .symbolEffect(.variableColor.iterative, isActive: true)
                    .accessibilityLabel("Job in progress")
            }
            Text(model.message)
        }
    }

    /// Fit modes use the viewport on each layout. A fixed scale is the last
    /// size the user chose with + or −, so resizing the window leaves it.
    private func pagePreview(_ image: CGImage) -> some View {
        GeometryReader { geo in
            let scale = resolvedScale(image: image, viewport: geo.size)
            ScrollView([.horizontal, .vertical]) {
                if let scale {
                    let drawnW = CGFloat(image.width) * scale
                    let drawnH = CGFloat(image.height) * scale
                    Image(decorative: image, scale: 1)
                        .resizable()
                        .interpolation(scale * displayScale >= 1 ? .none : .high)
                        // Every page has the same point size, so this frame
                        // does not change. Image will not repaint a new
                        // bitmap until its identity does.
                        .id(ObjectIdentifier(image))
                        .frame(width: drawnW, height: drawnH)
                        .background(Color.white)
                        .frame(minWidth: geo.size.width, minHeight: geo.size.height)
                }
            }
            // A new page recreates the scroll view so it starts at the top.
            // Zoom keeps this identity, so the offset stays.
            .id(model.pageIndex)
        }
        .onGeometryChange(for: CGSize.self) { $0.size } action: { viewport = $0 }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func resolvedScale(image: CGImage, viewport: CGSize) -> CGFloat? {
        let width = CGFloat(image.width)
        let height = CGFloat(image.height)
        guard width > 0, height > 0 else { return nil }
        switch scaleMode {
        case .fitWidth:
            guard viewport.width > 1 else { return nil }
            return viewport.width / width
        case .fitHeight:
            guard viewport.height > 1 else { return nil }
            return viewport.height / height
        case .fixed(let scale):
            return clampedScale(scale, image: image)
        }
    }

    private func zoom(_ factor: CGFloat, image: CGImage) {
        guard let current = resolvedScale(image: image, viewport: viewport) else { return }
        scaleMode = .fixed(clampedScale(current * factor, image: image))
    }

    private func canZoom(_ factor: CGFloat, image: CGImage) -> Bool {
        guard let current = resolvedScale(image: image, viewport: viewport) else { return false }
        let next = clampedScale(current * factor, image: image)
        if factor > 1 { return next > current * 1.001 }
        return next < current / 1.001
    }

    private func clampedScale(_ scale: CGFloat, image: CGImage) -> CGFloat {
        let limits = scaleLimits(image)
        return min(max(scale, limits.min), limits.max)
    }

    /// Fixed zoom keeps the page's long side between these point sizes.
    private func scaleLimits(_ image: CGImage) -> (min: CGFloat, max: CGFloat) {
        let longSide = CGFloat(max(image.width, image.height))
        guard longSide > 0 else { return (1, 1) }
        return (previewMinLongSide / longSide, previewMaxLongSide / longSide)
    }

    private var serverBar: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 16) {
                Toggle("Listen", isOn: listenBinding)
                TextField("Port", text: $portText)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 80)
                    .onSubmit { commitPortText(retry: true) }
                Toggle("Other computers", isOn: lanBinding)
                Spacer(minLength: 0)
                Button("Close Job") { model.closeActiveJob() }
                    .disabled(!model.canCloseJob(serverConnected: server.clientConnected))
            }
            if !server.address.isEmpty {
                Text(server.address)
                    .font(.caption)
                    .textSelection(.enabled)
            }
            if !server.bindError.isEmpty {
                Text(server.bindError)
                    .font(.caption)
                    .foregroundStyle(.red)
                    .textSelection(.enabled)
            }
        }
    }

    private var listenBinding: Binding<Bool> {
        Binding(
            get: { server.listening },
            set: { on in
                if on, !commitPortText(retry: false) { return }
                server.listening = on
            }
        )
    }

    private var lanBinding: Binding<Bool> {
        Binding(
            get: { server.allowLAN },
            set: { on in
                if !commitPortText(retry: false) { return }
                server.allowLAN = on
            }
        )
    }

    @discardableResult
    private func commitPortText(retry: Bool) -> Bool {
        let ok = server.commitPort(portText, rebindIfUnchanged: retry)
        if ok {
            portText = String(server.port)
        }
        return ok
    }
}

private struct HomeRoot: View {
    @ObservedObject var appModel: AppModel
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        ContentView(
            model: appModel.primary,
            server: appModel.server,
            onKey: { appModel.focused = appModel.primary },
            onClose: { appModel.closed(appModel.primary) }
        )
        .onAppear {
            appModel.setOpener { id in
                openWindow(id: "ticket", value: id)
            }
        }
        .onOpenURL { url in
            appModel.handleFile(url)
        }
    }
}

private struct TicketRoot: View {
    @ObservedObject var appModel: AppModel
    let id: UUID?
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        if let id, let model = appModel.model(for: id) {
            ContentView(
                model: model,
                server: appModel.server,
                onKey: { appModel.focused = model },
                onClose: { appModel.closed(model) }
            )
            .onAppear {
                appModel.setOpener { ticketID in
                    openWindow(id: "ticket", value: ticketID)
                }
            }
        }
    }
}

private struct WindowLifetime: NSViewRepresentable {
    var onWindow: (NSWindow) -> Void
    var onClose: () -> Void

    func makeNSView(context: Context) -> LifetimeView {
        let view = LifetimeView()
        view.onWindow = onWindow
        view.onClose = onClose
        return view
    }

    func updateNSView(_ view: LifetimeView, context: Context) {
        view.onWindow = onWindow
        view.onClose = onClose
    }
}

private final class LifetimeView: NSView {
    var onWindow: ((NSWindow) -> Void)?
    var onClose: (() -> Void)?
    private var observing: NSWindow?

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if let observing {
            NotificationCenter.default.removeObserver(self, name: NSWindow.willCloseNotification, object: observing)
            self.observing = nil
        }
        guard let window else { return }
        observing = window
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(willClose(_:)),
            name: NSWindow.willCloseNotification,
            object: window)
        onWindow?(window)
    }

    @objc private func willClose(_ note: Notification) {
        let close = onClose
        onClose = nil
        DispatchQueue.main.async {
            close?()
        }
    }

    deinit {
        if let observing {
            NotificationCenter.default.removeObserver(self, name: NSWindow.willCloseNotification, object: observing)
        }
    }
}
