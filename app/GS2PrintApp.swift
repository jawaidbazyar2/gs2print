import AppKit
import Combine
import SwiftUI

@main
struct GS2PrintApp: App {
    @StateObject private var model = JobModel()

    var body: some Scene {
        WindowGroup {
            ContentView(model: model)
        }
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Open…") {
                    model.openPanel()
                }
                .keyboardShortcut("o", modifiers: .command)
            }
        }
    }
}

@MainActor
final class JobModel: ObservableObject {
    @Published var image: CGImage?
    @Published var pageIndex = 0
    @Published var pageCount = 0
    @Published var message = "Open a printer dump."
    @Published var pdfURL: URL?
    @Published var error = ""

    let server = PrintServer()

    private var job: OpaquePointer?
    private var cancellables = Set<AnyCancellable>()
    private var networkJobOpen = false
    private var sawBytes = false
    private var followTail = true
    private var previewGeneration = 0
    private var previewInFlight = false
    private var previewDirty = false
    private var previewScheduled = false
    private var previewDue = Date.distantPast

    init() {
        server.onBytes = { [weak self] data in
            self?.receive(data)
        }
        server.onIdle = { [weak self] in
            self?.finishNetworkJob()
        }
        server.onDisconnect = { [weak self] in
            self?.finishNetworkJob()
        }
        server.objectWillChange.sink { [weak self] _ in
            self?.objectWillChange.send()
        }.store(in: &cancellables)
        server.restore()
    }

    func openPanel() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.title = "Open printer dump"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        open(url)
    }

    func closeServerJob() {
        server.closeClient()
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

    private func open(_ url: URL) {
        finishNetworkJob()
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

    private func receive(_ data: Data) {
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
        job = created
        networkJobOpen = true
        sawBytes = false
        followTail = true
        pageIndex = 0
        pageCount = 1
        error = ""
        message = "Printing"
    }

    /// Idle and socket close both end the job. A second call is a no-op, and
    /// a connection that never sent bytes does not write a PDF.
    private func finishNetworkJob() {
        guard networkJobOpen else { return }
        networkJobOpen = false
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

    /// The page bitmap is large enough that drawing it inline stalls the
    /// socket. One scan runs at a time, off the main thread; a newer copy
    /// replaces it when that scan finishes.
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
        let scale = 8
        guard width > 0, height > 0, stride > 0,
              Int(width) % scale == 0, Int(height) % scale == 0 else { return }
        let copied = Data(bytes: bits, count: Int(height) * Int(stride))
        let w = Int(width)
        let h = Int(height)
        let s = Int(stride)
        let gen = previewGeneration
        previewDirty = false
        previewInFlight = true
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let image = pageImage(copied, width: w, height: h, stride: s, scale: scale)
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

private func pageImage(_ bits: Data, width: Int, height: Int, stride: Int, scale: Int) -> CGImage? {
    let pw = width / scale
    let ph = height / scale
    guard pw > 0, ph > 0, bits.count >= height * stride else { return nil }
    var pixels = [UInt8](repeating: 255, count: pw * ph)
    bits.withUnsafeBytes { raw in
        guard let base = raw.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
        pixels.withUnsafeMutableBytes { dst in
            guard let out = dst.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
            for y in 0..<ph {
                let y0 = y * scale
                for x in 0..<pw {
                    var on = false
                    for dy in 0..<scale {
                        if base[(y0 + dy) * stride + x] != 0 {
                            on = true
                            break
                        }
                    }
                    if on { out[y * pw + x] = 0 }
                }
            }
        }
    }
    let data = Data(pixels) as CFData
    guard let provider = CGDataProvider(data: data) else { return nil }
    return CGImage(
        width: pw,
        height: ph,
        bitsPerComponent: 8,
        bitsPerPixel: 8,
        bytesPerRow: pw,
        space: CGColorSpaceCreateDeviceGray(),
        bitmapInfo: CGBitmapInfo(rawValue: 0),
        provider: provider,
        decode: nil,
        shouldInterpolate: false,
        intent: .defaultIntent
    )
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
    @State private var portText: String
    @State private var scaleMode: PreviewScale = .fitHeight
    @State private var viewport = CGSize.zero

    init(model: JobModel) {
        self.model = model
        _portText = State(initialValue: String(model.server.port))
    }

    var body: some View {
        VStack(spacing: 12) {
            if let image = model.image {
                Text(model.message)
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
                Text(model.message)
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
                        .interpolation(.none)
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
                Button("Close Job") { model.closeServerJob() }
                    .disabled(!model.server.clientConnected)
            }
            if !model.server.address.isEmpty {
                Text(model.server.address)
                    .font(.caption)
                    .textSelection(.enabled)
            }
            if !model.server.bindError.isEmpty {
                Text(model.server.bindError)
                    .font(.caption)
                    .foregroundStyle(.red)
                    .textSelection(.enabled)
            }
        }
    }

    private var listenBinding: Binding<Bool> {
        Binding(
            get: { model.server.listening },
            set: { on in
                if on, !commitPortText(retry: false) { return }
                model.server.listening = on
            }
        )
    }

    private var lanBinding: Binding<Bool> {
        Binding(
            get: { model.server.allowLAN },
            set: { on in
                if !commitPortText(retry: false) { return }
                model.server.allowLAN = on
            }
        )
    }

    @discardableResult
    private func commitPortText(retry: Bool) -> Bool {
        let ok = model.server.commitPort(portText, rebindIfUnchanged: retry)
        if ok {
            portText = String(model.server.port)
        }
        return ok
    }
}
