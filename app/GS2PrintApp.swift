import AppKit
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
    @Published var status = ""
    @Published var error = ""

    private var job: OpaquePointer?

    func openPanel() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.title = "Open printer dump"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        open(url)
    }

    func step(_ delta: Int) {
        let next = pageIndex + delta
        guard next >= 0, next < pageCount else { return }
        pageIndex = next
        image = Self.preview(job, page: next)
    }

    private func open(_ url: URL) {
        error = ""
        status = ""
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
            image = Self.preview(created, page: 0)
            message = url.lastPathComponent

            let pdf = Self.pdfURL(for: url)
            let ok = pdf.path.withCString { gs2_job_save_pdf(created, $0) }
            if ok == 0 {
                status = "Wrote \(pdf.lastPathComponent)"
            } else {
                error = "Could not write \(pdf.lastPathComponent)."
            }
        } catch {
            self.error = error.localizedDescription
        }
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

    private static func preview(_ job: OpaquePointer?, page: Int) -> CGImage? {
        guard let job else { return nil }
        var width: Int32 = 0
        var height: Int32 = 0
        var stride: Int32 = 0
        guard let bits = gs2_job_page_bits(job, Int32(page), &width, &height, &stride) else {
            return nil
        }
        let scale = 8
        guard Int(width) % scale == 0, Int(height) % scale == 0, stride > 0 else { return nil }
        let pw = Int(width) / scale
        let ph = Int(height) / scale
        var pixels = [UInt8](repeating: 255, count: pw * ph)
        for y in 0..<ph {
            for x in 0..<pw {
                let y0 = y * scale
                var on = false
                for dy in 0..<scale {
                    if bits[(y0 + dy) * Int(stride) + x] != 0 {
                        on = true
                        break
                    }
                }
                if on { pixels[y * pw + x] = 0 }
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
}

struct ContentView: View {
    @ObservedObject var model: JobModel

    var body: some View {
        VStack(spacing: 12) {
            if let image = model.image {
                Text(model.message)
                    .font(.headline)
                Image(decorative: image, scale: 1)
                    .resizable()
                    .scaledToFit()
                    .background(Color.white)
                    .padding(.horizontal)
                HStack {
                    Button("Previous") { model.step(-1) }
                        .disabled(model.pageIndex == 0)
                    Text("Page \(model.pageIndex + 1) of \(model.pageCount)")
                        .frame(minWidth: 140)
                    Button("Next") { model.step(1) }
                        .disabled(model.pageIndex + 1 >= model.pageCount)
                }
            } else {
                Spacer()
                Text(model.message)
                    .foregroundStyle(.secondary)
                Spacer()
            }
            if !model.status.isEmpty {
                Text(model.status)
                    .font(.caption)
                    .textSelection(.enabled)
            }
            if !model.error.isEmpty {
                Text(model.error)
                    .font(.caption)
                    .foregroundStyle(.red)
            }
        }
        .padding()
        .frame(minWidth: 720, minHeight: 640)
    }
}
